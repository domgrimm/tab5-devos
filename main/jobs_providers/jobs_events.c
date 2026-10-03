/* jobs_events: the Phase 6 system-event producers and topic schemas for Jobs
 * triggers (PLAN.md 8.1). The engine itself never talks to Wi-Fi/battery; this
 * bridge reads the compact sysmon snapshot in main's 1 Hz loop and publishes
 * typed events through devos_events. Emitting outside any engine lock, with
 * explicit bounded copies. No LVGL. */
#include "jobs_providers.h"
#include "devos_events.h"
#include "devos_jobs.h"

#include <stdio.h>
#include <string.h>

/* Threshold crossing with hysteresis, so a battery hovering at the boundary
 * does not fire repeatedly. */
#define JOBS_BATT_LOW_PCT   20
#define JOBS_BATT_REARM_PCT 25

static bool s_wifi_seen;         /* the first snapshot is not a transition */
static bool s_wifi_conn;
static bool s_batt_armed = true;

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

void jobs_events_register(void)
{
    devos_events_register_topic(&BOOT_S);
    devos_events_register_topic(&WIFI_UP_S);
    devos_events_register_topic(&WIFI_DOWN_S);
    devos_events_register_topic(&BATT_LOW_S);
}

static void publish(const char *topic, const char *provider, const char *payload)
{
    devos_event_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.topic, sizeof(ev.topic), "%s", topic);
    snprintf(ev.provider, sizeof(ev.provider), "%s", provider);
    devos_events_publish(&ev, payload, payload ? (uint32_t)strlen(payload) : 0);
}

void jobs_events_publish_boot(bool recovery)
{
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"boot_id\":%u,\"recovery\":%s}",
             0u, recovery ? "true" : "false");
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
}
