/* devos_qr: see devos_qr.h. */
#include "devos_qr.h"
#include "bsp_tab5_camera.h"
#include "devos_config.h"
#include "quirc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCAN_SIDE 720               /* centre square decoded at full resolution */

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
#else
#include <pthread.h>
#include <unistd.h>
static pthread_mutex_t s_mx_sim = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&s_mx_sim)
#define UNLOCK() pthread_mutex_unlock(&s_mx_sim)
static void *big_alloc(size_t n) { return malloc(n); }
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
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

/* Try to decode the codes quirc found; mirrored codes (front camera) too. */
static bool decode_all(struct quirc *q)
{
    static EXT_RAM_BSS_ATTR struct quirc_code code;     /* ~13 KB together */
    static EXT_RAM_BSS_ATTR struct quirc_data data;
    int n = quirc_count(q);
    for (int i = 0; i < n; i++) {
        quirc_extract(q, i, &code);
        quirc_decode_error_t e = quirc_decode(&code, &data);
        if (e == QUIRC_ERROR_DATA_ECC) {
            quirc_flip(&code);
            e = quirc_decode(&code, &data);
        }
        if (e == QUIRC_SUCCESS && data.payload_len > 0) {
            LOCK();
            size_t len = (size_t)data.payload_len < DEVOS_QR_TEXT_MAX - 1 ? (size_t)data.payload_len : DEVOS_QR_TEXT_MAX - 1;
            memcpy(s_result, data.payload, len);
            s_result[len] = '\0';
            s_state = DEVOS_QR_FOUND;
            UNLOCK();
            return true;
        }
    }
    return false;
}

static void scan_loop(void)
{
    struct quirc *q = quirc_new();
    if (!q || quirc_resize(q, SCAN_SIDE, SCAN_SIDE) < 0) {
        snprintf(s_err, sizeof(s_err), "Out of memory for the QR decoder");
        s_state = DEVOS_QR_ERROR;
        if (q) quirc_destroy(q);
        return;
    }
    int misses = 0;
    while (s_run) {
        int w, h;
        uint8_t *img = quirc_begin(q, &w, &h);
        if (!bsp_tab5_camera_grab_gray(img, w, h, 1, 1000)) {
            quirc_end(q);
            if (++misses >= 5) {
                snprintf(s_err, sizeof(s_err), "%s", bsp_tab5_camera_error()[0] ? bsp_tab5_camera_error() : "The camera stopped");
                s_state = DEVOS_QR_ERROR;
                break;
            }
            sleep_ms(50);
            continue;
        }
        misses = 0;
        /* preview: every 2nd pixel (quirc thresholds the image in place next) */
        LOCK();
        int step = w / DEVOS_QR_PREVIEW;
        for (int y = 0; y < DEVOS_QR_PREVIEW; y++) {
            const uint8_t *src = img + (size_t)y * step * w;
            uint8_t *dst = s_preview + (size_t)y * DEVOS_QR_PREVIEW;
            for (int x = 0; x < DEVOS_QR_PREVIEW; x++) dst[x] = src[x * step];
        }
        s_preview_gen++;
        if (!s_preview_gen) s_preview_gen = 1;
        UNLOCK();
        quirc_end(q);
        if (decode_all(q)) break;
    }
    quirc_destroy(q);
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
        for (int i = 0; i < 40 && s_task_alive; i++) sleep_ms(25);
        if (s_task_alive) return -1;
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
     * 16 KB: quirc_decode() keeps a ~9 KB datastream on the stack. The task
     * only exists while scanning. */
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
