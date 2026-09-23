/* devos_ota: see devos_ota.h. */
#include "devos_ota.h"
#include "devos_json.h"
#include "devos_net.h"
#include "devos_config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "nvs_flash.h"
#include "nvs.h"
#ifdef DEVOS_OTA_HTTPS
#include "esp_https_ota.h"
#endif
#endif

#ifndef ESP_PLATFORM
#define DEVOS_OTA_NVS_FILE TAB5_SD_MOUNT_POINT "/.devos/ota_nvs.json"
#endif

#define DEVOS_OTA_DEFAULT_FEED "http://10.2.132.54:8090/devos-manifest.json"

static char s_feed[DEVOS_OTA_FEED_MAX] = DEVOS_OTA_DEFAULT_FEED;
static char s_report[256] = "Never checked";
static char s_ver[64] = "";
static char s_url[DEVOS_OTA_FEED_MAX] = "";
static long s_size = 0;
static bool s_have_update = false;

static void persist(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "feed", s_feed);
        nvs_commit(h);
        nvs_close(h);
    }
#else
    FILE *f = fopen(DEVOS_OTA_NVS_FILE, "w");
    if (f) {
        fprintf(f, "{\n  \"feed\": \"%s\"\n}\n", s_feed);
        fclose(f);
    }
#endif
}

static void load(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_feed);
        nvs_get_str(h, "feed", s_feed, &len);
        nvs_close(h);
    }
#else
    FILE *f = fopen(DEVOS_OTA_NVS_FILE, "r");
    if (f) {
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) {
            char val[128];
            if (sscanf(buf, " \"feed\": \"%127[^\"]\"", val) == 1) {
                snprintf(s_feed, sizeof(s_feed), "%s", val);
            }
        }
        fclose(f);
    }
#endif
    if (!s_feed[0]) {
        snprintf(s_feed, sizeof(s_feed), "%s", DEVOS_OTA_DEFAULT_FEED);
    }
}

/* Split http://host[:port]/path (no TLS — LAN shelf only). */
static void str_copy(char *dst, const char *src, size_t dst_size)
{
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int split_url(const char *url, char *host, size_t hlen, int *port,
                      char *path, size_t plen)
{
    if (!url || !host || hlen < 2 || !path || plen < 2 || !port) return -1;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    size_t hlen_actual = slash ? (size_t)(slash - p) : strlen(p);
    char tmp[160];
    if (hlen_actual >= sizeof(tmp)) return -1;
    memcpy(tmp, p, hlen_actual);
    tmp[hlen_actual] = '\0';
    char *colon = strchr(tmp, ':');
    if (colon) {
        *colon = '\0';
        *port = atoi(colon + 1);
    } else {
        *port = 80;
    }
    str_copy(host, tmp, hlen);
    if (slash) {
        str_copy(path, slash, plen);
    } else {
        str_copy(path, "/", plen);
    }
    return (host[0] && *port > 0) ? 0 : -1;
}

void devos_ota_init(void)
{
    load();
}

int devos_ota_check(void)
{
    char host[96];
    int port = 0;
    char path[128];
    if (split_url(s_feed, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        snprintf(s_report, sizeof(s_report), "Bad feed URL");
        s_have_update = false;
        return -1;
    }
    static char resp[2048];
    int status = 0;
    if (devos_net_http_get(host, port, path, resp, sizeof(resp), 4000,
                           &status) != 0 ||
        status != 200) {
        snprintf(s_report, sizeof(s_report), "Feed unreachable (%s:%d)",
                 host, port);
        s_have_update = false;
        return -1;
    }
    char ver[64] = "";
    if (devos_json_get_str(resp, strlen(resp), "version", ver, sizeof(ver)) != 0) {
        snprintf(s_report, sizeof(s_report), "Bad manifest");
        s_have_update = false;
        return -1;
    }
    s_have_update = strcmp(ver, DEVOS_VERSION_STR) != 0;
    str_copy(s_ver, ver, sizeof(s_ver));
    devos_json_get_str(resp, strlen(resp), "url", s_url, sizeof(s_url));
    s_size = 0;
    {
        int sz = 0;
        if (devos_json_get_int(resp, strlen(resp), "size", &sz) == 0 && sz > 0) {
            s_size = sz;
        }
    }
    if (s_have_update) {
        snprintf(s_report, sizeof(s_report), "Update %s ready (%ld KB)", ver,
                 s_size / 1024);
    } else {
        snprintf(s_report, sizeof(s_report), "Up to date (%s)",
                 DEVOS_VERSION_STR);
    }
    return 0;
}

const char *devos_ota_update_text(void) { return s_report; }

bool devos_ota_has_update(void) { return s_have_update; }

int devos_ota_apply(void)
{
    if (!s_have_update || !s_url[0]) return -1;
#ifdef ESP_PLATFORM
#ifdef DEVOS_OTA_HTTPS
    /* ponytail: the real flash path; needs ota_0/ota_1 + esp_https_ota */
    esp_http_client_config_t cfg = {.url = s_url, .timeout_ms = 10000};
    esp_err_t err = esp_https_ota(&cfg);
    if (err == ESP_OK) {
        esp_restart();
    }
    return err == ESP_OK ? 0 : -1;
#else
    (void)0;
    return -1; /* IDF component not linked; wire esp_https_ota to enable */
#endif
#else
    snprintf(s_report, sizeof(s_report), "Sim dry-run: would flash %s",
             s_url);
    return 0;
#endif
}

void devos_ota_get_feed(char *out, size_t len)
{
    if (out && len) {
        snprintf(out, len, "%s", s_feed);
        out[len - 1] = '\0';
    }
}

int devos_ota_set_feed(const char *url)
{
    if (!url || !*url || strlen(url) >= sizeof(s_feed)) return -1;
    if (!strstr(url, "://")) return -1;
    snprintf(s_feed, sizeof(s_feed), "%s", url);
    persist();
    return 0;
}
