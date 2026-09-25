#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize Tab5 Board Support Package (MIPI-DSI, touch, INA226, RTC) */
bool bsp_tab5_init(void);

/* Battery / power telemetry (INA226 @ 0x41 on the internal I2C bus, 5 mOhm
 * shunt, plus the charger CHG_STAT line on the 0x44 IO expander). */
typedef struct {
    bool     valid;        /* INA226 answered */
    uint16_t voltage_mv;   /* battery pack (2S Li-ion) voltage */
    int32_t  current_ma;   /* + charging, - discharging */
    uint32_t power_mw;     /* |V x I| */
    bool     charging;     /* current flowing into the pack (> +15 mA) */
    bool     chg_stat;     /* raw charger CHG_STAT line (IO expander 0x44 P6) */
} bsp_tab5_power_t;

bool bsp_tab5_read_power(bsp_tab5_power_t *out);

/* Hardware RTC (Epson RX8130CE @ 0x32). Time is stored as UTC.
 * bsp_tab5_rtc_get() returns false if the chip is absent or its registers do
 * not hold a valid date (e.g. backup battery lost). */
bool bsp_tab5_rtc_get(struct tm *utc);
bool bsp_tab5_rtc_set(const struct tm *utc);

/* Set display backlight brightness (0 - 100%) */
void bsp_tab5_set_brightness(uint8_t percent);

/* Name of the detected display controller ("ST7121", "ST7123", "ILI9881C"). */
const char *bsp_tab5_panel_name(void);

/* Called (from the LVGL/GUI task) whenever the touchscreen is pressed, so the
 * power manager can treat touch as user activity. */
void bsp_tab5_set_touch_activity_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif
