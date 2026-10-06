/* jobs_events: the Phase 6 system-event producers and topic schemas for Jobs
 * triggers (PLAN.md 8.1). The engine itself never talks to Wi-Fi/battery; this
 * bridge reads the compact sysmon snapshot in main's 1 Hz loop and publishes
 * typed events through devos_events. Emitting outside any engine lock, with
 * explicit bounded copies. No LVGL. */
#include "jobs_providers.h"
#include "devos_events.h"
#include "devos_jobs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The SD mount point normally comes from devos_config.h; fall back to the same
 * values so this bridge also links into tests that don't carry that include
 * path (the boot counter is only read at runtime). */
#if defined(__has_include)
#  if __has_include("devos_config.h")
#    include "devos_config.h"
#  endif
#endif
#ifndef TAB5_SD_MOUNT_POINT
#  ifdef ESP_PLATFORM
#    define TAB5_SD_MOUNT_POINT "/sdcard"
#  else
#    define TAB5_SD_MOUNT_POINT "./sim_sdcard"
#  endif
#endif

/* Threshold crossing with hysteresis, so a battery hovering at the boundary
 * does not fire repeatedly. */
#define JOBS_BATT_LOW_PCT   20
#define JOBS_BATT_REARM_PCT 25

/* The boot counter is persisted (<SD>/.devos/boot_id) so a job can tell two
 * boots apart; on a card that isn't writable it falls back to 1 for the run. */
static unsigned s_boot_id;
static bool s_boot_id_loaded;
static unsigned s_events_dropped;

static unsigned load_next_boot_id(void)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/.devos/boot_id", TAB5_SD_MOUNT_POINT);
    unsigned prev = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        char buf[32];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        prev = (unsigned)strtoul(buf, NULL, 10);
    }
    unsigned next = prev + 1;
    if (!next) next = 1;
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "%u\n", next);
        fclose(f);
    }
    return next;
}

unsigned jobs_events_boot_id(void) { return s_boot_id; }
unsigned devos_jobs_events_dropped(void) { return s_events_dropped; }

static bool s_wifi_seen;         /* the first snapshot is not a transition */
static bool s_wifi_conn;
static bool s_batt_armed = true;
static bool s_vpn_seen;          /* Tailscale/WireGuard, same rule */
static bool s_ts_conn, s_wg_up;

static const devos_event_schema_t BOOT_S = {
    .topic = "system.boot",
    .fields = "boot_id:int,recovery:boolean",
    .description = "Emitted once per normal boot, after the ready barrier",
};
static const devos_event_schema_t WIFI_UP_S = {
    .topic = "network.wifi_connected",
    .fields = "ssid:string,ip:string",
    .description = "Wi-Fi associated (transition only)",
};
static const devos_event_schema_t WIFI_DOWN_S = {
    .topic = "network.wifi_disconnected",
    .fields = "ssid:string",
    .description = "Wi-Fi lost (transition only)",
};
static const devos_event_schema_t BATT_LOW_S = {
    .topic = "system.battery_below",
    .fields = "percent:int,charging:boolean",
    .description = "Battery dropped to/below the low threshold (valid, present)",
};
static const devos_event_schema_t TS_UP_S = {
    .topic = "network.tailscale_connected",
    .fields = "ip:string,hostname:string",
    .description = "Tailscale reached CONNECTED (transition only)",
};
static const devos_event_schema_t TS_DOWN_S = {
    .topic = "network.tailscale_disconnected",
    .fields = "hostname:string",
    .description = "Tailscale left CONNECTED (transition only)",
};
static const devos_event_schema_t WG_UP_S = {
    .topic = "network.wireguard_up",
    .fields = "name:string,address:string",
    .description = "A WireGuard tunnel completed its handshake (transition only)",
};
static const devos_event_schema_t WG_DOWN_S = {
    .topic = "network.wireguard_down",
    .fields = "name:string",
    .description = "A WireGuard tunnel went down (transition only)",
};
static const devos_event_schema_t MQTT_MSG_S = {
    .topic = "mqtt.message",
    .fields = "source:string,payload:string,retain:boolean,qos:int,truncated:boolean,seq:int",
    .description = "A message received from the configured MQTT broker",
};

void jobs_events_register(void)
{
    devos_events_register_topic(&BOOT_S);
    devos_events_register_topic(&WIFI_UP_S);
    devos_events_register_topic(&WIFI_DOWN_S);
    devos_events_register_topic(&BATT_LOW_S);
    devos_events_register_topic(&TS_UP_S);
    devos_events_register_topic(&TS_DOWN_S);
    devos_events_register_topic(&WG_UP_S);
    devos_events_register_topic(&WG_DOWN_S);
    devos_events_register_topic(&MQTT_MSG_S);
}

static void publish(const char *topic, const char *provider, const char *payload)
{
    devos_event_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.topic, sizeof(ev.topic), "%s", topic);
    snprintf(ev.provider, sizeof(ev.provider), "%s", provider);
    if (devos_events_publish(&ev, payload, payload ? (uint32_t)strlen(payload) : 0) != DEVOS_OK)
        s_events_dropped++;                 /* bounded queue full: never silent */
}

void jobs_events_publish_boot(bool recovery)
{
    if (!s_boot_id_loaded) {
        s_boot_id_loaded = true;
        s_boot_id = load_next_boot_id();
    }
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"boot_id\":%u,\"recovery\":%s}",
             s_boot_id, recovery ? "true" : "false");
    publish("system.boot", "system", payload);
}

void jobs_events_poll(const devos_jobs_system_t *s)
{
    if (!s) return;

    /* Wi-Fi: emit only real transitions, never the initial state. */
    if (!s_wifi_seen) {
        s_wifi_seen = true;
        s_wifi_conn = s->wifi_connected;
    } else if (s->wifi_connected != s_wifi_conn) {
        s_wifi_conn = s->wifi_connected;
        char payload[128];
        if (s->wifi_connected) {
            snprintf(payload, sizeof(payload), "{\"ssid\":\"%.32s\",\"ip\":\"%.19s\"}",
                     s->wifi_ssid, s->local_ip);
            publish("network.wifi_connected", "net", payload);
        } else {
            snprintf(payload, sizeof(payload), "{\"ssid\":\"%.32s\"}", s->wifi_ssid);
            publish("network.wifi_disconnected", "net", payload);
        }
    }

    /* Battery: only a valid, present pack can cross the threshold. */
    if (s->battery_valid && s->battery_present) {
        if (s_batt_armed && s->battery_percent <= JOBS_BATT_LOW_PCT) {
            char payload[64];
            snprintf(payload, sizeof(payload), "{\"percent\":%u,\"charging\":%s}",
                     (unsigned)s->battery_percent, s->charging ? "true" : "false");
            publish("system.battery_below", "power", payload);
            s_batt_armed = false;
        } else if (!s_batt_armed && s->battery_percent >= JOBS_BATT_REARM_PCT) {
            s_batt_armed = true;
        }
    }

    /* VPN: Tailscale/WireGuard transitions only; the first snapshot primes. */
    if (!s_vpn_seen) {
        s_vpn_seen = true;
        s_ts_conn = s->tailscale_online;
        s_wg_up = s->wireguard_online;
        return;
    }
    char payload[160];
    if (s->tailscale_online != s_ts_conn) {
        s_ts_conn = s->tailscale_online;
        if (s->tailscale_online)
            snprintf(payload, sizeof(payload), "{\"ip\":\"%.15s\",\"hostname\":\"%.39s\"}",
                     s->tailscale_ip, s->tailscale_hostname);
        else
            snprintf(payload, sizeof(payload), "{\"hostname\":\"%.39s\"}", s->tailscale_hostname);
        publish(s->tailscale_online ? "network.tailscale_connected"
                                    : "network.tailscale_disconnected", "tailnet", payload);
    }
    if (s->wireguard_online != s_wg_up) {
        s_wg_up = s->wireguard_online;
        if (s->wireguard_online)
            snprintf(payload, sizeof(payload), "{\"name\":\"%.31s\",\"address\":\"%.31s\"}",
                     s->wireguard_name, s->wireguard_address);
        else
            snprintf(payload, sizeof(payload), "{\"name\":\"%.31s\"}", s->wireguard_name);
        publish(s->wireguard_online ? "network.wireguard_up"
                                    : "network.wireguard_down", "wireguard", payload);
    }
}
