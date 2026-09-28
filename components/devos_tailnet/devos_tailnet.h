#pragma once
/* devos_tailnet: the Tab5's Tailscale client.
 *
 * On the device this drives MicroLink (components/microlink: ts2021 control
 * plane, WireGuard, DERP relays, DISCO/STUN NAT traversal). MicroLink adds a
 * WireGuard lwIP netif for 100.64.0.0/10, so ordinary sockets (SSH, HTTP...)
 * to tailnet IPs go through the tunnel with no special API.
 *
 * All calls are non-blocking and safe from the UI thread: connect/disconnect/
 * forget are handed to a background task on the network core. In the
 * simulator the host's own `tailscale status` is shown instead.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_TS_MAX_PEERS      32
#define DEVOS_TS_KEY_MAX        128
#define DEVOS_TS_HOSTNAME_MAX   40

typedef enum {
    DEVOS_TS_OFF = 0,           /* not running (user disconnected / never started) */
    DEVOS_TS_NEEDS_KEY,         /* never registered and no auth key saved */
    DEVOS_TS_WAIT_WIFI,         /* wants to connect, waiting for Wi-Fi */
    DEVOS_TS_CONNECTING,        /* STUN + control-plane connection */
    DEVOS_TS_REGISTERING,       /* Noise handshake, registration, netmap */
    DEVOS_TS_CONNECTED,
    DEVOS_TS_RECONNECTING,      /* control plane lost, retrying */
    DEVOS_TS_ERROR,             /* stopped: see info.last_error */
} devos_ts_state_t;

typedef struct {
    devos_ts_state_t state;
    char hostname[DEVOS_TS_HOSTNAME_MAX];   /* our device name on the tailnet */
    char ip[16];                            /* our 100.x address ("" if none) */
    char domain[64];                        /* MagicDNS suffix, e.g. tail1234.ts.net */
    char derp[24];                          /* home relay, e.g. "syd (region 19)" */
    int  derp_ms;                           /* its measured round trip (STUN), 0 = not measured */
    bool derp_connected;
    int  peer_count;
    int  peers_direct;                      /* peers reached without a relay */
    int64_t key_expiry;                     /* node key expiry (unix s), 0 = never */
    bool key_expired;
    bool registered;                        /* this device has node keys */
    bool has_auth_key;
    bool auto_connect;
    char last_error[160];
    char login_url[160];                    /* control plane asked for a login */
} devos_ts_info_t;

typedef struct {
    char name[64];              /* short name ("box") */
    char fqdn[96];              /* MagicDNS name ("box.tail1234.ts.net") */
    char ip[16];
    bool active;                /* in our WireGuard peer table */
    bool direct;                /* direct UDP path (vs DERP relay) */
    int  last_seen_s;           /* since last DISCO pong, -1 = never */
    int  ping_ms;               /* last ICMP ping: >=0 ms, -1 none, -2 timeout, -3 running */
} devos_ts_peer_t;

/* Load settings (NVS), consume /sdcard/ts_key if present, start the control
 * task and auto-connect when enabled. Call once after devos_net_init(). */
void devos_tailnet_init(void);

/* Start / stop (asynchronous; persist auto_connect). */
int  devos_tailnet_connect(void);
int  devos_tailnet_disconnect(void);
/* Log this device out: stop, erase node keys + cached peers + auth key. */
int  devos_tailnet_forget(void);

/* Auth key (tskey-auth-...). Saved in NVS; used for the next registration. */
int  devos_tailnet_set_auth_key(const char *key);
bool devos_tailnet_has_auth_key(void);
/* Device name on the tailnet (applies on the next connect). */
int  devos_tailnet_set_hostname(const char *name);
void devos_tailnet_get_hostname(char *out, size_t len);

void devos_tailnet_get_info(devos_ts_info_t *out);
int  devos_tailnet_peer_count(void);
int  devos_tailnet_get_peer(int index, devos_ts_peer_t *out);
/* Bumped whenever state or the peer list changes (cheap UI refresh check). */
uint32_t devos_tailnet_generation(void);

/* ICMP echo to a peer through the tunnel; result lands in peer.ping_ms. */
int  devos_tailnet_ping(int peer_index);
/* Call about once a second from the UI task: saves the peer cache to flash
 * once it has settled (flash writes stay on the UI core). */
void devos_tailnet_housekeeping(void);

/* MagicDNS: "box" or "box.tail1234.ts.net" -> "100.x.y.z" (connected only). */
int  devos_tailnet_resolve(const char *name, char *out_ip, size_t out_len);

const char *devos_tailnet_state_text(devos_ts_state_t state);

#ifdef __cplusplus
}
#endif
