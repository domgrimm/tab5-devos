/* devos_ota: see devos_ota.h. */
#include "devos_ota.h"
#include "devos_json.h"
#include "devos_config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DEVOS_OTA_DEFAULT_FEED "https://domgrimm.github.io/tab5-devos/ota/devos-manifest.json"
#define MANIFEST_MAX 4096

static char s_feed[DEVOS_OTA_FEED_MAX] = DEVOS_OTA_DEFAULT_FEED;
static char s_report[200] = "Not checked yet";
static char s_ver[32] = "";
static char s_url[256] = "";
static char s_sha[65] = "";
static char s_notes[200] = "";
static char s_build[65] = "";               /* the manifest's "build": its image's ELF SHA-256 */
static long s_size = 0;
static volatile devos_ota_state_t s_state = DEVOS_OTA_IDLE;
static volatile int s_progress = -1;

static const char *own_build(void);

long devos_ota_parse_version(const char *s)
{
    if (!s) return -1;
    while (*s && !isdigit((unsigned char)*s)) s++;
    int a = 0, b = 0, c = 0;
    if (sscanf(s, "%d.%d.%d", &a, &b, &c) < 2) return -1;
    if (a < 0 || b < 0 || c < 0 || a > 255 || b > 255 || c > 255) return -1;
    return ((long)a << 16) | ((long)b << 8) | (long)c;
}

/* Resolve a manifest "url" that may be relative to the feed URL. */
static void resolve_url(const char *feed, const char *ref, char *out, size_t len)
{
    if (strstr(ref, "://")) { snprintf(out, len, "%s", ref); return; }
    const char *scheme_end = strstr(feed, "://");
    const char *host_start = scheme_end ? scheme_end + 3 : feed;
    const char *path = strchr(host_start, '/');
    if (ref[0] == '/') {                            /* host-relative */
        int origin = path ? (int)(path - feed) : (int)strlen(feed);
        snprintf(out, len, "%.*s%s", origin, feed, ref);
        return;
    }
    const char *last = strrchr(feed, '/');          /* directory-relative */
    int dir = (last && last >= host_start) ? (int)(last - feed + 1) : (int)strlen(feed);
    snprintf(out, len, "%.*s%s%s", dir, feed, (last && last >= host_start) ? "" : "/", ref);
}

/* Parse a fetched manifest and decide whether it is an update. */
static int handle_manifest(const char *json)
{
    size_t n = strlen(json);
    char ver[32] = "", url[200] = "";
    if (devos_json_get_str(json, n, "version", ver, sizeof(ver)) != 0 ||
        devos_json_get_str(json, n, "url", url, sizeof(url)) != 0 || !url[0]) {
        snprintf(s_report, sizeof(s_report), "The update feed's manifest is missing \"version\" or \"url\"");
        return -1;
    }
    long remote = devos_ota_parse_version(ver), local = devos_ota_parse_version(DEVOS_VERSION_STR);
    if (remote < 0) {
        snprintf(s_report, sizeof(s_report), "Manifest version \"%.20s\" is not x.y.z", ver);
        return -1;
    }
    snprintf(s_ver, sizeof(s_ver), "%s", ver);
    resolve_url(s_feed, url, s_url, sizeof(s_url));
    s_sha[0] = s_notes[0] = '\0';
    devos_json_get_str(json, n, "sha256", s_sha, sizeof(s_sha));
    devos_json_get_str(json, n, "notes", s_notes, sizeof(s_notes));
    int sz = 0;
    s_size = (devos_json_get_int(json, n, "size", &sz) == 0 && sz > 0) ? sz : 0;
    /* The build id tells builds apart even when the version wasn't bumped. */
    s_build[0] = '\0';
    devos_json_get_str(json, n, "build", s_build, sizeof(s_build));
    const char *mine = own_build();
    size_t bl = strlen(s_build);
    bool known = bl >= 8 && mine[0];
    if (known && strncasecmp(s_build, mine, bl) == 0) {
        snprintf(s_report, sizeof(s_report), "Up to date: this build (%.8s) is the one on the feed", mine);
        return 0;
    }
    if (remote > local) {
        snprintf(s_report, sizeof(s_report), "Update %.20s available (%ld KB)", ver, s_size / 1024);
        return 1;
    }
    if (remote == local && known) {
        snprintf(s_report, sizeof(s_report), "New build of %.20s available (%.8s, %ld KB)", ver, s_build,
                 s_size / 1024);
        return 1;
    }
    snprintf(s_report, sizeof(s_report), "Up to date (%s; feed has %.20s)", DEVOS_VERSION_STR, ver);
    return 0;
}

#ifdef ESP_PLATFORM
/* ========================================================================
 * Device
 * ======================================================================== */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

static const char *TAG = "ota";

/* The ELF SHA-256 esptool stamps into every image: unique per build. */
static const char *own_build(void)
{
    static char hex[65];
    if (!hex[0]) {
        const uint8_t *d = esp_app_get_description()->app_elf_sha256;
        for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", d[i]);
    }
    return hex;
}

const char *devos_ota_build_text(void)
{
    static char t[64];
    if (!t[0]) {
        const esp_app_desc_t *d = esp_app_get_description();
        snprintf(t, sizeof(t), "build %.8s, %.12s %.5s", own_build(), d->date, d->time);
    }
    return t;
}

static void persist(void)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "feed", s_feed);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_feed);
        if (nvs_get_str(h, "feed", s_feed, &len) != ESP_OK) s_feed[0] = '\0';
        nvs_close(h);
    }
    if (!s_feed[0]) snprintf(s_feed, sizeof(s_feed), "%s", DEVOS_OTA_DEFAULT_FEED);
}

/* GET with redirects; returns an open client positioned at the body. */
static esp_http_client_handle_t http_open(const char *url, int64_t *content_len, char *err, size_t errlen)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { snprintf(err, errlen, "Out of memory"); return NULL; }
    for (int hop = 0; hop < 5; hop++) {
        if (esp_http_client_open(c, 0) != ESP_OK) {
            snprintf(err, errlen, "Could not connect to %.120s", url);
            break;
        }
        int64_t len = esp_http_client_fetch_headers(c);
        int st = esp_http_client_get_status_code(c);
        if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
            esp_http_client_set_redirection(c);
            esp_http_client_close(c);
            continue;
        }
        if (st != 200) {
            snprintf(err, errlen, "HTTP %d from %.120s", st, url);
            esp_http_client_close(c);
            break;
        }
        *content_len = len;
        return c;
    }
    esp_http_client_cleanup(c);
    return NULL;
}

static void check_worker(void)
{
    char err[160] = "";
    int64_t len = 0;
    esp_http_client_handle_t c = http_open(s_feed, &len, err, sizeof(err));
    if (!c) {
        snprintf(s_report, sizeof(s_report), "Update feed unreachable: %s", err);
        s_state = DEVOS_OTA_FAILED;
        return;
    }
    char *buf = calloc(1, MANIFEST_MAX);
    int total = 0;
    while (buf && total < MANIFEST_MAX - 1) {
        int r = esp_http_client_read(c, buf + total, MANIFEST_MAX - 1 - total);
        if (r <= 0) break;
        total += r;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!buf || total == 0) {
        free(buf);
        snprintf(s_report, sizeof(s_report), "Update feed returned nothing");
        s_state = DEVOS_OTA_FAILED;
        return;
    }
    int rc = handle_manifest(buf);
    free(buf);
    s_state = rc > 0 ? DEVOS_OTA_AVAILABLE : rc == 0 ? DEVOS_OTA_UP_TO_DATE : DEVOS_OTA_FAILED;
}

static void hex32(const unsigned char *d, char *out)
{
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
    out[64] = '\0';
}

static void install_worker(void)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        snprintf(s_report, sizeof(s_report), "No OTA partition to write to (check partitions.csv)");
        s_state = DEVOS_OTA_FAILED;
        return;
    }
    char err[160] = "";
    int64_t len = 0;
    esp_http_client_handle_t c = http_open(s_url, &len, err, sizeof(err));
    if (!c) {
        snprintf(s_report, sizeof(s_report), "Download failed: %s", err);
        s_state = DEVOS_OTA_FAILED;
        return;
    }
    long expect = s_size > 0 ? s_size : (long)len;
    if (expect > (long)target->size) {
        snprintf(s_report, sizeof(s_report), "Image (%ld KB) is larger than the OTA slot (%lu KB)",
                 expect / 1024, (unsigned long)(target->size / 1024));
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        s_state = DEVOS_OTA_FAILED;
        return;
    }

    esp_ota_handle_t ota = 0;
    /* Erase as we go (not the whole image up front) so the UI keeps running. */
    esp_err_t e = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (e != ESP_OK) {
        snprintf(s_report, sizeof(s_report), "Could not prepare %s: %s", target->label, esp_err_to_name(e));
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        s_state = DEVOS_OTA_FAILED;
        return;
    }
    ESP_LOGI(TAG, "writing %s (%ld bytes) to %s", s_url, expect, target->label);

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    char *buf = malloc(4096);
    long got = 0;
    bool ok = buf != NULL;
    while (ok) {
        int r = esp_http_client_read(c, buf, 4096);
        if (r < 0) { snprintf(s_report, sizeof(s_report), "Download interrupted"); ok = false; break; }
        if (r == 0) {
            if (esp_http_client_is_complete_data_received(c) || (expect > 0 && got >= expect)) break;
            snprintf(s_report, sizeof(s_report), "Download ended early (%ld of %ld KB)", got / 1024, expect / 1024);
            ok = false;
            break;
        }
        mbedtls_sha256_update(&sha, (const unsigned char *)buf, (size_t)r);
        e = esp_ota_write(ota, buf, (size_t)r);
        if (e != ESP_OK) {
            snprintf(s_report, sizeof(s_report), "Flash write failed: %s", esp_err_to_name(e));
            ok = false;
            break;
        }
        got += r;
        if (expect > 0) {
            s_progress = (int)((got * 100) / expect);
            snprintf(s_report, sizeof(s_report), "Downloading %.20s: %d%% (%ld of %ld KB)", s_ver, s_progress,
                     got / 1024, expect / 1024);
        } else {
            snprintf(s_report, sizeof(s_report), "Downloading %.20s: %ld KB", s_ver, got / 1024);
        }
    }
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (ok && expect > 0 && got != expect) {
        snprintf(s_report, sizeof(s_report), "Size mismatch: got %ld bytes, manifest says %ld", got, expect);
        ok = false;
    }
    if (ok) {
        s_state = DEVOS_OTA_VERIFYING;
        char hex[65];
        hex32(digest, hex);
        if (s_sha[0] && strcasecmp(hex, s_sha) != 0) {
            snprintf(s_report, sizeof(s_report), "Checksum mismatch: the download is not the published image");
            ok = false;
        }
    }
    if (!ok) {
        esp_ota_abort(ota);
        s_state = DEVOS_OTA_FAILED;
        s_progress = -1;
        return;
    }
    e = esp_ota_end(ota);                           /* validates the image */
    if (e == ESP_OK) e = esp_ota_set_boot_partition(target);
    if (e != ESP_OK) {
        snprintf(s_report, sizeof(s_report), "Image rejected: %s", esp_err_to_name(e));
        s_state = DEVOS_OTA_FAILED;
        s_progress = -1;
        return;
    }
    /* After the restart devos_ota_init() says whether it took (or was rolled back). */
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "want_part", target->label);
        nvs_set_str(h, "want_ver", s_ver);
        nvs_commit(h);
        nvs_close(h);
    }
    snprintf(s_report, sizeof(s_report), "Installed %.20s. Restarting...", s_ver);
    s_state = DEVOS_OTA_REBOOTING;
    ESP_LOGI(TAG, "update installed to %s, restarting", target->label);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

typedef enum { JOB_CHECK, JOB_INSTALL } ota_job_t;

static void ota_task(void *arg)
{
    if ((ota_job_t)(intptr_t)arg == JOB_CHECK) check_worker();
    else install_worker();
    vTaskDelete(NULL);
}

static int start_job(ota_job_t job)
{
    /* Internal-RAM stack: esp_ota_write writes flash. */
    return xTaskCreatePinnedToCore(ota_task, "ota", 8192, (void *)(intptr_t)job, 4, NULL, DEVOS_CORE_NET_CRYPTO) ==
                   pdPASS
               ? 0
               : -1;
}

void devos_ota_mark_boot_ok(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed (%s)", run->label);
    }
}

void devos_ota_init(void)
{
    load();
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run) ESP_LOGI(TAG, "running from %s (%s), feed %s", run->label, devos_ota_build_text(), s_feed);

    /* Did the last install take? It restarted into want_part; the bootloader
     * rolls back to the old slot if the new image never confirmed itself. */
    nvs_handle_t h;
    char part[20] = "", ver[32] = "";
    size_t l1 = sizeof(part), l2 = sizeof(ver);
    if (!run || nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_get_str(h, "want_part", part, &l1) == ESP_OK) {
        if (nvs_get_str(h, "want_ver", ver, &l2) != ESP_OK) ver[0] = '\0';
        if (strcmp(part, run->label) == 0) {
            snprintf(s_report, sizeof(s_report), "Updated to %.20s (%s)", ver, devos_ota_build_text());
        } else {
            snprintf(s_report, sizeof(s_report), "The update to %.20s didn't start, so the Tab5 went back to "
                     "the firmware before it (%s)", ver, run->label);
            s_state = DEVOS_OTA_FAILED;
        }
        ESP_LOGW(TAG, "%s", s_report);
        nvs_erase_key(h, "want_part");
        nvs_erase_key(h, "want_ver");
        nvs_commit(h);
    }
    nvs_close(h);
}

#else
/* ========================================================================
 * Simulator: check only (curl), installing is a dry run
 * ======================================================================== */
#include <pthread.h>

#define DEVOS_OTA_NVS_FILE TAB5_SD_MOUNT_POINT "/.devos/ota_nvs.json"

static void persist(void)
{
    FILE *f = fopen(DEVOS_OTA_NVS_FILE, "w");
    if (f) {
        fprintf(f, "{\n  \"feed\": \"%s\"\n}\n", s_feed);
        fclose(f);
    }
}

static void load(void)
{
    FILE *f = fopen(DEVOS_OTA_NVS_FILE, "r");
    if (f) {
        char buf[256], val[DEVOS_OTA_FEED_MAX];
        while (fgets(buf, sizeof(buf), f)) {
            if (sscanf(buf, " \"feed\": \"%159[^\"]\"", val) == 1) snprintf(s_feed, sizeof(s_feed), "%s", val);
        }
        fclose(f);
    }
    if (!s_feed[0]) snprintf(s_feed, sizeof(s_feed), "%s", DEVOS_OTA_DEFAULT_FEED);
}

static void *sim_check(void *arg)
{
    (void)arg;
    char cmd[DEVOS_OTA_FEED_MAX + 64];
    snprintf(cmd, sizeof(cmd), "curl -fsSL --max-time 6 '%s' 2>/dev/null", s_feed);
    static char buf[MANIFEST_MAX];
    size_t total = 0;
    FILE *p = popen(cmd, "r");
    if (p) {
        total = fread(buf, 1, sizeof(buf) - 1, p);
        pclose(p);
    }
    buf[total] = '\0';
    if (total == 0) {
        snprintf(s_report, sizeof(s_report), "Update feed unreachable: %.120s", s_feed);
        s_state = DEVOS_OTA_FAILED;
        return NULL;
    }
    int rc = handle_manifest(buf);
    s_state = rc > 0 ? DEVOS_OTA_AVAILABLE : rc == 0 ? DEVOS_OTA_UP_TO_DATE : DEVOS_OTA_FAILED;
    return NULL;
}

static int start_job(int install)
{
    if (install) {
        snprintf(s_report, sizeof(s_report), "Simulator dry run: would install %.20s from %.100s", s_ver, s_url);
        s_state = DEVOS_OTA_AVAILABLE;
        return 0;
    }
    pthread_t th;
    if (pthread_create(&th, NULL, sim_check, NULL) != 0) return -1;
    pthread_detach(th);
    return 0;
}

#define JOB_CHECK   0
#define JOB_INSTALL 1

void devos_ota_mark_boot_ok(void) {}

/* Tests can pretend to be a build (DEVOS_SIM_BUILD); otherwise versions decide. */
static const char *own_build(void)
{
    const char *b = getenv("DEVOS_SIM_BUILD");
    return b ? b : "";
}

const char *devos_ota_build_text(void)
{
    return "simulator build, " __DATE__;
}

void devos_ota_init(void)
{
    load();
}
#endif

bool devos_ota_busy(void)
{
    return s_state == DEVOS_OTA_CHECKING || s_state == DEVOS_OTA_DOWNLOADING || s_state == DEVOS_OTA_VERIFYING ||
           s_state == DEVOS_OTA_REBOOTING;
}

int devos_ota_check(void)
{
    if (devos_ota_busy()) return -1;
    if (!strstr(s_feed, "://")) {
        snprintf(s_report, sizeof(s_report), "Bad feed URL");
        s_state = DEVOS_OTA_FAILED;
        return -1;
    }
    s_state = DEVOS_OTA_CHECKING;
    snprintf(s_report, sizeof(s_report), "Checking %.150s ...", s_feed);
    if (start_job(JOB_CHECK) != 0) {
        snprintf(s_report, sizeof(s_report), "Could not start the update check");
        s_state = DEVOS_OTA_FAILED;
        return -1;
    }
    return 0;
}

int devos_ota_apply(void)
{
    if (s_state != DEVOS_OTA_AVAILABLE || !s_url[0]) return -1;
    s_state = DEVOS_OTA_DOWNLOADING;
    s_progress = 0;
    snprintf(s_report, sizeof(s_report), "Starting download of %.20s ...", s_ver);
    if (start_job(JOB_INSTALL) != 0) {
        snprintf(s_report, sizeof(s_report), "Could not start the installer");
        s_state = DEVOS_OTA_AVAILABLE;
        s_progress = -1;
        return -1;
    }
    return 0;
}

bool devos_ota_has_update(void) { return s_state == DEVOS_OTA_AVAILABLE; }
devos_ota_state_t devos_ota_state(void) { return s_state; }
int devos_ota_progress(void) { return s_state == DEVOS_OTA_DOWNLOADING ? s_progress : -1; }
const char *devos_ota_update_text(void) { return s_report; }
const char *devos_ota_available_version(void) { return s_state == DEVOS_OTA_AVAILABLE ? s_ver : ""; }
const char *devos_ota_notes(void) { return s_notes; }

void devos_ota_get_feed(char *out, size_t len)
{
    if (out && len) snprintf(out, len, "%s", s_feed);
}

int devos_ota_set_feed(const char *url)
{
    if (!url || !*url || strlen(url) >= sizeof(s_feed) || !strstr(url, "://")) return -1;
    snprintf(s_feed, sizeof(s_feed), "%s", url);
    persist();
    if (!devos_ota_busy()) {
        s_state = DEVOS_OTA_IDLE;
        snprintf(s_report, sizeof(s_report), "Not checked yet");
    }
    return 0;
}
