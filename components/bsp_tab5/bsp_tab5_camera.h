#pragma once

/* Tab5 camera: SmartSens SC2356 (esp_cam_sensor calls it "SC202CS"), 1-lane
 * MIPI CSI, SCCB 0x36 on the internal I2C bus, reset held high by the 0x43
 * IO expander. Pipeline: RAW8 1280x720 -> CSI -> ISP -> RGB565 in PSRAM,
 * converted to grayscale on demand, with a simple software auto-exposure.
 *
 * The simulator has no camera: it serves TAB5_SD_MOUNT_POINT
 * "/.devos/camera.pgm" (binary P5 greyscale) as every frame, if present.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Detect / set up on first use and start streaming. */
bool bsp_tab5_camera_start(void);
void bsp_tab5_camera_stop(void);
bool bsp_tab5_camera_streaming(void);

/* Sensor frame size (0 x 0 before the first successful start). */
void bsp_tab5_camera_size(int *w, int *h);

/* Wait for the next frame and write a w x h greyscale window from its centre,
 * sampling every `step` pixels (1 = full resolution). False on timeout or if
 * the camera isn't streaming. */
bool bsp_tab5_camera_grab_gray(uint8_t *out, int w, int h, int step, int timeout_ms);

/* Why the last start / grab failed ("" if it didn't). */
const char *bsp_tab5_camera_error(void);

#ifdef __cplusplus
}
#endif
