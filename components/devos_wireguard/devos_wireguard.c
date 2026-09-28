/* devos_wireguard: see devos_wireguard.h. */
#include "devos_wireguard.h"
#include "devos_config.h"
#include "devos_net.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef ESP_PLATFORM
#include "devos_tailnet.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/ip.h"
#include "lwip/ip_addr.h"
#include "lwip/mem.h"
#include "lwip/netif.h"
#include "lwip/udp.h"
#include "nvs.h"
#include "wireguard.h"
#include "wireguard-platform.h"
#include "wireguardif.h"
static const char *TAG = "devos_wg";
static SemaphoreHandle_t s_mx;
/* Safe before *_init(): other tasks (sysmon, launcher tiles) may ask for
 * status first, and a NULL semaphore asserts. */
#define LOCK()   do { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_mx) xSemaphoreGive(s_mx); } while (0)
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
static pthread_mutex_t s_mx_sim = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&s_mx_sim)
#define UNLOCK() pthread_mutex_unlock(&s_mx_sim)
static void sim_tick(void);
#endif

static char s_names[DEVOS_WG_MAX_TUNNELS][DEVOS_WG_NAME_MAX];
static int s_count;
static devos_wg_info_t s_info = { .state = DEVOS_WG_OFF, .active = -1, .handshake_age_s = -1, .last_rx_age_s = -1 };
static devos_wg_config_t s_run;          /* config of the running tunnel */
static volatile uint32_t s_gen;

/* ------------------------------------------------------------------ parsing */
static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* A WireGuard key: base64 of exactly 32 bytes (44 chars, one '='). */
static bool key_ok(const char *k)
{
    if (strlen(k) != 44 || k[43] != '=') return false;
    for (int i = 0; i < 43; i++) if (b64_val((unsigned char)k[i]) < 0) return false;
    return true;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

/* "a.b.c.d[/n]" -> ip + mask (network order). */
static bool parse_cidr4(const char *s, devos_wg_cidr_t *out)
{
    unsigned a, b, c, d, n = 32;
    char tail;
    int got = sscanf(s, "%u.%u.%u.%u/%u%c", &a, &b, &c, &d, &n, &tail);
    if (got != 4 && got != 5) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255 || n > 32) return false;
    uint32_t host = (a << 24) | (b << 16) | (c << 8) | d;
    uint32_t m = n ? 0xFFFFFFFFu << (32 - n) : 0;
    out->ip = htonl(host);
    out->mask = htonl(m);
    return true;
}

int devos_wg_parse(const char *text, devos_wg_config_t *out, char *err, size_t err_cap)
{
    memset(out, 0, sizeof(*out));
    if (!text) text = "";
    char *buf = strdup(text);
    if (!buf) {
        snprintf(err, err_cap, "Out of memory");
        return -1;
    }
    enum { SEC_NONE, SEC_IF, SEC_PEER } sec = SEC_NONE;
    devos_wg_peer_t *peer = NULL;
    int rc = -1;
    bool have_if = false;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *l = trim(line);
        if (!*l) continue;
        if (*l == '[') {
            if (!strncasecmp(l, "[Interface]", 11)) {
                sec = SEC_IF;
                have_if = true;
            } else if (!strncasecmp(l, "[Peer]", 6)) {
                if (out->peer_n >= DEVOS_WG_MAX_PEERS) {
                    snprintf(err, err_cap, "Too many [Peer] sections (max %d)", DEVOS_WG_MAX_PEERS);
                    goto done;
                }
                peer = &out->peers[out->peer_n++];
                sec = SEC_PEER;
            } else {
                sec = SEC_NONE;
            }
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(l), *val = trim(eq + 1);
        if (sec == SEC_IF) {
            if (!strcasecmp(key, "PrivateKey")) snprintf(out->private_key, sizeof(out->private_key), "%s", val);
            else if (!strcasecmp(key, "Address")) {
                char *s2 = NULL;
                for (char *a = strtok_r(val, ",", &s2); a; a = strtok_r(NULL, ",", &s2)) {
                    if (!out->address.ip && parse_cidr4(trim(a), &out->address)) break;
                }
            } else if (!strcasecmp(key, "DNS")) {
                char *s2 = NULL;
                for (char *a = strtok_r(val, ",", &s2); a; a = strtok_r(NULL, ",", &s2)) {
                    devos_wg_cidr_t c;
                    if (!out->dns && parse_cidr4(trim(a), &c) && c.mask == 0xFFFFFFFFu) out->dns = c.ip;
                }
            } else if (!strcasecmp(key, "MTU")) out->mtu = atoi(val);
            else if (!strcasecmp(key, "ListenPort")) out->listen_port = atoi(val);
        } else if (sec == SEC_PEER && peer) {
            if (!strcasecmp(key, "PublicKey")) snprintf(peer->public_key, sizeof(peer->public_key), "%s", val);
            else if (!strcasecmp(key, "PresharedKey")) snprintf(peer->preshared_key, sizeof(peer->preshared_key), "%s", val);
            else if (!strcasecmp(key, "Endpoint")) {
                if (val[0] == '[') {
                    snprintf(err, err_cap, "IPv6 endpoints aren't supported (%s)", val);
                    goto done;
                }
                char *colon = strrchr(val, ':');
                if (!colon) {
                    snprintf(err, err_cap, "Endpoint needs a port (host:port)");
                    goto done;
                }
                *colon = '\0';
                snprintf(peer->endpoint_host, sizeof(peer->endpoint_host), "%s", trim(val));
                peer->endpoint_port = atoi(colon + 1);
            } else if (!strcasecmp(key, "AllowedIPs")) {
                char *s2 = NULL;
                for (char *a = strtok_r(val, ",", &s2); a; a = strtok_r(NULL, ",", &s2)) {
                    a = trim(a);
                    devos_wg_cidr_t c;
                    if (strchr(a, ':')) { peer->allowed_ipv6_skipped = true; continue; }
                    if (!parse_cidr4(a, &c)) {
                        snprintf(err, err_cap, "Bad AllowedIPs entry: %.40s", a);
                        goto done;
                    }
                    if (peer->allowed_n >= DEVOS_WG_MAX_ALLOWED) {
                        snprintf(err, err_cap, "Too many AllowedIPs (max %d IPv4 per peer)", DEVOS_WG_MAX_ALLOWED);
                        goto done;
                    }
                    c.ip &= c.mask;
                    peer->allowed[peer->allowed_n++] = c;
                    if (c.mask == 0) out->full_tunnel = true;
                }
            } else if (!strcasecmp(key, "PersistentKeepalive")) {
                peer->keepalive_s = strcasecmp(val, "off") ? atoi(val) : 0;
            }
        }
    }
    if (!have_if) { snprintf(err, err_cap, "No [Interface] section - is this a WireGuard config?"); goto done; }
    if (!key_ok(out->private_key)) { snprintf(err, err_cap, "PrivateKey is missing or not a WireGuard key"); goto done; }
    if (!out->address.ip) { snprintf(err, err_cap, "Address needs an IPv4 address (e.g. 10.8.0.2/24)"); goto done; }
    if (!out->peer_n) { snprintf(err, err_cap, "No [Peer] section"); goto done; }
    bool endpoint = false;
    for (int i = 0; i < out->peer_n; i++) {
        devos_wg_peer_t *p = &out->peers[i];
        if (!key_ok(p->public_key)) { snprintf(err, err_cap, "Peer %d: PublicKey is missing or invalid", i + 1); goto done; }
        if (p->preshared_key[0] && !key_ok(p->preshared_key)) { snprintf(err, err_cap, "Peer %d: PresharedKey is invalid", i + 1); goto done; }
        if (!p->allowed_n) { snprintf(err, err_cap, "Peer %d: no IPv4 AllowedIPs", i + 1); goto done; }
        if (p->endpoint_host[0]) {
            if (p->endpoint_port <= 0 || p->endpoint_port > 65535) { snprintf(err, err_cap, "Peer %d: bad Endpoint port", i + 1); goto done; }
            endpoint = true;
        }
    }
    if (!endpoint) { snprintf(err, err_cap, "No peer has an Endpoint to connect to"); goto done; }
    if (out->mtu && (out->mtu < 576 || out->mtu > 1420)) out->mtu = 1420;   /* no IP reassembly: stay <= 1420 */
    if (out->listen_port < 0 || out->listen_port > 65535) out->listen_port = 0;
    rc = 0;
done:
    memset(buf, 0, strlen(text));
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ storage */
#ifdef ESP_PLATFORM
#define WG_NVS_NS "wireguard"

static bool store_get(int idx, char *name, size_t ncap, char *conf, size_t ccap)
{
    nvs_handle_t h;
    if (nvs_open(WG_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char k[16];
    size_t l = ncap;
    snprintf(k, sizeof(k), "n%d", idx);
    bool ok = nvs_get_str(h, k, name, &l) == ESP_OK;
    if (ok && conf) {
        l = ccap;
        snprintf(k, sizeof(k), "c%d", idx);
        ok = nvs_get_str(h, k, conf, &l) == ESP_OK;
    }
    nvs_close(h);
    return ok;
}

static bool store_set(int idx, const char *name, const char *conf)
{
    nvs_handle_t h;
    if (nvs_open(WG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    char k[16];
    bool ok;
    snprintf(k, sizeof(k), "n%d", idx);
    ok = nvs_set_str(h, k, name) == ESP_OK;
    snprintf(k, sizeof(k), "c%d", idx);
    ok = ok && nvs_set_str(h, k, conf) == ESP_OK;
    ok = ok && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void store_del(int idx)
{
    nvs_handle_t h;
    if (nvs_open(WG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char k[16];
    snprintf(k, sizeof(k), "n%d", idx);
    nvs_erase_key(h, k);
    snprintf(k, sizeof(k), "c%d", idx);
    nvs_erase_key(h, k);
    nvs_commit(h);
    nvs_close(h);
}
#else
static void sim_path(int idx, char *out, size_t cap)
{
    snprintf(out, cap, TAB5_SD_MOUNT_POINT "/.devos/wg_%d.conf", idx);
}

static bool store_get(int idx, char *name, size_t ncap, char *conf, size_t ccap)
{
    char p[128];
    sim_path(idx, p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    char line[128];
    bool ok = fgets(line, sizeof(line), f) && !strncmp(line, "# name: ", 8);
    if (ok) {
        line[strcspn(line, "\n")] = '\0';
        snprintf(name, ncap, "%s", line + 8);
        if (conf) {
            size_t n = fread(conf, 1, ccap - 1, f);
            conf[n] = '\0';
        }
    }
    fclose(f);
    return ok;
}

static bool store_set(int idx, const char *name, const char *conf)
{
    char p[128];
    sim_path(idx, p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (!f) return false;
    fprintf(f, "# name: %s\n%s", name, conf);
    fclose(f);
    return true;
}

static void store_del(int idx)
{
    char p[128];
    sim_path(idx, p, sizeof(p));
    remove(p);
}
#endif

static void load_names(void)
{
    s_count = 0;
    for (int i = 0; i < DEVOS_WG_MAX_TUNNELS; i++) {
        if (!store_get(i, s_names[s_count], sizeof(s_names[0]), NULL, 0)) break;
        s_count++;
    }
}

int devos_wg_count(void) { return s_count; }

const char *devos_wg_name(int idx)
{
    return (idx >= 0 && idx < s_count) ? s_names[idx] : "";
}

int devos_wg_get_config(int idx, devos_wg_config_t *out)
{
    if (idx < 0 || idx >= s_count) return -1;
    char name[DEVOS_WG_NAME_MAX];
    char *conf = calloc(1, DEVOS_WG_CONF_MAX);
    if (!conf) return -1;
    char err[80];
    int rc = store_get(idx, name, sizeof(name), conf, DEVOS_WG_CONF_MAX) ? devos_wg_parse(conf, out, err, sizeof(err)) : -1;
    memset(conf, 0, DEVOS_WG_CONF_MAX);
    free(conf);
    return rc;
}

int devos_wg_add(const char *name, const char *conf_text, char *err, size_t err_cap)
{
    devos_wg_config_t c;
    if (!name || !name[0]) { snprintf(err, err_cap, "Give the tunnel a name"); return -1; }
    if (strlen(conf_text) >= DEVOS_WG_CONF_MAX) { snprintf(err, err_cap, "Config is too big (max 2 KB)"); return -1; }
    if (devos_wg_parse(conf_text, &c, err, err_cap) != 0) return -1;
    memset(&c, 0, sizeof(c));
    int idx = -1;
    for (int i = 0; i < s_count; i++) if (!strcmp(s_names[i], name)) idx = i;
    if (idx < 0) {
        if (s_count >= DEVOS_WG_MAX_TUNNELS) { snprintf(err, err_cap, "Already %d tunnels - delete one first", DEVOS_WG_MAX_TUNNELS); return -1; }
        idx = s_count;
    }
    if (idx == s_info.active && devos_wg_active()) { snprintf(err, err_cap, "Disconnect \"%s\" first", name); return -1; }
    if (!store_set(idx, name, conf_text)) { snprintf(err, err_cap, "Couldn't save it"); return -1; }
    load_names();
    s_gen++;
    return idx;
}

int devos_wg_remove(int idx)
{
    if (idx < 0 || idx >= s_count) return -1;
    if (idx == s_info.active && devos_wg_active()) return -1;
    char name[DEVOS_WG_NAME_MAX];
    char *conf = calloc(1, DEVOS_WG_CONF_MAX);
    if (!conf) return -1;
    for (int i = idx + 1; i < s_count; i++) {            /* shift the rest down */
        if (store_get(i, name, sizeof(name), conf, DEVOS_WG_CONF_MAX)) store_set(i - 1, name, conf);
    }
    memset(conf, 0, DEVOS_WG_CONF_MAX);
    free(conf);
    store_del(s_count - 1);
    if (s_info.active > idx) s_info.active--;
    else if (s_info.active == idx) s_info.active = -1;
    load_names();
    s_gen++;
    return 0;
}

/* ------------------------------------------------------------------ status */
uint32_t devos_wg_generation(void) { return s_gen; }

void devos_wg_get_info(devos_wg_info_t *out)
{
#ifndef ESP_PLATFORM
    sim_tick();
#endif
    LOCK();
    *out = s_info;
    UNLOCK();
}

bool devos_wg_active(void)
{
    return s_info.state == DEVOS_WG_CONNECTING || s_info.state == DEVOS_WG_UP;
}

static void set_state(devos_wg_state_t st, const char *error)
{
    LOCK();
    s_info.state = st;
    if (error) snprintf(s_info.error, sizeof(s_info.error), "%s", error);
    UNLOCK();
    s_gen++;
}

static void fmt_ip(uint32_t nbo, char *out, size_t cap)
{
    const uint8_t *b = (const uint8_t *)&nbo;
    snprintf(out, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static int mask_bits(uint32_t mask_nbo)
{
    uint32_t m = ntohl(mask_nbo);
    int n = 0;
    while (m & 0x80000000u) { n++; m <<= 1; }
    return n;
}

/* devos_net route hook: app connections to AllowedIPs use our address. */
static bool route_cb(uint32_t dest, uint32_t *src)
{
    if (s_info.state != DEVOS_WG_UP) return false;
    for (int i = 0; i < s_run.peer_n; i++) {
        const devos_wg_peer_t *p = &s_run.peers[i];
        for (int k = 0; k < p->allowed_n; k++) {
            if ((dest & p->allowed[k].mask) == p->allowed[k].ip) {
                *src = s_run.address.ip;
                return true;
            }
        }
    }
    return false;
}

static void info_for_start(int idx, const devos_wg_config_t *c)
{
    char ip[16];
    LOCK();
    s_info.active = idx;
    snprintf(s_info.name, sizeof(s_info.name), "%s", s_names[idx]);
    fmt_ip(c->address.ip, ip, sizeof(ip));
    snprintf(s_info.address, sizeof(s_info.address), "%s/%d", ip, mask_bits(c->address.mask));
    s_info.handshake_age_s = s_info.last_rx_age_s = -1;
    s_info.full_tunnel = c->full_tunnel;
    s_info.error[0] = '\0';
    s_info.endpoint[0] = '\0';
    s_info.public_key[0] = '\0';
    UNLOCK();
}

#ifdef ESP_PLATFORM
/* ------------------------------------------------------------------ device tunnel */
typedef enum { CMD_CONNECT, CMD_DISCONNECT } cmd_t;
typedef struct { cmd_t cmd; int idx; } msg_t;

static QueueHandle_t s_q;
static struct netif s_netif;
static bool s_netif_added;
static bool s_dns_set;                   /* our DNS is in place; s_dns_prev to restore */
static ip_addr_t s_dns_prev;
static uint32_t s_endpoint_ip[DEVOS_WG_MAX_PEERS];

typedef struct {
    bool link;
    int hs_ms, rx_ms;
    char pub[48];
} wg_status_t;

static esp_err_t tun_up_cb(void *ctx)
{
    const devos_wg_config_t *c = ctx;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    struct wireguardif_init_data wid = {
        .private_key = c->private_key,
        .listen_port = (u16_t)c->listen_port,
        .bind_netif = sta ? (struct netif *)esp_netif_get_netif_impl(sta) : NULL,
    };
    wireguardif_enable_socket_bind();
    /* Added without an address, which is then set directly. Not via
     * netif_add(addr) / netif_set_addr(): they fire lwIP's address-changed
     * callback, and esp_netif (no LWIP_ESP_NETIF_DATA in this build) reads
     * netif->state of *every* netif as its own esp_netif_t. Ours is the
     * WireGuard init data, later the device, so it dereferenced garbage and
     * the Tab5 rebooted on connect. MicroLink sets its address the same way. */
    if (!netif_add(&s_netif, NULL, NULL, NULL, &wid, wireguardif_init, ip_input)) return ESP_FAIL;
    s_netif_added = true;
    ip4_addr_set_u32(ip_2_ip4(&s_netif.ip_addr), c->address.ip);
    ip4_addr_set_u32(ip_2_ip4(&s_netif.netmask), c->address.mask);
    ip4_addr_set_u32(ip_2_ip4(&s_netif.gw), 0);
    if (c->mtu) s_netif.mtu = (u16_t)c->mtu;
    netif_set_up(&s_netif);
    for (int i = 0; i < c->peer_n; i++) {
        const devos_wg_peer_t *p = &c->peers[i];
        struct wireguardif_peer wp;
        uint8_t psk[32];
        size_t psk_len = sizeof(psk);
        wireguardif_peer_init(&wp);
        wp.public_key = p->public_key;
        wp.preshared_key = (p->preshared_key[0] && wireguard_base64_decode(p->preshared_key, psk, &psk_len) && psk_len == 32)
                           ? psk : NULL;
        ip_addr_t aip = IPADDR4_INIT(p->allowed[0].ip), amask = IPADDR4_INIT(p->allowed[0].mask);
        wp.allowed_ip = aip;
        wp.allowed_mask = amask;
        ip_addr_t ep = IPADDR4_INIT(s_endpoint_ip[i]);
        wp.endpoint_ip = ep;
        wp.endport_port = (u16_t)p->endpoint_port;
        wp.keep_alive = (u16_t)p->keepalive_s;
        u8_t pi;
        err_t e = wireguardif_add_peer(&s_netif, &wp, &pi);
        memset(psk, 0, sizeof(psk));
        if (e != ERR_OK) return ESP_FAIL;
        for (int k = 1; k < p->allowed_n; k++) {
            ip_addr_t xi = IPADDR4_INIT(p->allowed[k].ip), xm = IPADDR4_INIT(p->allowed[k].mask);
            wireguardif_add_allowed_ip(&s_netif, pi, &xi, &xm);
        }
        if (s_endpoint_ip[i]) wireguardif_connect(&s_netif, pi);
    }
    return ESP_OK;
}

static esp_err_t tun_down_cb(void *ctx)
{
    (void)ctx;
    if (s_dns_set) {                                /* give Wi-Fi's DNS back */
        dns_setserver(0, &s_dns_prev);
        s_dns_set = false;
    }
    if (!s_netif_added) return ESP_OK;
    struct wireguard_device *dev = (struct wireguard_device *)s_netif.state;
    wireguardif_shutdown(&s_netif);                 /* stops its timer */
    netif_set_link_down(&s_netif);
    netif_set_down(&s_netif);
    netif_remove(&s_netif);
    if (dev) {                                      /* upstream never frees these */
        if (dev->udp_pcb) udp_remove(dev->udp_pcb);
        memset(dev, 0, sizeof(*dev));               /* keys, session state */
        mem_free(dev);
    }
    memset(&s_netif, 0, sizeof(s_netif));
    s_netif_added = false;
    return ESP_OK;
}

static esp_err_t tun_status_cb(void *ctx)
{
    wg_status_t *st = ctx;
    st->hs_ms = st->rx_ms = -1;
    st->link = false;
    if (!s_netif_added || !s_netif.state) return ESP_OK;
    struct wireguard_device *dev = (struct wireguard_device *)s_netif.state;
    st->link = netif_is_link_up(&s_netif);
    uint32_t now = wireguard_sys_now();
    for (int i = 0; i < WIREGUARD_MAX_PEERS; i++) {
        struct wireguard_peer *p = &dev->peers[i];
        if (!p->valid) continue;
        if (p->curr_keypair.valid) st->hs_ms = (int)(now - p->curr_keypair.keypair_millis);
        if (p->last_rx) st->rx_ms = (int)(now - p->last_rx);
        break;
    }
    size_t l = sizeof(st->pub);
    wireguard_base64_encode(dev->public_key, 32, st->pub, &l);
    /* full tunnel: the config's DNS while up (re-asserted: DHCP renewals reset it) */
    if (st->link && s_run.full_tunnel && s_run.dns) {
        ip_addr_t want = IPADDR4_INIT(s_run.dns);
        const ip_addr_t *cur = dns_getserver(0);
        if (!s_dns_set) {
            s_dns_prev = *cur;
            s_dns_set = true;
        }
        if (!ip_addr_cmp(cur, &want)) dns_setserver(0, &want);
    }
    return ESP_OK;
}

static bool tailscale_running(void)
{
    devos_ts_info_t ti;
    devos_tailnet_get_info(&ti);
    return ti.state != DEVOS_TS_OFF && ti.state != DEVOS_TS_NEEDS_KEY && ti.state != DEVOS_TS_ERROR;
}

static void do_disconnect(void)
{
    esp_netif_tcpip_exec(tun_down_cb, NULL);
    LOCK();
    s_info.handshake_age_s = s_info.last_rx_age_s = -1;
    UNLOCK();
}

static void do_connect(int idx)
{
    static devos_wg_config_t c;
    char err[160];
    do_disconnect();
    if (devos_wg_get_config(idx, &c) != 0) {
        set_state(DEVOS_WG_ERROR, "That tunnel's config can't be read");
        return;
    }
    info_for_start(idx, &c);
    if (tailscale_running()) {
        set_state(DEVOS_WG_ERROR, "Turn off Tailscale first - the two can't run at the same time");
        return;
    }
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ipi;
    if (!sta || esp_netif_get_ip_info(sta, &ipi) != ESP_OK || ipi.ip.addr == 0) {
        set_state(DEVOS_WG_ERROR, "Connect to Wi-Fi first");
        return;
    }
    set_state(DEVOS_WG_CONNECTING, "");
    /* resolve endpoints (blocking DNS is fine on this task) */
    for (int i = 0; i < c.peer_n; i++) {
        s_endpoint_ip[i] = 0;
        if (!c.peers[i].endpoint_host[0]) continue;
        char ip[46];
        if (devos_net_resolve(c.peers[i].endpoint_host, ip, sizeof(ip)) != 0 || inet_pton(AF_INET, ip, &s_endpoint_ip[i]) != 1) {
            snprintf(err, sizeof(err), "Can't resolve %.100s", c.peers[i].endpoint_host);
            set_state(DEVOS_WG_ERROR, err);
            return;
        }
        if (i == 0) {
            LOCK();
            if (!strcmp(ip, c.peers[0].endpoint_host)) snprintf(s_info.endpoint, sizeof(s_info.endpoint), "%s:%d", ip, c.peers[0].endpoint_port);
            else snprintf(s_info.endpoint, sizeof(s_info.endpoint), "%.60s:%d (%s)", c.peers[0].endpoint_host, c.peers[0].endpoint_port, ip);
            UNLOCK();
        }
    }
    LOCK();
    s_run = c;
    UNLOCK();
    if (esp_netif_tcpip_exec(tun_up_cb, &c) != ESP_OK) {
        esp_netif_tcpip_exec(tun_down_cb, NULL);
        set_state(DEVOS_WG_ERROR, c.listen_port ? "Couldn't start the tunnel (is ListenPort in use?)" : "Couldn't start the tunnel");
        return;
    }
    memset(&c, 0, sizeof(c));
    ESP_LOGI(TAG, "tunnel %s started", s_info.name);
}

static void wg_task(void *arg)
{
    (void)arg;
    int64_t started = 0;
    for (;;) {
        msg_t m;
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (m.cmd == CMD_CONNECT) {
                do_connect(m.idx);
                started = esp_log_timestamp();
            } else {
                do_disconnect();
                set_state(DEVOS_WG_OFF, "");
            }
            continue;
        }
        if (s_info.state != DEVOS_WG_CONNECTING && s_info.state != DEVOS_WG_UP) continue;
        wg_status_t st;
        esp_netif_tcpip_exec(tun_status_cb, &st);
        LOCK();
        s_info.handshake_age_s = st.hs_ms < 0 ? -1 : st.hs_ms / 1000;
        s_info.last_rx_age_s = st.rx_ms < 0 ? -1 : st.rx_ms / 1000;
        snprintf(s_info.public_key, sizeof(s_info.public_key), "%s", st.pub);
        devos_wg_state_t want = st.link ? DEVOS_WG_UP : DEVOS_WG_CONNECTING;
        bool changed = want != s_info.state;
        s_info.state = want;
        if (want == DEVOS_WG_CONNECTING && esp_log_timestamp() - started > 20000)
            snprintf(s_info.error, sizeof(s_info.error), "No handshake yet - check the endpoint, keys and that the server knows our public key");
        else if (want == DEVOS_WG_UP) s_info.error[0] = '\0';
        UNLOCK();
        if (changed) s_gen++;
    }
}

void devos_wg_init(void)
{
    if (s_q) return;
    s_mx = xSemaphoreCreateMutex();
    s_q = xQueueCreate(4, sizeof(msg_t));
    load_names();
    devos_net_set_route_hook(route_cb);
    xTaskCreatePinnedToCore(wg_task, "wg", 6144, NULL, 4, NULL, DEVOS_CORE_NET_CRYPTO);
}

int devos_wg_connect(int idx)
{
    if (idx < 0 || idx >= s_count || !s_q) return -1;
    msg_t m = { CMD_CONNECT, idx };
    set_state(DEVOS_WG_CONNECTING, "");
    return xQueueSend(s_q, &m, 0) == pdTRUE ? 0 : -1;
}

void devos_wg_disconnect(void)
{
    if (!s_q) return;
    msg_t m = { CMD_DISCONNECT, -1 };
    xQueueSend(s_q, &m, 0);
}

#else  /* ------------------------------------------------------------------ simulator */
static time_t s_sim_started;

void devos_wg_init(void)
{
    load_names();
    devos_net_set_route_hook(route_cb);
}

int devos_wg_connect(int idx)
{
    devos_wg_config_t c;
    if (idx < 0 || idx >= s_count || devos_wg_get_config(idx, &c) != 0) return -1;
    info_for_start(idx, &c);
    LOCK();
    snprintf(s_info.endpoint, sizeof(s_info.endpoint), "%.90s:%d", c.peers[0].endpoint_host, c.peers[0].endpoint_port);
    snprintf(s_info.public_key, sizeof(s_info.public_key), "(shown on the Tab5)");
    UNLOCK();
    s_sim_started = time(NULL);
    set_state(DEVOS_WG_CONNECTING, "");
    return 0;
}

void devos_wg_disconnect(void)
{
    set_state(DEVOS_WG_OFF, "");
}

/* No tunnels in the simulator: "handshake" after 2 s so the UI can be seen. */
static void sim_tick(void)
{
    if (s_info.state == DEVOS_WG_CONNECTING && time(NULL) - s_sim_started >= 2) {
        LOCK();
        s_run.peer_n = 0;               /* never route real sim traffic */
        snprintf(s_info.error, sizeof(s_info.error), "Simulator: no real tunnel");
        UNLOCK();
        set_state(DEVOS_WG_UP, NULL);
    }
    if (s_info.state == DEVOS_WG_UP) {
        LOCK();
        s_info.handshake_age_s = (int)(time(NULL) - s_sim_started - 2);
        s_info.last_rx_age_s = 1;
        UNLOCK();
    }
}
#endif
