#include "bsp_tab5.h"
#include "devos_config.h"
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "driver/i2c.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ppa.h"
#endif

bool bsp_tab5_init(void)
{
#ifdef ESP_PLATFORM
    /* 1. Initialize I2C buses */
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TAB5_PIN_I2C_SDA,
        .scl_io_num = TAB5_PIN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000
    };
    i2c_param_config(TAB5_I2C_PORT, &i2c_conf);
    i2c_driver_install(TAB5_I2C_PORT, i2c_conf.mode, 0, 0, 0);

    /* 2. Initialize MIPI-DSI ST7123 1280x720 panel & LVGL buffers in PSRAM */
    /* Handled in target bringup */
#endif
    return true;
}

bool bsp_tab5_read_power(uint16_t *voltage_mv, int16_t *current_ma, uint16_t *power_mw)
{
    if (voltage_mv) *voltage_mv = 7820;
    if (current_ma) *current_ma = -410;
    if (power_mw) *power_mw = 3206;
    return true;
}

bool bsp_tab5_read_rtc(uint8_t *hour, uint8_t *min, uint8_t *sec)
{
    if (hour) *hour = 14;
    if (min) *min = 28;
    if (sec) *sec = 0;
    return true;
}

void bsp_tab5_set_brightness(uint8_t percent)
{
    (void)percent;
}
