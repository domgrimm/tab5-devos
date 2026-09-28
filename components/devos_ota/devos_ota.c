/* devos_ota: see devos_ota.h. */
#include "devos_ota.h"
#include "devos_json.h"
#include "devos_http.h"
#include "devos_crypto.h"
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
static volatile size_t s_got;               /* bytes downloaded so far (devos_http counts) */

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

/* ========================================================================
 * Platform: storage, the OTA slot, restarting
 *
 * An update is downloaded whole into RAM (PSRAM on the Tab5), checked
 * (size, SHA-256, image header, build id), and only then written to the
 * flash slot in one pass with no network running. A dropped connection can
 * never leave a half-written slot, and the flash writes (which stall Wi-Fi)
 * don't overlap the download. Each stage is recorded, so a restart halfway
 * is reported at the next boot.
 * ======================================================================== */
typedef enum { JOB_CHECK, JOB_INSTALL } ota_job_t;

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "ota";
static const esp_partition_t *s_target;
static esp_ota_handle_t s_ota;

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

static void kv_set(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    if (val) nvs_set_str(h, key, val);
    else nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

static bool kv_get(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    out[0] = '\0';
    if (nvs_open("ota", NVS_READONLY, &h) != ESP_OK) return false;
    size_t l = cap;
    bool ok = nvs_get_str(h, key, out, &l) == ESP_OK;
    nvs_close(h);
    if (!ok) out[0] = '\0';
    return ok;
}

static void persist(void) { kv_set("feed", s_feed); }

static void load(void)
{
    if (!kv_get("feed", s_feed, sizeof(s_feed))) s_feed[0] = '\0';
    if (!s_feed[0]) snprintf(s_feed, sizeof(s_feed), "%s", DEVOS_OTA_DEFAULT_FEED);
}

/* The largest image the slot takes (0: no slot). */
static size_t slot_size(void)
{
    s_target = esp_ota_get_next_update_partition(NULL);
    return s_target ? s_target->size : 0;
}

static int slot_begin(size_t len, char *err, size_t cap)
{
    esp_err_t e = esp_ota_begin(s_target, len, &s_ota);
    if (e != ESP_OK) snprintf(err, cap, "Could not prepare %s: %s", s_target->label, esp_err_to_name(e));
    return e == ESP_OK ? 0 : -1;
}

static int slot_write(const uint8_t *d, size_t n, char *err, size_t cap)
{
    esp_err_t e = esp_ota_write(s_ota, d, n);
    if (e != ESP_OK) {
        snprintf(err, cap, "Flash write failed: %s", esp_err_to_name(e));
        esp_ota_abort(s_ota);
    }
    return e == ESP_OK ? 0 : -1;
}

static int slot_finish(char *err, size_t cap)
{
    esp_err_t e = esp_ota_end(s_ota);                   /* validates the image */
    if (e == ESP_OK) e = esp_ota_set_boot_partition(s_target);
    if (e != ESP_OK) snprintf(err, cap, "Image rejected: %s", esp_err_to_name(e));
    return e == ESP_OK ? 0 : -1;
}

static void restart_into_update(void)
{
    kv_set("want_part", s_target->label);   /* devos_ota_init says whether it took */
    kv_set("want_ver", s_ver);
    kv_set("stage", NULL);
    snprintf(s_report, sizeof(s_report), "Installed %.20s. Restarting...", s_ver);
    s_state = DEVOS_OTA_REBOOTING;
    ESP_LOGI(TAG, "update %s written to %s, restarting", s_ver, s_target->label);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

/* Why a worker task couldn't start: its stack comes from internal RAM. */
static const char *low_ram_note(void)
{
    static char t[64];
    snprintf(t, sizeof(t), " (internal RAM low: largest free block %u KB)",
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
    return t;
}

static void check_worker(void);
static void install_worker(void);

static void ota_task(void *arg)
{
    if ((ota_job_t)(intptr_t)arg == JOB_CHECK) check_worker();
    else install_worker();
    vTaskDelete(NULL);
}

static int start_job(ota_job_t job)
{
    /* Internal-RAM stack (esp_ota_write writes flash), big enough for a TLS
     * handshake like the devos_http worker's. */
    return xTaskCreatePinnedToCore(ota_task, "ota", 12288, (void *)(intptr_t)job, 4, NULL, DEVOS_CORE_NET_CRYPTO) ==
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

static const char *reset_text(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC:    return "it crashed";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "a watchdog fired";
    case ESP_RST_BROWNOUT: return "the power dipped";
    case ESP_RST_POWERON:  return "it was switched off";
    case ESP_RST_SW:       return "it was restarted";
    default:               return "it restarted";
    }
}

void devos_ota_init(void)
{
    load();
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run) ESP_LOGI(TAG, "running from %s (%s), feed %s", run->label, devos_ota_build_text(), s_feed);
    if (!run) return;

    /* Did the last install take? It restarted into want_part; the bootloader
     * rolls back to the old slot if the new image never confirmed itself.
     * A stage left behind means the Tab5 restarted halfway through. */
    char part[20], ver[32], stage[16];
    if (kv_get("want_part", part, sizeof(part))) {
        kv_get("want_ver", ver, sizeof(ver));
        if (strcmp(part, run->label) == 0) {
            snprintf(s_report, sizeof(s_report), "Updated to %.20s (%s)", ver, devos_ota_build_text());
        } else {
            snprintf(s_report, sizeof(s_report), "The update to %.20s didn't start, so the Tab5 went back to "
                     "the firmware before it (%s)", ver, run->label);
            s_state = DEVOS_OTA_FAILED;
        }
        ESP_LOGW(TAG, "%s", s_report);
        kv_set("want_part", NULL);
        kv_set("want_ver", NULL);
    } else if (kv_get("stage", stage, sizeof(stage))) {
        kv_get("stage_ver", ver, sizeof(ver));
        snprintf(s_report, sizeof(s_report), "The update to %.20s stopped while %s: %s. The firmware is "
                 "unchanged - try again", ver, stage, reset_text());
        s_state = DEVOS_OTA_FAILED;
        ESP_LOGW(TAG, "%s", s_report);
    }
    kv_set("stage", NULL);
    kv_set("stage_ver", NULL);
}

#else
/* ------------------------------------------------------------------ simulator */
#include <pthread.h>

#define DEVOS_OTA_NVS_FILE TAB5_SD_MOUNT_POINT "/.devos/ota_nvs.json"
#define SIM_SLOT_FILE      TAB5_SD_MOUNT_POINT "/.devos/ota_slot.bin"
#define SIM_SLOT_SIZE      (4 * 1024 * 1024)
static FILE *s_slot;

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

static void kv_set(const char *key, const char *val) { (void)key; (void)val; }

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

static size_t slot_size(void) { return SIM_SLOT_SIZE; }

static int slot_begin(size_t len, char *err, size_t cap)
{
    (void)len;
    s_slot = fopen(SIM_SLOT_FILE, "wb");
    if (!s_slot) snprintf(err, cap, "Could not open %s", SIM_SLOT_FILE);
    return s_slot ? 0 : -1;
}

static int slot_write(const uint8_t *d, size_t n, char *err, size_t cap)
{
    if (fwrite(d, 1, n, s_slot) == n) return 0;
    snprintf(err, cap, "Could not write %s", SIM_SLOT_FILE);
    fclose(s_slot);
    s_slot = NULL;
    return -1;
}

static int slot_finish(char *err, size_t cap)
{
    int bad = fclose(s_slot);
    s_slot = NULL;
    if (bad) snprintf(err, cap, "Could not write %s", SIM_SLOT_FILE);
    return bad ? -1 : 0;
}

/* The simulator stops where a Tab5 would restart. */
static void restart_into_update(void)
{
    snprintf(s_report, sizeof(s_report), "Simulator: %.20s verified and written to .devos/ota_slot.bin "
             "(a Tab5 restarts into it now)", s_ver);
    s_state = DEVOS_OTA_UP_TO_DATE;
    s_progress = -1;
}

static const char *low_ram_note(void) { return ""; }

static void check_worker(void);
static void install_worker(void);

static void *ota_thread(void *arg)
{
    if ((ota_job_t)(intptr_t)arg == JOB_CHECK) check_worker();
    else install_worker();
    return NULL;
}

static int start_job(ota_job_t job)
{
    pthread_t th;
    if (pthread_create(&th, NULL, ota_thread, (void *)(intptr_t)job) != 0) return -1;
    pthread_detach(th);
    return 0;
}

void devos_ota_mark_boot_ok(void) {}

void devos_ota_init(void)
{
    load();
}
#endif

/* ========================================================================
 * Workers (their own task / thread)
 * ======================================================================== */
static void fail(const char *why)
{
    snprintf(s_report, sizeof(s_report), "%s", why);
    s_state = DEVOS_OTA_FAILED;
    s_progress = -1;
    kv_set("stage", NULL);
}

static void check_worker(void)
{
    devos_http_req_t q = { .url = s_feed, .timeout_ms = 15000, .max_redirects = 5, .max_body = MANIFEST_MAX };
    devos_http_resp_t r;
    char why[200];
    if (devos_http_request(&q, &r) != 0 || r.status != 200 || !r.body_len) {
        if (!r.status) snprintf(why, sizeof(why), "Update feed unreachable: %.150s", r.error);
        else snprintf(why, sizeof(why), "Update feed: HTTP %d from %.150s", r.status, s_feed);
        devos_http_resp_free(&r);
        fail(why);
        return;
    }
    int rc = handle_manifest(r.body);
    devos_http_resp_free(&r);
    s_state = rc > 0 ? DEVOS_OTA_AVAILABLE : rc == 0 ? DEVOS_OTA_UP_TO_DATE : DEVOS_OTA_FAILED;
}

static void hex32(const uint8_t *d, char *out)
{
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
    out[64] = '\0';
}

/* The image's own checks: ESP app image magic, app description, and the build
 * id the manifest promised. */
static bool image_ok(const uint8_t *d, size_t n, char *why, size_t cap)
{
    if (n < 32 + 176 || d[0] != 0xE9) {
        snprintf(why, cap, "The download is not a firmware image");
        return false;
    }
    uint32_t magic = (uint32_t)d[32] | (uint32_t)d[33] << 8 | (uint32_t)d[34] << 16 | (uint32_t)d[35] << 24;
    if (magic != 0xABCD5432u) {
        snprintf(why, cap, "The download has no app description (not a devOS image?)");
        return false;
    }
    if (strlen(s_build) >= 8) {
        char hex[65];
        hex32(d + 32 + 144, hex);
        if (strncasecmp(hex, s_build, strlen(s_build)) != 0) {
            snprintf(why, cap, "The download is a different build (%.8s) from the one the feed lists (%.8s)", hex,
                     s_build);
            return false;
        }
    }
    return true;
}

static void install_worker(void)
{
    char why[200];
    size_t room = slot_size();
    if (!room) {
        fail("No OTA partition to write to (check partitions.csv)");
        return;
    }
    if (s_size > 0 && (size_t)s_size > room) {
        snprintf(why, sizeof(why), "Image (%ld KB) is larger than the OTA slot (%lu KB)", s_size / 1024,
                 (unsigned long)(room / 1024));
        fail(why);
        return;
    }
    kv_set("stage_ver", s_ver);
    kv_set("stage", "downloading");

    /* 1. download, whole */
    s_got = 0;
    devos_http_req_t q = { .url = s_url, .timeout_ms = 20000, .max_redirects = 5, .max_body = room,
                           .progress = &s_got };
    devos_http_resp_t r;
    memset(&r, 0, sizeof(r));
    s_progress = 0;
    snprintf(s_report, sizeof(s_report), "Downloading %.20s...", s_ver);
    /* the request runs on this task; progress is read by the UI meanwhile */
    int rc = devos_http_request(&q, &r);
    size_t n = r.body_len;
    if (rc != 0 || r.status != 200) {
        if (!r.status) snprintf(why, sizeof(why), "Download failed: %.150s", r.error);
        else snprintf(why, sizeof(why), "Download failed: HTTP %d", r.status);
    } else if (r.truncated) {
        snprintf(why, sizeof(why), "The image is larger than the OTA slot (%lu KB)", (unsigned long)(room / 1024));
    } else if (r.error[0]) {
        snprintf(why, sizeof(why), "Download ended early (%lu of %ld KB): %.80s", (unsigned long)(n / 1024),
                 s_size / 1024, r.error);
    } else if (s_size > 0 && n != (size_t)s_size) {
        snprintf(why, sizeof(why), "Size mismatch: got %lu bytes, the feed says %ld", (unsigned long)n, s_size);
    } else {
        why[0] = '\0';
    }
    if (why[0]) {
        devos_http_resp_free(&r);
        fail(why);
        return;
    }

    /* 2. verify */
    s_state = DEVOS_OTA_VERIFYING;
    snprintf(s_report, sizeof(s_report), "Checking %.20s...", s_ver);
    const uint8_t *img = (const uint8_t *)r.body;
    uint8_t digest[32];
    char hex[65];
    devos_hash(DEVOS_HASH_SHA256, img, n, digest);
    hex32(digest, hex);
    if (s_sha[0] && strcasecmp(hex, s_sha) != 0) {
        devos_http_resp_free(&r);
        fail("Checksum mismatch: the download is not the published image");
        return;
    }
    if (!image_ok(img, n, why, sizeof(why))) {
        devos_http_resp_free(&r);
        fail(why);
        return;
    }

    /* 3. write, in one pass, from a small internal-RAM bounce buffer */
    kv_set("stage", "writing");
    s_state = DEVOS_OTA_WRITING;
    s_progress = 0;
    enum { CHUNK = 4096 };
#ifdef ESP_PLATFORM
    uint8_t *bounce = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    uint8_t *bounce = malloc(CHUNK);
#endif
    if (!bounce || slot_begin(n, why, sizeof(why)) != 0) {
        free(bounce);
        devos_http_resp_free(&r);
        fail(bounce ? why : "Out of memory");
        return;
    }
    for (size_t off = 0; off < n; off += CHUNK) {
        size_t k = n - off < CHUNK ? n - off : CHUNK;
        memcpy(bounce, img + off, k);
        if (slot_write(bounce, k, why, sizeof(why)) != 0) {
            free(bounce);
            devos_http_resp_free(&r);
            fail(why);
            return;
        }
        s_progress = (int)((off + k) * 100 / n);
        snprintf(s_report, sizeof(s_report), "Installing %.20s: %d%%", s_ver, s_progress);
    }
    free(bounce);
    devos_http_resp_free(&r);
    if (slot_finish(why, sizeof(why)) != 0) {
        fail(why);
        return;
    }
    restart_into_update();
}

bool devos_ota_busy(void)
{
    return s_state == DEVOS_OTA_CHECKING || s_state == DEVOS_OTA_DOWNLOADING || s_state == DEVOS_OTA_VERIFYING ||
           s_state == DEVOS_OTA_WRITING || s_state == DEVOS_OTA_REBOOTING;
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
        snprintf(s_report, sizeof(s_report), "Could not start the update check%s", low_ram_note());
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
        snprintf(s_report, sizeof(s_report), "Could not start the installer%s", low_ram_note());
        s_state = DEVOS_OTA_AVAILABLE;
        s_progress = -1;
        return -1;
    }
    return 0;
}

bool devos_ota_has_update(void) { return s_state == DEVOS_OTA_AVAILABLE; }
devos_ota_state_t devos_ota_state(void) { return s_state; }
int devos_ota_progress(void)
{
    if (s_state == DEVOS_OTA_DOWNLOADING) return s_size > 0 ? (int)((uint64_t)s_got * 100 / (uint64_t)s_size) : 0;
    return s_state == DEVOS_OTA_WRITING ? s_progress : -1;
}

const char *devos_ota_update_text(void)
{
    if (s_state != DEVOS_OTA_DOWNLOADING) return s_report;
    static char t[96];                          /* the UI's task only */
    size_t got = s_got;
    if (s_size > 0)
        snprintf(t, sizeof(t), "Downloading %.20s: %d%% (%lu of %ld KB)", s_ver, devos_ota_progress(),
                 (unsigned long)(got / 1024), s_size / 1024);
    else snprintf(t, sizeof(t), "Downloading %.20s: %lu KB", s_ver, (unsigned long)(got / 1024));
    return t;
}
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
