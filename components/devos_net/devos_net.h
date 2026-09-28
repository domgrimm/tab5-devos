#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_WIFI_MAX_SAVED 8    /* remembered networks (NVS) */
#define DEVOS_WIFI_MAX_SCAN  24   /* cached scan results (deduped, strongest first) */

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;             /* wifi_auth_mode_t; 0 = open */
} devos_wifi_ap_t;

typedef enum {
    DEVOS_WIFI_STATE_OFF = 0,     /* radio unavailable (disabled / C6 not responding) */
    DEVOS_WIFI_STATE_IDLE,        /* radio up, not connected, nothing in progress */
    DEVOS_WIFI_STATE_CONNECTING,
    DEVOS_WIFI_STATE_CONNECTED,   /* associated and has an IP */
    DEVOS_WIFI_STATE_FAILED,      /* last connect attempt failed; see last_error */
} devos_wifi_state_t;

typedef struct {
    devos_wifi_state_t state;
    bool connected;
    char ssid[33];                /* connected / connecting network */
    int8_t rssi;
    char bssid[18];               /* the access point, "aa:bb:cc:dd:ee:ff" ("" = none) */
    uint8_t channel;
    char ip[16];
    char gateway[16];
    char netmask[16];
    char dns[16];
    char last_error[64];          /* human-readable reason for the last failure */
} devos_wifi_status_t;

typedef struct {
    char ssid[33];
} devos_wifi_saved_t;

/* --- Wi-Fi management --------------------------------------------------------
 * All calls are non-blocking and safe from the GUI task: radio work runs on a
 * Core 0 worker. Poll devos_net_wifi_get_status() / the scan getters for
 * progress. */
int devos_net_init(void);
int devos_net_wifi_get_status(devos_wifi_status_t *out_status);
/* Refresh RSSI from the radio (an RPC to the C6): background tasks only. */
void devos_net_wifi_refresh_rssi(void);

int  devos_net_wifi_scan_start(void);
bool devos_net_wifi_scan_busy(void);
/* Copies cached results (strongest first); returns the count. */
int  devos_net_wifi_scan_results(devos_wifi_ap_t *out, int max);
/* Bumps each time new scan results land (lets the UI refresh only on change). */
uint32_t devos_net_wifi_scan_generation(void);

/* Every BSS (access point radio) the last scan heard, hidden ones included,
 * in the order the radio reported them - for the Wi-Fi site survey. The C6
 * is a 2.4 GHz radio: channels 1-14. */
#define DEVOS_WIFI_MAX_BSS 48
#define DEVOS_WIFI_PHY_B   0x01
#define DEVOS_WIFI_PHY_G   0x02
#define DEVOS_WIFI_PHY_N   0x04
#define DEVOS_WIFI_PHY_AX  0x08
typedef struct {
    char ssid[33];                /* "" = hidden network */
    uint8_t bssid[6];
    uint8_t channel;              /* primary channel */
    int8_t second;                /* 40 MHz: +1 channel above, -1 below, 0 = 20 MHz */
    int8_t rssi;
    uint8_t authmode;             /* wifi_auth_mode_t */
    uint8_t phy;                  /* DEVOS_WIFI_PHY_* bits */
} devos_wifi_bss_t;
int devos_net_wifi_bss_results(devos_wifi_bss_t *out, int max);

/* Connect to ssid. password == NULL uses the saved password (or none for open
 * networks). The network is remembered once it connects successfully. */
int  devos_net_wifi_connect(const char *ssid, const char *password);
int  devos_net_wifi_disconnect(void);

int  devos_net_wifi_saved_list(devos_wifi_saved_t *out, int max);
bool devos_net_wifi_is_saved(const char *ssid);
int  devos_net_wifi_forget(const char *ssid);

const char *devos_net_wifi_state_text(devos_wifi_state_t state);
/* 0..4 bars for an RSSI in dBm. */
int devos_net_wifi_signal_bars(int rssi);

/* Transparent Socket Transport & Virtual Routing */
bool devos_net_is_tailnet_target(const char *host_or_ip);
int devos_net_resolve(const char *hostname, char *out_ip, size_t out_len);

/* Virtual Socket Layer (Routes seamlessly between Wi-Fi and WireGuard) */
int devos_net_socket_connect(const char *host, int port, int timeout_ms);
int devos_net_socket_send(int sock, const void *data, size_t len);
int devos_net_socket_recv(int sock, void *buf, size_t max_len, int timeout_ms);
int devos_net_socket_close(int sock);
/* Blocking full-buffer send; NOSIGNAL where available so a server RST
 * returns -1 instead of SIGPIPE-killing the caller. */
int devos_net_socket_send_all(int sock, const void *data, size_t len);

/* Tunnel routing: lwIP has no route table, so a VPN (the WireGuard app)
 * registers a hook; for each connection, if it returns true the socket is
 * bound to *src_ip (network byte order) and lwIP sends it out of the netif
 * that owns that address. */
typedef bool (*devos_net_route_fn)(uint32_t dest_ip, uint32_t *src_ip);
void devos_net_set_route_hook(devos_net_route_fn fn);
/* Apply the VPN routing to a socket of your own (UDP, raw ICMP) before its
 * first send: binds it to the tunnel address if a VPN claims dest_ip
 * (network byte order). devos_net_socket_connect* already do this. */
void devos_net_socket_route(int sock, uint32_t dest_ip);

/* Non-blocking variant: start returns an in-progress fd (or -1); poll wait
 * until it returns 0 (connected) or -1 (failed). 1 means keep polling. */
int devos_net_socket_connect_start(const char *host, int port);
int devos_net_socket_connect_wait(int sock, int timeout_ms);

/* Servers: a TCP socket listening on every interface (Wi-Fi and any VPN
 * tunnel), or -1. accept waits up to wait_ms for a client: returns its fd
 * (with io_timeout_ms send / receive timeouts) and, if peer is given, its
 * address; -2 if nobody came in time, -1 on error. */
int devos_net_socket_listen(int port, int backlog);
int devos_net_socket_accept(int listen_sock, int wait_ms, int io_timeout_ms, char *peer, size_t peer_cap);

/* One-shot HTTP/1.1 GET (blocking, bounded by timeout_ms). Returns 0 with
 * status + body shifted to resp[0], -1 on transport error. UI actions only. */
int devos_net_http_get(const char *host, int port, const char *path,
                       char *resp, size_t cap, int timeout_ms,
                       int *status_out);

#ifdef __cplusplus
}
#endif
