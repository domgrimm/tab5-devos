#include "bsp_tab5.h"
#include "devos_config.h"
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
#include "esp_idf_version.h"
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 3)
#include <math.h>
#include "esp_clk_tree.h"
#include "hal/mipi_dsi_host_ll.h"
#include "hal/mipi_dsi_brg_ll.h"
#define TAB5_DSI_TIMING_FIXUP 1
#endif

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

/* Double-buffered scanout for tear-free updates (see disp_flush_cb). */
#define TAB5_NUM_FBS 2

static esp_lcd_panel_handle_t s_panel = NULL;
static lv_display_t *s_disp = NULL;
static void *s_fb[TAB5_NUM_FBS] = { NULL, NULL };
static int s_draw_fb_index = 1;         /* back buffer we render into (fb[0] shown at boot) */
static void *s_lv_buf = NULL;           /* full-screen landscape LVGL draw buffer (DIRECT mode) */
static int s_lv_stride_px = 0;          /* LVGL buffer row stride, in pixels */
static SemaphoreHandle_t s_vsync_sem = NULL;
static tab5_panel_t s_panel_type = TAB5_PANEL_UNKNOWN;
static esp_lcd_touch_handle_t s_tp = NULL;
static lv_indev_t *s_indev = NULL;
static bool s_touch_int_gated = false;  /* TDDI: read I2C only while INT asserted */

/* -------------------------------------------------------------------------
 * LVGL flush + tear-free frame-buffer swap
 *
 * Anti-tearing (mirrors espressif/esp_lvgl_port + esp-bsp): the DPI panel owns
 * TAB5_NUM_FBS scanout framebuffers and only ever displays one at a time. LVGL
 * renders the whole landscape frame into a persistent off-screen buffer
 * (DIRECT mode, single buffer). On the last flush of a refresh we rotate that
 * complete frame 90 deg CW into the *back* framebuffer -- never the one being
 * scanned out, so there is no tearing -- then hand that framebuffer to
 * esp_lcd_panel_draw_bitmap(), which the DPI driver switches to at the next
 * VSYNC (zero-copy). We wait for on_refresh_done before reusing the old
 * framebuffer, so the switch has taken effect first.
 *
 * The previous approach (single scanout FB, CPU rotating dirty pixels straight
 * into it while the DSI DMA was reading it) is what caused the flicker.
 * ----------------------------------------------------------------------- */

/* Fires from ISR after each full frame has been scanned out of the DPI panel. */
static bool dpi_refresh_done_cb(esp_lcd_panel_handle_t panel,
                                esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;
    BaseType_t hp_task_woken = pdFALSE;
    if (s_vsync_sem) {
        xSemaphoreGiveFromISR(s_vsync_sem, &hp_task_woken);
    }
    return hp_task_woken == pdTRUE;
}

/* Rotate the full landscape frame (DEVOS_SCREEN_WIDTH x DEVOS_SCREEN_HEIGHT,
 * RGB565, row stride src_stride_px pixels) 90 deg CW into the portrait scanout
 * framebuffer (TAB5_PANEL_H_RES x TAB5_PANEL_V_RES). Tiled so both source reads
 * and destination writes stay cache-local -- a naive transpose thrashes the
 * PSRAM cache and makes the full-frame rotation far too slow.
 *   panel_x = ly,  panel_y = (V_RES-1) - lx
 */
#define TAB5_ROT_TILE 32
_Static_assert(DEVOS_SCREEN_WIDTH == TAB5_PANEL_V_RES && DEVOS_SCREEN_HEIGHT == TAB5_PANEL_H_RES,
               "rotate_landscape_to_fb assumes a 90-degree map between the landscape UI "
               "and the portrait panel; update the rotation if the geometry changes");
static void rotate_landscape_to_fb(const uint16_t *src, int src_stride_px, uint16_t *dst)
{
    for (int by = 0; by < DEVOS_SCREEN_HEIGHT; by += TAB5_ROT_TILE) {
        int y_end = by + TAB5_ROT_TILE;
        if (y_end > DEVOS_SCREEN_HEIGHT) y_end = DEVOS_SCREEN_HEIGHT;
        for (int bx = 0; bx < DEVOS_SCREEN_WIDTH; bx += TAB5_ROT_TILE) {
            int x_end = bx + TAB5_ROT_TILE;
            if (x_end > DEVOS_SCREEN_WIDTH) x_end = DEVOS_SCREEN_WIDTH;
            for (int ly = by; ly < y_end; ly++) {
                const uint16_t *src_row = &src[ly * src_stride_px];
                for (int lx = bx; lx < x_end; lx++) {
                    int panel_y = (TAB5_PANEL_V_RES - 1) - lx;
                    dst[panel_y * TAB5_PANEL_H_RES + ly] = src_row[lx];
                }
            }
        }
    }
}

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;

    /* LVGL keeps the complete frame in s_lv_buf (DIRECT mode); accumulate
     * partial renders and only push a full frame on the last flush. */
    if (!lv_display_flush_is_last(disp) || !s_lv_buf || !s_fb[s_draw_fb_index]) {
        lv_display_flush_ready(disp);
        return;
    }

    uint16_t *back_fb = (uint16_t *)s_fb[s_draw_fb_index];
    rotate_landscape_to_fb((const uint16_t *)s_lv_buf, s_lv_stride_px, back_fb);

    /* Flush the rotated frame from CPU cache to PSRAM so the DSI DMA sees it. */
    esp_cache_msync(back_fb, TAB5_PANEL_H_RES * TAB5_PANEL_V_RES * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    /* Switch scanout to this framebuffer at the next VSYNC (zero-copy: the DPI
     * driver just repoints its DMA because back_fb is one of its own FBs). */
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, TAB5_PANEL_H_RES, TAB5_PANEL_V_RES, back_fb);

    /* Wait until the switch has actually taken effect before we reuse the FB we
     * were displaying. Bounded so a missed VSYNC IRQ degrades to (at worst)
     * tearing instead of hanging the GUI task. */
    if (s_vsync_sem) {
        xSemaphoreTake(s_vsync_sem, 0);
        if (xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
            static uint32_t s_vsync_timeouts = 0;
            if ((++s_vsync_timeouts % 60) == 1) {
                ESP_LOGW(TAG, "VSYNC wait timed out (%lu); refresh_done not firing?",
                         (unsigned long)s_vsync_timeouts);
            }
        }
    }

    /* The framebuffer we just displayed becomes the next back buffer. */
    s_draw_fb_index ^= 1;

    static uint32_t s_flush_count = 0;
    if (++s_flush_count <= 3 || (s_flush_count % 600 == 0)) {
        ESP_LOGI(TAG, "frame #%lu pushed (back fb now index %d)",
                 (unsigned long)s_flush_count, s_draw_fb_index);
    }

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
    /* Values taken from M5GFX (m5stack/M5GFX, Panel setup in M5GFX.cpp), which
     * is the display stack that ships flicker-free on this exact hardware.
     * NOTE: the ST7121's 70 MHz DPI clock is only programmed correctly on
     * ESP-IDF >= 5.5.3; on older IDF see tab5_dsi_fixup_horizontal_timing(). */
    switch (panel) {
    case TAB5_PANEL_ILI9881C:
        return (tab5_panel_timing_t){ "ILI9881C", 1040, 80, 40, 140, 40, 4, 20, 20 };
    case TAB5_PANEL_ST7121:
        return (tab5_panel_timing_t){ "ST7121", 900, 70, 2, 40, 40, 20, 24, 200 };
    case TAB5_PANEL_ST7123:
    default:
        return (tab5_panel_timing_t){ "ST7123", 1040, 80, 2, 40, 40, 2, 8, 220 };
    }
}

#if TAB5_DSI_TIMING_FIXUP
/* -------------------------------------------------------------------------
 * Backport of the ESP-IDF v5.5.3 MIPI-DSI horizontal timing fix
 * (components/hal/mipi_dsi_hal.c: mipi_dsi_hal_host_dpi_set_horizontal_timing).
 *
 * Before 5.5.3 the DPI driver truncates the DPI clock divider and then derives
 * the DSI host line timing from the *resulting* clock, truncating each field to
 * whole lane-byte clocks without fixing up the total. For the ST7121 (70 MHz
 * asked of the 240 MHz PLL -> div 3 -> really 80 MHz) that gives a ~65 Hz
 * refresh instead of ~57 Hz, and a DSI host line ~1.8 lane-byte clocks shorter
 * than the DPI line: the panel flickers even on a static framebuffer. (80 MHz
 * panels divide exactly, so ST7123/ILI9881C are barely affected.) M5GFX, which
 * is flicker-free here, relies on IDF >= 5.5.3 for this.
 *
 * Re-program both register sets exactly as 5.5.3 does: host timing from the
 * *requested* clock (rounded, total compensated), and the bridge front porch
 * stretched so the real clock still yields the requested line period. Must run
 * after the DPI panel is created and before esp_lcd_panel_init() starts video.
 * ----------------------------------------------------------------------- */
static void tab5_dsi_fixup_horizontal_timing(const esp_lcd_dpi_panel_config_t *dpi,
                                             uint32_t lane_bit_rate_mbps)
{
    soc_module_clk_t clk_src = (soc_module_clk_t)(dpi->dpi_clk_src ? dpi->dpi_clk_src
                                                                   : MIPI_DSI_DPI_CLK_SRC_DEFAULT);
    uint32_t src_hz = 0;
    if (esp_clk_tree_src_get_freq_hz(clk_src, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &src_hz) != ESP_OK
        || src_hz == 0 || dpi->dpi_clock_freq_mhz == 0) {
        ESP_LOGW(TAG, "DSI timing fix-up skipped (unknown DPI clock)");
        return;
    }
    /* Same integer divider IDF < 5.5.3 programmed into the hardware. */
    const uint32_t src_mhz = src_hz / 1000 / 1000;
    const uint32_t div = src_mhz / dpi->dpi_clock_freq_mhz;
    const float expect_mhz = (float)dpi->dpi_clock_freq_mhz;
    const float real_mhz = (float)src_mhz / (float)div;

    const esp_lcd_video_timing_t *t = &dpi->video_timing;
    const uint32_t htotal = t->hsync_pulse_width + t->hsync_back_porch + t->h_size + t->hsync_front_porch;

    /* DSI host: lane-byte-clock units, derived from the requested DPI clock. */
    const float ratio = (float)lane_bit_rate_mbps / expect_mhz / 8.0f;
    const uint32_t host_hsw = (uint32_t)roundf(t->hsync_pulse_width * ratio);
    const uint32_t host_hbp = (uint32_t)roundf(t->hsync_back_porch * ratio);
    const uint32_t host_act = (uint32_t)roundf(t->h_size * ratio);
    const uint32_t host_hfp = (uint32_t)roundf(t->hsync_front_porch * ratio);
    const int host_comp = (int)roundf(htotal * ratio) - (int)(host_hsw + host_hbp + host_act + host_hfp);
    mipi_dsi_host_ll_dpi_set_horizontal_timing(MIPI_DSI_LL_GET_HOST(0), host_hsw, host_hbp,
                                               host_act + host_comp, host_hfp);

    /* DSI bridge: pixel units at the real DPI clock; stretch the front porch so
     * the line period (and so the refresh rate) is what was requested. */
    const int brg_comp = (int)roundf(real_mhz / expect_mhz * htotal) - (int)htotal;
    const uint32_t brg_hfp = t->hsync_front_porch + brg_comp;
    mipi_dsi_brg_ll_set_horizontal_timing(MIPI_DSI_LL_GET_BRG(0), t->hsync_pulse_width,
                                          t->hsync_back_porch, t->h_size, brg_hfp);
    mipi_dsi_brg_ll_update_dpi_config(MIPI_DSI_LL_GET_BRG(0));

    const uint32_t vtotal = t->vsync_pulse_width + t->vsync_back_porch + t->v_size + t->vsync_front_porch;
    const uint32_t refresh_x10 = (uint32_t)(real_mhz * 1e7f / ((htotal + brg_comp) * vtotal));
    ESP_LOGI(TAG, "DSI timing fix-up (IDF < 5.5.3): DPI clk %lu MHz (asked %lu), host "
                  "hsw/hbp/act/hfp=%lu/%lu/%lu/%lu, bridge hfp %lu->%lu, refresh %lu.%lu Hz",
             (unsigned long)(src_mhz / div), (unsigned long)dpi->dpi_clock_freq_mhz,
             (unsigned long)host_hsw, (unsigned long)host_hbp, (unsigned long)(host_act + host_comp),
             (unsigned long)host_hfp, (unsigned long)t->hsync_front_porch, (unsigned long)brg_hfp,
             (unsigned long)(refresh_x10 / 10), (unsigned long)(refresh_x10 % 10));
}
#endif /* TAB5_DSI_TIMING_FIXUP */

/* -------------------------------------------------------------------------
 * Touch input -> LVGL indev (GT911 for the ILI9881C rev, ST7123 TDDI otherwise)
 * ----------------------------------------------------------------------- */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    if (!s_tp) { data->state = LV_INDEV_STATE_RELEASED; return; }

    /* TDDI panels (ST7121/ST7123) time-share their panel lines between display
     * driving and touch sensing, so hammering the touch controller with I2C on
     * every LVGL poll (~33 Hz) interrupts the display -> continuous flicker.
     * Gate the I2C read on the touch INT line (active low) so we only talk to
     * the controller while a finger is actually down -- the same effect as
     * esp-bsp's interrupt-driven touch. A slow fallback poll keeps touch alive
     * even if an INT edge is ever missed. */
    if (s_touch_int_gated) {
        static uint32_t idle_skips = 0;
        if (gpio_get_level(TAB5_PIN_TOUCH_INT) != 0) {   /* INT high = no touch */
            if (++idle_skips < 30) {                     /* ~1 s fallback at 33 Hz */
                data->state = LV_INDEV_STATE_RELEASED;
                return;                                  /* skip the I2C read */
            }
            idle_skips = 0;
        } else {
            idle_skips = 0;
        }
    }

    uint16_t tx = 0, ty = 0, strength = 0;
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(s_tp);
    bool pressed = esp_lcd_touch_get_coordinates(s_tp, &tx, &ty, &strength, &cnt, 1);

    static bool was_pressed = false;
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
        /* Diagnostic on the press edge only: shows whether the controller
         * returns points and how raw coords map to LVGL space (for calibration). */
        if (!was_pressed) {
            ESP_LOGI(TAG, "touch: raw(tx=%u ty=%u cnt=%u) -> lvgl(%ld,%ld)",
                     tx, ty, cnt, (long)lx, (long)ly);
        }
        was_pressed = true;
        data->point.x = lx;
        data->point.y = ly;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        was_pressed = false;
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
        /* TDDI (ST7121/ST7123): poll the shared INT line so touch_read_cb only
         * hits the I2C bus while a finger is down (the panel time-shares its
         * lines with touch sensing, so idle polling flickers the display).
         * Configured as a plain input; we read its level, no ISR. */
        gpio_config_t int_cfg = {
            .mode = GPIO_MODE_INPUT,
            .pin_bit_mask = 1ULL << TAB5_PIN_TOUCH_INT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&int_cfg);
        s_touch_int_gated = true;

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
        .num_fbs = TAB5_NUM_FBS,   /* double-buffered for tear-free swaps */
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

#if TAB5_DSI_TIMING_FIXUP
    /* DPI panel exists (timing registers programmed) but video hasn't started. */
    tab5_dsi_fixup_horizontal_timing(&dpi_cfg, t.lane_bit_rate_mbps);
#endif

    ESP_LOGI(TAG, "Resetting panel...");
    TAB5_TRY(esp_lcd_panel_reset(s_panel), "panel reset");

    ESP_LOGI(TAG, "Initializing panel (sending vendor init commands)...");
    TAB5_TRY(esp_lcd_panel_init(s_panel), "panel init");

    ESP_LOGI(TAG, "Enabling display output...");
    TAB5_TRY(esp_lcd_panel_disp_on_off(s_panel, true), "display on");

    /* 6. Retrieve both hardware scanout framebuffers. Validate them explicitly
     *    before use: if the driver ever hands back fewer than TAB5_NUM_FBS we
     *    must not memset() a NULL pointer (that would panic before the console
     *    is usable). Any failure here degrades gracefully so serial stays up. */
    esp_err_t fb_ret = esp_lcd_dpi_panel_get_frame_buffer(s_panel, TAB5_NUM_FBS, &s_fb[0], &s_fb[1]);
    if (fb_ret != ESP_OK || s_fb[0] == NULL || s_fb[1] == NULL) {
        ESP_LOGE(TAG, "get frame buffers failed: %s (fb0=%p fb1=%p)",
                 esp_err_to_name(fb_ret), s_fb[0], s_fb[1]);
        return (fb_ret != ESP_OK) ? fb_ret : ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Scanout framebuffers @%p, @%p (clearing both)...", s_fb[0], s_fb[1]);
    for (int i = 0; i < TAB5_NUM_FBS; i++) {
        memset(s_fb[i], 0, TAB5_PANEL_H_RES * TAB5_PANEL_V_RES * 2);
        esp_cache_msync(s_fb[i], TAB5_PANEL_H_RES * TAB5_PANEL_V_RES * 2,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
    s_draw_fb_index = 1;  /* fb[0] is scanned out first; render into fb[1] */

    /* VSYNC signalling for tear-free framebuffer switching (see disp_flush_cb). */
    s_vsync_sem = xSemaphoreCreateCounting(1, 0);
    if (s_vsync_sem) {
        esp_lcd_dpi_panel_event_callbacks_t cbs = {
            .on_refresh_done = dpi_refresh_done_cb,
        };
        esp_err_t cb_ret = esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL);
        if (cb_ret != ESP_OK) {
            ESP_LOGW(TAG, "register DPI event callbacks failed: %s "
                          "(frames will not wait for VSYNC)", esp_err_to_name(cb_ret));
        }
    } else {
        ESP_LOGW(TAG, "failed to create VSYNC semaphore; frames will not wait for VSYNC");
    }

    /* 7. Create LVGL display: native 1280x720 landscape (rotated in flush_cb).
     *    DIRECT mode with one persistent full-screen buffer: LVGL keeps the whole
     *    landscape frame here and only redraws changed areas; disp_flush_cb
     *    rotates the complete frame into the back framebuffer. */
    ESP_LOGI(TAG, "Creating LVGL display (%dx%d)...",
             DEVOS_SCREEN_WIDTH, DEVOS_SCREEN_HEIGHT);
    s_disp = lv_display_create(DEVOS_SCREEN_WIDTH, DEVOS_SCREEN_HEIGHT);
    lv_display_set_user_data(s_disp, s_panel);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);

    uint32_t lv_stride = lv_draw_buf_width_to_stride(DEVOS_SCREEN_WIDTH, LV_COLOR_FORMAT_RGB565);
    s_lv_stride_px = (int)(lv_stride / 2);   /* RGB565: 2 bytes per pixel */
    size_t lv_buf_sz = (size_t)lv_stride * DEVOS_SCREEN_HEIGHT;
    s_lv_buf = heap_caps_aligned_alloc(64, lv_buf_sz, MALLOC_CAP_SPIRAM);
    if (!s_lv_buf) {
        ESP_LOGE(TAG, "Failed to allocate %zu-byte LVGL frame buffer from PSRAM!", lv_buf_sz);
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(s_disp, s_lv_buf, NULL, lv_buf_sz, LV_DISPLAY_RENDER_MODE_DIRECT);
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
