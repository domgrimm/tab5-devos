#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize Tab5 Board Support Package (MIPI-DSI, GT911, INA226, RTC) */
bool bsp_tab5_init(void);

/* Poll power telemetry from INA226 */
bool bsp_tab5_read_power(uint16_t *voltage_mv, int16_t *current_ma, uint16_t *power_mw);

/* Read RTC clock from RX8130CE */
bool bsp_tab5_read_rtc(uint8_t *hour, uint8_t *min, uint8_t *sec);

/* Set display brightness (0 - 100%) */
void bsp_tab5_set_brightness(uint8_t percent);

/* Diagnostic: paint static vertical colour bars straight into the scanout
 * framebuffer(s) (no LVGL). Used by the display-only test build to check whether
 * the flicker is present with nothing but the DPI running. */
void bsp_tab5_fill_test_pattern(void);

#ifdef __cplusplus
}
#endif
