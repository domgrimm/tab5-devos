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

const char *devos_wol_error(void) { return s_err; }
const char *devos_wol_last_target(void) { return s_target; }
