/* devos_tailnet: Tailscale client (MicroLink on the device, the host's
 * `tailscale status` in the simulator). See devos_tailnet.h. */
#include "devos_tailnet.h"
#include "devos_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#define DEFAULT_HOSTNAME "devos-tab5"
#define TS_KEY_FILE      TAB5_SD_MOUNT_POINT "/ts_key"

/* Settings (persisted) */
static struct {
    char auth_key[DEVOS_TS_KEY_MAX];
    char hostname[DEVOS_TS_HOSTNAME_MAX];
    bool auto_connect;
    bool registered;            /* completed a registration at least once */
} s_cfg;

/* Published state (s_lock on the device) */
static devos_ts_info_t s_info;
static EXT_RAM_BSS_ATTR devos_ts_peer_t s_peers[DEVOS_TS_MAX_PEERS];   /* ~6 KB: PSRAM */
static int s_npeers;
static volatile uint32_t s_gen;

/* Ping results survive peer-list refreshes (keyed by IP). */
typedef struct { char ip[16]; int ms; } ping_slot_t;
static ping_slot_t s_pings[DEVOS_TS_MAX_PEERS];

const char *devos_tailnet_state_text(devos_ts_state_t st)
{
    switch (st) {
    case DEVOS_TS_OFF:          return "Off";
    case DEVOS_TS_NEEDS_KEY:    return "Not set up";
    case DEVOS_TS_WAIT_WIFI:    return "Waiting for Wi-Fi";
    case DEVOS_TS_CONNECTING:   return "Connecting";
    case DEVOS_TS_REGISTERING:  return "Signing in";
    case DEVOS_TS_CONNECTED:    return "Connected";
    case DEVOS_TS_RECONNECTING: return "Reconnecting";
    case DEVOS_TS_ERROR:        return "Error";
    default:                    return "?";
    }
}

static int ping_lookup(const char *ip)
{
    for (int i = 0; i < DEVOS_TS_MAX_PEERS; i++) {
        if (s_pings[i].ip[0] && strcmp(s_pings[i].ip, ip) == 0) return s_pings[i].ms;
    }
    return -1;
}

static void ping_store(const char *ip, int ms)
{
    int slot = -1;
    for (int i = 0; i < DEVOS_TS_MAX_PEERS; i++) {
        if (strcmp(s_pings[i].ip, ip) == 0) { slot = i; break; }
        if (slot < 0 && !s_pings[i].ip[0]) slot = i;
    }
    if (slot < 0) slot = 0;
    snprintf(s_pings[slot].ip, sizeof(s_pings[slot].ip), "%s", ip);
    s_pings[slot].ms = ms;
    for (int i = 0; i < s_npeers; i++) {
        if (strcmp(s_peers[i].ip, ip) == 0) s_peers[i].ping_ms = ms;
    }
    s_gen++;
}

/* "box.tail1234.ts.net" -> name "box", domain "tail1234.ts.net" */
static void split_fqdn(const char *fqdn, char *name, size_t nlen, char *domain, size_t dlen)
{
    const char *dot = strchr(fqdn, '.');
    if (!dot) {
        snprintf(name, nlen, "%s", fqdn);
        if (domain && dlen) domain[0] = '\0';
        return;
    }
    snprintf(name, nlen, "%.*s", (int)(dot - fqdn), fqdn);
    if (domain && dlen) snprintf(domain, dlen, "%s", dot + 1);
}

/* One-shot enrolment: a file named ts_key in the SD card root holding a
 * tskey-... is saved as the auth key and deleted. */
static bool consume_ts_key_file(void)
{
    FILE *f = fopen(TS_KEY_FILE, "r");
    if (!f) return false;
    char key[DEVOS_TS_KEY_MAX] = { 0 };
    size_t n = fread(key, 1, sizeof(key) - 1, f);
    fclose(f);
    while (n > 0 && (key[n - 1] == '\n' || key[n - 1] == '\r' || key[n - 1] == ' ' || key[n - 1] == '\t')) {
        key[--n] = '\0';
    }
    if (n == 0 || strncmp(key, "tskey-", 6) != 0) {
        printf("[tailnet] %s ignored (expected tskey-...)\n", TS_KEY_FILE);
        return false;
    }
    snprintf(s_cfg.auth_key, sizeof(s_cfg.auth_key), "%s", key);
    s_cfg.auto_connect = true;
    remove(TS_KEY_FILE);
    printf("[tailnet] auth key taken from %s (file deleted)\n", TS_KEY_FILE);
    return true;
}

#ifdef ESP_PLATFORM
/* ========================================================================
 * Device: MicroLink
 * ======================================================================== */
#include "microlink.h"
#include "microlink_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"

static const char *TAG = "tailnet";

#define NVS_NS "tailscale"

typedef enum { CMD_CONNECT, CMD_DISCONNECT, CMD_FORGET } ts_cmd_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_cmdq;
static microlink_t *s_ml;           /* owned by the control task */
static bool s_want;                 /* control task: user wants the tunnel up */
static uint32_t s_sta_ip;           /* Wi-Fi address the client was started on */
static char s_ml_key[DEVOS_TS_KEY_MAX];      /* strings handed to MicroLink */
static char s_ml_name[DEVOS_TS_HOSTNAME_MAX];

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static void cfg_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "auth_key", s_cfg.auth_key);
    nvs_set_str(h, "hostname", s_cfg.hostname);
    nvs_set_u8(h, "auto_connect", s_cfg.auto_connect ? 1 : 0);
    nvs_set_u8(h, "registered", s_cfg.registered ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static void cfg_load(void)
{
    snprintf(s_cfg.hostname, sizeof(s_cfg.hostname), "%s", DEFAULT_HOSTNAME);
    s_cfg.auto_connect = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t l = sizeof(s_cfg.auth_key);
    if (nvs_get_str(h, "auth_key", s_cfg.auth_key, &l) != ESP_OK) s_cfg.auth_key[0] = '\0';
    l = sizeof(s_cfg.hostname);
    if (nvs_get_str(h, "hostname", s_cfg.hostname, &l) != ESP_OK || !s_cfg.hostname[0]) {
        snprintf(s_cfg.hostname, sizeof(s_cfg.hostname), "%s", DEFAULT_HOSTNAME);
    }
    uint8_t v = 1;
    if (nvs_get_u8(h, "auto_connect", &v) == ESP_OK) s_cfg.auto_connect = v != 0;
    v = 0;
    if (nvs_get_u8(h, "registered", &v) == ESP_OK) s_cfg.registered = v != 0;
    nvs_close(h);
}

static uint32_t sta_ip(void)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (n && esp_netif_get_ip_info(n, &ip) == ESP_OK) return ip.ip.addr;
    return 0;
}

/* Publish state + settings-derived fields (control task). */
static void publish(devos_ts_state_t st)
{
    LOCK();
    if (s_info.state != st) s_gen++;
    s_info.state = st;
    s_info.registered = s_cfg.registered;
    s_info.has_auth_key = s_cfg.auth_key[0] != '\0';
    s_info.auto_connect = s_cfg.auto_connect;
    snprintf(s_info.hostname, sizeof(s_info.hostname), "%s", s_cfg.hostname);
    if (st != DEVOS_TS_CONNECTED && st != DEVOS_TS_RECONNECTING) {
        s_info.ip[0] = '\0';
        s_info.derp_connected = false;
    }
    UNLOCK();
}

static void set_error(const char *msg, const char *url)
{
    LOCK();
    snprintf(s_info.last_error, sizeof(s_info.last_error), "%s", msg ? msg : "");
    snprintf(s_info.login_url, sizeof(s_info.login_url), "%s", url ? url : "");
    s_gen++;
    UNLOCK();
}

static void stop_client(void)
{
    if (!s_ml) return;
    ESP_LOGI(TAG, "stopping MicroLink");
    microlink_destroy(s_ml);            /* ~3 s: waits for its tasks to exit */
    s_ml = NULL;
    s_sta_ip = 0;
    LOCK();
    s_npeers = 0;
    s_info.peer_count = s_info.peers_direct = 0;
    s_gen++;
    UNLOCK();
}

static bool start_client(void)
{
    /* MicroLink's four task stacks (~42 KB) and its sockets need internal RAM. */
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_int < 72 * 1024 || heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 16 * 1024) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Not enough internal memory to start Tailscale (%u KB free)",
                 (unsigned)(free_int / 1024));
        set_error(msg, NULL);
        return false;
    }
    /* The auth key is only needed for the first registration; afterwards the
     * node key identifies us (and one-off keys would be rejected). */
    snprintf(s_ml_key, sizeof(s_ml_key), "%s", s_cfg.registered ? "" : s_cfg.auth_key);
    snprintf(s_ml_name, sizeof(s_ml_name), "%s", s_cfg.hostname);
    microlink_config_t mc = {
        .auth_key = s_ml_key,
        .device_name = s_ml_name,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = 0,                 /* Kconfig ML_MAX_PEERS */
    };
    s_ml = microlink_init(&mc);
    if (!s_ml) {
        set_error("Tailscale client failed to initialise", NULL);
        return false;
    }
    if (microlink_start(s_ml) != ESP_OK) {
        microlink_destroy(s_ml);
        s_ml = NULL;
        set_error("Tailscale client failed to start", NULL);
        return false;
    }
    s_sta_ip = sta_ip();
    ESP_LOGI(TAG, "MicroLink started as %s", s_ml_name);
    return true;
}

static devos_ts_state_t map_state(microlink_state_t st)
{
    switch (st) {
    case ML_STATE_REGISTERING:  return DEVOS_TS_REGISTERING;
    case ML_STATE_CONNECTED:    return DEVOS_TS_CONNECTED;
    case ML_STATE_RECONNECTING: return DEVOS_TS_RECONNECTING;
    case ML_STATE_ERROR:        return DEVOS_TS_ERROR;
    default:                    return DEVOS_TS_CONNECTING;
    }
}

/* Copy the live client state for the UI (control task). */
static void snapshot(void)
{
    microlink_t *ml = s_ml;
    devos_ts_state_t st = map_state(microlink_get_state(ml));
    uint64_t now = ml_get_time_ms();
    char domain[64] = "";

    static EXT_RAM_BSS_ATTR devos_ts_peer_t tmp[DEVOS_TS_MAX_PEERS];
    int n = microlink_get_peer_count(ml);
    if (n > DEVOS_TS_MAX_PEERS) n = DEVOS_TS_MAX_PEERS;
    int direct = 0;
    for (int i = 0; i < n; i++) {
        microlink_peer_info_t pi;
        memset(&pi, 0, sizeof(pi));
        devos_ts_peer_t *p = &tmp[i];
        memset(p, 0, sizeof(*p));
        if (microlink_get_peer_info(ml, i, &pi) != ESP_OK) continue;
        pi.hostname[sizeof(pi.hostname) - 1] = '\0';
        snprintf(p->fqdn, sizeof(p->fqdn), "%s", pi.hostname);
        split_fqdn(pi.hostname, p->name, sizeof(p->name), domain[0] ? NULL : domain, sizeof(domain));
        microlink_ip_to_str(pi.vpn_ip, p->ip);
        p->active = pi.online;
        p->direct = pi.direct_path;
        uint64_t pong = ml->peers[i].last_pong_recv_ms;
        p->last_seen_s = pong ? (int)((now - pong) / 1000) : -1;
        if (p->direct) direct++;
    }

    char derp[24] = "";
    uint16_t region = ml->derp_home_region;
    if (region) {
        const char *code = "";
        for (int r = 0; r < ml->derp_region_count && r < ML_MAX_DERP_REGIONS; r++) {
            if (ml->derp_regions[r].region_id == region) { code = ml->derp_regions[r].code; break; }
        }
        if (code[0]) snprintf(derp, sizeof(derp), "%.7s (region %u)", code, (unsigned)region);
        else snprintf(derp, sizeof(derp), "region %u", (unsigned)region);
    }

    LOCK();
    for (int i = 0; i < n; i++) tmp[i].ping_ms = ping_lookup(tmp[i].ip);
    if (n != s_npeers || memcmp(s_peers, tmp, sizeof(devos_ts_peer_t) * (size_t)n) != 0 || s_info.state != st) {
        s_gen++;
    }
    memcpy(s_peers, tmp, sizeof(devos_ts_peer_t) * (size_t)n);
    s_npeers = n;
    s_info.state = st;
    s_info.peer_count = n;
    s_info.peers_direct = direct;
    uint32_t vip = microlink_get_vpn_ip(ml);
    if (vip && (st == DEVOS_TS_CONNECTED || st == DEVOS_TS_RECONNECTING)) microlink_ip_to_str(vip, s_info.ip);
    else s_info.ip[0] = '\0';
    if (domain[0]) snprintf(s_info.domain, sizeof(s_info.domain), "%s", domain);
    snprintf(s_info.derp, sizeof(s_info.derp), "%s", derp);
    s_info.derp_connected = (xEventGroupGetBits(ml->events) & ML_EVT_DERP_CONNECTED) != 0;
    s_info.key_expiry = ml->key_expiry_epoch;
    s_info.key_expired = ml->key_expired;
    UNLOCK();
}

static void control_tick(void)
{
    uint32_t ip = sta_ip();
    if (!s_want) {
        stop_client();
        publish(!s_cfg.registered && !s_cfg.auth_key[0] ? DEVOS_TS_NEEDS_KEY : DEVOS_TS_OFF);
        return;
    }
    if (!s_ml) {
        if (!s_cfg.registered && !s_cfg.auth_key[0]) {
            s_want = false;
            publish(DEVOS_TS_NEEDS_KEY);
            return;
        }
        if (!ip) { publish(DEVOS_TS_WAIT_WIFI); return; }
        set_error("", NULL);
        publish(DEVOS_TS_CONNECTING);
        if (!start_client()) {
            s_want = false;
            publish(DEVOS_TS_ERROR);
        }
        return;
    }

    /* Wi-Fi dropped: tear down, restart when it is back. */
    if (!ip) {
        stop_client();
        publish(DEVOS_TS_WAIT_WIFI);
        return;
    }
    /* New network / new DHCP address: rebind sockets, keep the session. */
    if (ip != s_sta_ip) {
        ESP_LOGI(TAG, "Wi-Fi address changed, rebinding");
        microlink_rebind(s_ml);
        s_sta_ip = ip;
    }

    /* The control plane refused us (bad/expired/used key, removed device). */
    if (s_ml->reg_error[0] || s_ml->auth_url[0]) {
        char err[160], url[160];
        snprintf(err, sizeof(err), "%s", s_ml->reg_error);
        snprintf(url, sizeof(url), "%s", s_ml->auth_url);
        stop_client();
        s_want = false;
        if (err[0]) {
            char msg[160];
            snprintf(msg, sizeof(msg), "Tailscale refused this device: %.120s", err);
            set_error(msg, url[0] ? url : NULL);
        } else {
            set_error(s_cfg.registered
                          ? "This device needs to be re-authorised: enter a new auth key."
                          : "Sign-in needed: enter an auth key (tskey-auth-...).",
                      url);
        }
        if (s_cfg.registered) {             /* node no longer valid */
            s_cfg.registered = false;
            cfg_save();
        }
        publish(DEVOS_TS_ERROR);
        return;
    }

    snapshot();
    if (s_info.state == DEVOS_TS_CONNECTED && !s_cfg.registered) {
        /* First successful registration: the node key is our identity now, so
         * the auth key is no longer needed (or kept). */
        s_cfg.registered = true;
        s_cfg.auth_key[0] = '\0';
        cfg_save();
        publish(DEVOS_TS_CONNECTED);
    }
    if (s_ml->key_expired) {
        set_error("This device's Tailscale key has expired: enter a new auth key.", NULL);
    }
}

static void control_task(void *arg)
{
    (void)arg;
    for (;;) {
        ts_cmd_t cmd;
        if (xQueueReceive(s_cmdq, &cmd, pdMS_TO_TICKS(500)) == pdTRUE) {
            switch (cmd) {
            case CMD_CONNECT:
                s_want = true;
                set_error("", NULL);
                break;
            case CMD_DISCONNECT:
                s_want = false;
                break;
            case CMD_FORGET:
                s_want = false;
                stop_client();
                microlink_factory_reset();
                s_cfg.registered = false;
                s_cfg.auth_key[0] = '\0';
                cfg_save();
                set_error("", NULL);
                ESP_LOGI(TAG, "node keys and cached peers erased");
                break;
            }
        }
        control_tick();
    }
}

static int send_cmd(ts_cmd_t c)
{
    return (s_cmdq && xQueueSend(s_cmdq, &c, pdMS_TO_TICKS(100)) == pdTRUE) ? 0 : -1;
}

void devos_tailnet_init(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    s_cmdq = xQueueCreate(4, sizeof(ts_cmd_t));
    cfg_load();
    if (consume_ts_key_file()) {
        s_cfg.registered = false;           /* explicit new key: register with it */
        cfg_save();
    }
    s_want = s_cfg.auto_connect && (s_cfg.registered || s_cfg.auth_key[0]);
    publish(s_want ? DEVOS_TS_WAIT_WIFI : (!s_cfg.registered && !s_cfg.auth_key[0] ? DEVOS_TS_NEEDS_KEY
                                                                                     : DEVOS_TS_OFF));
    /* Internal-RAM stack: this task writes NVS (flash), which a PSRAM stack
     * must not do. */
    xTaskCreatePinnedToCore(control_task, "ts_ctl", 6144, NULL, 4, NULL, DEVOS_CORE_NET_CRYPTO);
}

int devos_tailnet_connect(void)
{
    if (!s_cfg.auto_connect) {
        s_cfg.auto_connect = true;
        cfg_save();
    }
    return send_cmd(CMD_CONNECT);
}

int devos_tailnet_disconnect(void)
{
    s_cfg.auto_connect = false;
    cfg_save();
    return send_cmd(CMD_DISCONNECT);
}

int devos_tailnet_forget(void)
{
    return send_cmd(CMD_FORGET);
}

int devos_tailnet_set_auth_key(const char *key)
{
    if (!key) return -1;
    while (*key == ' ') key++;
    snprintf(s_cfg.auth_key, sizeof(s_cfg.auth_key), "%s", key);
    size_t n = strlen(s_cfg.auth_key);
    while (n > 0 && (s_cfg.auth_key[n - 1] == ' ' || s_cfg.auth_key[n - 1] == '\n')) s_cfg.auth_key[--n] = '\0';
    if (n) s_cfg.registered = false;        /* register afresh with the new key */
    cfg_save();
    LOCK();
    s_info.has_auth_key = n > 0;
    s_info.registered = s_cfg.registered;
    s_gen++;
    UNLOCK();
    return 0;
}

int devos_tailnet_set_hostname(const char *name)
{
    if (!name) return -1;
    /* DNS label: lower-case letters, digits and '-' */
    char clean[DEVOS_TS_HOSTNAME_MAX];
    size_t j = 0;
    for (size_t i = 0; name[i] && j < sizeof(clean) - 1; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') clean[j++] = c;
        else if (c == ' ' || c == '_' || c == '.') clean[j++] = '-';
    }
    clean[j] = '\0';
    snprintf(s_cfg.hostname, sizeof(s_cfg.hostname), "%s", j ? clean : DEFAULT_HOSTNAME);
    cfg_save();
    LOCK();
    snprintf(s_info.hostname, sizeof(s_info.hostname), "%s", s_cfg.hostname);
    s_gen++;
    UNLOCK();
    return 0;
}

void devos_tailnet_get_info(devos_ts_info_t *out)
{
    if (!out) return;
    if (!s_lock) { memset(out, 0, sizeof(*out)); return; }
    LOCK();
    *out = s_info;
    UNLOCK();
}

int devos_tailnet_peer_count(void)
{
    if (!s_lock) return 0;
    LOCK();
    int n = s_npeers;
    UNLOCK();
    return n;
}

int devos_tailnet_get_peer(int index, devos_ts_peer_t *out)
{
    if (!out || !s_lock) return -1;
    LOCK();
    int ok = index >= 0 && index < s_npeers;
    if (ok) *out = s_peers[index];
    UNLOCK();
    return ok ? 0 : -1;
}

int devos_tailnet_resolve(const char *name, char *out_ip, size_t out_len)
{
    if (!name || !out_ip || out_len == 0 || !s_lock) return -1;
    int rc = -1;
    LOCK();
    if (s_info.state == DEVOS_TS_CONNECTED || s_info.state == DEVOS_TS_RECONNECTING) {
        if (strcasecmp(name, s_info.hostname) == 0 && s_info.ip[0]) {
            snprintf(out_ip, out_len, "%s", s_info.ip);
            rc = 0;
        }
        for (int i = 0; rc != 0 && i < s_npeers; i++) {
            if (strcasecmp(name, s_peers[i].name) == 0 || strcasecmp(name, s_peers[i].fqdn) == 0) {
                snprintf(out_ip, out_len, "%s", s_peers[i].ip);
                rc = 0;
            }
        }
    }
    UNLOCK();
    return rc;
}

/* ---- ICMP ping through the tunnel (esp_ping) ---- */
static void ping_success(esp_ping_handle_t h, void *arg)
{
    uint32_t ms = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    LOCK();
    ping_store((const char *)arg, (int)ms);
    UNLOCK();
}

static void ping_timeout(esp_ping_handle_t h, void *arg)
{
    (void)h;
    LOCK();
    ping_store((const char *)arg, -2);
    UNLOCK();
}

static void ping_end(esp_ping_handle_t h, void *arg)
{
    esp_ping_delete_session(h);
    free(arg);
}

int devos_tailnet_ping(int peer_index)
{
    devos_ts_peer_t p;
    if (devos_tailnet_get_peer(peer_index, &p) != 0) return -1;
    ip_addr_t target;
    memset(&target, 0, sizeof(target));
    if (!ipaddr_aton(p.ip, &target)) return -1;
    char *ip = strdup(p.ip);
    if (!ip) return -1;
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = 1;
    cfg.timeout_ms = 3000;
    cfg.task_stack_size = 3072;
    esp_ping_callbacks_t cbs = { .cb_args = ip, .on_ping_success = ping_success,
                                 .on_ping_timeout = ping_timeout, .on_ping_end = ping_end };
    esp_ping_handle_t h;
    if (esp_ping_new_session(&cfg, &cbs, &h) != ESP_OK) {
        free(ip);
        return -1;
    }
    LOCK();
    ping_store(p.ip, -3);
    UNLOCK();
    esp_ping_start(h);
    return 0;
}

void devos_tailnet_housekeeping(void)
{
    ml_peer_nvs_flush_if_idle(20000);
}

#else
/* ========================================================================
 * Simulator: mirror the host's tailscale (tools/sim/tailscale_live.py)
 * ======================================================================== */
#include <pthread.h>

#define SIM_CONFIG_FILE TAB5_SD_MOUNT_POINT "/.devos/tailscale.json"
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;

static void cfg_save(void)
{
    FILE *f = fopen(SIM_CONFIG_FILE, "w");
    if (!f) return;
    fprintf(f, "{\n  \"auth_key\": \"%s\",\n  \"hostname\": \"%s\",\n  \"auto_connect\": %s,\n  \"registered\": %s\n}\n",
            s_cfg.auth_key, s_cfg.hostname, s_cfg.auto_connect ? "true" : "false",
            s_cfg.registered ? "true" : "false");
    fclose(f);
}

static void cfg_load(void)
{
    snprintf(s_cfg.hostname, sizeof(s_cfg.hostname), "%s", DEFAULT_HOSTNAME);
    s_cfg.auto_connect = true;
    FILE *f = fopen(SIM_CONFIG_FILE, "r");
    if (!f) return;
    char line[256], val[128];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, " \"auth_key\": \"%127[^\"]\"", val) == 1) snprintf(s_cfg.auth_key, sizeof(s_cfg.auth_key), "%s", val);
        else if (sscanf(line, " \"hostname\": \"%39[^\"]\"", val) == 1) snprintf(s_cfg.hostname, sizeof(s_cfg.hostname), "%s", val);
        else if (strstr(line, "\"auto_connect\": false")) s_cfg.auto_connect = false;
        else if (strstr(line, "\"registered\": true")) s_cfg.registered = true;
    }
    fclose(f);
}

static void sim_publish_settings(void)
{
    s_info.registered = s_cfg.registered;
    s_info.has_auth_key = s_cfg.auth_key[0] != '\0';
    s_info.auto_connect = s_cfg.auto_connect;
    snprintf(s_info.hostname, sizeof(s_info.hostname), "%s", s_cfg.hostname);
}

/* Read the host's tailnet. Returns false if the host has no usable tailscale. */
static bool sim_load(void)
{
    FILE *fp = popen("./tools/sim/tailscale_live.py 2>/dev/null", "r");
    if (!fp) return false;
    char line[512];
    bool self = false;
    int n = 0, direct = 0;
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *f[12] = { 0 };
        int nf = 0;
        for (char *tok = strtok(line, "|"); tok && nf < 12; tok = strtok(NULL, "|")) f[nf++] = tok;
        if (nf >= 5 && strcmp(f[0], "SELF") == 0) {
            snprintf(s_info.domain, sizeof(s_info.domain), "%s", f[2]);
            snprintf(s_info.ip, sizeof(s_info.ip), "%s", f[3]);
            snprintf(s_info.derp, sizeof(s_info.derp), "%.20s", f[4]);
            self = true;
        } else if (nf >= 7 && strcmp(f[0], "PEER") == 0 && n < DEVOS_TS_MAX_PEERS) {
            devos_ts_peer_t *p = &s_peers[n++];
            memset(p, 0, sizeof(*p));
            snprintf(p->name, sizeof(p->name), "%s", f[1]);
            snprintf(p->fqdn, sizeof(p->fqdn), "%s", f[2]);
            snprintf(p->ip, sizeof(p->ip), "%s", f[3]);
            p->direct = atoi(f[5]) != 0;
            p->active = atoi(f[6]) != 0;
            p->last_seen_s = p->active ? 0 : -1;
            p->ping_ms = ping_lookup(p->ip);
            if (p->direct) direct++;
        }
    }
    pclose(fp);
    s_npeers = self ? n : 0;
    s_info.peer_count = s_npeers;
    s_info.peers_direct = direct;
    s_info.derp_connected = self;
    return self;
}

void devos_tailnet_init(void)
{
    cfg_load();
    consume_ts_key_file();
    sim_publish_settings();
    s_info.state = DEVOS_TS_OFF;
    if (s_cfg.auto_connect) devos_tailnet_connect();
}

int devos_tailnet_connect(void)
{
    s_cfg.auto_connect = true;
    cfg_save();
    pthread_mutex_lock(&s_mx);
    s_info.last_error[0] = '\0';
    if (sim_load()) {
        s_info.state = DEVOS_TS_CONNECTED;
        s_cfg.registered = true;
    } else {
        s_info.state = DEVOS_TS_ERROR;
        snprintf(s_info.last_error, sizeof(s_info.last_error),
                 "Simulator: the host has no running tailscale (tailscale status failed)");
    }
    sim_publish_settings();
    s_gen++;
    pthread_mutex_unlock(&s_mx);
    return 0;
}

int devos_tailnet_disconnect(void)
{
    s_cfg.auto_connect = false;
    cfg_save();
    pthread_mutex_lock(&s_mx);
    s_info.state = DEVOS_TS_OFF;
    s_info.ip[0] = '\0';
    s_npeers = 0;
    s_info.peer_count = s_info.peers_direct = 0;
    sim_publish_settings();
    s_gen++;
    pthread_mutex_unlock(&s_mx);
    return 0;
}

int devos_tailnet_forget(void)
{
    devos_tailnet_disconnect();
    s_cfg.registered = false;
    s_cfg.auth_key[0] = '\0';
    cfg_save();
    pthread_mutex_lock(&s_mx);
    s_info.state = DEVOS_TS_NEEDS_KEY;
    sim_publish_settings();
    s_gen++;
    pthread_mutex_unlock(&s_mx);
    return 0;
}

int devos_tailnet_set_auth_key(const char *key)
{
    if (!key) return -1;
    snprintf(s_cfg.auth_key, sizeof(s_cfg.auth_key), "%s", key);
    cfg_save();
    sim_publish_settings();
    s_gen++;
    return 0;
}

int devos_tailnet_set_hostname(const char *name)
{
    if (!name) return -1;
    snprintf(s_cfg.hostname, sizeof(s_cfg.hostname), "%s", name[0] ? name : DEFAULT_HOSTNAME);
    cfg_save();
    sim_publish_settings();
    s_gen++;
    return 0;
}

void devos_tailnet_get_info(devos_ts_info_t *out)
{
    if (!out) return;
    pthread_mutex_lock(&s_mx);
    *out = s_info;
    pthread_mutex_unlock(&s_mx);
}

int devos_tailnet_peer_count(void)
{
    return s_npeers;
}

int devos_tailnet_get_peer(int index, devos_ts_peer_t *out)
{
    if (!out) return -1;
    pthread_mutex_lock(&s_mx);
    int ok = index >= 0 && index < s_npeers;
    if (ok) *out = s_peers[index];
    pthread_mutex_unlock(&s_mx);
    return ok ? 0 : -1;
}

int devos_tailnet_resolve(const char *name, char *out_ip, size_t out_len)
{
    if (!name || !out_ip || out_len == 0 || s_info.state != DEVOS_TS_CONNECTED) return -1;
    for (int i = 0; i < s_npeers; i++) {
        if (strcasecmp(name, s_peers[i].name) == 0 || strcasecmp(name, s_peers[i].fqdn) == 0) {
            snprintf(out_ip, out_len, "%s", s_peers[i].ip);
            return 0;
        }
    }
    return -1;
}

static void *sim_ping_thread(void *arg)
{
    char *ip = arg;
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "ping -c 1 -W 3 %s 2>/dev/null", ip);
    int ms = -2;
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char out[256];
        while (fgets(out, sizeof(out), fp)) {
            char *t = strstr(out, "time=");
            if (t) ms = (int)(atof(t + 5) + 0.5);
        }
        pclose(fp);
    }
    pthread_mutex_lock(&s_mx);
    ping_store(ip, ms);
    pthread_mutex_unlock(&s_mx);
    free(ip);
    return NULL;
}

void devos_tailnet_housekeeping(void) {}

int devos_tailnet_ping(int peer_index)
{
    devos_ts_peer_t p;
    if (devos_tailnet_get_peer(peer_index, &p) != 0) return -1;
    char *ip = strdup(p.ip);
    if (!ip) return -1;
    pthread_mutex_lock(&s_mx);
    ping_store(p.ip, -3);
    pthread_mutex_unlock(&s_mx);
    pthread_t th;
    if (pthread_create(&th, NULL, sim_ping_thread, ip) != 0) { free(ip); return -1; }
    pthread_detach(th);
    return 0;
}
#endif

bool devos_tailnet_has_auth_key(void)
{
    return s_cfg.auth_key[0] != '\0';
}

void devos_tailnet_get_hostname(char *out, size_t len)
{
    if (out && len) snprintf(out, len, "%s", s_cfg.hostname);
}

uint32_t devos_tailnet_generation(void)
{
    return s_gen;
}
