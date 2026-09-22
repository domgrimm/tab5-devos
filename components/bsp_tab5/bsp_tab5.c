#include "bsp_tab5.h"
#include "devos_config.h"
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_cache.h"
#include "esp_lcd_st7123.h"

static const char *TAG = "bsp_tab5";

/* -------------------------------------------------------------------------
 * Internal I2C Bus Pins & IO Expander Addresses
 * ----------------------------------------------------------------------- */
#define TAB5_INTERNAL_I2C_PORT       I2C_NUM_1
#define TAB5_PIN_INTERNAL_I2C_SDA    31
#define TAB5_PIN_INTERNAL_I2C_SCL    32
#define TAB5_I2C_ADDR_PI4IOE1        0x43  /* Display/Touch/Camera/Audio resets */
#define TAB5_I2C_ADDR_PI4IOE2        0x44  /* Power rails (WLAN, USB, Charging) */

/* PI4IOE5V6408 Registers */
#define PI4IO_REG_CHIP_RESET         0x01
#define PI4IO_REG_IO_DIR             0x03
#define PI4IO_REG_OUT_SET            0x05
#define PI4IO_REG_OUT_H_IM           0x07
#define PI4IO_REG_IN_DEF_STA         0x09
#define PI4IO_REG_PULL_EN            0x0B
#define PI4IO_REG_PULL_SEL           0x0D

/* -------------------------------------------------------------------------
 * Backlight (LEDC PWM on GPIO22)
 * ----------------------------------------------------------------------- */
#define TAB5_PIN_BK_LIGHT            22
#define TAB5_BK_LEDC_CHAN            LEDC_CHANNEL_0
#define TAB5_BK_LEDC_FREQ            5000

/* -------------------------------------------------------------------------
 * Native Display Geometry (720x1280 Portrait Panel, Rotated 90° to 1280x720)
 * ----------------------------------------------------------------------- */
#define TAB5_PANEL_H_RES             720
#define TAB5_PANEL_V_RES             1280
#define TAB5_LVGL_DRAW_BUF_LINES     80

static esp_lcd_panel_handle_t s_panel = NULL;
static lv_display_t *s_disp = NULL;
static void *s_fb0 = NULL;

/* -------------------------------------------------------------------------
 * LVGL Flush Callback & Direct Framebuffer Rotation
 * ----------------------------------------------------------------------- */
static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (!s_fb0) {
        lv_display_flush_ready(disp);
        return;
    }

    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;

    static uint32_t s_flush_count = 0;
    if (++s_flush_count <= 5 || (s_flush_count % 300 == 0)) {
        ESP_LOGI(TAG, "disp_flush_cb #%lu: area [%ld,%ld - %ld,%ld] (%ldx%ld)",
                 (unsigned long)s_flush_count,
                 (long)area->x1, (long)area->y1, (long)area->x2, (long)area->y2,
                 (long)w, (long)h);
    }

    // Rotate 90° clockwise directly into hardware scanout framebuffer:
    // Landscape [x: 0..1279, y: 0..719] -> Portrait [px: 0..719, py: 0..1279]
    // panel_x = y
    // panel_y = 1279 - x
    uint16_t *dst_fb = (uint16_t *)s_fb0;
    const uint16_t *src = (const uint16_t *)px_map;

    for (int y = 0; y < h; y++) {
        int panel_x = area->y1 + y;
        const uint16_t *src_row = &src[y * w];
        for (int x = 0; x < w; x++) {
            int panel_y = 1279 - (area->x1 + x);
            dst_fb[panel_y * TAB5_PANEL_H_RES + panel_x] = src_row[x];
        }
    }

    // Write back dirty lines from CPU cache to PSRAM so DSI DMA sees updated pixels
    int rot_y1 = 1279 - area->x2;
    int rot_y2 = 1279 - area->x1;
    if (rot_y1 < 0) rot_y1 = 0;
    if (rot_y2 > 1279) rot_y2 = 1279;

    uint8_t *cache_sync_start = (uint8_t *)s_fb0 + (rot_y1 * TAB5_PANEL_H_RES) * 2;
    size_t cache_sync_size = (rot_y2 - rot_y1 + 1) * TAB5_PANEL_H_RES * 2;
    esp_cache_msync(cache_sync_start, cache_sync_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    lv_display_flush_ready(disp);
}

/* -------------------------------------------------------------------------
 * Internal I2C Expander Bringup (PI4IOE5V6408)
 * ----------------------------------------------------------------------- */
static void i2c_write_reg(i2c_port_t port, uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, buf, 2, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
}

static void bsp_io_expanders_init(void)
{
    ESP_LOGI(TAG, "Initializing PI4IOE5V6408 expanders on I2C_1 (GPIO 31/32)...");

    /* --- Expander 1 (0x43): Display, Touch, Camera Resets --- */
    i2c_write_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE1, PI4IO_REG_CHIP_RESET, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(5));

    /* Enable pull-ups */
    i2c_write_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE1, PI4IO_REG_PULL_SEL, 0b01111111);
    i2c_write_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE1, PI4IO_REG_PULL_EN,  0b01111111);

    /* Output latch 0 on P4 (LCD_RST) */
    i2c_write_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE1, PI4IO_REG_OUT_SET,  0b01100110);
    i2c_write_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE1, PI4IO_REG_OUT_H_IM, 0b00000000);

    /* Assert LCD_RST low via output */
    i2c_write_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE1, PI4IO_REG_IO_DIR,   0b01111111);
    vTaskDelay(pdMS_TO_TICKS(15));

    /* Release LCD_RST by switching P4 to input pull-up (safe 1.8V release) */
    i2c_write_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE1, PI4IO_REG_IO_DIR,   0b01101111);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "Expander 1 initialized: LCD_RST released via pull-up.");

    /* --- Expander 2 (0x44): Power Rails (WLAN, USB5V, Charge) --- */
    i2c_write_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE2, PI4IO_REG_CHIP_RESET, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(5));

    i2c_write_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE2, PI4IO_REG_IO_DIR,    0b10111001);
    i2c_write_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE2, PI4IO_REG_OUT_H_IM, 0b00000110);
    i2c_write_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE2, PI4IO_REG_PULL_SEL,  0b10111001);
    i2c_write_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE2, PI4IO_REG_PULL_EN,   0b11111001);
    /* Enable WLAN_PWR_EN (P0), USB5V_EN (P3), CHG_EN (P7) */
    i2c_write_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE2, PI4IO_REG_OUT_SET,   0b10001001);
    ESP_LOGI(TAG, "Expander 2 initialized: WLAN/USB power enabled.");
}

/* -------------------------------------------------------------------------
 * Backlight Bringup (LEDC PWM on GPIO22)
 * ----------------------------------------------------------------------- */
static void bsp_backlight_init(void)
{
    ESP_LOGI(TAG, "Configuring backlight LEDC on GPIO%d...", TAB5_PIN_BK_LIGHT);
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_12_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = TAB5_BK_LEDC_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {
        .gpio_num = TAB5_PIN_BK_LIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = TAB5_BK_LEDC_CHAN,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 4095, /* 100% full brightness */
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
    ESP_LOGI(TAG, "Backlight enabled at 100%% brightness.");
}

/* -------------------------------------------------------------------------
 * MIPI-DSI Display Initialization (ST7123 / 720x1280 @ 70MHz)
 * ----------------------------------------------------------------------- */
static void bsp_display_init(void)
{
    /* 1. Power on MIPI DSI PHY LDO (channel 3, 2.5V) */
    ESP_LOGI(TAG, "Powering MIPI DSI PHY LDO (2.5V)...");
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy));

    /* 2. Create MIPI DSI bus (2 lanes @ 965 Mbps) */
    ESP_LOGI(TAG, "Creating MIPI DSI bus (2 lanes @ 965 Mbps)...");
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 965,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &mipi_dsi_bus));

    /* 3. Create DBI command IO */
    ESP_LOGI(TAG, "Creating MIPI DSI DBI command IO...");
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_cfg, &dbi_io));

    /* 4. Configure DPI video timing (720x1280 native portrait @ 70 MHz) */
    ESP_LOGI(TAG, "Creating DPI panel config (%dx%d @ 70 MHz)...",
             TAB5_PANEL_H_RES, TAB5_PANEL_V_RES);
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 70,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = TAB5_PANEL_H_RES,
            .v_size = TAB5_PANEL_V_RES,
            .hsync_pulse_width = 2,
            .hsync_back_porch = 40,
            .hsync_front_porch = 40,
            .vsync_pulse_width = 2,
            .vsync_back_porch = 8,
            .vsync_front_porch = 220,
        },
        .flags.use_dma2d = false,
    };

    /* 5. Initialize ST7123 panel with vendor command sequence */
    st7123_vendor_config_t vendor_cfg = {
        .init_cmds = NULL, /* uses official default vendor init commands in esp_lcd_st7123.c */
        .init_cmds_size = 0,
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num = 2,
        },
    };

    const esp_lcd_panel_dev_config_t panel_dev_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 24,
        .vendor_config = &vendor_cfg,
    };

    ESP_LOGI(TAG, "Creating ST7123 panel...");
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7123(dbi_io, &panel_dev_cfg, &s_panel));

    ESP_LOGI(TAG, "Resetting ST7123 panel...");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));

    ESP_LOGI(TAG, "Initializing ST7123 panel (sending vendor init commands)...");
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    ESP_LOGI(TAG, "Enabling ST7123 display output...");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* 6. Retrieve continuous hardware scanout framebuffer */
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &s_fb0));
    ESP_LOGI(TAG, "Hardware scanout framebuffer @%p (clearing to test pattern)...", s_fb0);
    uint16_t *fb = (uint16_t *)s_fb0;
    for (int i = 0; i < TAB5_PANEL_H_RES * TAB5_PANEL_V_RES; i++) {
        fb[i] = 0x001F; /* Test blue fill so hardware scanout is immediately verified */
    }
    esp_cache_msync(s_fb0, TAB5_PANEL_H_RES * TAB5_PANEL_V_RES * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    /* 7. Create LVGL display: native 1280x720 landscape */
    ESP_LOGI(TAG, "Creating LVGL display (%dx%d)...",
             DEVOS_SCREEN_WIDTH, DEVOS_SCREEN_HEIGHT);
    s_disp = lv_display_create(DEVOS_SCREEN_WIDTH, DEVOS_SCREEN_HEIGHT);
    lv_display_set_user_data(s_disp, s_panel);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);

    size_t draw_buf_sz = DEVOS_SCREEN_WIDTH * TAB5_LVGL_DRAW_BUF_LINES * sizeof(lv_color_t);
    void *buf1 = heap_caps_malloc(draw_buf_sz, MALLOC_CAP_SPIRAM);
    void *buf2 = heap_caps_malloc(draw_buf_sz, MALLOC_CAP_SPIRAM);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate draw buffers from PSRAM!");
        return;
    }
    lv_display_set_buffers(s_disp, buf1, buf2, draw_buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, disp_flush_cb);

    /* 8. Enable Backlight */
    bsp_backlight_init();

    ESP_LOGI(TAG, "Tab5 MIPI-DSI Display Bringup Complete!");
}
#endif /* ESP_PLATFORM */

bool bsp_tab5_init(void)
{
#ifdef ESP_PLATFORM
    /* 1. Initialize External I2C Bus for Keyboard (Port 0: GPIO 0 SDA, GPIO 1 SCL) */
    i2c_config_t ext_i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TAB5_PIN_I2C_SDA,
        .scl_io_num = TAB5_PIN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    i2c_param_config(TAB5_I2C_PORT, &ext_i2c_conf);
    i2c_driver_install(TAB5_I2C_PORT, ext_i2c_conf.mode, 0, 0, 0);

    /* 2. Initialize Internal I2C Bus (Port 1: GPIO 31 SDA, GPIO 32 SCL) */
    i2c_config_t int_i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TAB5_PIN_INTERNAL_I2C_SDA,
        .scl_io_num = TAB5_PIN_INTERNAL_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    i2c_param_config(TAB5_INTERNAL_I2C_PORT, &int_i2c_conf);
    i2c_driver_install(TAB5_INTERNAL_I2C_PORT, int_i2c_conf.mode, 0, 0, 0);

    /* 3. Initialize IO Expanders & Release Screen Reset */
    bsp_io_expanders_init();

    /* 4. Initialize Display Pipeline & Backlight */
    bsp_display_init();
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
#ifdef ESP_PLATFORM
    uint32_t duty = (uint32_t)percent * 4095 / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, TAB5_BK_LEDC_CHAN, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, TAB5_BK_LEDC_CHAN);
#else
    (void)percent;
#endif
}
