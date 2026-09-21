#include "microlink.h"
#include "devos_config.h"
#include "devos_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
static const char *TAG = "microlink";
#else
#define TAG "microlink"
#endif

#define MICROLINK_CONFIG_FILE TAB5_SD_MOUNT_POINT "/.devos/tailscale_nvs.json"

static microlink_config_t s_config;
static microlink_status_t s_status;
static microlink_event_cb_t s_listener = NULL;
static void *s_listener_data = NULL;

static void notify_state_change(microlink_state_t new_state)
{
    s_status.state = new_state;

    /* Synchronize with devos_telemetry */
    devos_telemetry_t t;
    memcpy(&t, devos_telemetry_get(), sizeof(devos_telemetry_t));
    if (new_state == MICROLINK_STATE_CONNECTED) {
        t.tailscale_online = true;
        strncpy(t.tailscale_ip, s_status.assigned_ip, sizeof(t.tailscale_ip) - 1);
        strncpy(t.tailscale_derp, s_status.derp_relay_name, sizeof(t.tailscale_derp) - 1);
        t.tailscale_peers_online = 0;
        for (int i = 0; i < s_status.peer_count; i++) {
            if (s_status.peers[i].is_online) {
                t.tailscale_peers_online++;
            }
        }
    } else {
        t.tailscale_online = false;
        strncpy(t.tailscale_ip, "Offline", sizeof(t.tailscale_ip) - 1);
        strncpy(t.tailscale_derp, "None", sizeof(t.tailscale_derp) - 1);
        t.tailscale_peers_online = 0;
    }
    devos_telemetry_update(&t);

    if (s_listener) {
        s_listener(new_state, s_listener_data);
    }
}

static void init_default_peers(void)
{
    s_status.peer_count = 6;

    /* Peer 0: Workstation */
    strncpy(s_status.peers[0].name, "workstation", sizeof(s_status.peers[0].name));
    strncpy(s_status.peers[0].fqdn, "workstation.tailnet", sizeof(s_status.peers[0].fqdn));
    strncpy(s_status.peers[0].ip, "100.64.1.2", sizeof(s_status.peers[0].ip));
    s_status.peers[0].os_type = MICROLINK_PEER_OS_LINUX;
    strncpy(s_status.peers[0].os_desc, "Linux x86_64", sizeof(s_status.peers[0].os_desc));
    s_status.peers[0].is_direct = true;
    s_status.peers[0].ping_ms = 2;
    s_status.peers[0].is_online = true;
    s_status.peers[0].last_seen_sec = 4;
    s_status.peers[0].rx_bytes = 1420580;
    s_status.peers[0].tx_bytes = 824100;

    /* Peer 1: MacBook Pro */
    strncpy(s_status.peers[1].name, "macbook-pro", sizeof(s_status.peers[1].name));
    strncpy(s_status.peers[1].fqdn, "macbook-pro.tailnet", sizeof(s_status.peers[1].fqdn));
    strncpy(s_status.peers[1].ip, "100.77.11.90", sizeof(s_status.peers[1].ip));
    s_status.peers[1].os_type = MICROLINK_PEER_OS_MACOS;
    strncpy(s_status.peers[1].os_desc, "macOS Sonoma", sizeof(s_status.peers[1].os_desc));
    s_status.peers[1].is_direct = true;
    s_status.peers[1].ping_ms = 4;
    s_status.peers[1].is_online = true;
    s_status.peers[1].last_seen_sec = 12;
    s_status.peers[1].rx_bytes = 945200;
    s_status.peers[1].tx_bytes = 530100;

    /* Peer 2: Home NAS */
    strncpy(s_status.peers[2].name, "home-nas", sizeof(s_status.peers[2].name));
    strncpy(s_status.peers[2].fqdn, "home-nas.tailnet", sizeof(s_status.peers[2].fqdn));
    strncpy(s_status.peers[2].ip, "100.80.3.15", sizeof(s_status.peers[2].ip));
    s_status.peers[2].os_type = MICROLINK_PEER_OS_BSD;
    strncpy(s_status.peers[2].os_desc, "TrueNAS Core", sizeof(s_status.peers[2].os_desc));
    s_status.peers[2].is_direct = false;
    s_status.peers[2].ping_ms = 18;
    s_status.peers[2].is_online = true;
    s_status.peers[2].last_seen_sec = 25;
    s_status.peers[2].rx_bytes = 5120000;
    s_status.peers[2].tx_bytes = 2048000;

    /* Peer 3: Production Cluster */
    strncpy(s_status.peers[3].name, "prod-cluster", sizeof(s_status.peers[3].name));
    strncpy(s_status.peers[3].fqdn, "prod-cluster.tailnet", sizeof(s_status.peers[3].fqdn));
    strncpy(s_status.peers[3].ip, "100.99.20.1", sizeof(s_status.peers[3].ip));
    s_status.peers[3].os_type = MICROLINK_PEER_OS_LINUX;
    strncpy(s_status.peers[3].os_desc, "Ubuntu 24.04", sizeof(s_status.peers[3].os_desc));
    s_status.peers[3].is_direct = true;
    s_status.peers[3].ping_ms = 12;
    s_status.peers[3].is_online = true;
    s_status.peers[3].last_seen_sec = 8;
    s_status.peers[3].rx_bytes = 2100400;
    s_status.peers[3].tx_bytes = 1430000;

    /* Peer 4: Backup Server */
    strncpy(s_status.peers[4].name, "backup-box", sizeof(s_status.peers[4].name));
    strncpy(s_status.peers[4].fqdn, "backup-box.tailnet", sizeof(s_status.peers[4].fqdn));
    strncpy(s_status.peers[4].ip, "100.100.4.5", sizeof(s_status.peers[4].ip));
    s_status.peers[4].os_type = MICROLINK_PEER_OS_LINUX;
    strncpy(s_status.peers[4].os_desc, "Debian 12", sizeof(s_status.peers[4].os_desc));
    s_status.peers[4].is_direct = false;
    s_status.peers[4].ping_ms = 42;
    s_status.peers[4].is_online = false;
    s_status.peers[4].last_seen_sec = 3600;
    s_status.peers[4].rx_bytes = 0;
    s_status.peers[4].tx_bytes = 0;

    /* Peer 5: Mobile Dev */
    strncpy(s_status.peers[5].name, "iphone-dev", sizeof(s_status.peers[5].name));
    strncpy(s_status.peers[5].fqdn, "iphone-dev.tailnet", sizeof(s_status.peers[5].fqdn));
    strncpy(s_status.peers[5].ip, "100.115.8.20", sizeof(s_status.peers[5].ip));
    s_status.peers[5].os_type = MICROLINK_PEER_OS_MOBILE;
    strncpy(s_status.peers[5].os_desc, "iOS 18", sizeof(s_status.peers[5].os_desc));
    s_status.peers[5].is_direct = true;
    s_status.peers[5].ping_ms = 14;
    s_status.peers[5].is_online = true;
    s_status.peers[5].last_seen_sec = 60;
    s_status.peers[5].rx_bytes = 320000;
    s_status.peers[5].tx_bytes = 110000;
}

int microlink_init(const microlink_config_t *config)
{
    memset(&s_status, 0, sizeof(s_status));

    if (config) {
        memcpy(&s_config, config, sizeof(microlink_config_t));
    } else {
        strncpy(s_config.hostname, "devos-tab5", sizeof(s_config.hostname));
        s_config.auto_connect = true;
        s_config.derp_region_pref = 19; /* Sydney */
    }

    /* Load persistent config */
    microlink_nvs_load();

    /* Populate initial node status */
    strncpy(s_status.node_name, s_config.hostname, sizeof(s_status.node_name));
    strncpy(s_status.tailnet_domain, "devos.tailnet", sizeof(s_status.tailnet_domain));
    strncpy(s_status.assigned_ip, "100.77.11.92", sizeof(s_status.assigned_ip));
    strncpy(s_status.derp_relay_name, "DERP-19 (Sydney)", sizeof(s_status.derp_relay_name));
    s_status.derp_ping_ms = 18;
    s_status.mtu = 1280;
    s_status.is_wireguard_hw = true;
    s_status.total_rx_bytes = 10485760;
    s_status.total_tx_bytes = 5242880;

    init_default_peers();

    if (s_config.auto_connect) {
        microlink_connect();
    } else {
        notify_state_change(MICROLINK_STATE_DISCONNECTED);
    }

    return 0;
}

int microlink_connect(void)
{
    notify_state_change(MICROLINK_STATE_CONNECTING);

    /* In a real device, performs ts2021 DERP handshake and WireGuard configuration */
    notify_state_change(MICROLINK_STATE_CONNECTED);
    return 0;
}

int microlink_disconnect(void)
{
    notify_state_change(MICROLINK_STATE_DISCONNECTED);
    return 0;
}

microlink_state_t microlink_get_state(void)
{
    return s_status.state;
}

int microlink_get_status(microlink_status_t *out_status)
{
    if (!out_status) return -1;
    memcpy(out_status, &s_status, sizeof(microlink_status_t));
    return 0;
}

int microlink_ping_derp(int *out_ping_ms)
{
    if (s_status.state != MICROLINK_STATE_CONNECTED) {
        if (out_ping_ms) *out_ping_ms = -1;
        return -1;
    }

    /* Simulate dynamic ping latency variance (16-22 ms) */
    int variance = (rand() % 7) - 3;
    s_status.derp_ping_ms = 18 + variance;
    if (s_status.derp_ping_ms < 10) s_status.derp_ping_ms = 10;

    if (out_ping_ms) {
        *out_ping_ms = s_status.derp_ping_ms;
    }
    return 0;
}

int microlink_ping_peer(int peer_idx, int *out_ping_ms)
{
    if (peer_idx < 0 || peer_idx >= s_status.peer_count) {
        if (out_ping_ms) *out_ping_ms = -1;
        return -1;
    }

    if (!s_status.peers[peer_idx].is_online || s_status.state != MICROLINK_STATE_CONNECTED) {
        if (out_ping_ms) *out_ping_ms = -1;
        return -1;
    }

    int base = s_status.peers[peer_idx].is_direct ? 3 : 20;
    int var = (rand() % 5) - 2;
    int latency = base + var;
    if (latency < 1) latency = 1;

    s_status.peers[peer_idx].ping_ms = latency;
    if (out_ping_ms) {
        *out_ping_ms = latency;
    }
    return 0;
}

int microlink_refresh_peers(void)
{
    if (s_status.state != MICROLINK_STATE_CONNECTED) {
        return -1;
    }

    for (int i = 0; i < s_status.peer_count; i++) {
        if (s_status.peers[i].is_online) {
            microlink_ping_peer(i, NULL);
        }
    }
    return 0;
}

int microlink_set_auth_key(const char *auth_key)
{
    if (!auth_key) return -1;
    strncpy(s_config.auth_key, auth_key, sizeof(s_config.auth_key) - 1);
    microlink_nvs_save();
    return 0;
}

int microlink_get_auth_key(char *out_buf, size_t buf_len)
{
    if (!out_buf || buf_len == 0) return -1;
    strncpy(out_buf, s_config.auth_key, buf_len - 1);
    out_buf[buf_len - 1] = '\0';
    return 0;
}

int microlink_nvs_save(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    esp_err_t err = nvs_open("tailscale", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_str(h, "auth_key", s_config.auth_key);
        nvs_set_str(h, "hostname", s_config.hostname);
        nvs_set_u8(h, "auto_connect", s_config.auto_connect ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
        return 0;
    }
    return -1;
#else
    FILE *f = fopen(MICROLINK_CONFIG_FILE, "w");
    if (f) {
        fprintf(f, "{\n  \"auth_key\": \"%s\",\n  \"hostname\": \"%s\",\n  \"auto_connect\": %s\n}\n",
                s_config.auth_key, s_config.hostname, s_config.auto_connect ? "true" : "false");
        fclose(f);
        return 0;
    }
    return -1;
#endif
}

int microlink_nvs_load(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    esp_err_t err = nvs_open("tailscale", NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t len = sizeof(s_config.auth_key);
        nvs_get_str(h, "auth_key", s_config.auth_key, &len);
        len = sizeof(s_config.hostname);
        nvs_get_str(h, "hostname", s_config.hostname, &len);
        uint8_t ac = 1;
        nvs_get_u8(h, "auto_connect", &ac);
        s_config.auto_connect = (ac != 0);
        nvs_close(h);
        return 0;
    }
    return -1;
#else
    FILE *f = fopen(MICROLINK_CONFIG_FILE, "r");
    if (f) {
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) {
            char val[128];
            if (sscanf(buf, " \"auth_key\": \"%127[^\"]\"", val) == 1) {
                strncpy(s_config.auth_key, val, sizeof(s_config.auth_key) - 1);
            } else if (sscanf(buf, " \"hostname\": \"%31[^\"]\"", val) == 1) {
                strncpy(s_config.hostname, val, sizeof(s_config.hostname) - 1);
            }
        }
        fclose(f);
        return 0;
    }
    return -1;
#endif
}

void microlink_add_state_listener(microlink_event_cb_t cb, void *user_data)
{
    s_listener = cb;
    s_listener_data = user_data;
}
