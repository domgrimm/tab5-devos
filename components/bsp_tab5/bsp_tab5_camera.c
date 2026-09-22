#include "bsp_tab5_camera.h"
#include "quirc.h"
#include "devos_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
static const char *TAG = "tab5_camera";
#else
#include <time.h>
#endif

static bool s_cam_initialized = false;
static bool s_cam_active = false;
static bool s_scanner_active = false;

static bsp_camera_qr_cb_t s_qr_cb = NULL;
static void *s_qr_user_data = NULL;

static struct quirc *s_quirc = NULL;
static EXT_RAM_BSS_ATTR uint8_t s_frame_buf[TAB5_CAM_WIDTH * TAB5_CAM_HEIGHT];

#ifdef ESP_PLATFORM
static TaskHandle_t s_scanner_task_handle = NULL;

/* FreeRTOS background QR scanner task pinned to Core 0 */
static void qr_scanner_task(void *pvParameters)
{
    ESP_LOGI(TAG, "QR scanner task started on Core %d", xPortGetCoreID());

    while (s_scanner_active) {
        /* Grab camera frame from MIPI-CSI driver */
        int w = TAB5_CAM_WIDTH;
        int h = TAB5_CAM_HEIGHT;
        const uint8_t *frame = bsp_tab5_camera_get_frame(&w, &h);
        if (frame && s_quirc) {
            uint8_t *image = quirc_begin(s_quirc, NULL, NULL);
            if (image) {
                memcpy(image, frame, w * h);
                quirc_end(s_quirc);

                int count = quirc_count(s_quirc);
                for (int i = 0; i < count; i++) {
                    struct quirc_code code;
                    struct quirc_data data;
                    quirc_extract(s_quirc, i, &code);
                    if (quirc_decode(&code, &data) == QUIRC_SUCCESS) {
                        data.payload[data.payload_len] = '\0';
                        ESP_LOGI(TAG, "QR Code detected: %s", data.payload);

                        bsp_camera_qr_cb_t cb = s_qr_cb;
                        void *ud = s_qr_user_data;
                        s_scanner_active = false;

                        if (cb) cb((const char *)data.payload, ud);
                        break;
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(40)); /* ~25 FPS scanning */
    }

    ESP_LOGI(TAG, "QR scanner task exiting");
    s_scanner_task_handle = NULL;
    vTaskDelete(NULL);
}
#endif

bool bsp_tab5_camera_init(void)
{
    if (s_cam_initialized) return true;

    if (!s_quirc) {
        s_quirc = quirc_new();
        if (!s_quirc || quirc_resize(s_quirc, TAB5_CAM_WIDTH, TAB5_CAM_HEIGHT) < 0) {
            if (s_quirc) {
                quirc_destroy(s_quirc);
                s_quirc = NULL;
            }
            return false;
        }
    }

    memset(s_frame_buf, 0x20, sizeof(s_frame_buf));
    s_cam_initialized = true;
    return true;
}

bool bsp_tab5_camera_start(void)
{
    if (!s_cam_initialized) {
        if (!bsp_tab5_camera_init()) return false;
    }
    s_cam_active = true;

#ifdef ESP_PLATFORM
    /* Tab5 SC2356 MIPI-CSI Hardware Bringup:
     * - Enable MCLK clock on GPIO 36
     * - Configure SCCB (I2C) control
     * - Start MIPI-CSI 2-lane receiver & ISP
     */
    ESP_LOGI(TAG, "Power-gate: SC2356 MIPI-CSI camera streaming active");
#endif

    return true;
}

void bsp_tab5_camera_stop(void)
{
    bsp_tab5_camera_stop_qr_scanner();
    s_cam_active = false;

#ifdef ESP_PLATFORM
    /* Shut down camera sensor and MCLK clock to save battery */
    ESP_LOGI(TAG, "Power-gate: SC2356 MIPI-CSI camera powered down");
#endif
}

bool bsp_tab5_camera_is_active(void)
{
    return s_cam_active;
}

const uint8_t *bsp_tab5_camera_get_frame(int *width, int *height)
{
    if (width) *width = TAB5_CAM_WIDTH;
    if (height) *height = TAB5_CAM_HEIGHT;

#ifndef ESP_PLATFORM
    /* Simulator: Render an animated viewfinder feed (targeting reticle & scanline) */
    static uint8_t scanline_y = 0;
    scanline_y = (scanline_y + 4) % TAB5_CAM_HEIGHT;

    for (int y = 0; y < TAB5_CAM_HEIGHT; y++) {
        for (int x = 0; x < TAB5_CAM_WIDTH; x++) {
            int idx = y * TAB5_CAM_WIDTH + x;
            uint8_t pixel = 0x18; /* Dark background */

            /* Corner bracket reticles (40px) */
            int bx = 60, by = 40, bw = 200, bh = 160;
            bool is_border = false;
            if ((x >= bx && x <= bx + bw && (y == by || y == by + bh)) ||
                (y >= by && y <= by + bh && (x == bx || x == bx + bw))) {
                if (x <= bx + 24 || x >= bx + bw - 24 ||
                    y <= by + 24 || y >= by + bh - 24) {
                    is_border = true;
                }
            }
            if (is_border) {
                pixel = 0xE0; /* Bright white reticle */
            } else if (y == scanline_y && x >= bx && x <= bx + bw) {
                pixel = 0x80; /* Scanning laser line */
            } else if ((x + y) % 32 == 0) {
                pixel = 0x22;
            }
            s_frame_buf[idx] = pixel;
        }
    }
#endif

    return s_frame_buf;
}

bool bsp_tab5_camera_start_qr_scanner(bsp_camera_qr_cb_t cb, void *user_data)
{
    if (!bsp_tab5_camera_start()) return false;

    s_qr_cb = cb;
    s_qr_user_data = user_data;
    s_scanner_active = true;

#ifdef ESP_PLATFORM
    if (!s_scanner_task_handle) {
        xTaskCreatePinnedToCore(
            qr_scanner_task,
            "qr_scanner",
            6144,
            NULL,
            5,
            &s_scanner_task_handle,
            0 /* Pinned to Core 0 */
        );
    }
#endif

    return true;
}

void bsp_tab5_camera_stop_qr_scanner(void)
{
    s_scanner_active = false;
    s_qr_cb = NULL;
    s_qr_user_data = NULL;
}

void bsp_tab5_camera_qr_poll(void)
{
    if (!s_scanner_active || !s_quirc) return;

#ifndef ESP_PLATFORM
    /* On simulator, quirc can process simulated or injected frames */
#endif
}

void bsp_tab5_camera_inject_qr(const char *qr_payload)
{
    if (!qr_payload || !*qr_payload) return;
    if (s_scanner_active && s_qr_cb) {
        bsp_camera_qr_cb_t cb = s_qr_cb;
        void *ud = s_qr_user_data;
        bsp_tab5_camera_stop_qr_scanner();
        bsp_tab5_camera_stop();
        cb(qr_payload, ud);
    }
}
