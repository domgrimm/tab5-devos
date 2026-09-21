#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
} devos_wifi_ap_t;

typedef struct {
    bool connected;
    char ssid[33];
    int8_t rssi;
    char ip[16];
    char gateway[16];
    char netmask[16];
    char dns[16];
} devos_wifi_status_t;

/* Wi-Fi Management */
int devos_net_init(void);
int devos_net_wifi_connect(const char *ssid, const char *password);
int devos_net_wifi_disconnect(void);
int devos_net_wifi_get_status(devos_wifi_status_t *out_status);
int devos_net_wifi_scan(devos_wifi_ap_t *out_aps, int max_aps, int *out_count);

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

/* Non-blocking variant: start returns an in-progress fd (or -1); poll wait
 * until it returns 0 (connected) or -1 (failed). 1 means keep polling. */
int devos_net_socket_connect_start(const char *host, int port);
int devos_net_socket_connect_wait(int sock, int timeout_ms);

/* One-shot HTTP/1.1 GET (blocking, bounded by timeout_ms). Returns 0 with
 * status + body shifted to resp[0], -1 on transport error. UI actions only. */
int devos_net_http_get(const char *host, int port, const char *path,
                       char *resp, size_t cap, int timeout_ms,
                       int *status_out);

#ifdef __cplusplus
}
#endif
