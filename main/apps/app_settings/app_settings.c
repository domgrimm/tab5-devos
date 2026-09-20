#include "app_settings.h"
#include "devos_config.h"
#include "devos_theme.h"
#include <stdio.h>

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;

static void btn_theme_toggle_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_theme_toggle();
}

static void settings_init(void)
{
    const devos_palette_t *p = devos_theme_get();

    screen = lv_obj_create(lv_screen_active());
    app_descriptor.screen = screen;
    lv_obj_set_size(screen, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(screen, 0, DEVOS_TOP_BAR_HEIGHT);
    lv_obj_set_style_bg_color(screen, p->bg, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 16, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);

    /* Section 1: Display & Themes */
    lv_obj_t *card_disp = lv_obj_create(screen);
    lv_obj_set_size(card_disp, (DEVOS_SCREEN_WIDTH - 48) / 2, 220);
    lv_obj_set_pos(card_disp, 0, 0);
    lv_obj_set_style_bg_color(card_disp, p->surface, 0);
    lv_obj_set_style_border_color(card_disp, p->surface_border, 0);
    lv_obj_set_style_border_width(card_disp, 1, 0);
    lv_obj_set_style_radius(card_disp, 6, 0);
    lv_obj_set_style_pad_all(card_disp, 14, 0);

    lv_obj_t *lbl_d = lv_label_create(card_disp);
    lv_label_set_text(lbl_d, "DISPLAY & PALETTES");
    lv_obj_set_style_text_font(lbl_d, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_d, p->accent_primary, 0);

    lv_obj_t *lbl_desc = lv_label_create(card_disp);
    lv_label_set_text(lbl_desc, "Toggle between Dark Cyberdeck and High-Contrast Light mode.");
    lv_obj_set_pos(lbl_desc, 0, 26);
    lv_obj_set_style_text_color(lbl_desc, p->text_secondary, 0);

    lv_obj_t *btn_theme = lv_button_create(card_disp);
    lv_obj_set_size(btn_theme, 200, 38);
    lv_obj_set_pos(btn_theme, 0, 60);
    lv_obj_set_style_bg_color(btn_theme, p->surface_active, 0);
    lv_obj_set_style_border_color(btn_theme, p->accent_primary, 0);
    lv_obj_set_style_border_width(btn_theme, 1, 0);
    lv_obj_set_style_radius(btn_theme, 4, 0);
    lv_obj_add_event_cb(btn_theme, btn_theme_toggle_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_bt = lv_label_create(btn_theme);
    lv_label_set_text(lbl_bt, "Toggle Theme (Fn + T)");
    lv_obj_center(lbl_bt);
    lv_obj_set_style_text_color(lbl_bt, p->accent_primary, 0);

    /* Section 2: Power & Battery (INA226) */
    lv_obj_t *card_pwr = lv_obj_create(screen);
    lv_obj_set_size(card_pwr, (DEVOS_SCREEN_WIDTH - 48) / 2, 220);
    lv_obj_set_pos(card_pwr, (DEVOS_SCREEN_WIDTH - 48) / 2 + 16, 0);
    lv_obj_set_style_bg_color(card_pwr, p->surface, 0);
    lv_obj_set_style_border_color(card_pwr, p->surface_border, 0);
    lv_obj_set_style_border_width(card_pwr, 1, 0);
    lv_obj_set_style_radius(card_pwr, 6, 0);
    lv_obj_set_style_pad_all(card_pwr, 14, 0);

    lv_obj_t *lbl_p = lv_label_create(card_pwr);
    lv_label_set_text(lbl_p, "POWER & TELEMETRY (TI INA226)");
    lv_obj_set_style_text_font(lbl_p, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_p, p->accent_secondary, 0);

    lv_obj_t *lbl_pwr_info = lv_label_create(card_pwr);
    lv_label_set_text(lbl_pwr_info,
        "Battery Pack: NP-F550 Li-ion 7.4V (2200 mAh)\n"
        "Bus Voltage:  7.82 V\n"
        "Current Draw: -410 mA\n"
        "Power Draw:   3.21 W\n"
        "Capacity:     94% (Remaining: ~4.8 hours)\n"
        "Standby Mode: Dynamic throttle to 160MHz after 2m");
    lv_obj_set_pos(lbl_pwr_info, 0, 26);
    lv_obj_set_style_text_font(lbl_pwr_info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_pwr_info, p->text_primary, 0);

    /* Section 3: Hardware & Storage Info */
    lv_obj_t *card_hw = lv_obj_create(screen);
    lv_obj_set_size(card_hw, DEVOS_SCREEN_WIDTH - 32, 200);
    lv_obj_set_pos(card_hw, 0, 236);
    lv_obj_set_style_bg_color(card_hw, p->surface, 0);
    lv_obj_set_style_border_color(card_hw, p->surface_border, 0);
    lv_obj_set_style_border_width(card_hw, 1, 0);
    lv_obj_set_style_radius(card_hw, 6, 0);
    lv_obj_set_style_pad_all(card_hw, 14, 0);

    lv_obj_t *lbl_h = lv_label_create(card_hw);
    lv_label_set_text(lbl_h, "SYSTEM HARDWARE & MEMORY ARCHITECTURE");
    lv_obj_set_style_text_font(lbl_h, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_h, p->accent_primary, 0);

    lv_obj_t *lbl_hw_info = lv_label_create(card_hw);
    lv_label_set_text(lbl_hw_info,
        "SoC: ESP32-P4 Dual-Core RISC-V @ 400 MHz  |  Co-SoC: ESP32-C6 (SDIO ESP-Hosted Wi-Fi 6)\n"
        "Display: 5.0\" 1280x720 MIPI-DSI ST7123  |  Touch: Goodix GT911 (I2C)\n"
        "Keyboard: A164 70-Key Matrix (STM32F030 Ext.Port1 I2C @ 0x6D, INT GPIO 50)\n"
        "Storage: MicroSD Slot mounted at /sdcard (FATFS VFS) - 29.4 GB Free / 31.2 GB Total\n"
        "Memory: 32 MB External PSRAM (Allocated: LVGL buffers, terminal scrollback, markdown AST)\n"
        "Internal SRAM: 428 KB Free (Reserved >120 KB contiguous for mbedTLS / SSH handshakes)");
    lv_obj_set_pos(lbl_hw_info, 0, 26);
    lv_obj_set_style_text_font(lbl_hw_info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_hw_info, p->text_primary, 0);
}

static void settings_show(void) {}
static void settings_hide(void) {}

devos_app_descriptor_t *app_settings_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_SETTINGS;
    app_descriptor.name = "Settings";
    app_descriptor.title = "Settings";
    app_descriptor.subtitle = "System & Hardware Configuration";
    app_descriptor.screen = screen;
    app_descriptor.init = settings_init;
    app_descriptor.show = settings_show;
    app_descriptor.hide = settings_hide;
    app_descriptor.handle_key = NULL;

    return &app_descriptor;
}
