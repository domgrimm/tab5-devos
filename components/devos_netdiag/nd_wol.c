/* devos_netdiag > wol: Wake-on-LAN magic packets. No LVGL.
 *
 * A magic packet is 6 x 0xFF followed by the target MAC repeated 16 times
 * (102 bytes), sent to UDP port 9 (discard) - some NICs also listen on 7, so
 * both are sent. The default destination is the subnet broadcast plus
 * 255.255.255.255; a machine on another network needs its address (or a
 * directed broadcast) typed in, and then the socket is routed through any VPN
 * that claims it, like every other connection (AGENTS.md #7). */
#include "devos_netdiag.h"
#include "devos_net.h"
#include "devos_config.h"
#include "nd_int.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <lwip/sockets.h>
#include <lwip/inet.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static char s_err[96];
static char s_target[48];

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int devos_wol_parse_mac(const char *text, uint8_t mac[6], char *err, size_t errcap)
{
    uint8_t nib[12];
    int n = 0;
    if (!text || !*text) { if (err) snprintf(err, errcap, "Enter a MAC address"); return -1; }
    for (const char *p = text; *p; p++) {
        if (*p == ':' || *p == '-' || *p == '.' || *p == ' ') continue;
        int v = hex_nibble(*p);
        if (v < 0) { if (err) snprintf(err, errcap, "Not a hex digit: '%c'", *p); return -1; }
        if (n >= 12) { if (err) snprintf(err, errcap, "Too long (a MAC is 12 hex digits)"); return -1; }
        nib[n++] = (uint8_t)v;
    }
    if (n != 12) { if (err) snprintf(err, errcap, "A MAC needs 12 hex digits (got %d)", n); return -1; }
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)((nib[i * 2] << 4) | nib[i * 2 + 1]);
    if (err && errcap) err[0] = '\0';
    return 0;
}

void devos_wol_format_mac(const uint8_t mac[6], char *out, size_t cap)
{
    snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Resolve the destination string to an address: "" = broadcast, an IP is used
 * as-is, anything else goes through the shared resolver (mDNS / DNS). */
static int resolve_dest(const char *addr, struct in_addr *out, char *err, size_t errcap)
{
    if (!addr || !*addr) {
        out->s_addr = htonl(INADDR_BROADCAST);
        return 0;
    }
    if (inet_pton(AF_INET, addr, out) == 1) return 0;
    char ip[16];
    if (devos_net_resolve(addr, ip, sizeof(ip)) != 0 ||
        inet_pton(AF_INET, ip, out) != 1) {
        if (err && errcap) snprintf(err, errcap, "Could not resolve \"%.40s\"", addr ? addr : "");
        return -1;
    }
    return 0;
}

static int send_one(const uint8_t mac[6], struct in_addr dst, int port, char *err, size_t errcap)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        if (err && errcap) snprintf(err, errcap, "Could not open a UDP socket");
        return -1;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    devos_net_socket_route(s, dst.s_addr);          /* via a VPN if it claims it */

    uint8_t pkt[102];
    memset(pkt, 0xff, 6);
    for (int i = 1; i <= 16; i++) memcpy(pkt + i * 6, mac, 6);

    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr = dst };
    int r = (int)sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to));
    close(s);
    if (r != (int)sizeof(pkt)) {
        if (err && errcap) snprintf(err, errcap, "Send failed");
        return -1;
    }
    return 0;
}

int devos_wol_send_ex(const uint8_t mac[6], const char *addr,
                      char *target_out, size_t target_cap,
                      char *err_out, size_t err_cap)
{
    if (err_out && err_cap) err_out[0] = '\0';
    if (target_out && target_cap) target_out[0] = '\0';
    struct in_addr dst;
    if (resolve_dest(addr, &dst, err_out, err_cap) != 0) return -1;
    /* Port 9 is the convention; 7 (echo) is a belt-and-braces retry, ignored
     * if it fails. */
    int r = send_one(mac, dst, 9, err_out, err_cap);
    if (r == 0) send_one(mac, dst, 7, NULL, 0);

    struct in_addr show;
    if (!addr || !*addr) show.s_addr = htonl(INADDR_BROADCAST);
    else show = dst;
    char ip[16];
    inet_ntop(AF_INET, &show, ip, sizeof(ip));
    if (target_out && target_cap) snprintf(target_out, target_cap, "%s:9", ip);
    return r;
}

int devos_wol_send(const uint8_t mac[6], const char *addr)
{
    return devos_wol_send_ex(mac, addr, s_target, sizeof(s_target), s_err, sizeof(s_err));
}

/* ------------------------------------------------------------------ async */
/* Request-specific sends (Jobs network.wol): resolution runs on a worker so a
 * hostname never blocks the scheduler task, and each ticket owns its result. */
#define DEVOS_WOL_MAX 2
static EXT_RAM_BSS_ATTR struct {
    nd_mutex_t mx;
    bool used, busy, stop;
    uint32_t id;
    uint8_t mac[6];
    char addr[64];
    bool ok;
    char target[48];
    char error[96];
} W[DEVOS_WOL_MAX];
static uint32_t W_seq;

void nd_wol_init(void)
{
    for (int i = 0; i < DEVOS_WOL_MAX; i++) nd_mutex_create(&W[i].mx);
}

static void wol_task(void *arg)
{
    int slot = (int)(intptr_t)arg;
    uint8_t mac[6];
    char addr[64];
    uint32_t id;
    nd_lock(&W[slot].mx);
    memcpy(mac, W[slot].mac, 6);
    snprintf(addr, sizeof(addr), "%s", W[slot].addr);
    id = W[slot].id;
    nd_unlock(&W[slot].mx);

    char target[48] = "", err[96] = "";
    int rc = devos_wol_send_ex(mac, addr, target, sizeof(target), err, sizeof(err));

    nd_lock(&W[slot].mx);
    if (W[slot].used && W[slot].id == id && !W[slot].stop) {
        W[slot].ok = rc == 0;
        snprintf(W[slot].target, sizeof(W[slot].target), "%s", target);
        snprintf(W[slot].error, sizeof(W[slot].error), "%s", err);
        W[slot].busy = false;
    }
    nd_unlock(&W[slot].mx);
}

int devos_wol_submit(const uint8_t mac[6], const char *addr)
{
    if (addr && strlen(addr) >= sizeof(W[0].addr)) return 0;   /* would truncate */
    int slot = -1;
    for (int i = 0; i < DEVOS_WOL_MAX; i++) {
        nd_lock(&W[i].mx);
        bool free_slot = !W[i].used;
        nd_unlock(&W[i].mx);
        if (free_slot) { slot = i; break; }
    }
    if (slot < 0) return 0;
    nd_lock(&W[slot].mx);
    W[slot].used = true;
    W[slot].busy = true;
    W[slot].stop = false;
    W[slot].ok = false;
    W[slot].target[0] = '\0';
    W[slot].error[0] = '\0';
    memcpy(W[slot].mac, mac, 6);
    snprintf(W[slot].addr, sizeof(W[slot].addr), "%s", addr ? addr : "");
    W[slot].id = ++W_seq;
    if (!W[slot].id) W[slot].id = ++W_seq;
    uint32_t id = W[slot].id;
    nd_unlock(&W[slot].mx);
    if (nd_spawn(wol_task, (void *)(intptr_t)slot, "jobswol", 4096) != 0) {
        nd_lock(&W[slot].mx);
        if (W[slot].used && W[slot].id == id) {
            W[slot].busy = false;
            snprintf(W[slot].error, sizeof(W[slot].error), "Couldn't start the send");
        }
        nd_unlock(&W[slot].mx);
        return 0;
    }
    return (int)id;
}

int devos_wol_poll(int ticket, bool *ok, char *target, size_t target_cap, char *error, size_t error_cap)
{
    if (ticket <= 0) return -1;
    for (int i = 0; i < DEVOS_WOL_MAX; i++) {
        nd_lock(&W[i].mx);
        if (W[i].used && W[i].id == (uint32_t)ticket) {
            bool busy = W[i].busy;
            if (!busy) {
                if (ok) *ok = W[i].ok;
                if (target && target_cap) snprintf(target, target_cap, "%s", W[i].target);
                if (error && error_cap) snprintf(error, error_cap, "%s", W[i].error);
            }
            nd_unlock(&W[i].mx);
            return busy ? 0 : 1;
        }
        nd_unlock(&W[i].mx);
    }
    return -1;
}

void devos_wol_cancel(int ticket)
{
    for (int i = 0; i < DEVOS_WOL_MAX; i++) {
        nd_lock(&W[i].mx);
        if (W[i].used && W[i].id == (uint32_t)ticket) {
            W[i].stop = true;
            if (W[i].busy) {
                /* Finish it here: the worker skips a stopped ticket, so without
                 * this poll() would return "running" forever. The magic packet
                 * may still have gone out, so `ok` stays false and the error
                 * says the outcome is unknown. */
                W[i].busy = false;
                W[i].ok = false;
                W[i].target[0] = '\0';
                snprintf(W[i].error, sizeof(W[i].error),
                         "cancelled; the packet may still have been sent");
            }
            nd_unlock(&W[i].mx);
            return;
        }
        nd_unlock(&W[i].mx);
    }
}

void devos_wol_release(int ticket)
{
    for (int i = 0; i < DEVOS_WOL_MAX; i++) {
        nd_lock(&W[i].mx);
        if (W[i].used && W[i].id == (uint32_t)ticket) { W[i].used = false; nd_unlock(&W[i].mx); return; }
        nd_unlock(&W[i].mx);
    }
}

const char *devos_wol_error(void) { return s_err; }
const char *devos_wol_last_target(void) { return s_target; }
