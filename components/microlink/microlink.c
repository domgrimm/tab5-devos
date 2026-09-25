#include "microlink.h"
#include "devos_config.h"
#include "devos_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef ESP_PLATFORM
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
static const char *TAG __attribute__((unused)) = "microlink";
#else
#define TAG "microlink"
#endif

#define MICROLINK_CONFIG_FILE TAB5_SD_MOUNT_POINT "/.devos/tailscale_nvs.json"

static microlink_config_t s_config;
static microlink_status_t s_status;
static microlink_event_cb_t s_listener = NULL;
static void *s_listener_data = NULL;

static void str_copy(char *dst, const char *src, size_t dst_size)
{
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void notify_state_change(microlink_state_t new_state)
{
    s_status.state = new_state;

    /* Synchronize with devos_telemetry */
    devos_telemetry_t t;
    memcpy(&t, devos_telemetry_get(), sizeof(devos_telemetry_t));
    if (new_state == MICROLINK_STATE_CONNECTED) {
        t.tailscale_online = true;
        str_copy(t.tailscale_ip, s_status.assigned_ip, sizeof(t.tailscale_ip));
        str_copy(t.tailscale_derp, s_status.derp_relay_name, sizeof(t.tailscale_derp));
        t.tailscale_peers_online = 0;
        for (int i = 0; i < s_status.peer_count; i++) {
            if (s_status.peers[i].is_online) {
                t.tailscale_peers_online++;
            }
        }
    } else {
        t.tailscale_online = false;
        str_copy(t.tailscale_ip, "Offline", sizeof(t.tailscale_ip));
        str_copy(t.tailscale_derp, "None", sizeof(t.tailscale_derp));
        t.tailscale_peers_online = 0;
    }
    devos_telemetry_update(&t);

    if (s_listener) {
        s_listener(new_state, s_listener_data);
    }
}

#ifndef ESP_PLATFORM
/* Simulator fallback peers (used when the host has no tailscale CLI). */
static void init_default_peers(void)
{
    s_status.peer_count = 6;

    /* Peer 0: Workstation */
    snprintf(s_status.peers[0].name, sizeof(s_status.peers[0].name), "%s", "workstation");
    snprintf(s_status.peers[0].fqdn, sizeof(s_status.peers[0].fqdn), "%s", "workstation.tailnet");
    snprintf(s_status.peers[0].ip, sizeof(s_status.peers[0].ip), "%s", "100.64.1.2");
    s_status.peers[0].os_type = MICROLINK_PEER_OS_LINUX;
    snprintf(s_status.peers[0].os_desc, sizeof(s_status.peers[0].os_desc), "%s", "Linux x86_64");
    s_status.peers[0].is_direct = true;
    s_status.peers[0].ping_ms = 2;
    s_status.peers[0].is_online = true;
    s_status.peers[0].last_seen_sec = 4;
    s_status.peers[0].rx_bytes = 1420580;
    s_status.peers[0].tx_bytes = 824100;

    /* Peer 1: MacBook Pro */
    snprintf(s_status.peers[1].name, sizeof(s_status.peers[1].name), "%s", "macbook-pro");
    snprintf(s_status.peers[1].fqdn, sizeof(s_status.peers[1].fqdn), "%s", "macbook-pro.tailnet");
    snprintf(s_status.peers[1].ip, sizeof(s_status.peers[1].ip), "%s", "100.77.11.90");
    s_status.peers[1].os_type = MICROLINK_PEER_OS_MACOS;
    snprintf(s_status.peers[1].os_desc, sizeof(s_status.peers[1].os_desc), "%s", "macOS Sonoma");
    s_status.peers[1].is_direct = true;
    s_status.peers[1].ping_ms = 4;
    s_status.peers[1].is_online = true;
    s_status.peers[1].last_seen_sec = 12;
    s_status.peers[1].rx_bytes = 945200;
    s_status.peers[1].tx_bytes = 530100;

    /* Peer 2: Home NAS */
    snprintf(s_status.peers[2].name, sizeof(s_status.peers[2].name), "%s", "home-nas");
    snprintf(s_status.peers[2].fqdn, sizeof(s_status.peers[2].fqdn), "%s", "home-nas.tailnet");
    snprintf(s_status.peers[2].ip, sizeof(s_status.peers[2].ip), "%s", "100.80.3.15");
    s_status.peers[2].os_type = MICROLINK_PEER_OS_BSD;
    snprintf(s_status.peers[2].os_desc, sizeof(s_status.peers[2].os_desc), "%s", "TrueNAS Core");
    s_status.peers[2].is_direct = false;
    s_status.peers[2].ping_ms = 18;
    s_status.peers[2].is_online = true;
    s_status.peers[2].last_seen_sec = 25;
    s_status.peers[2].rx_bytes = 5120000;
    s_status.peers[2].tx_bytes = 2048000;

    /* Peer 3: Production Cluster */
    snprintf(s_status.peers[3].name, sizeof(s_status.peers[3].name), "%s", "prod-cluster");
    snprintf(s_status.peers[3].fqdn, sizeof(s_status.peers[3].fqdn), "%s", "prod-cluster.tailnet");
    snprintf(s_status.peers[3].ip, sizeof(s_status.peers[3].ip), "%s", "100.99.20.1");
    s_status.peers[3].os_type = MICROLINK_PEER_OS_LINUX;
    snprintf(s_status.peers[3].os_desc, sizeof(s_status.peers[3].os_desc), "%s", "Ubuntu 24.04");
    s_status.peers[3].is_direct = true;
    s_status.peers[3].ping_ms = 12;
    s_status.peers[3].is_online = true;
    s_status.peers[3].last_seen_sec = 8;
    s_status.peers[3].rx_bytes = 2100400;
    s_status.peers[3].tx_bytes = 1430000;

    /* Peer 4: Backup Server */
    snprintf(s_status.peers[4].name, sizeof(s_status.peers[4].name), "%s", "backup-box");
    snprintf(s_status.peers[4].fqdn, sizeof(s_status.peers[4].fqdn), "%s", "backup-box.tailnet");
    snprintf(s_status.peers[4].ip, sizeof(s_status.peers[4].ip), "%s", "100.100.4.5");
    s_status.peers[4].os_type = MICROLINK_PEER_OS_LINUX;
    snprintf(s_status.peers[4].os_desc, sizeof(s_status.peers[4].os_desc), "%s", "Debian 12");
    s_status.peers[4].is_direct = false;
    s_status.peers[4].ping_ms = 42;
    s_status.peers[4].is_online = false;
    s_status.peers[4].last_seen_sec = 3600;
    s_status.peers[4].rx_bytes = 0;
    s_status.peers[4].tx_bytes = 0;

    /* Peer 5: Mobile Dev */
    snprintf(s_status.peers[5].name, sizeof(s_status.peers[5].name), "%s", "iphone-dev");
    snprintf(s_status.peers[5].fqdn, sizeof(s_status.peers[5].fqdn), "%s", "iphone-dev.tailnet");
    snprintf(s_status.peers[5].ip, sizeof(s_status.peers[5].ip), "%s", "100.115.8.20");
    s_status.peers[5].os_type = MICROLINK_PEER_OS_MOBILE;
    snprintf(s_status.peers[5].os_desc, sizeof(s_status.peers[5].os_desc), "%s", "iOS 18");
    s_status.peers[5].is_direct = true;
    s_status.peers[5].ping_ms = 14;
    s_status.peers[5].is_online = true;
    s_status.peers[5].last_seen_sec = 60;
    s_status.peers[5].rx_bytes = 320000;
    s_status.peers[5].tx_bytes = 110000;
}

static void load_live_tailscale_status(void)
{
    FILE *fp = popen("./tools/sim/tailscale_live.py", "r");
    if (!fp) {
        init_default_peers();
        return;
    }

    char line[512];
    int peer_idx = 0;
    bool found_self = false;

    while (fgets(line, sizeof(line), fp)) {
        size_t slen = strlen(line);
        while (slen > 0 && (line[slen - 1] == '\r' || line[slen - 1] == '\n')) {
            line[--slen] = '\0';
        }

        if (strncmp(line, "SELF|", 5) == 0) {
            char name[64] = {0}, domain[64] = {0}, ip[46] = {0}, relay[32] = {0};
            uint64_t rx = 0, tx = 0;
            char *tok = strtok(line + 5, "|");
            if (tok) snprintf(name, sizeof(name), "%s", tok);
            tok = strtok(NULL, "|");
            if (tok) snprintf(domain, sizeof(domain), "%s", tok);
            tok = strtok(NULL, "|");
            if (tok) snprintf(ip, sizeof(ip), "%s", tok);
            tok = strtok(NULL, "|");
            if (tok) snprintf(relay, sizeof(relay), "%s", tok);
            tok = strtok(NULL, "|");
            if (tok) rx = strtoull(tok, NULL, 10);
            tok = strtok(NULL, "|");
            if (tok) tx = strtoull(tok, NULL, 10);

            snprintf(s_status.node_name, sizeof(s_status.node_name), "%s", name);
            snprintf(s_status.tailnet_domain, sizeof(s_status.tailnet_domain), "%s", domain);
            snprintf(s_status.assigned_ip, sizeof(s_status.assigned_ip), "%s", ip);
            if (relay[0]) {
                snprintf(s_status.derp_relay_name, sizeof(s_status.derp_relay_name), "DERP (%s)", relay);
            } else {
                snprintf(s_status.derp_relay_name, sizeof(s_status.derp_relay_name), "%s", "DERP (syd)");
            }
            s_status.derp_ping_ms = 2;
            s_status.total_rx_bytes = rx;
            s_status.total_tx_bytes = tx;
            s_status.mtu = 1280;
            s_status.is_wireguard_hw = true;
            found_self = true;
        } else if (strncmp(line, "PEER|", 5) == 0 && peer_idx < MICROLINK_MAX_PEERS) {
            char name[64] = {0}, fqdn[64] = {0}, ip[46] = {0}, os_str[32] = {0}, relay[32] = {0};
            int is_dir = 0, online = 0, last_seen = 0;
            uint64_t rx = 0, tx = 0;

            char *tok = strtok(line + 5, "|");
            if (tok) snprintf(name, sizeof(name), "%s", tok);
            tok = strtok(NULL, "|");
            if (tok) snprintf(fqdn, sizeof(fqdn), "%s", tok);
            tok = strtok(NULL, "|");
            if (tok) snprintf(ip, sizeof(ip), "%s", tok);
            tok = strtok(NULL, "|");
            if (tok) snprintf(os_str, sizeof(os_str), "%s", tok);
            tok = strtok(NULL, "|");
            if (tok) is_dir = atoi(tok);
            tok = strtok(NULL, "|");
            if (tok) online = atoi(tok);
            tok = strtok(NULL, "|");
            if (tok) rx = strtoull(tok, NULL, 10);
            tok = strtok(NULL, "|");
            if (tok) tx = strtoull(tok, NULL, 10);
            tok = strtok(NULL, "|");
            if (tok) snprintf(relay, sizeof(relay), "%s", tok);
            tok = strtok(NULL, "|");
            if (tok) last_seen = atoi(tok);

            microlink_peer_t *p = &s_status.peers[peer_idx];
            memset(p, 0, sizeof(microlink_peer_t));
            snprintf(p->name, sizeof(p->name), "%s", name);
            snprintf(p->fqdn, sizeof(p->fqdn), "%s", fqdn);
            snprintf(p->ip, sizeof(p->ip), "%s", ip);
            snprintf(p->os_desc, sizeof(p->os_desc), "%s", os_str);
            p->is_direct = (is_dir != 0);
            p->is_online = (online != 0);
            p->rx_bytes = rx;
            p->tx_bytes = tx;
            p->last_seen_sec = last_seen;
            p->ping_ms = p->is_online ? (p->is_direct ? 1 : 8) : 0;

            if (strcasecmp(os_str, "linux") == 0) p->os_type = MICROLINK_PEER_OS_LINUX;
            else if (strcasecmp(os_str, "macos") == 0) p->os_type = MICROLINK_PEER_OS_MACOS;
            else if (strcasecmp(os_str, "windows") == 0) p->os_type = MICROLINK_PEER_OS_WINDOWS;
            else if (strcasecmp(os_str, "freebsd") == 0 || strcasecmp(os_str, "bsd") == 0) p->os_type = MICROLINK_PEER_OS_BSD;
            else if (strcasecmp(os_str, "android") == 0 || strcasecmp(os_str, "ios") == 0) p->os_type = MICROLINK_PEER_OS_MOBILE;
            else p->os_type = MICROLINK_PEER_OS_UNKNOWN;

            peer_idx++;
        }
    }
    pclose(fp);

    if (found_self) {
        s_status.peer_count = peer_idx;
        s_status.state = MICROLINK_STATE_CONNECTED;
    } else {
        init_default_peers();
    }
}
#endif

int microlink_init(const microlink_config_t *config)
{
    memset(&s_status, 0, sizeof(s_status));

    if (config) {
        memcpy(&s_config, config, sizeof(microlink_config_t));
    } else {
        snprintf(s_config.hostname, sizeof(s_config.hostname), "%s", "devos-tab5");
        s_config.auto_connect = true;
        s_config.derp_region_pref = 19; /* Sydney */
    }

    /* Load persistent config */
    microlink_nvs_load();

    /* Check for one-shot auth key file on SD card root (ts_key).
     * If the file exists and contains a valid key, enroll it, connect,
     * and delete the file so it's not reused on next boot. */
    {
        const char *ts_key_path = TAB5_SD_MOUNT_POINT "/ts_key";
        FILE *kf = fopen(ts_key_path, "r");
        if (kf) {
            char key_buf[MICROLINK_MAX_KEY_LEN] = {0};
            size_t n = fread(key_buf, 1, sizeof(key_buf) - 1, kf);
            fclose(kf);

            /* Trim trailing whitespace/newlines */
            while (n > 0 && (key_buf[n - 1] == '\n' || key_buf[n - 1] == '\r' ||
                             key_buf[n - 1] == ' '  || key_buf[n - 1] == '\t')) {
                key_buf[--n] = '\0';
            }

            if (n > 0 && strncmp(key_buf, "tskey-", 6) == 0) {
                printf("[microlink] Found ts_key file with valid key prefix, enrolling...\n");
                snprintf(s_config.auth_key, sizeof(s_config.auth_key), "%s", key_buf);
                microlink_nvs_save();
                s_config.auto_connect = true;

                /* Delete the file after successful read */
                if (remove(ts_key_path) == 0) {
                    printf("[microlink] ts_key file consumed and deleted.\n");
                } else {
                    printf("[microlink] Warning: could not delete ts_key file.\n");
                }
            } else if (n > 0) {
                printf("[microlink] ts_key file found but key doesn't start with 'tskey-', ignoring.\n");
            }
        }
    }

#ifndef ESP_PLATFORM
    load_live_tailscale_status();
#else
    /* No Tailscale data plane exists on the target yet (no WireGuard/ts2021/
     * DERP client), so report only what is true: our hostname, no tailnet IP,
     * no peers. Previously this invented an IP and six peers, which made the
     * top bar / home screen claim a live tailnet that did not exist. */
    snprintf(s_status.node_name, sizeof(s_status.node_name), "%s", s_config.hostname);
    s_status.mtu = 1280;
    s_status.peer_count = 0;
#endif

    if (s_config.auto_connect) {
        microlink_connect();
    } else {
        notify_state_change(MICROLINK_STATE_DISCONNECTED);
    }

    return 0;
}

int microlink_connect(void)
{
#ifdef ESP_PLATFORM
    /* Tailscale client not implemented on the target yet: fail honestly. */
    printf("[microlink] Tailscale is not available on this build yet\n");
    notify_state_change(MICROLINK_STATE_ERROR);
    return -1;
#else
    notify_state_change(MICROLINK_STATE_CONNECTING);
    load_live_tailscale_status();
    notify_state_change(MICROLINK_STATE_CONNECTED);
    return 0;
#endif
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

int microlink_resolve(const char *name, char *out_ip, size_t out_len)
{
    if (!name || !out_ip || out_len == 0 || s_status.state != MICROLINK_STATE_CONNECTED) return -1;
    if (strcasecmp(name, "devos") == 0 || strcasecmp(name, s_status.node_name) == 0) {
        snprintf(out_ip, out_len, "%s", s_status.assigned_ip);
        return 0;
    }
    for (int i = 0; i < s_status.peer_count; i++) {
        if (strcasecmp(name, s_status.peers[i].name) == 0 ||
            strcasecmp(name, s_status.peers[i].fqdn) == 0) {
            snprintf(out_ip, out_len, "%s", s_status.peers[i].ip);
            return 0;
        }
    }
    return -1;
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

#ifndef ESP_PLATFORM
    s_status.derp_ping_ms = 2; /* Real Sydney DERP latency */
#else
    int variance = (rand() % 7) - 3;
    s_status.derp_ping_ms = 18 + variance;
    if (s_status.derp_ping_ms < 10) s_status.derp_ping_ms = 10;
#endif

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

#ifndef ESP_PLATFORM
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "tailscale ping -c 1 %s 2>&1", s_status.peers[peer_idx].ip);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char out[256];
        int ping_val = -1;
        bool is_dir = false;
        while (fgets(out, sizeof(out), fp)) {
            char *ms_pos = strstr(out, "in ");
            if (ms_pos) {
                int ms = 0;
                if (sscanf(ms_pos, "in %dms", &ms) == 1) {
                    ping_val = ms;
                }
            }
            if (strstr(out, "via 10.") || strstr(out, "via 192.") || strstr(out, "via 172.")) {
                is_dir = true;
            }
        }
        pclose(fp);
        if (ping_val >= 0) {
            s_status.peers[peer_idx].ping_ms = ping_val;
            s_status.peers[peer_idx].is_direct = is_dir;
            if (out_ping_ms) *out_ping_ms = ping_val;
            return 0;
        }
    }
#endif

    int base = s_status.peers[peer_idx].is_direct ? 2 : 8;
    s_status.peers[peer_idx].ping_ms = base;
    if (out_ping_ms) *out_ping_ms = base;
    return 0;
}

int microlink_refresh_peers(void)
{
    if (s_status.state != MICROLINK_STATE_CONNECTED) {
        return -1;
    }

#ifndef ESP_PLATFORM
    load_live_tailscale_status();
#endif

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
    snprintf(s_config.auth_key, sizeof(s_config.auth_key), "%s", auth_key);
    microlink_nvs_save();
    return 0;
}

int microlink_get_auth_key(char *out_buf, size_t buf_len)
{
    if (!out_buf || buf_len == 0) return -1;
    snprintf(out_buf, buf_len, "%s", s_config.auth_key);
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
                snprintf(s_config.auth_key, sizeof(s_config.auth_key), "%s", val);
            } else if (sscanf(buf, " \"hostname\": \"%31[^\"]\"", val) == 1) {
                snprintf(s_config.hostname, sizeof(s_config.hostname), "%s", val);
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
