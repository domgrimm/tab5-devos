#include "bsp_tab5.h"
#include "devos_config.h"
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "driver/gpio.h"

static const char *TAG = "bsp_tab5";

/* -------------------------------------------------------------------------
 * Tab5 MIPI-DSI Display Parameters (ST7123 / 5.0" 1280x720)
 * Refresh Rate ≈ 80 MHz / (1280+40+40+10) × (720+10+10+4) ≈ 60 Hz
 * ----------------------------------------------------------------------- */
#define TAB5_MIPI_DSI_LANE_NUM           2
#define TAB5_MIPI_DSI_LANE_BITRATE_MBPS  1000
#define TAB5_MIPI_DSI_DPI_CLK_MHZ       60
#define TAB5_MIPI_DSI_PHY_LDO_CHAN       3      /* Internal LDO_VO3 → VDD_MIPI_DPHY */
#define TAB5_MIPI_DSI_PHY_LDO_MV        2500

/* Video Timing (sync, porch) */
#define TAB5_LCD_HSYNC    10
#define TAB5_LCD_HBP      40
#define TAB5_LCD_HFP      40
#define TAB5_LCD_VSYNC    4
#define TAB5_LCD_VBP      10
#define TAB5_LCD_VFP      10

/* Backlight (LEDC PWM on GPIO22) */
#define TAB5_PIN_BK_LIGHT 22
#define TAB5_BK_LEDC_CHAN LEDC_CHANNEL_0
#define TAB5_BK_LEDC_FREQ 5000

/* LVGL draw buffer: 1/10th of screen height */
#define TAB5_LVGL_DRAW_BUF_LINES  (DEVOS_SCREEN_HEIGHT / 10)

static esp_lcd_panel_handle_t s_panel = NULL;
static lv_display_t *s_disp = NULL;

/* LVGL flush callback — pushes rendered buffer to DPI panel via DMA2D */
static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
}

/* DPI panel event: color transfer done → notify LVGL the buffer is free */
static bool notify_flush_ready(esp_lcd_panel_handle_t panel,
                               esp_lcd_dpi_panel_event_data_t *edata,
                               void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

static void bsp_display_init(void)
{
    /* 1. Power on the MIPI DSI PHY via internal LDO */
    ESP_LOGI(TAG, "Powering MIPI DSI PHY LDO (2.5V)...");
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = TAB5_MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = TAB5_MIPI_DSI_PHY_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy));

    /* 2. Create the MIPI DSI bus (initializes D-PHY) */
    ESP_LOGI(TAG, "Creating MIPI DSI bus (%d lanes, %d Mbps)...",
             TAB5_MIPI_DSI_LANE_NUM, TAB5_MIPI_DSI_LANE_BITRATE_MBPS);
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = TAB5_MIPI_DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = TAB5_MIPI_DSI_LANE_BITRATE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

    /* 3. Create DBI IO for sending LCD commands (virtual channel 0) */
    ESP_LOGI(TAG, "Creating MIPI DSI DBI command IO...");
    esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io));

    /* 4. Create the DPI video panel with timing and DMA2D */
    ESP_LOGI(TAG, "Creating DPI panel (%dx%d @ %d MHz)...",
             DEVOS_SCREEN_WIDTH, DEVOS_SCREEN_HEIGHT, TAB5_MIPI_DSI_DPI_CLK_MHZ);
    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = TAB5_MIPI_DSI_DPI_CLK_MHZ,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 2,
        .video_timing = {
            .h_size = DEVOS_SCREEN_WIDTH,
            .v_size = DEVOS_SCREEN_HEIGHT,
            .hsync_back_porch = TAB5_LCD_HBP,
            .hsync_pulse_width = TAB5_LCD_HSYNC,
            .hsync_front_porch = TAB5_LCD_HFP,
            .vsync_back_porch = TAB5_LCD_VBP,
            .vsync_pulse_width = TAB5_LCD_VSYNC,
            .vsync_front_porch = TAB5_LCD_VFP,
        },
        .flags.use_dma2d = true,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_dpi(mipi_dsi_bus, &dpi_config, &s_panel));

    /* 5. Initialize DPI panel video stream and DMA */
    ESP_LOGI(TAG, "Initializing DPI panel video mode and DMA...");
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    /* 6. Create LVGL display with double-buffered PSRAM draw buffers */
    ESP_LOGI(TAG, "Creating LVGL display (%dx%d)...",
             DEVOS_SCREEN_WIDTH, DEVOS_SCREEN_HEIGHT);
    s_disp = lv_display_create(DEVOS_SCREEN_WIDTH, DEVOS_SCREEN_HEIGHT);
    lv_display_set_user_data(s_disp, s_panel);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);

    size_t draw_buf_sz = DEVOS_SCREEN_WIDTH * TAB5_LVGL_DRAW_BUF_LINES * sizeof(lv_color_t);
    void *buf1 = heap_caps_malloc(draw_buf_sz, MALLOC_CAP_SPIRAM);
    void *buf2 = heap_caps_malloc(draw_buf_sz, MALLOC_CAP_SPIRAM);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers from PSRAM!");
        return;
    }
    lv_display_set_buffers(s_disp, buf1, buf2, draw_buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, disp_flush_cb);

    /* 7. Register DPI event callback so LVGL knows when buffer is free */
    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = notify_flush_ready,
    };
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, s_disp));

    /* 8. Backlight: LEDC PWM on GPIO22 */
    ESP_LOGI(TAG, "Enabling backlight on GPIO%d...", TAB5_PIN_BK_LIGHT);
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = TAB5_BK_LEDC_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = TAB5_BK_LEDC_CHAN,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = TAB5_PIN_BK_LIGHT,
        .duty = 200, /* ~78% brightness */
        .hpoint = 0,
    };
    ledc_channel_config(&ledc_channel);

    ESP_LOGI(TAG, "Display initialization complete.");
}
#endif /* ESP_PLATFORM */

bool bsp_tab5_init(void)
{
#ifdef ESP_PLATFORM
    /* 1. Initialize I2C buses (keyboard, sensors) */
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

    /* 2. Initialize MIPI-DSI display + LVGL + backlight */
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
    uint32_t duty = (uint32_t)percent * 255 / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, TAB5_BK_LEDC_CHAN, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, TAB5_BK_LEDC_CHAN);
#else
    (void)percent;
#endif
}
