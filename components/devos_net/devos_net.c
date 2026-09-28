#include "devos_net.h"
#include "devos_tailnet.h"
#include "devos_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>

#ifdef ESP_PLATFORM
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
static const char *TAG = "devos_net";
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/select.h>
#include <time.h>
#define TAG "devos_net"
#endif

const char *devos_net_wifi_state_text(devos_wifi_state_t state)
{
    switch (state) {
    case DEVOS_WIFI_STATE_OFF:        return "Unavailable";
    case DEVOS_WIFI_STATE_IDLE:       return "Not connected";
    case DEVOS_WIFI_STATE_CONNECTING: return "Connecting";
    case DEVOS_WIFI_STATE_CONNECTED:  return "Connected";
    case DEVOS_WIFI_STATE_FAILED:     return "Connection failed";
    default:                          return "?";
    }
}

int devos_net_wifi_signal_bars(int rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

/* Dedupe by SSID (keep the strongest BSS), drop hidden SSIDs, sort by RSSI. */
static int scan_normalize(devos_wifi_ap_t *in, int n, devos_wifi_ap_t *out, int max)
{
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (in[i].ssid[0] == '\0') continue;
        int j;
        for (j = 0; j < count; j++) {
            if (strcmp(out[j].ssid, in[i].ssid) == 0) break;
        }
        if (j < count) {
            if (in[i].rssi > out[j].rssi) out[j] = in[i];
        } else if (count < max) {
            out[count++] = in[i];
        }
    }
    for (int i = 1; i < count; i++) {          /* insertion sort, strongest first */
        devos_wifi_ap_t k = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rssi < k.rssi) { out[j + 1] = out[j]; j--; }
        out[j + 1] = k;
    }
    return count;
}

#ifdef ESP_PLATFORM
/* =========================================================================
 * Wi-Fi station manager. The ESP32-P4 has no radio: esp_wifi_remote proxies
 * esp_wifi_* to the ESP32-C6 over ESP-Hosted (SDIO), so every radio call is an
 * RPC that can take a while. All of them run on one Core 0 worker task that
 * owns the connection state machine; esp_event handlers only forward events to
 * it, and the public API only enqueues commands. Readers get a mutex-guarded
 * snapshot, so nothing here ever blocks the GUI task.
 * ========================================================================= */
#define WIFI_NVS_NS        "wifi"
#define WIFI_NVS_KEY_NETS  "nets"

typedef struct {
    char ssid[33];
    char pass[65];
} saved_net_t;

typedef struct {
    uint8_t count;
    saved_net_t e[DEVOS_WIFI_MAX_SAVED];   /* most recently used first */
} saved_blob_t;

typedef enum {
    CMD_SCAN = 0, CMD_CONNECT, CMD_DISCONNECT, CMD_FORGET, CMD_RETRY, CMD_AUTO,
    EV_START, EV_DISCONNECTED, EV_SCAN_DONE, EV_GOT_IP,
} wifi_cmd_type_t;

typedef struct {
    wifi_cmd_type_t type;
    union {
        struct {
            char ssid[33];
            char pass[65];
            bool has_pass;
        } net;
        uint8_t reason;
        esp_netif_ip_info_t ip;
    };
} wifi_cmd_t;

static SemaphoreHandle_t  s_lock = NULL;
static QueueHandle_t      s_cmdq = NULL;
static esp_timer_handle_t s_timer = NULL;
static wifi_cmd_type_t    s_timer_action = CMD_RETRY;
static esp_netif_t       *s_sta_netif = NULL;

/* Guarded by s_lock (read from any task). */
static devos_wifi_status_t s_st;
static devos_wifi_ap_t     s_scan[DEVOS_WIFI_MAX_SCAN];
static int                 s_scan_count = 0;
static devos_wifi_bss_t    s_bss[DEVOS_WIFI_MAX_BSS];
static int                 s_bss_count = 0;
static uint32_t            s_scan_gen = 0;
static saved_blob_t        s_saved;
static volatile bool       s_scan_pending = false;   /* requested by the UI */
static volatile bool       s_scan_running = false;   /* radio is scanning */

/* Worker-task-only state machine. */
static char s_target_ssid[33];
static char s_target_pass[65];
static bool s_target_user = false;      /* user asked for this network */
static int  s_attempts = 0;
static bool s_user_disconnected = false;
static int64_t s_expect_leave_until = 0; /* ignore our own ASSOC_LEAVE until (us) */
static bool s_auto_scan = false;        /* scan started to pick a known network */
static int  s_backoff_idx = 0;

static const uint32_t s_backoff_ms[] = { 2000, 5000, 10000, 30000, 60000 };

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static void post(const wifi_cmd_t *c)
{
    if (!s_cmdq || xQueueSend(s_cmdq, c, 0) != pdTRUE) {
        ESP_LOGW(TAG, "wifi command queue full (cmd %d dropped)", (int)c->type);
    }
}

static void set_state(devos_wifi_state_t st, const char *ssid, const char *err)
{
    lock();
    s_st.state = st;
    s_st.connected = (st == DEVOS_WIFI_STATE_CONNECTED);
    if (ssid) snprintf(s_st.ssid, sizeof(s_st.ssid), "%s", ssid);
    if (err) snprintf(s_st.last_error, sizeof(s_st.last_error), "%s", err);
    if (st != DEVOS_WIFI_STATE_CONNECTED) {
        s_st.ip[0] = s_st.gateway[0] = s_st.netmask[0] = s_st.dns[0] = '\0';
        s_st.rssi = 0;
    }
    unlock();
}

static devos_wifi_state_t get_state(void)
{
    lock();
    devos_wifi_state_t st = s_st.state;
    unlock();
    return st;
}

/* ---- saved networks (NVS) ---- */
static void saved_persist(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    lock();
    saved_blob_t copy = s_saved;
    unlock();
    nvs_set_blob(h, WIFI_NVS_KEY_NETS, &copy, sizeof(copy));
    nvs_erase_key(h, "ssid");    /* legacy single-network keys */
    nvs_erase_key(h, "pass");
    nvs_commit(h);
    nvs_close(h);
}

static void saved_load(void)
{
    memset(&s_saved, 0, sizeof(s_saved));
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_saved);
    if (nvs_get_blob(h, WIFI_NVS_KEY_NETS, &s_saved, &len) != ESP_OK || len != sizeof(s_saved) ||
        s_saved.count > DEVOS_WIFI_MAX_SAVED) {
        memset(&s_saved, 0, sizeof(s_saved));
        /* Migrate the old single-network format. */
        size_t sl = sizeof(s_saved.e[0].ssid), pl = sizeof(s_saved.e[0].pass);
        if (nvs_get_str(h, "ssid", s_saved.e[0].ssid, &sl) == ESP_OK && s_saved.e[0].ssid[0]) {
            if (nvs_get_str(h, "pass", s_saved.e[0].pass, &pl) != ESP_OK) s_saved.e[0].pass[0] = '\0';
            s_saved.count = 1;
        }
    }
    nvs_close(h);
}

/* Returns index or -1. Caller holds s_lock. */
static int saved_find_locked(const char *ssid)
{
    for (int i = 0; i < s_saved.count; i++) {
        if (strcmp(s_saved.e[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

/* Insert/move to front (most recently used). */
static void saved_remember(const char *ssid, const char *pass)
{
    lock();
    int i = saved_find_locked(ssid);
    saved_net_t n;
    snprintf(n.ssid, sizeof(n.ssid), "%s", ssid);
    snprintf(n.pass, sizeof(n.pass), "%s", pass ? pass : "");
    bool changed = (i != 0) || strcmp(s_saved.e[0].pass, n.pass) != 0;
    if (i < 0) {
        i = (s_saved.count < DEVOS_WIFI_MAX_SAVED) ? s_saved.count++ : DEVOS_WIFI_MAX_SAVED - 1;
    }
    memmove(&s_saved.e[1], &s_saved.e[0], (size_t)i * sizeof(saved_net_t));
    s_saved.e[0] = n;
    unlock();
    if (changed) saved_persist();
}

static bool saved_remove(const char *ssid)
{
    lock();
    int i = saved_find_locked(ssid);
    if (i >= 0) {
        memmove(&s_saved.e[i], &s_saved.e[i + 1], (size_t)(s_saved.count - i - 1) * sizeof(saved_net_t));
        s_saved.count--;
        memset(&s_saved.e[s_saved.count], 0, sizeof(saved_net_t));
    }
    unlock();
    if (i >= 0) saved_persist();
    return i >= 0;
}

/* ---- helpers ---- */
static bool reason_is_auth(uint8_t r)
{
    return r == WIFI_REASON_AUTH_EXPIRE || r == WIFI_REASON_MIC_FAILURE ||
           r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_AUTH_FAIL ||
           r == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

static void reason_text(uint8_t r, char *buf, size_t len)
{
    if (reason_is_auth(r)) {
        snprintf(buf, len, "Wrong password");
    } else if (r == WIFI_REASON_NO_AP_FOUND) {
        snprintf(buf, len, "Network not found");
    } else if (r == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
               r == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD) {
        snprintf(buf, len, "Unsupported security type");
    } else if (r == WIFI_REASON_BEACON_TIMEOUT) {
        snprintf(buf, len, "Signal lost");
    } else if (r == WIFI_REASON_ASSOC_FAIL || r == WIFI_REASON_CONNECTION_FAIL) {
        snprintf(buf, len, "Access point refused the connection");
    } else {
        snprintf(buf, len, "Could not connect (reason %u)", r);
    }
}

static void schedule(wifi_cmd_type_t action, uint32_t ms)
{
    if (!s_timer) return;
    esp_timer_stop(s_timer);
    s_timer_action = action;
    esp_timer_start_once(s_timer, (uint64_t)ms * 1000ULL);
}

static void timer_cb(void *arg)
{
    (void)arg;
    wifi_cmd_t c = { .type = s_timer_action };
    post(&c);
}

static uint32_t backoff_next(void)
{
    uint32_t ms = s_backoff_ms[s_backoff_idx];
    if (s_backoff_idx < (int)(sizeof(s_backoff_ms) / sizeof(s_backoff_ms[0])) - 1) s_backoff_idx++;
    return ms;
}

static uint8_t cached_authmode(const char *ssid, bool *found)
{
    uint8_t mode = 0;
    *found = false;
    lock();
    for (int i = 0; i < s_scan_count; i++) {
        if (strcmp(s_scan[i].ssid, ssid) == 0) { mode = s_scan[i].authmode; *found = true; break; }
    }
    unlock();
    return mode;
}

static void begin_connect(const char *ssid, const char *pass, bool user)
{
    snprintf(s_target_ssid, sizeof(s_target_ssid), "%s", ssid);
    snprintf(s_target_pass, sizeof(s_target_pass), "%s", pass ? pass : "");
    s_target_user = user;
    s_attempts = 0;
    s_user_disconnected = false;
    if (s_timer) esp_timer_stop(s_timer);
    if (s_scan_running) {
        esp_wifi_scan_stop();
        s_scan_running = false;
        s_auto_scan = false;
    }

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    /* The driver fields are fixed-size and NOT NUL-terminated when full: a
     * 32-char SSID / 64-char hex PSK must be copied whole (snprintf would drop
     * the last character). */
    memcpy(wc.sta.ssid, s_target_ssid, strnlen(s_target_ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, s_target_pass, strnlen(s_target_pass, sizeof(wc.sta.password)));
    bool known = false;
    uint8_t am = cached_authmode(s_target_ssid, &known);
    if (s_target_pass[0] == '\0') {
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else if (known && am == WIFI_AUTH_WPA_PSK) {
        wc.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;     /* legacy WPA-only AP */
    } else {
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;    /* WPA2 or better (incl. WPA3) */
    }
    wc.sta.pmf_cfg.capable = true;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    /* Keep the last failure visible while auto-connecting; a user attempt
     * starts clean. */
    set_state(DEVOS_WIFI_STATE_CONNECTING, s_target_ssid, user ? "" : NULL);
    /* Drop any current association / attempt first. Our own disconnect shows
     * up as an ASSOC_LEAVE event, which must not count as a failure. */
    s_expect_leave_until = esp_timer_get_time() + 2000000;
    esp_wifi_disconnect();
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
        set_state(DEVOS_WIFI_STATE_FAILED, s_target_ssid, "Invalid network settings");
        return;
    }
    ESP_LOGI(TAG, "Connecting to '%s'%s", s_target_ssid, user ? " (user)" : " (auto)");
    s_attempts = 1;
    if (esp_wifi_connect() != ESP_OK) {
        schedule(CMD_RETRY, 1000);
    }
}

static bool pick_known_network(char ssid[33], char pass[65])
{
    bool found = false;
    lock();
    /* Scan results are strongest first: take the first one we know. */
    for (int i = 0; i < s_scan_count && !found; i++) {
        int k = saved_find_locked(s_scan[i].ssid);
        if (k >= 0) {
            memcpy(ssid, s_saved.e[k].ssid, sizeof(s_saved.e[k].ssid));
            memcpy(pass, s_saved.e[k].pass, sizeof(s_saved.e[k].pass));
            found = true;
        }
    }
    unlock();
    return found;
}

static int saved_count(void)
{
    lock();
    int n = s_saved.count;
    unlock();
    return n;
}

/* ---- worker ---- */
static void start_scan(bool auto_pick)
{
    s_scan_pending = false;
    if (s_scan_running) {
        s_auto_scan = s_auto_scan || auto_pick;
        return;
    }
    /* hidden networks too: the site survey counts them (the join list drops them) */
    wifi_scan_config_t sc = { .show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    if (esp_wifi_scan_start(&sc, false) == ESP_OK) {
        s_scan_running = true;
        s_auto_scan = auto_pick;
    } else if (auto_pick) {
        schedule(CMD_AUTO, 5000);   /* radio busy (e.g. mid-connect): try later */
    }
}

static void on_scan_done(void)
{
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > DEVOS_WIFI_MAX_BSS) n = DEVOS_WIFI_MAX_BSS;
    wifi_ap_record_t *recs = n ? calloc(n, sizeof(wifi_ap_record_t)) : NULL;
    devos_wifi_ap_t *raw = n ? calloc(n, sizeof(devos_wifi_ap_t)) : NULL;
    if (n == 0) {
        esp_wifi_clear_ap_list();
        lock();
        s_scan_count = 0;
        s_bss_count = 0;
        s_scan_gen++;
        unlock();
    } else if (recs && raw && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        static devos_wifi_bss_t bss[DEVOS_WIFI_MAX_BSS];
        for (int i = 0; i < n; i++) {
            snprintf(raw[i].ssid, sizeof(raw[i].ssid), "%s", (const char *)recs[i].ssid);
            raw[i].rssi = recs[i].rssi;
            raw[i].authmode = (uint8_t)recs[i].authmode;
            devos_wifi_bss_t *b = &bss[i];
            memcpy(b->ssid, raw[i].ssid, sizeof(b->ssid));
            memcpy(b->bssid, recs[i].bssid, 6);
            b->channel = recs[i].primary;
            b->second = recs[i].second == WIFI_SECOND_CHAN_ABOVE ? 1 : recs[i].second == WIFI_SECOND_CHAN_BELOW ? -1 : 0;
            b->rssi = recs[i].rssi;
            b->authmode = (uint8_t)recs[i].authmode;
            b->phy = (recs[i].phy_11b ? DEVOS_WIFI_PHY_B : 0) | (recs[i].phy_11g ? DEVOS_WIFI_PHY_G : 0) |
                     (recs[i].phy_11n ? DEVOS_WIFI_PHY_N : 0) | (recs[i].phy_11ax ? DEVOS_WIFI_PHY_AX : 0);
        }
        devos_wifi_ap_t norm[DEVOS_WIFI_MAX_SCAN];
        int count = scan_normalize(raw, n, norm, DEVOS_WIFI_MAX_SCAN);
        lock();
        memcpy(s_scan, norm, sizeof(devos_wifi_ap_t) * count);
        s_scan_count = count;
        memcpy(s_bss, bss, sizeof(devos_wifi_bss_t) * n);
        s_bss_count = n;
        s_scan_gen++;
        unlock();
    } else {
        esp_wifi_clear_ap_list();   /* always release the driver's list */
    }
    free(recs);
    free(raw);
    s_scan_running = false;

    if (s_auto_scan) {
        s_auto_scan = false;
        devos_wifi_state_t st = get_state();
        bool busy = st == DEVOS_WIFI_STATE_CONNECTED ||
                    (st == DEVOS_WIFI_STATE_CONNECTING && s_target_user);
        if (!busy && !s_user_disconnected) {
            char ssid[33], pass[65];
            if (pick_known_network(ssid, pass)) {
                begin_connect(ssid, pass, false);
            } else {
                if (st != DEVOS_WIFI_STATE_FAILED) set_state(DEVOS_WIFI_STATE_IDLE, "", NULL);
                schedule(CMD_AUTO, 30000);   /* keep looking while disconnected */
            }
        }
    }
}

static void on_disconnected(uint8_t reason)
{
    devos_wifi_state_t st = get_state();
    if (reason == WIFI_REASON_ASSOC_LEAVE && esp_timer_get_time() < s_expect_leave_until) {
        s_expect_leave_until = 0;
        return;   /* our own disconnect (switching networks / new attempt) */
    }

    if (s_user_disconnected) {
        set_state(DEVOS_WIFI_STATE_IDLE, "", "");
        return;
    }
    if (s_target_ssid[0] == '\0') {
        /* Straggler after we gave up: keep a FAILED message on screen. */
        if (st != DEVOS_WIFI_STATE_FAILED) set_state(DEVOS_WIFI_STATE_IDLE, "", NULL);
        return;
    }
    if (st == DEVOS_WIFI_STATE_CONNECTED) {
        /* Lost an established link: reconnect quietly with backoff. */
        ESP_LOGW(TAG, "Wi-Fi link to '%s' lost (reason %u); reconnecting", s_target_ssid, reason);
        s_target_user = false;
        s_attempts = 0;
        set_state(DEVOS_WIFI_STATE_CONNECTING, s_target_ssid, "");
        schedule(CMD_RETRY, 1000);
        return;
    }

    char why[40];
    reason_text(reason, why, sizeof(why));
    bool auth = reason_is_auth(reason);
    int limit = auth ? 2 : (s_target_user ? 4 : 3);
    ESP_LOGW(TAG, "Connect to '%s' failed: %s (attempt %d/%d)", s_target_ssid, why, s_attempts, limit);
    if (s_attempts < limit) {
        schedule(CMD_RETRY, auth ? 500 : 1000u * (uint32_t)s_attempts);
        return;
    }

    /* Give up on this network. */
    char msg[64];
    snprintf(msg, sizeof(msg), "%.22s: %s", s_target_ssid, why);
    if (s_target_user) {
        set_state(DEVOS_WIFI_STATE_FAILED, s_target_ssid, msg);
    } else {
        set_state(DEVOS_WIFI_STATE_IDLE, "", msg);
    }
    s_target_ssid[0] = '\0';
    s_target_user = false;
    if (saved_count() > 0) schedule(CMD_AUTO, backoff_next());
}

static void on_got_ip(const esp_netif_ip_info_t *ip)
{
    wifi_ap_record_t ap;
    bool have_ap = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    lock();
    esp_ip4addr_ntoa(&ip->ip, s_st.ip, sizeof(s_st.ip));
    esp_ip4addr_ntoa(&ip->gw, s_st.gateway, sizeof(s_st.gateway));
    esp_ip4addr_ntoa(&ip->netmask, s_st.netmask, sizeof(s_st.netmask));
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, s_st.dns, sizeof(s_st.dns));
    }
    if (have_ap) s_st.rssi = ap.rssi;
    s_st.state = DEVOS_WIFI_STATE_CONNECTED;
    s_st.connected = true;
    snprintf(s_st.ssid, sizeof(s_st.ssid), "%s", s_target_ssid);
    s_st.last_error[0] = '\0';
    unlock();
    ESP_LOGI(TAG, "Wi-Fi connected to '%s', IP %s", s_target_ssid, s_st.ip);

    saved_remember(s_target_ssid, s_target_pass);
    s_target_user = false;
    s_attempts = 0;
    s_backoff_idx = 0;
}

static void wifi_worker(void *arg)
{
    (void)arg;
    wifi_cmd_t c;
    for (;;) {
        if (xQueueReceive(s_cmdq, &c, portMAX_DELAY) != pdTRUE) continue;
        switch (c.type) {
        case EV_START:
            set_state(DEVOS_WIFI_STATE_IDLE, "", "");
            if (saved_count() > 0) start_scan(true);
            break;
        case CMD_SCAN:
            start_scan(false);
            break;
        case CMD_AUTO:
            if (get_state() != DEVOS_WIFI_STATE_CONNECTED && !s_user_disconnected && saved_count() > 0) {
                start_scan(true);
            }
            break;
        case CMD_CONNECT: {
            char pass[65] = "";
            if (c.net.has_pass) {
                snprintf(pass, sizeof(pass), "%s", c.net.pass);
            } else {
                lock();
                int k = saved_find_locked(c.net.ssid);
                if (k >= 0) snprintf(pass, sizeof(pass), "%s", s_saved.e[k].pass);
                unlock();
            }
            begin_connect(c.net.ssid, pass, true);
            memset(pass, 0, sizeof(pass));
            break;
        }
        case CMD_RETRY:
            if (s_target_ssid[0] && !s_user_disconnected) {
                s_attempts++;
                if (esp_wifi_connect() != ESP_OK) schedule(CMD_RETRY, 2000);
            }
            break;
        case CMD_DISCONNECT:
            s_user_disconnected = true;
            s_target_ssid[0] = '\0';
            if (s_timer) esp_timer_stop(s_timer);
            esp_wifi_disconnect();
            set_state(DEVOS_WIFI_STATE_IDLE, "", "");
            break;
        case CMD_FORGET: {
            saved_remove(c.net.ssid);
            devos_wifi_state_t st = get_state();
            if (strcmp(c.net.ssid, s_target_ssid) == 0 &&
                (st == DEVOS_WIFI_STATE_CONNECTED || st == DEVOS_WIFI_STATE_CONNECTING)) {
                s_user_disconnected = true;
                s_target_ssid[0] = '\0';
                if (s_timer) esp_timer_stop(s_timer);
                esp_wifi_disconnect();
                set_state(DEVOS_WIFI_STATE_IDLE, "", "");
            }
            break;
        }
        case EV_DISCONNECTED:
            on_disconnected(c.reason);
            break;
        case EV_SCAN_DONE:
            on_scan_done();
            break;
        case EV_GOT_IP:
            on_got_ip(&c.ip);
            break;
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    wifi_cmd_t c;
    memset(&c, 0, sizeof(c));
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        c.type = EV_START;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        c.type = EV_DISCONNECTED;
        c.reason = ((wifi_event_sta_disconnected_t *)data)->reason;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        c.type = EV_SCAN_DONE;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        c.type = EV_GOT_IP;
        c.ip = ((ip_event_got_ip_t *)data)->ip_info;
    } else {
        return;
    }
    post(&c);
}

int devos_net_init(void)
{
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = DEVOS_WIFI_STATE_OFF;

#if !CONFIG_DEVOS_ENABLE_WIFI
    /* Wi-Fi bring-up (esp_wifi_init) starts the ESP-Hosted SDIO transport to
     * the C6. If the C6 has no ESP-Hosted slave firmware, that transport's
     * task aborts -> boot loop, which we cannot catch here. So Wi-Fi is
     * opt-in: flash the C6 (tools/flash_c6_slave.sh), then set
     * CONFIG_DEVOS_ENABLE_WIFI=y. Until then, boot without a radio. */
    snprintf(s_st.last_error, sizeof(s_st.last_error), "Wi-Fi disabled in this build");
    ESP_LOGW(TAG, "Wi-Fi disabled (CONFIG_DEVOS_ENABLE_WIFI=n); "
                  "enable it after flashing the ESP32-C6 ESP-Hosted slave");
    return 0;
#else
    s_lock = xSemaphoreCreateMutex();
    s_cmdq = xQueueCreate(12, sizeof(wifi_cmd_t));
    if (!s_lock || !s_cmdq) return -1;
    saved_load();

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return -1;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return -1;
    }
    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();

    /* esp_wifi_init brings up the ESP-Hosted transport to the C6. If the C6 is
     * not flashed with the slave firmware this fails; stay up without Wi-Fi. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s (is the ESP32-C6 ESP-Hosted slave flashed?)",
                 esp_err_to_name(err));
        set_state(DEVOS_WIFI_STATE_OFF, "", "Wi-Fi co-processor (ESP32-C6) not responding");
        return -1;
    }

    const esp_timer_create_args_t targs = { .callback = timer_cb, .name = "wifi_retry" };
    esp_timer_create(&targs, &s_timer);
    xTaskCreatePinnedToCore(wifi_worker, "wifi_mgr", 6144, NULL, 4, NULL, DEVOS_CORE_NET_CRYPTO);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL);

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    err = esp_wifi_start();   /* STA_START -> worker auto-connects to a known network */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        set_state(DEVOS_WIFI_STATE_OFF, "", "Wi-Fi failed to start");
        return -1;
    }
    ESP_LOGI(TAG, "Wi-Fi station started (radio on ESP32-C6 via ESP-Hosted), %d saved network(s)",
             s_saved.count);
    return 0;
#endif /* CONFIG_DEVOS_ENABLE_WIFI */
}

int devos_net_wifi_get_status(devos_wifi_status_t *out_status)
{
    if (!out_status) return -1;
    lock();
    memcpy(out_status, &s_st, sizeof(*out_status));
    unlock();
    return 0;
}

void devos_net_wifi_refresh_rssi(void)
{
    if (!s_cmdq || get_state() != DEVOS_WIFI_STATE_CONNECTED) return;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        lock();
        if (s_st.state == DEVOS_WIFI_STATE_CONNECTED) s_st.rssi = ap.rssi;
        unlock();
    }
}

int devos_net_wifi_scan_start(void)
{
    if (!s_cmdq || get_state() == DEVOS_WIFI_STATE_OFF) return -1;
    s_scan_pending = true;   /* the UI shows "Scanning" immediately */
    wifi_cmd_t c = { .type = CMD_SCAN };
    post(&c);
    return 0;
}

bool devos_net_wifi_scan_busy(void)
{
    return s_scan_pending || s_scan_running;
}

int devos_net_wifi_scan_results(devos_wifi_ap_t *out, int max)
{
    if (!out || max <= 0) return 0;
    lock();
    int n = s_scan_count < max ? s_scan_count : max;
    memcpy(out, s_scan, sizeof(devos_wifi_ap_t) * n);
    unlock();
    return n;
}

int devos_net_wifi_bss_results(devos_wifi_bss_t *out, int max)
{
    if (!out || max <= 0) return 0;
    lock();
    int n = s_bss_count < max ? s_bss_count : max;
    memcpy(out, s_bss, sizeof(devos_wifi_bss_t) * n);
    unlock();
    return n;
}

uint32_t devos_net_wifi_scan_generation(void)
{
    lock();
    uint32_t g = s_scan_gen;
    unlock();
    return g;
}

int devos_net_wifi_connect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || !s_cmdq || get_state() == DEVOS_WIFI_STATE_OFF) return -1;
    wifi_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.type = CMD_CONNECT;
    snprintf(c.net.ssid, sizeof(c.net.ssid), "%s", ssid);
    if (password) {
        c.net.has_pass = true;
        snprintf(c.net.pass, sizeof(c.net.pass), "%s", password);
    }
    /* Reflect the request immediately so the UI never shows a stale state. */
    set_state(DEVOS_WIFI_STATE_CONNECTING, ssid, "");
    post(&c);
    memset(&c, 0, sizeof(c));
    return 0;
}

int devos_net_wifi_disconnect(void)
{
    if (!s_cmdq) return -1;
    wifi_cmd_t c = { .type = CMD_DISCONNECT };
    post(&c);
    return 0;
}

int devos_net_wifi_saved_list(devos_wifi_saved_t *out, int max)
{
    if (!out || max <= 0) return 0;
    lock();
    int n = s_saved.count < max ? s_saved.count : max;
    for (int i = 0; i < n; i++) snprintf(out[i].ssid, sizeof(out[i].ssid), "%s", s_saved.e[i].ssid);
    unlock();
    return n;
}

bool devos_net_wifi_is_saved(const char *ssid)
{
    if (!ssid) return false;
    lock();
    bool r = saved_find_locked(ssid) >= 0;
    unlock();
    return r;
}

int devos_net_wifi_forget(const char *ssid)
{
    if (!ssid || !s_cmdq) return -1;
    wifi_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.type = CMD_FORGET;
    snprintf(c.net.ssid, sizeof(c.net.ssid), "%s", ssid);
    post(&c);
    return 0;
}

#else  /* !ESP_PLATFORM — simulator: a canned radio with realistic timing */

static devos_wifi_status_t s_st;
static devos_wifi_ap_t s_scan[DEVOS_WIFI_MAX_SCAN];
static int s_scan_count = 0;
static devos_wifi_bss_t s_bss[DEVOS_WIFI_MAX_BSS];
static int s_bss_count = 0;
static uint32_t s_scan_gen = 0;
static bool s_scan_busy = false;
static uint64_t s_scan_t0 = 0;
static uint64_t s_conn_t0 = 0;
static char s_pending_pass[65];
static struct { char ssid[33]; char pass[65]; } s_saved[DEVOS_WIFI_MAX_SAVED];
static int s_saved_count = 0;

static uint64_t sim_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int sim_saved_find(const char *ssid)
{
    for (int i = 0; i < s_saved_count; i++) {
        if (strcmp(s_saved[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

static void sim_tick(void)
{
    uint64_t now = sim_ms();
    if (s_scan_busy && now - s_scan_t0 >= 1500) {
        static const devos_wifi_ap_t canned[] = {
            {"HomeWiFi", -58, 3}, {"Office-5G", -64, 4}, {"Phone_Hotspot", -72, 3},
            {"Guest-IoT", -80, 0}, {"Neighbour", -88, 3},
        };
        s_scan_count = scan_normalize((devos_wifi_ap_t *)canned, (int)(sizeof(canned) / sizeof(canned[0])),
                                      s_scan, DEVOS_WIFI_MAX_SCAN);
        /* a made-up neighbourhood for the site survey: signals drift, one AP comes and goes */
        static const struct { const char *ssid; uint8_t mac5; uint8_t ch; int8_t sec; int8_t rssi; uint8_t auth, phy; } nb[] = {
            {"HomeWiFi", 0x11, 6, 0, -58, 3, 0x0f}, {"HomeWiFi", 0x12, 1, 0, -71, 3, 0x0f},
            {"Office-5G", 0x21, 11, -1, -64, 4, 0x0e}, {"Phone_Hotspot", 0x31, 6, 0, -72, 3, 0x07},
            {"Guest-IoT", 0x41, 1, 0, -80, 0, 0x07}, {"Neighbour", 0x51, 3, 1, -88, 3, 0x06},
            {"", 0x61, 11, 0, -83, 3, 0x07}, {"TELSTRA4F2A", 0x71, 9, 0, -77, 4, 0x0f},
            {"ESP_7F21C3", 0x81, 1, 0, -66, 0, 0x07}, {"Printer-Setup", 0x91, 13, 0, -85, 0, 0x03},
            {"Cafe WiFi", 0xa1, 6, 0, -90, 0, 0x07}, {"", 0xb1, 4, 0, -92, 3, 0x06},
        };
        static int drift[sizeof(nb) / sizeof(nb[0])];
        static unsigned seed = 12345;
        s_bss_count = 0;
        for (unsigned i = 0; i < sizeof(nb) / sizeof(nb[0]) && s_bss_count < DEVOS_WIFI_MAX_BSS; i++) {
            seed = seed * 1103515245u + 12345u;
            drift[i] += (int)((seed >> 16) % 7) - 3;
            if (drift[i] > 6) drift[i] = 6;
            if (drift[i] < -6) drift[i] = -6;
            if (nb[i].mac5 == 0xa1 && (s_scan_gen / 4) % 2) continue;
            devos_wifi_bss_t *b = &s_bss[s_bss_count++];
            snprintf(b->ssid, sizeof(b->ssid), "%s", nb[i].ssid);
            uint8_t mac[6] = {0x24, 0x0a, 0xc4, 0x3e, 0x90, nb[i].mac5};
            memcpy(b->bssid, mac, 6);
            b->channel = nb[i].ch;
            b->second = nb[i].sec;
            b->rssi = (int8_t)(nb[i].rssi + drift[i]);
            b->authmode = nb[i].auth;
            b->phy = nb[i].phy;
        }
        s_scan_gen++;
        s_scan_busy = false;
    }
    if (s_st.state == DEVOS_WIFI_STATE_CONNECTING && now - s_conn_t0 >= 1200) {
        if (strcmp(s_pending_pass, "wrong") == 0) {   /* lets the failure UI be exercised */
            s_st.state = DEVOS_WIFI_STATE_FAILED;
            snprintf(s_st.last_error, sizeof(s_st.last_error), "Wrong password");
            return;
        }
        s_st.state = DEVOS_WIFI_STATE_CONNECTED;
        s_st.connected = true;
        s_st.rssi = -60;
        snprintf(s_st.ip, sizeof(s_st.ip), "192.168.1.150");
        snprintf(s_st.gateway, sizeof(s_st.gateway), "192.168.1.1");
        snprintf(s_st.netmask, sizeof(s_st.netmask), "255.255.255.0");
        snprintf(s_st.dns, sizeof(s_st.dns), "192.168.1.1");
        int i = sim_saved_find(s_st.ssid);
        if (i < 0 && s_saved_count < DEVOS_WIFI_MAX_SAVED) i = s_saved_count++;
        if (i >= 0) {
            snprintf(s_saved[i].ssid, sizeof(s_saved[i].ssid), "%s", s_st.ssid);
            snprintf(s_saved[i].pass, sizeof(s_saved[i].pass), "%s", s_pending_pass);
        }
    }
}

int devos_net_init(void)
{
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = DEVOS_WIFI_STATE_CONNECTED;
    s_st.connected = true;
    snprintf(s_st.ssid, sizeof(s_st.ssid), "HomeWiFi");
    s_st.rssi = -58;
    snprintf(s_st.ip, sizeof(s_st.ip), "192.168.1.150");
    snprintf(s_st.gateway, sizeof(s_st.gateway), "192.168.1.1");
    snprintf(s_st.netmask, sizeof(s_st.netmask), "255.255.255.0");
    snprintf(s_st.dns, sizeof(s_st.dns), "192.168.1.1");
    snprintf(s_saved[0].ssid, sizeof(s_saved[0].ssid), "HomeWiFi");
    s_saved_count = 1;
    return 0;
}

int devos_net_wifi_get_status(devos_wifi_status_t *out_status)
{
    if (!out_status) return -1;
    sim_tick();
    memcpy(out_status, &s_st, sizeof(*out_status));
    return 0;
}

void devos_net_wifi_refresh_rssi(void) {}

int devos_net_wifi_scan_start(void)
{
    if (!s_scan_busy) { s_scan_busy = true; s_scan_t0 = sim_ms(); }
    return 0;
}

bool devos_net_wifi_scan_busy(void) { sim_tick(); return s_scan_busy; }

int devos_net_wifi_scan_results(devos_wifi_ap_t *out, int max)
{
    if (!out || max <= 0) return 0;
    sim_tick();
    int n = s_scan_count < max ? s_scan_count : max;
    memcpy(out, s_scan, sizeof(devos_wifi_ap_t) * n);
    return n;
}

uint32_t devos_net_wifi_scan_generation(void) { sim_tick(); return s_scan_gen; }

int devos_net_wifi_bss_results(devos_wifi_bss_t *out, int max)
{
    if (!out || max <= 0) return 0;
    sim_tick();
    int n = s_bss_count < max ? s_bss_count : max;
    memcpy(out, s_bss, sizeof(devos_wifi_bss_t) * n);
    return n;
}

int devos_net_wifi_connect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) return -1;
    int i = sim_saved_find(ssid);
    snprintf(s_pending_pass, sizeof(s_pending_pass), "%s",
             password ? password : (i >= 0 ? s_saved[i].pass : ""));
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = DEVOS_WIFI_STATE_CONNECTING;
    snprintf(s_st.ssid, sizeof(s_st.ssid), "%s", ssid);
    s_conn_t0 = sim_ms();
    return 0;
}

int devos_net_wifi_disconnect(void)
{
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = DEVOS_WIFI_STATE_IDLE;
    return 0;
}

int devos_net_wifi_saved_list(devos_wifi_saved_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = s_saved_count < max ? s_saved_count : max;
    for (int i = 0; i < n; i++) snprintf(out[i].ssid, sizeof(out[i].ssid), "%s", s_saved[i].ssid);
    return n;
}

bool devos_net_wifi_is_saved(const char *ssid) { return ssid && sim_saved_find(ssid) >= 0; }

int devos_net_wifi_forget(const char *ssid)
{
    int i = ssid ? sim_saved_find(ssid) : -1;
    if (i < 0) return -1;
    memmove(&s_saved[i], &s_saved[i + 1], (size_t)(s_saved_count - i - 1) * sizeof(s_saved[0]));
    s_saved_count--;
    if (strcmp(s_st.ssid, ssid) == 0) devos_net_wifi_disconnect();
    return 0;
}

#endif /* ESP_PLATFORM */

bool devos_net_is_tailnet_target(const char *host_or_ip)
{
    if (!host_or_ip) return false;

    /* Check domain suffix: .ts.net or .tailnet */
    size_t len = strlen(host_or_ip);
    if (len > 7 && strcasecmp(host_or_ip + len - 7, ".ts.net") == 0) {
        return true;
    }
    if (len > 8 && strcasecmp(host_or_ip + len - 8, ".tailnet") == 0) {
        return true;
    }

    /* Check IPv4 CGNAT range: 100.64.0.0/10 (100.64.0.0 to 100.127.255.255) */
    int a, b, c, d;
    if (sscanf(host_or_ip, "%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
        if (a == 100 && b >= 64 && b <= 127) {
            return true;
        }
    }

    return false;
}

/* host.local: one-shot mDNS A query (lwIP's resolver doesn't do mDNS).
 * Sent from an ephemeral port, so responders answer us directly
 * (RFC 6762 legacy unicast); on the device it goes out of the Wi-Fi netif. */
static int mdns_resolve(const char *name, char *out_ip, size_t out_len, int timeout_ms)
{
    uint8_t q[300];
    size_t o = 12;
    memset(q, 0, 12);
    q[0] = 0x4d;
    q[1] = 0x44;
    q[5] = 1;                                           /* one question */
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        if (!l || l > 63 || o + l + 6 > sizeof(q)) return -1;
        q[o++] = (uint8_t)l;
        memcpy(q + o, p, l);
        o += l;
        p += l;
        if (*p == '.') p++;
    }
    q[o++] = 0;
    q[o++] = 0; q[o++] = 1;                             /* A */
    q[o++] = 0; q[o++] = 1;                             /* IN */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
#ifdef ESP_PLATFORM
    lock();
    struct in_addr ifa = { 0 };
    bool have_if = s_st.connected && inet_aton(s_st.ip, &ifa);
    unlock();
    if (have_if) setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));
#else
    const char *sim_if = getenv("DEVOS_SIM_MDNS_IF");    /* same knob as the Network app */
    struct in_addr ifa;
    if (sim_if && inet_aton(sim_if, &ifa)) setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));
#endif
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(5353);
    to.sin_addr.s_addr = inet_addr("224.0.0.251");
    int rc = -1;
    for (int attempt = 0; attempt < 2 && rc; attempt++) {
        sendto(s, q, o, 0, (struct sockaddr *)&to, sizeof(to));
        int wait = timeout_ms / 2;
        while (rc) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(s, &r);
            struct timeval tv = { .tv_sec = wait / 1000, .tv_usec = (wait % 1000) * 1000 };
            if (select(s + 1, &r, NULL, NULL, &tv) <= 0) break;
            uint8_t m[512];
            int n = (int)recv(s, m, sizeof(m), 0);
            if (n < 12 || !(m[2] & 0x80)) continue;
            int an = m[6] << 8 | m[7], off = 12;
            int qd = m[4] << 8 | m[5];
            /* skip the questions, then look for an A answer */
            for (int i = 0; i < qd + an && off < n && rc; i++) {
                while (off < n && m[off] && (m[off] & 0xc0) != 0xc0) off += m[off] + 1;
                off += off < n && (m[off] & 0xc0) == 0xc0 ? 2 : 1;
                if (i < qd) { off += 4; continue; }
                if (off + 10 > n) break;
                int type = m[off] << 8 | m[off + 1], rdlen = m[off + 8] << 8 | m[off + 9];
                off += 10;
                if (type == 1 && rdlen == 4 && off + 4 <= n) {
                    snprintf(out_ip, out_len, "%u.%u.%u.%u", m[off], m[off + 1], m[off + 2], m[off + 3]);
                    rc = 0;
                }
                off += rdlen;
            }
        }
    }
    close(s);
    return rc;
}

int devos_net_resolve(const char *hostname, char *out_ip, size_t out_len)
{
    if (!hostname || !out_ip || out_len < 16) return -1;

    /* 1. If already an IP address, copy directly */
    struct in_addr addr;
    if (inet_aton(hostname, &addr) != 0) {
        snprintf(out_ip, out_len, "%s", hostname);
        out_ip[out_len - 1] = '\0';
        return 0;
    }

    /* 2. MagicDNS lookup on the tailnet (only while it is up). */
    if (devos_tailnet_resolve(hostname, out_ip, out_len) == 0) {
        return 0;
    }

    /* 3. host.local over mDNS */
    size_t hl = strlen(hostname);
    if (hl > 6 && !strcasecmp(hostname + hl - 6, ".local") && mdns_resolve(hostname, out_ip, out_len, 1500) == 0) {
        return 0;
    }

    /* 4. Fall back to standard DNS resolution */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, NULL, &hints, &res) == 0 && res) {
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
        inet_ntop(AF_INET, &sin->sin_addr, out_ip, out_len);
        freeaddrinfo(res);
        return 0;
    }

    return -1;
}

static devos_net_route_fn s_route_hook;

void devos_net_set_route_hook(devos_net_route_fn fn)
{
    s_route_hook = fn;
}

/* Bind to the tunnel's address when a VPN claims this destination. */
static void route_bind(int sock, const struct sockaddr_in *dest)
{
    uint32_t src = 0;
    if (!s_route_hook || !s_route_hook(dest->sin_addr.s_addr, &src) || !src) return;
    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = src;
    if (bind(sock, (struct sockaddr *)&me, sizeof(me)) != 0) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "couldn't bind to the tunnel address (errno %d)", errno);
#endif
    }
}

void devos_net_socket_route(int sock, uint32_t dest_ip)
{
    struct sockaddr_in d;
    memset(&d, 0, sizeof(d));
    d.sin_family = AF_INET;
    d.sin_addr.s_addr = dest_ip;
    route_bind(sock, &d);
}

int devos_net_socket_connect(const char *host, int port, int timeout_ms)
{
    if (!host || port <= 0 || port > 65535) return -1;

    char resolved_ip[46];
    if (devos_net_resolve(host, resolved_ip, sizeof(resolved_ip)) != 0) {
        return -1;
    }

    bool is_tailnet = devos_net_is_tailnet_target(resolved_ip);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }

    /* Set timeouts */
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, resolved_ip, &dest.sin_addr);

    if (is_tailnet) {
        /* Tailnet WireGuard Routing */
        #ifdef ESP_PLATFORM
        ESP_LOGI(TAG, "Virtual routing to %s:%d through MicroLink WireGuard tunnel", resolved_ip, port);
        #endif
    } else {
        #ifdef ESP_PLATFORM
        ESP_LOGI(TAG, "Standard routing to %s:%d through Wi-Fi gateway", resolved_ip, port);
        #endif
    }

    route_bind(sock, &dest);
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        close(sock);
        return -1;
    }

    return sock;
}

int devos_net_socket_send(int sock, const void *data, size_t len)
{
    if (sock < 0 || !data || len == 0) return -1;
    return (int)send(sock, data, len, 0);
}

int devos_net_socket_recv(int sock, void *buf, size_t max_len, int timeout_ms)
{
    if (sock < 0 || !buf || max_len == 0) return -1;

    if (timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    return (int)recv(sock, buf, max_len, 0);
}

int devos_net_socket_close(int sock)
{
    if (sock >= 0) {
        return close(sock);
    }
    return -1;
}

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

int devos_net_socket_send_all(int sock, const void *data, size_t len)
{
    if (sock < 0 || !data) return -1;
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, (const char *)data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

int devos_net_http_get(const char *host, int port, const char *path,
                       char *resp, size_t cap, int timeout_ms,
                       int *status_out)
{
    if (!host || !path || !resp || cap < 64) return -1;
    if (status_out) *status_out = 0;
    resp[0] = '\0';

    int fd = devos_net_socket_connect(host, port, timeout_ms);
    if (fd < 0) return -1;

    char req[512];
    int hlen = snprintf(req, sizeof(req),
                        "GET %s HTTP/1.1\r\nHost: %s:%d\r\n"
                        "Connection: close\r\n\r\n",
                        path, host, port);
    int rc = -1;
    size_t total = 0;
    if (hlen > 0 &&
        devos_net_socket_send_all(fd, req, (size_t)hlen) == 0) {
        for (;;) {
            size_t room = cap - 1 - total;
            if (room == 0) break;
            size_t want = room > 1024 ? 1024 : room;
            int n = devos_net_socket_recv(fd, resp + total, want, timeout_ms);
            if (n <= 0) break;
            total += (size_t)n;
        }
        resp[total] = '\0';
        rc = 0;
    }
    devos_net_socket_close(fd);
    if (rc != 0) return -1;

    int status = 0;
    if (sscanf(resp, "HTTP/%*d.%*d %d", &status) != 1 &&
        sscanf(resp, "HTTP/%*d %d", &status) != 1) {
        return -1;
    }
    if (status_out) *status_out = status;
    char *eoh = strstr(resp, "\r\n\r\n");
    if (!eoh) return -1;
    size_t bl = strlen(eoh + 4);
    memmove(resp, eoh + 4, bl + 1);
    return 0;
}

/* Non-blocking connect split across poll ticks so the UI task never stalls
 * on SYN timeouts. Portable across BSD sockets and lwIP. */
int devos_net_socket_connect_start(const char *host, int port)
{
    if (!host || port <= 0 || port > 65535) return -1;

    char resolved_ip[46];
    if (devos_net_resolve(host, resolved_ip, sizeof(resolved_ip)) != 0) {
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, resolved_ip, &dest.sin_addr);

    route_bind(sock, &dest);
    int rc = connect(sock, (struct sockaddr *)&dest, sizeof(dest));
    if (rc == 0) {
        /* Instant (loopback): restore blocking immediately */
        if (flags >= 0) {
            fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
        }
        return sock;
    }
    if (errno == EINPROGRESS
#ifdef EWOULDBLOCK
        || errno == EWOULDBLOCK
#endif
#ifdef EAGAIN
        || errno == EAGAIN
#endif
    ) {
        return sock;
    }
    close(sock);
    return -1;
}

/* Returns 0 connected, 1 still in progress (call again), -1 failed.
 * On success the socket is back to blocking with the given timeouts. */
int devos_net_socket_connect_wait(int sock, int timeout_ms)
{
    if (sock < 0) return -1;

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int rc = select(sock + 1, NULL, &wfds, NULL, &tv);
    if (rc == 0) return 1; /* timeout: still in progress */
    if (rc < 0) {
        if (errno == EINTR) return 1;
        return -1;
    }

    int err = 0;
    socklen_t errlen = sizeof(err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errlen) != 0 || err != 0) {
        return -1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
    }
    struct timeval tio;
    tio.tv_sec = timeout_ms / 1000;
    tio.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tio, sizeof(tio));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tio, sizeof(tio));
    return 0;
}

static void set_io_timeouts(int sock, int timeout_ms)
{
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

int devos_net_socket_listen(int port, int backlog)
{
    if (port <= 0 || port > 65535) return -1;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
#ifndef ESP_PLATFORM
    /* the simulator forks ssh: don't hand it the port */
    fcntl(sock, F_SETFD, FD_CLOEXEC);
#endif
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_port = htons((uint16_t)port);
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&me, sizeof(me)) != 0 || listen(sock, backlog) != 0) {
        close(sock);
        return -1;
    }
    return sock;
}

int devos_net_socket_accept(int listen_sock, int wait_ms, int io_timeout_ms, char *peer, size_t peer_cap)
{
    if (listen_sock < 0) return -1;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listen_sock, &rfds);
    struct timeval tv;
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;
    int rc = select(listen_sock + 1, &rfds, NULL, NULL, &tv);
    if (rc == 0 || (rc < 0 && errno == EINTR)) return -2;
    if (rc < 0) return -1;

    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    int sock = accept(listen_sock, (struct sockaddr *)&from, &flen);
    if (sock < 0) return (errno == EAGAIN || errno == EINTR) ? -2 : -1;
#ifndef ESP_PLATFORM
    fcntl(sock, F_SETFD, FD_CLOEXEC);
#endif
    set_io_timeouts(sock, io_timeout_ms);
    if (peer && peer_cap) {
        if (!inet_ntop(AF_INET, &from.sin_addr, peer, (socklen_t)peer_cap)) peer[0] = '\0';
    }
    return sock;
}
