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
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_st7123.h"
#include "esp_lcd_ili9881c.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_st7123.h"
#include "tab5_panel_init_data.h"

static const char *TAG = "bsp_tab5";

/* Log an esp_err_t failure and return it (graceful degrade instead of abort).
 * Replaces ESP_ERROR_CHECK() in the display path so a bring-up failure prints a
 * diagnostic and lets the rest of the system (and the serial console) come up. */
#define TAB5_TRY(expr, what)                                                   \
    do {                                                                       \
        esp_err_t _err = (expr);                                               \
        if (_err != ESP_OK) {                                                  \
            ESP_LOGE(TAG, "%s failed: %s", (what), esp_err_to_name(_err));     \
            return _err;                                                       \
        }                                                                      \
    } while (0)

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

/* -------------------------------------------------------------------------
 * Display controller auto-detection (ported from espressif/esp-bsp
 * bsp/m5stack_tab5 bsp_get_board_version()).
 *
 * The Tab5 shipped with three display revisions, each needing a different
 * controller/init sequence. We identify the board by probing the touch
 * controller on the internal I2C bus (the touch chip is the reliable tell,
 * since the two newer panels use integrated TDDI touch):
 *   - ST712x TDDI touch @ 0x55 present -> read firmware version reg 0x0000:
 *         fw == 1 -> ST7121 (newest),  fw == 3 -> ST7123
 *   - GT911 touch @ 0x14/0x5D present  -> ILI9881C (original, pre Oct-2025)
 *
 * Override with -DTAB5_FORCE_PANEL=TAB5_PANEL_xxx if detection misfires.
 * ----------------------------------------------------------------------- */
typedef enum {
    TAB5_PANEL_UNKNOWN = 0,
    TAB5_PANEL_ILI9881C,   /* + GT911 touch */
    TAB5_PANEL_ST7123,     /* TDDI */
    TAB5_PANEL_ST7121,     /* TDDI */
} tab5_panel_t;

#define TAB5_TOUCH_ADDR_ST712X        0x55
#define TAB5_TOUCH_ADDR_GT911         0x5D
#define TAB5_TOUCH_ADDR_GT911_BACKUP  0x14
#define TAB5_PIN_TOUCH_INT            23    /* shared INT (BSP_LCD_TOUCH_INT) */

static esp_lcd_panel_handle_t s_panel = NULL;
static lv_display_t *s_disp = NULL;
static void *s_fb0 = NULL;
static tab5_panel_t s_panel_type = TAB5_PANEL_UNKNOWN;
static esp_lcd_touch_handle_t s_tp = NULL;
static lv_indev_t *s_indev = NULL;

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
static esp_err_t i2c_write_reg(i2c_port_t port, uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, buf, 2, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t i2c_read_reg(i2c_port_t port, uint8_t addr, uint8_t reg, uint8_t *val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return ret;
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
    esp_err_t wlan_ret = i2c_write_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE2, PI4IO_REG_OUT_SET, 0b10001001);

    /* Diagnostic: confirm the WLAN-power expander (0x44) actually responds and
     * that WIFI_EN (P0) is configured to drive high. A C6 that is silent on
     * SDIO (send_scr 0xffffffff) is usually unpowered or held in reset. */
    uint8_t io_dir = 0xFF, out_set = 0xFF, out_him = 0xFF;
    bool present = (i2c_read_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE2, PI4IO_REG_IO_DIR, &io_dir) == ESP_OK);
    i2c_read_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE2, PI4IO_REG_OUT_SET, &out_set);
    i2c_read_reg(TAB5_INTERNAL_I2C_PORT, TAB5_I2C_ADDR_PI4IOE2, PI4IO_REG_OUT_H_IM, &out_him);
    ESP_LOGI(TAG, "Expander 2 (0x44): ack=%s wlan_write=%s | IO_DIR=0x%02X OUT_SET=0x%02X OUT_H_IM=0x%02X "
                  "(WIFI_EN P0 -> dir_out=%d level_hi=%d driven=%d)",
             present ? "yes" : "NO", esp_err_to_name(wlan_ret),
             io_dir, out_set, out_him,
             (io_dir & 1), (out_set & 1), !(out_him & 1));
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
 * Internal I2C helpers (legacy driver) for touch-controller probing
 * ----------------------------------------------------------------------- */
static bool tab5_i2c_probe(i2c_port_t port, uint8_t addr7)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr7 << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret == ESP_OK;
}

static esp_err_t tab5_i2c_read_reg16(i2c_port_t port, uint8_t addr7, uint16_t reg,
                                     uint8_t *buf, size_t len)
{
    uint8_t reg_addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr7 << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, reg_addr, sizeof(reg_addr), true);
    i2c_master_start(cmd); /* repeated start */
    i2c_master_write_byte(cmd, (addr7 << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

/* Identify the display controller by probing the touch chip (see enum above). */
static tab5_panel_t tab5_detect_panel(void)
{
#ifdef TAB5_FORCE_PANEL
    tab5_panel_t forced = (TAB5_FORCE_PANEL);
    ESP_LOGW(TAG, "TAB5_FORCE_PANEL set: skipping auto-detect (panel=%d)", (int)forced);
    return forced;
#else
    /* Touch shares the display power/reset released by the IO expanders; give
     * it a moment to boot before probing. */
    vTaskDelay(pdMS_TO_TICKS(200));

    if (tab5_i2c_probe(TAB5_INTERNAL_I2C_PORT, TAB5_TOUCH_ADDR_ST712X)) {
        uint8_t fw = 0xFF;
        esp_err_t ret = tab5_i2c_read_reg16(TAB5_INTERNAL_I2C_PORT, TAB5_TOUCH_ADDR_ST712X,
                                            0x0000, &fw, 1);
        if (ret == ESP_OK && fw == 1) {
            ESP_LOGI(TAG, "Detected board rev 3: LCD ST7121, TDDI touch (fw=%u)", fw);
            return TAB5_PANEL_ST7121;
        }
        if (ret == ESP_OK && fw == 3) {
            ESP_LOGI(TAG, "Detected board rev 2: LCD ST7123, TDDI touch (fw=%u)", fw);
            return TAB5_PANEL_ST7123;
        }
        ESP_LOGW(TAG, "ST712x touch present but fw=%u (read %s); assuming ST7123",
                 fw, esp_err_to_name(ret));
        return TAB5_PANEL_ST7123;
    }

    if (tab5_i2c_probe(TAB5_INTERNAL_I2C_PORT, TAB5_TOUCH_ADDR_GT911_BACKUP) ||
        tab5_i2c_probe(TAB5_INTERNAL_I2C_PORT, TAB5_TOUCH_ADDR_GT911)) {
        ESP_LOGI(TAG, "Detected board rev 1: LCD ILI9881C, GT911 touch");
        return TAB5_PANEL_ILI9881C;
    }

    ESP_LOGW(TAG, "Display auto-detect failed (no known touch controller on I2C_1); "
                  "defaulting to ST7123. Override with -DTAB5_FORCE_PANEL=TAB5_PANEL_xxx");
    return TAB5_PANEL_ST7123;
#endif
}

/* Per-controller MIPI timing (from esp-bsp bsp/m5stack_tab5/src/bsp_display.c). */
typedef struct {
    const char *name;
    uint32_t    lane_bit_rate_mbps;
    uint32_t    dpi_clock_freq_mhz;
    uint16_t    hsync_pulse_width, hsync_back_porch, hsync_front_porch;
    uint16_t    vsync_pulse_width, vsync_back_porch, vsync_front_porch;
} tab5_panel_timing_t;

static tab5_panel_timing_t tab5_timing_for(tab5_panel_t panel)
{
    switch (panel) {
    case TAB5_PANEL_ILI9881C:
        return (tab5_panel_timing_t){ "ILI9881C", 1000, 60, 40, 140, 40, 4, 20, 20 };
    case TAB5_PANEL_ST7121:
        return (tab5_panel_timing_t){ "ST7121", 965, 70, 2, 40, 40, 20, 24, 200 };
    case TAB5_PANEL_ST7123:
    default:
        return (tab5_panel_timing_t){ "ST7123", 1000, 70, 2, 40, 40, 2, 8, 220 };
    }
}

/* -------------------------------------------------------------------------
 * Touch input -> LVGL indev (GT911 for the ILI9881C rev, ST7123 TDDI otherwise)
 * ----------------------------------------------------------------------- */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    if (!s_tp) { data->state = LV_INDEV_STATE_RELEASED; return; }

    uint16_t tx = 0, ty = 0, strength = 0;
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(s_tp);
    bool pressed = esp_lcd_touch_get_coordinates(s_tp, &tx, &ty, &strength, &cnt, 1);

    if (pressed && cnt > 0) {
        /* The controller reports in the panel's native portrait frame
         * (tx: 0..H_RES-1, ty: 0..V_RES-1). Apply the inverse of the 90deg CW
         * rotation used in disp_flush_cb (panel_x=ly, panel_y=V_RES-1-lx):
         *   lx = (V_RES-1) - ty,  ly = tx
         * If touch is flipped/rotated on your unit, flip the signs here. */
        int32_t lx = (int32_t)(TAB5_PANEL_V_RES - 1) - (int32_t)ty;
        int32_t ly = (int32_t)tx;
        if (lx < 0) lx = 0;
        if (lx > DEVOS_SCREEN_WIDTH - 1)  lx = DEVOS_SCREEN_WIDTH - 1;
        if (ly < 0) ly = 0;
        if (ly > DEVOS_SCREEN_HEIGHT - 1) ly = DEVOS_SCREEN_HEIGHT - 1;
        data->point.x = lx;
        data->point.y = ly;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void bsp_tab5_touch_init(void)
{
    if (!s_disp) return;

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = TAB5_PANEL_H_RES,   /* native portrait; rotation done in read cb */
        .y_max = TAB5_PANEL_V_RES,
        .rst_gpio_num = -1,          /* reset shared with the panel via PI4IOE */
        .int_gpio_num = -1,          /* polled from the LVGL read callback */
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_err_t ret;

    if (s_panel_type == TAB5_PANEL_ILI9881C) {
        /* ver-1 fix: the GT911 INT line has a pull-up to 3V3 that blocks it;
         * hold it low (matches esp-bsp). */
        gpio_config_t int_cfg = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << TAB5_PIN_TOUCH_INT,
        };
        gpio_config(&int_cfg);
        gpio_set_level(TAB5_PIN_TOUCH_INT, 0);

        esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        io_cfg.scl_speed_hz = 0;  /* legacy v1 i2c-lcd IO rejects a nonzero value */
        io_cfg.dev_addr = tab5_i2c_probe(TAB5_INTERNAL_I2C_PORT, TAB5_TOUCH_ADDR_GT911_BACKUP)
                          ? TAB5_TOUCH_ADDR_GT911_BACKUP : TAB5_TOUCH_ADDR_GT911;
        ret = esp_lcd_new_panel_io_i2c_v1((esp_lcd_i2c_bus_handle_t)(uint32_t)TAB5_INTERNAL_I2C_PORT,
                                          &io_cfg, &tp_io);
        if (ret == ESP_OK) ret = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_tp);
    } else {
        esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_ST7123_CONFIG();
        io_cfg.scl_speed_hz = 0;  /* legacy v1 i2c-lcd IO rejects a nonzero value */
        ret = esp_lcd_new_panel_io_i2c_v1((esp_lcd_i2c_bus_handle_t)(uint32_t)TAB5_INTERNAL_I2C_PORT,
                                          &io_cfg, &tp_io);
        if (ret == ESP_OK) ret = esp_lcd_touch_new_i2c_st7123(tp_io, &tp_cfg, &s_tp);
    }

    if (ret != ESP_OK || !s_tp) {
        ESP_LOGE(TAG, "Touch init failed: %s", esp_err_to_name(ret));
        s_tp = NULL;
        return;
    }

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read_cb);
    lv_indev_set_display(s_indev, s_disp);
    ESP_LOGI(TAG, "Touch input ready (%s)",
             s_panel_type == TAB5_PANEL_ILI9881C ? "GT911" : "ST7123 TDDI");
}

/* -------------------------------------------------------------------------
 * MIPI-DSI Display Initialization (auto-detected ILI9881C / ST7123 / ST7121)
 * ----------------------------------------------------------------------- */
static esp_err_t bsp_display_init(void)
{
    /* 0. Detect which display controller this board revision uses */
    s_panel_type = tab5_detect_panel();
    tab5_panel_timing_t t = tab5_timing_for(s_panel_type);
    ESP_LOGI(TAG, "Display controller: %s (DSI %lu Mbps, DPI %lu MHz)",
             t.name, (unsigned long)t.lane_bit_rate_mbps, (unsigned long)t.dpi_clock_freq_mhz);

    /* 1. Power on MIPI DSI PHY LDO (channel 3, 2.5V) */
    ESP_LOGI(TAG, "Powering MIPI DSI PHY LDO (2.5V)...");
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    TAB5_TRY(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy), "acquire MIPI DSI PHY LDO");

    /* 2. Create MIPI DSI bus (2 data lanes) */
    ESP_LOGI(TAG, "Creating MIPI DSI bus (2 lanes @ %lu Mbps)...",
             (unsigned long)t.lane_bit_rate_mbps);
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = t.lane_bit_rate_mbps,
    };
    TAB5_TRY(esp_lcd_new_dsi_bus(&bus_cfg, &mipi_dsi_bus), "create MIPI DSI bus");

    /* 3. Create DBI command IO */
    ESP_LOGI(TAG, "Creating MIPI DSI DBI command IO...");
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    TAB5_TRY(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_cfg, &dbi_io), "create DBI IO");

    /* 4. Configure DPI video timing (720x1280 native portrait) */
    ESP_LOGI(TAG, "Creating DPI panel config (%dx%d @ %lu MHz)...",
             TAB5_PANEL_H_RES, TAB5_PANEL_V_RES, (unsigned long)t.dpi_clock_freq_mhz);
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = t.dpi_clock_freq_mhz,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = TAB5_PANEL_H_RES,
            .v_size = TAB5_PANEL_V_RES,
            .hsync_pulse_width = t.hsync_pulse_width,
            .hsync_back_porch = t.hsync_back_porch,
            .hsync_front_porch = t.hsync_front_porch,
            .vsync_pulse_width = t.vsync_pulse_width,
            .vsync_back_porch = t.vsync_back_porch,
            .vsync_front_porch = t.vsync_front_porch,
        },
        .flags.use_dma2d = false,
    };

    /* 5. Instantiate the detected panel driver with its vendor init sequence.
     *    ST7123 and ST7121 share the ST7123 DCS driver (different init table);
     *    ILI9881C uses its own driver (it emits SLPOUT/MADCTL/COLMOD itself). */
    const esp_lcd_panel_dev_config_t panel_dev_base = {
        .reset_gpio_num = -1,  /* LCD_RST handled via the PI4IOE expander */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,  /* RGB565 */
    };

    if (s_panel_type == TAB5_PANEL_ILI9881C) {
        ili9881c_vendor_config_t vendor_cfg = {
            .init_cmds = disp_init_data_ili9881c,
            .init_cmds_size = sizeof(disp_init_data_ili9881c) / sizeof(disp_init_data_ili9881c[0]),
            .mipi_config = {
                .dsi_bus = mipi_dsi_bus,
                .dpi_config = &dpi_cfg,
                .lane_num = 2,
            },
        };
        esp_lcd_panel_dev_config_t cfg = panel_dev_base;
        cfg.vendor_config = &vendor_cfg;
        ESP_LOGI(TAG, "Creating ILI9881C panel...");
        TAB5_TRY(esp_lcd_new_panel_ili9881c(dbi_io, &cfg, &s_panel), "create ILI9881C panel");
    } else {
        st7123_vendor_config_t vendor_cfg = {
            /* ST7123 uses the driver's built-in default table (init_cmds=NULL);
             * ST7121 needs its own table. */
            .init_cmds = (s_panel_type == TAB5_PANEL_ST7121) ? disp_init_data_st7121 : NULL,
            .init_cmds_size = (s_panel_type == TAB5_PANEL_ST7121)
                                  ? (sizeof(disp_init_data_st7121) / sizeof(disp_init_data_st7121[0]))
                                  : 0,
            .mipi_config = {
                .dsi_bus = mipi_dsi_bus,
                .dpi_config = &dpi_cfg,
                .lane_num = 2,
            },
        };
        esp_lcd_panel_dev_config_t cfg = panel_dev_base;
        cfg.vendor_config = &vendor_cfg;
        ESP_LOGI(TAG, "Creating %s panel...", t.name);
        TAB5_TRY(esp_lcd_new_panel_st7123(dbi_io, &cfg, &s_panel), "create ST7123/ST7121 panel");
    }

    ESP_LOGI(TAG, "Resetting panel...");
    TAB5_TRY(esp_lcd_panel_reset(s_panel), "panel reset");

    ESP_LOGI(TAG, "Initializing panel (sending vendor init commands)...");
    TAB5_TRY(esp_lcd_panel_init(s_panel), "panel init");

    ESP_LOGI(TAG, "Enabling display output...");
    TAB5_TRY(esp_lcd_panel_disp_on_off(s_panel, true), "display on");

    /* 6. Retrieve continuous hardware scanout framebuffer */
    TAB5_TRY(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &s_fb0), "get frame buffer");
    ESP_LOGI(TAG, "Hardware scanout framebuffer @%p (clearing to test pattern)...", s_fb0);
    uint16_t *fb = (uint16_t *)s_fb0;
    for (int i = 0; i < TAB5_PANEL_H_RES * TAB5_PANEL_V_RES; i++) {
        fb[i] = 0x001F; /* Blue fill: if you see solid blue, panel + backlight work */
    }
    esp_cache_msync(s_fb0, TAB5_PANEL_H_RES * TAB5_PANEL_V_RES * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    /* 7. Create LVGL display: native 1280x720 landscape (rotated in flush_cb) */
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
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(s_disp, buf1, buf2, draw_buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, disp_flush_cb);

    /* 8. Enable Backlight */
    bsp_backlight_init();

    /* 9. Touch input (LVGL pointer indev) */
    bsp_tab5_touch_init();

    ESP_LOGI(TAG, "Tab5 MIPI-DSI Display Bringup Complete! (%s)", t.name);
    return ESP_OK;
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
    esp_err_t disp_err = bsp_display_init();
    if (disp_err != ESP_OK) {
        ESP_LOGE(TAG, "Display bring-up failed (%s); continuing so the console stays alive",
                 esp_err_to_name(disp_err));
    }
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
