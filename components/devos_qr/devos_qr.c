/* devos_qr: see devos_qr.h. */
#include "devos_qr.h"
#include "bsp_tab5_camera.h"
#include "devos_config.h"
#include "quirc_internal.h"               /* quirc.h + counters for the log */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCAN_SIDE    720            /* centre square of the 1280x720 frame */
#define GRAB_WAIT_MS 300            /* per frame; short so stop() is prompt */
#define MAX_MISSES   10             /* ~3 s without a picture = camera failed */
#define LOG_EVERY_MS 2000
/* last full-resolution frame of a scan that found nothing, for debugging (the
 * simulator replays .devos/camera.pgm) */
#define DUMP_FILE    TAB5_SD_MOUNT_POINT "/.devos/qr_last.pgm"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
static SemaphoreHandle_t s_mx;
#define LOCK()   do { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_mx) xSemaphoreGive(s_mx); } while (0)
static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}
static void sleep_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
#include "esp_log.h"
#include "esp_timer.h"
static const char *TAG = "qr";
#define QLOG(...) ESP_LOGI(TAG, __VA_ARGS__)
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
#else
#include <pthread.h>
#include <unistd.h>
static pthread_mutex_t s_mx_sim = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&s_mx_sim)
#define UNLOCK() pthread_mutex_unlock(&s_mx_sim)
static void *big_alloc(size_t n) { return malloc(n); }
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#include <time.h>
#define QLOG(fmt, ...) printf("[qr] " fmt "\n", ##__VA_ARGS__)
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

static volatile devos_qr_state_t s_state;
static volatile bool s_run, s_task_alive;
static char s_err[96];
static char *s_result;                     /* DEVOS_QR_TEXT_MAX */
static uint8_t *s_preview;                 /* DEVOS_QR_PREVIEW^2 */
static uint32_t s_preview_gen;

devos_qr_state_t devos_qr_state(void) { return s_state; }
const char *devos_qr_error(void) { return s_err; }

bool devos_qr_take_result(char *out, size_t cap)
{
    bool ok = false;
    LOCK();
    if (s_state == DEVOS_QR_FOUND && s_result && cap) {
        snprintf(out, cap, "%s", s_result);
        s_state = DEVOS_QR_IDLE;
        ok = true;
    }
    UNLOCK();
    return ok;
}

uint32_t devos_qr_preview(uint8_t *out)
{
    LOCK();
    uint32_t g = s_preview_gen;
    if (g && s_preview && out) memcpy(out, s_preview, DEVOS_QR_PREVIEW * DEVOS_QR_PREVIEW);
    UNLOCK();
    return g;
}

/* Try to decode the codes quirc found; mirrored codes too, and each at the
 * neighbouring grid sizes when quirc's size estimate is a version out. */
static bool decode_all(struct quirc *q, quirc_decode_error_t *last_err)
{
    static EXT_RAM_BSS_ATTR struct quirc_code code;     /* ~13 KB together */
    static EXT_RAM_BSS_ATTR struct quirc_data data;
    static const int resize[3] = { 0, -4, 4 };
    int n = quirc_count(q);
    for (int i = 0; i < n; i++) {
        int size0 = q->grids[i].grid_size;
        quirc_decode_error_t first = QUIRC_SUCCESS;
        for (int k = 0; k < 3; k++) {
            /* only a plausible code (it failed ECC) is worth refitting */
            if (k && first != QUIRC_ERROR_DATA_ECC && first != QUIRC_ERROR_FORMAT_ECC) break;
            if (k && quirc_refit(q, i, size0 + resize[k]) < 0) continue;
            quirc_extract(q, i, &code);
            quirc_decode_error_t e = quirc_decode(&code, &data);
            if (e == QUIRC_ERROR_DATA_ECC) {
                quirc_flip(&code);
                e = quirc_decode(&code, &data);
            }
            if (!k) first = *last_err = e;
            else if (e == QUIRC_SUCCESS) *last_err = e;
            if (e == QUIRC_SUCCESS && data.payload_len > 0) {
                LOCK();
                size_t len = (size_t)data.payload_len < DEVOS_QR_TEXT_MAX - 1 ? (size_t)data.payload_len : DEVOS_QR_TEXT_MAX - 1;
                memcpy(s_result, data.payload, len);
                s_result[len] = '\0';
                s_state = DEVOS_QR_FOUND;
                UNLOCK();
                QLOG("decoded a version %d code, %d bytes%s", data.version, data.payload_len,
                     k ? " (after correcting the grid size)" : "");
                return true;
            }
        }
    }
    return false;
}

/* Local-mean binarisation (quirc's thresholding before it switched to one
 * global Otsu threshold): copes with the uneven light, glare and lens
 * vignetting of a real camera. In place, to 0 / 255, which quirc's Otsu then
 * splits cleanly. `avg` holds w ints. */
static void binarize_local(uint8_t *img, int w, int h, int *avg)
{
    int s = w / 8 < 1 ? 1 : w / 8;
    int aw = 0, au = 0;
    for (int y = 0; y < h; y++) {
        uint8_t *row = img + (size_t)y * w;
        memset(avg, 0, (size_t)w * sizeof(int));
        for (int x = 0; x < w; x++) {                /* serpentine running means */
            int a = (y & 1) ? x : w - 1 - x, b = w - 1 - a;
            aw = aw * (s - 1) / s + row[a];
            au = au * (s - 1) / s + row[b];
            avg[a] += aw;
            avg[b] += au;
        }
        for (int x = 0; x < w; x++) row[x] = row[x] < avg[x] * (100 - 5) / (200 * s) ? 0 : 255;
    }
}

static void dump_frame(const uint8_t *img, int w, int h)
{
    FILE *f = fopen(DUMP_FILE, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    bool ok = fwrite(img, 1, (size_t)w * h, f) == (size_t)w * h;
    ok = fclose(f) == 0 && ok;
    if (ok) QLOG("no code decoded; last frame saved to %s", DUMP_FILE);
}

/* Each frame gets one treatment, in rotation, so what one misses another may
 * catch: full resolution or half (2x2 averaged: less sensor noise and screen
 * moire), quirc's global threshold or the local one. */
static const struct { bool half, local; const char *name; } PASSES[4] = {
    { false, false, "full/global" },
    { true,  true,  "half/local" },
    { false, true,  "full/local" },
    { true,  false, "half/global" },
};

static void scan_loop(void)
{
    struct quirc *qs[2] = { quirc_new(), quirc_new() };      /* full, half */
    int *avg = big_alloc(SCAN_SIDE * sizeof(int));
    uint8_t *last = big_alloc(SCAN_SIDE * SCAN_SIDE);        /* grey copy for dump_frame */
    if (!qs[0] || !qs[1] || !avg || !last ||
        quirc_resize(qs[0], SCAN_SIDE, SCAN_SIDE) < 0 || quirc_resize(qs[1], SCAN_SIDE / 2, SCAN_SIDE / 2) < 0) {
        snprintf(s_err, sizeof(s_err), "Out of memory for the QR decoder");
        s_state = DEVOS_QR_ERROR;
        goto out;
    }
    int misses = 0, frames = 0, total = 0;
    bool found = false, have_last = false;
    /* stats for the periodic log line */
    int64_t t_log = now_ms(), t_work = 0;
    int lo = 255, hi = 0, best_caps = 0, best_grids = 0;
    uint32_t luma_sum = 0, luma_n = 0;
    quirc_decode_error_t last_err = QUIRC_SUCCESS;
    bool any_decode = false;
    while (s_run && !found) {
        int pi = total & 3, w, h;
        struct quirc *q = qs[PASSES[pi].half];
        uint8_t *img = quirc_begin(q, &w, &h);
        if (!bsp_tab5_camera_grab_gray(img, w, h, PASSES[pi].half ? 2 : 1, GRAB_WAIT_MS)) {
            if (++misses >= MAX_MISSES) {
                snprintf(s_err, sizeof(s_err), "%s", bsp_tab5_camera_error()[0] ? bsp_tab5_camera_error() : "The camera stopped");
                s_state = DEVOS_QR_ERROR;
                QLOG("giving up: %s", s_err);
                break;
            }
            sleep_ms(20);
            continue;
        }
        misses = 0;
        total++;
        frames++;
        int64_t t0 = now_ms();
        /* preview (before any thresholding), 360 x 360 */
        LOCK();
        int step = w / DEVOS_QR_PREVIEW;
        for (int y = 0; y < DEVOS_QR_PREVIEW; y++) {
            const uint8_t *src = img + (size_t)y * step * w;
            uint8_t *dst = s_preview + (size_t)y * DEVOS_QR_PREVIEW;
            for (int x = 0; x < DEVOS_QR_PREVIEW; x++) {
                uint8_t v = src[x * step];
                dst[x] = v;
                if (v < lo) lo = v;
                if (v > hi) hi = v;
                luma_sum += v;
            }
        }
        luma_n += DEVOS_QR_PREVIEW * DEVOS_QR_PREVIEW;
        s_preview_gen++;
        if (!s_preview_gen) s_preview_gen = 1;
        UNLOCK();
        if (!PASSES[pi].half) {
            memcpy(last, img, (size_t)w * h);
            have_last = true;
        }
        if (PASSES[pi].local) binarize_local(img, w, h, avg);
        quirc_end(q);
        if (q->num_capstones > best_caps) best_caps = q->num_capstones;
        if (quirc_count(q) > best_grids) best_grids = quirc_count(q);
        if (quirc_count(q)) any_decode = true;
        found = decode_all(q, &last_err);
        if (found) QLOG("found by the %s pass after %d frames", PASSES[pi].name, total);
        t_work += now_ms() - t0;
        int64_t now = now_ms();
        if (now - t_log >= LOG_EVERY_MS) {
            QLOG("%d frames/%d ms, %d ms decoding each; luma %d..%d mean %d; best %d finder patterns, %d codes; %s",
                 frames, (int)(now - t_log), (int)(t_work / frames), lo, hi, (int)(luma_sum / luma_n),
                 best_caps, best_grids, any_decode ? quirc_strerror(last_err) : "nothing to decode");
            t_log = now;
            t_work = 0;
            frames = 0;
            lo = 255;
            hi = 0;
            luma_sum = luma_n = 0;
            best_caps = best_grids = 0;
            any_decode = false;
        }
    }
    if (!found && have_last && total >= 20) dump_frame(last, SCAN_SIDE, SCAN_SIDE);
out:
    for (int i = 0; i < 2; i++) {
        if (qs[i]) quirc_destroy(qs[i]);
    }
    free(avg);
    free(last);
}

#ifdef ESP_PLATFORM
static void scan_task(void *arg)
{
    (void)arg;
    scan_loop();
    bsp_tab5_camera_stop();
    s_run = false;
    s_task_alive = false;
    vTaskDelete(NULL);
}
#else
static void *scan_thread(void *arg)
{
    (void)arg;
    scan_loop();
    bsp_tab5_camera_stop();
    s_run = false;
    s_task_alive = false;
    return NULL;
}
#endif

int devos_qr_start(void)
{
#ifdef ESP_PLATFORM
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
#endif
    if (s_task_alive) {                          /* still winding down */
        for (int i = 0; i < 80 && s_task_alive; i++) sleep_ms(25);
        if (s_task_alive) {
            snprintf(s_err, sizeof(s_err), "The scanner is still stopping. Try again.");
            s_state = DEVOS_QR_ERROR;
            return -1;
        }
    }
    if (!s_result) s_result = big_alloc(DEVOS_QR_TEXT_MAX);
    if (!s_preview) s_preview = big_alloc(DEVOS_QR_PREVIEW * DEVOS_QR_PREVIEW);
    if (!s_result || !s_preview) {
        snprintf(s_err, sizeof(s_err), "Out of memory");
        s_state = DEVOS_QR_ERROR;
        return -1;
    }
    s_err[0] = '\0';
    LOCK();
    s_preview_gen = 0;
    UNLOCK();
    if (!bsp_tab5_camera_start()) {
        snprintf(s_err, sizeof(s_err), "%s", bsp_tab5_camera_error());
        s_state = DEVOS_QR_ERROR;
        return -1;
    }
    s_state = DEVOS_QR_SCANNING;
    s_run = true;
    s_task_alive = true;
#ifdef ESP_PLATFORM
    /* network core: keeps the UI smooth while quirc works on 720x720 frames.
     * 16 KB: quirc_decode() keeps a ~9 KB datastream on the stack and
     * quirc_end() a 1 KB histogram. The task only exists while scanning. */
    if (xTaskCreatePinnedToCore(scan_task, "qr_scan", 16384, NULL, 3, NULL, DEVOS_CORE_NET_CRYPTO) != pdPASS) {
        s_task_alive = s_run = false;
        bsp_tab5_camera_stop();
        snprintf(s_err, sizeof(s_err), "Couldn't start the scanner");
        s_state = DEVOS_QR_ERROR;
        return -1;
    }
#else
    pthread_t t;
    pthread_create(&t, NULL, scan_thread, NULL);
    pthread_detach(t);
#endif
    return 0;
}

void devos_qr_stop(void)
{
    s_run = false;                               /* the task stops the camera */
    if (s_state == DEVOS_QR_SCANNING) s_state = DEVOS_QR_IDLE;
}
