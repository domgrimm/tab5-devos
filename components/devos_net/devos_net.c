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
#define TAG "devos_net"
#endif

static devos_wifi_status_t s_wifi_status;

int devos_net_init(void)
{
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
