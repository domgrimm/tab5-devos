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
#include "esp_wifi_default.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
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

#ifdef ESP_PLATFORM
/* =========================================================================
 * Real Wi-Fi station. The ESP32-P4 has no radio: esp_wifi_remote transparently
 * proxies these standard esp_wifi_* calls to the ESP32-C6 over ESP-Hosted
 * (SDIO). Credentials are persisted in NVS so the device auto-reconnects.
 * ========================================================================= */
#define DEVOS_WIFI_CONNECTED_BIT  BIT0
#define DEVOS_WIFI_FAIL_BIT       BIT1
#define DEVOS_WIFI_MAX_RETRY      5
#define DEVOS_WIFI_NVS_NS         "wifi"

static esp_netif_t       *s_sta_netif = NULL;
static EventGroupHandle_t s_wifi_events = NULL;
static int                s_retry_num = 0;
static bool               s_wifi_started = false;

static void devos_wifi_save_creds(const char *ssid, const char *password)
{
    nvs_handle_t h;
    if (nvs_open(DEVOS_WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid ? ssid : "");
    nvs_set_str(h, "pass", password ? password : "");
    nvs_commit(h);
    nvs_close(h);
}

static bool devos_wifi_load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(DEVOS_WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_str(h, "ssid", ssid, &ssid_len) == ESP_OK) && ssid[0] != '\0';
    if (ok && nvs_get_str(h, "pass", pass, &pass_len) != ESP_OK) {
        pass[0] = '\0';
    }
    nvs_close(h);
    return ok;
}

static void devos_wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_status.connected = false;
        s_wifi_status.ip[0] = '\0';
        if (s_retry_num < DEVOS_WIFI_MAX_RETRY) {
            s_retry_num++;
            esp_wifi_connect();
            ESP_LOGW(TAG, "Wi-Fi disconnected; retry %d/%d", s_retry_num, DEVOS_WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_events, DEVOS_WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Wi-Fi connect failed after %d retries", DEVOS_WIFI_MAX_RETRY);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&e->ip_info.ip, s_wifi_status.ip, sizeof(s_wifi_status.ip));
        esp_ip4addr_ntoa(&e->ip_info.gw, s_wifi_status.gateway, sizeof(s_wifi_status.gateway));
        esp_ip4addr_ntoa(&e->ip_info.netmask, s_wifi_status.netmask, sizeof(s_wifi_status.netmask));
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, s_wifi_status.dns, sizeof(s_wifi_status.dns));
        }
        s_wifi_status.connected = true;
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_events, DEVOS_WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi got IP: %s", s_wifi_status.ip);
    }
}

int devos_net_init(void)
{
    memset(&s_wifi_status, 0, sizeof(s_wifi_status));

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return -1;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return -1;
    }

    if (!s_wifi_events) s_wifi_events = xEventGroupCreate();
    if (!s_sta_netif)   s_sta_netif = esp_netif_create_default_wifi_sta();

    /* esp_wifi_init brings up the ESP-Hosted transport to the C6. If the C6 is
     * not yet flashed with the ESP-Hosted slave firmware this will fail; we log
     * and continue (non-fatal) so the rest of the system still boots. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s (is the ESP32-C6 ESP-Hosted slave flashed?)",
                 esp_err_to_name(err));
        return -1;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        devos_wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        devos_wifi_event_handler, NULL, NULL);

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return -1;
    }
    s_wifi_started = true;
    ESP_LOGI(TAG, "Wi-Fi station started (radio on ESP32-C6 via ESP-Hosted)");

    /* Auto-connect to saved credentials, if any. */
    char ssid[33] = {0}, pass[65] = {0};
    if (devos_wifi_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "Connecting to saved SSID '%s'", ssid);
        devos_net_wifi_connect(ssid, pass);
    }
    return 0;
}

int devos_net_wifi_connect(const char *ssid, const char *password)
{
    if (!ssid || !s_wifi_started) return -1;

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    if (password) strncpy((char *)wc.sta.password, password, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) return -1;

    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_events, DEVOS_WIFI_CONNECTED_BIT | DEVOS_WIFI_FAIL_BIT);
    esp_wifi_disconnect();
    if (esp_wifi_connect() != ESP_OK) return -1;

    strncpy(s_wifi_status.ssid, ssid, sizeof(s_wifi_status.ssid) - 1);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           DEVOS_WIFI_CONNECTED_BIT | DEVOS_WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & DEVOS_WIFI_CONNECTED_BIT) {
        devos_wifi_save_creds(ssid, password);
        return 0;
    }
    return -1;
}

int devos_net_wifi_disconnect(void)
{
    if (s_wifi_started) {
        s_retry_num = DEVOS_WIFI_MAX_RETRY; /* stop auto-retry loop */
        esp_wifi_disconnect();
    }
    s_wifi_status.connected = false;
    s_wifi_status.ssid[0] = '\0';
    s_wifi_status.ip[0] = '\0';
    return 0;
}

int devos_net_wifi_get_status(devos_wifi_status_t *out_status)
{
    if (!out_status) return -1;
    if (s_wifi_status.connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_wifi_status.rssi = ap.rssi;
            strncpy(s_wifi_status.ssid, (const char *)ap.ssid, sizeof(s_wifi_status.ssid) - 1);
        }
    }
    memcpy(out_status, &s_wifi_status, sizeof(devos_wifi_status_t));
    return 0;
}

int devos_net_wifi_scan(devos_wifi_ap_t *out_aps, int max_aps, int *out_count)
{
    if (!out_aps || max_aps <= 0 || !out_count) return -1;
    *out_count = 0;
    if (!s_wifi_started) return -1;

    if (esp_wifi_scan_start(NULL, true) != ESP_OK) return -1;

    uint16_t num = (uint16_t)max_aps;
    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (!recs) { esp_wifi_scan_stop(); return -1; }

    if (esp_wifi_scan_get_ap_records(&num, recs) != ESP_OK) {
        free(recs);
        return -1;
    }

    int count = (num > (uint16_t)max_aps) ? max_aps : (int)num;
    for (int i = 0; i < count; i++) {
        memset(&out_aps[i], 0, sizeof(out_aps[i]));
        strncpy(out_aps[i].ssid, (const char *)recs[i].ssid, sizeof(out_aps[i].ssid) - 1);
        out_aps[i].rssi = recs[i].rssi;
        out_aps[i].authmode = (uint8_t)recs[i].authmode;
    }
    free(recs);
    *out_count = count;
    return 0;
}

#else  /* !ESP_PLATFORM — simulator keeps a canned Wi-Fi so the UI is testable */

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

#endif /* ESP_PLATFORM */

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
