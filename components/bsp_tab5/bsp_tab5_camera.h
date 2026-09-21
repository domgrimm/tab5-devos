#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TAB5_CAM_WIDTH  320
#define TAB5_CAM_HEIGHT 240

typedef void (*bsp_camera_qr_cb_t)(const char *decoded_text, void *user_data);

/* Initialize camera peripheral structures */
bool bsp_tab5_camera_init(void);

/* Power-gate: power on sensor and begin MIPI-CSI streaming at 320x240 grayscale */
bool bsp_tab5_camera_start(void);

/* Stop streaming and power down camera to preserve battery */
void bsp_tab5_camera_stop(void);

/* Returns true if the camera sensor is actively streaming */
bool bsp_tab5_camera_is_active(void);

/* Grab latest grayscale frame buffer (320x240, 8-bit luma) */
const uint8_t *bsp_tab5_camera_get_frame(int *width, int *height);

/* Start background QR scanning on Core 0. When a QR is detected, callback is fired */
bool bsp_tab5_camera_start_qr_scanner(bsp_camera_qr_cb_t cb, void *user_data);

/* Stop background QR scanner */
void bsp_tab5_camera_stop_qr_scanner(void);

/* Poll QR scanner (non-blocking, called periodically or in FreeRTOS task) */
void bsp_tab5_camera_qr_poll(void);

/* Simulator / testing: inject a simulated QR code payload */
void bsp_tab5_camera_inject_qr(const char *qr_payload);

#ifdef __cplusplus
}
#endif
