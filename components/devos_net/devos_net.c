#include "devos_net.h"
#include "microlink.h"
#include "devos_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>

#ifdef ESP_PLATFORM
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
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
#define TAG "devos_net"
#endif

static devos_wifi_status_t s_wifi_status;

int devos_net_init(void)
{
#ifdef ESP_PLATFORM
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
    }
#endif

    memset(&s_wifi_status, 0, sizeof(s_wifi_status));
    s_wifi_status.connected = true;
    strncpy(s_wifi_status.ssid, "DevNet", sizeof(s_wifi_status.ssid));
    s_wifi_status.rssi = -58;
    strncpy(s_wifi_status.ip, "192.168.1.150", sizeof(s_wifi_status.ip));
    strncpy(s_wifi_status.gateway, "192.168.1.1", sizeof(s_wifi_status.gateway));
    strncpy(s_wifi_status.netmask, "255.255.255.0", sizeof(s_wifi_status.netmask));
    strncpy(s_wifi_status.dns, "192.168.1.1", sizeof(s_wifi_status.dns));

    return 0;
}

int devos_net_wifi_connect(const char *ssid, const char *password)
{
    (void)password;
    if (!ssid) return -1;

    strncpy(s_wifi_status.ssid, ssid, sizeof(s_wifi_status.ssid) - 1);
    s_wifi_status.connected = true;
    s_wifi_status.rssi = -60;
    strncpy(s_wifi_status.ip, "192.168.1.150", sizeof(s_wifi_status.ip));
    return 0;
}

int devos_net_wifi_disconnect(void)
{
    s_wifi_status.connected = false;
    s_wifi_status.ssid[0] = '\0';
    s_wifi_status.ip[0] = '\0';
    return 0;
}

int devos_net_wifi_get_status(devos_wifi_status_t *out_status)
{
    if (!out_status) return -1;
    memcpy(out_status, &s_wifi_status, sizeof(devos_wifi_status_t));
    return 0;
}

int devos_net_wifi_scan(devos_wifi_ap_t *out_aps, int max_aps, int *out_count)
{
    if (!out_aps || max_aps <= 0 || !out_count) return -1;

    /* Simulated AP scan list */
    const devos_wifi_ap_t default_aps[] = {
        {"DevNet", -58, 3},
        {"Workplace-5G", -64, 4},
        {"M5_Hotspot", -72, 3},
        {"Guest-IoT", -80, 2}
    };

    int count = sizeof(default_aps) / sizeof(default_aps[0]);
    if (count > max_aps) count = max_aps;

    memcpy(out_aps, default_aps, count * sizeof(devos_wifi_ap_t));
    *out_count = count;
    return 0;
}

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

int devos_net_resolve(const char *hostname, char *out_ip, size_t out_len)
{
    if (!hostname || !out_ip || out_len < 16) return -1;

    /* 1. If already an IP address, copy directly */
    struct in_addr addr;
    if (inet_aton(hostname, &addr) != 0) {
        strncpy(out_ip, hostname, out_len - 1);
        out_ip[out_len - 1] = '\0';
        return 0;
    }

    /* 2. MagicDNS lookup via MicroLink peer table */
    microlink_status_t ml_status;
    if (microlink_get_status(&ml_status) == 0) {
        /* Check local node */
        if (strcasecmp(hostname, "devos") == 0 ||
            strcasecmp(hostname, ml_status.node_name) == 0 ||
            strcasecmp(hostname, "devos.tailnet") == 0) {
            strncpy(out_ip, ml_status.assigned_ip, out_len - 1);
            out_ip[out_len - 1] = '\0';
            return 0;
        }

        /* Check peers */
        for (int i = 0; i < ml_status.peer_count; i++) {
            if (strcasecmp(hostname, ml_status.peers[i].name) == 0 ||
                strcasecmp(hostname, ml_status.peers[i].fqdn) == 0) {
                strncpy(out_ip, ml_status.peers[i].ip, out_len - 1);
                out_ip[out_len - 1] = '\0';
                return 0;
            }
        }
    }

    /* 3. Fall back to standard DNS resolution */
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

int devos_net_socket_connect(const char *host, int port, int timeout_ms)
{
    if (!host || port <= 0 || port > 65535) return -1;

    char resolved_ip[MICROLINK_MAX_IP_LEN];
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

    char resolved_ip[MICROLINK_MAX_IP_LEN];
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
