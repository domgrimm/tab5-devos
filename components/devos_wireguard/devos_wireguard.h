#pragma once

/* devos_wireguard: a plain WireGuard tunnel from a wg-quick config.
 *
 * Tunnels (up to DEVOS_WG_MAX_TUNNELS) are kept in NVS: they hold private
 * keys, so an imported .conf file isn't needed on the SD card afterwards.
 * One tunnel runs at a time, and not together with Tailscale (both want
 * UDP 51820, the routes and scarce internal RAM).
 *
 * The tunnel is vendored wireguard-lwip in its normal mode: its own UDP
 * socket bound to the Wi-Fi netif, everything on the lwIP thread. Routing:
 * the Address subnet reaches the tunnel directly; for AllowedIPs outside it
 * (including 0.0.0.0/0) devos_net binds app connections to our tunnel
 * address (devos_net_set_route_hook). DNS lookups still use Wi-Fi's DNS.
 *
 * The simulator parses and stores tunnels but only pretends to connect.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_WG_MAX_TUNNELS   4
#define DEVOS_WG_NAME_MAX      32
#define DEVOS_WG_CONF_MAX      2048
#define DEVOS_WG_MAX_PEERS     4
#define DEVOS_WG_MAX_ALLOWED   4

typedef struct {
    uint32_t ip, mask;                  /* network byte order */
} devos_wg_cidr_t;

typedef struct {
    char public_key[48];                /* base64 */
    char preshared_key[48];             /* base64 or "" */
    char endpoint_host[96];
    int endpoint_port;
    devos_wg_cidr_t allowed[DEVOS_WG_MAX_ALLOWED];
    int allowed_n;
    bool allowed_ipv6_skipped;
    int keepalive_s;                    /* 0 = off */
} devos_wg_peer_t;

typedef struct {
    char private_key[48];               /* base64 */
    devos_wg_cidr_t address;            /* first IPv4 Address */
    uint32_t dns;                       /* first IPv4 DNS (0 = none) */
    int mtu;                            /* 0 = default 1420 */
    int listen_port;                    /* 0 = any */
    devos_wg_peer_t peers[DEVOS_WG_MAX_PEERS];
    int peer_n;
    bool full_tunnel;                   /* some peer has 0.0.0.0/0 */
} devos_wg_config_t;

typedef enum {
    DEVOS_WG_OFF = 0,
    DEVOS_WG_CONNECTING,                /* resolving the endpoint / handshaking */
    DEVOS_WG_UP,                        /* handshake done */
    DEVOS_WG_ERROR,                     /* stopped; see info.error */
} devos_wg_state_t;

typedef struct {
    devos_wg_state_t state;
    int active;                         /* tunnel index, -1 = none */
    char name[DEVOS_WG_NAME_MAX];
    char address[32];                   /* "10.8.0.2/24" */
    char endpoint[112];                 /* "vpn.example.com:51820 (203.0.113.5)" */
    char public_key[48];                /* ours, to add on the server */
    int handshake_age_s;                /* -1 = none yet */
    int last_rx_age_s;                  /* -1 = nothing received */
    bool full_tunnel;
    char error[160];
} devos_wg_info_t;

/* Parse a wg-quick config. 0 = ok; otherwise err says what's wrong. */
int devos_wg_parse(const char *text, devos_wg_config_t *out, char *err, size_t err_cap);

void devos_wg_init(void);

/* Saved tunnels. add() validates, stores and returns the new index (or -1
 * with err set). A tunnel with the same name is replaced. */
int devos_wg_count(void);
const char *devos_wg_name(int idx);
int devos_wg_add(const char *name, const char *conf_text, char *err, size_t err_cap);
int devos_wg_remove(int idx);
/* The parsed config of a saved tunnel (for display). */
int devos_wg_get_config(int idx, devos_wg_config_t *out);

int devos_wg_connect(int idx);          /* asynchronous; -1 with info.error set */
void devos_wg_disconnect(void);
bool devos_wg_active(void);             /* connecting or up */
void devos_wg_get_info(devos_wg_info_t *out);
uint32_t devos_wg_generation(void);     /* bumps on state changes */

#ifdef __cplusplus
}
#endif
