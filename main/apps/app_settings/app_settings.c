#include "app_settings.h"
#include "devos_config.h"
#include "devos_theme.h"
#include <stdio.h>

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;

/* Theme-tracked widgets (statics so apply_theme can repaint them) */
static lv_obj_t *card_disp = NULL;
static lv_obj_t *lbl_d = NULL;
static lv_obj_t *lbl_desc = NULL;
static lv_obj_t *theme_switch = NULL;
static lv_obj_t *card_pwr = NULL;
static lv_obj_t *lbl_p = NULL;
static lv_obj_t *lbl_pwr_info = NULL;
static lv_obj_t *card_hw = NULL;
static lv_obj_t *lbl_h = NULL;
static lv_obj_t *lbl_hw_info = NULL;

static void apply_theme(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    if (!screen) return;

    lv_obj_set_style_bg_color(screen, p->bg, 0);

    if (card_disp) {
        lv_obj_set_style_bg_color(card_disp, p->surface, 0);
        lv_obj_set_style_border_color(card_disp, p->surface_border, 0);
    }
    if (lbl_d) lv_obj_set_style_text_color(lbl_d, p->accent_primary, 0);
    if (lbl_desc) lv_obj_set_style_text_color(lbl_desc, p->text_secondary, 0);
    if (theme_switch) {
        /* ponytail: knob = text_primary reads on both tracks; sync knob with Fn+T too */
        lv_obj_set_style_bg_color(theme_switch, p->surface_active,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(theme_switch, p->surface_border,
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(theme_switch, p->accent_primary,
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(theme_switch, p->text_primary,
                                  LV_PART_KNOB | LV_STATE_DEFAULT);
        if (devos_theme_get_type() == DEVOS_THEME_LIGHT) {
            lv_obj_add_state(theme_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(theme_switch, LV_STATE_CHECKED);
        }
    }

    if (card_pwr) {
        lv_obj_set_style_bg_color(card_pwr, p->surface, 0);
        lv_obj_set_style_border_color(card_pwr, p->surface_border, 0);
    }
    if (lbl_p) lv_obj_set_style_text_color(lbl_p, p->accent_secondary, 0);
    if (lbl_pwr_info) lv_obj_set_style_text_color(lbl_pwr_info, p->text_primary, 0);

    if (card_hw) {
        lv_obj_set_style_bg_color(card_hw, p->surface, 0);
        lv_obj_set_style_border_color(card_hw, p->surface_border, 0);
    }
    if (lbl_h) lv_obj_set_style_text_color(lbl_h, p->accent_primary, 0);
    if (lbl_hw_info) lv_obj_set_style_text_color(lbl_hw_info, p->text_primary, 0);
}

static void theme_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    devos_theme_set(lv_obj_has_state(sw, LV_STATE_CHECKED)
                    ? DEVOS_THEME_LIGHT : DEVOS_THEME_DARK);
}

/* Sun icon: core + 8 rays. Moon icon: crescent via offset cut-out.
 * Both read the live palette at draw time, so no theme listener needed. */
static void sun_icon_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    const devos_palette_t *p = devos_theme_get();

    int cx = (coords.x1 + coords.x2) / 2;
    int cy = (coords.y1 + coords.y2) / 2;

    /* Rays */
    static const int8_t dirs[8][2] = {
        {0, -1}, {1, -1}, {1, 0}, {1, 1},
        {0, 1}, {-1, 1}, {-1, 0}, {-1, -1},
    };
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = p->accent_warning;
    line_dsc.width = 2;
    line_dsc.round_end = 1;
    for (int i = 0; i < 8; i++) {
        line_dsc.p1.x = cx + dirs[i][0] * 8;
        line_dsc.p1.y = cy + dirs[i][1] * 8;
        line_dsc.p2.x = cx + dirs[i][0] * 11;
        line_dsc.p2.y = cy + dirs[i][1] * 11;
        lv_draw_line(layer, &line_dsc);
    }

    /* Core */
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.radius = LV_RADIUS_CIRCLE;
    rect_dsc.bg_color = p->accent_warning;
    rect_dsc.bg_opa = LV_OPA_COVER;
    rect_dsc.border_width = 0;
    lv_area_t core = {cx - 5, cy - 5, cx + 5, cy + 5};
    lv_draw_rect(layer, &rect_dsc, &core);
}

static void moon_icon_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    const devos_palette_t *p = devos_theme_get();

    int cx = (coords.x1 + coords.x2) / 2;
    int cy = (coords.y1 + coords.y2) / 2;

    /* Full disc */
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.radius = LV_RADIUS_CIRCLE;
    rect_dsc.bg_color = p->text_primary;
    rect_dsc.bg_opa = LV_OPA_COVER;
    rect_dsc.border_width = 0;
    lv_area_t disc = {cx - 7, cy - 7, cx + 7, cy + 7};
    lv_draw_rect(layer, &rect_dsc, &disc);

    /* Offset cut-out in the card bg color -> crescent */
    rect_dsc.bg_color = p->surface;
    lv_area_t cut = {cx - 3, cy - 9, cx + 9, cy + 3};
    lv_draw_rect(layer, &rect_dsc, &cut);
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
    card_disp = lv_obj_create(screen);
    lv_obj_set_size(card_disp, (DEVOS_SCREEN_WIDTH - 48) / 2, 220);
    lv_obj_set_pos(card_disp, 0, 0);
    lv_obj_set_style_bg_color(card_disp, p->surface, 0);
    lv_obj_set_style_border_color(card_disp, p->surface_border, 0);
    lv_obj_set_style_border_width(card_disp, 1, 0);
    lv_obj_set_style_radius(card_disp, 6, 0);
    lv_obj_set_style_pad_all(card_disp, 14, 0);

    lbl_d = lv_label_create(card_disp);
    lv_label_set_text(lbl_d, "DISPLAY & PALETTES");
    lv_obj_set_style_text_font(lbl_d, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_d, p->accent_primary, 0);

    lbl_desc = lv_label_create(card_disp);
    lv_label_set_text(lbl_desc, "Toggle between Dark Cyberdeck and High-Contrast Light mode.");
    lv_obj_set_pos(lbl_desc, 0, 26);
    lv_obj_set_style_text_color(lbl_desc, p->text_secondary, 0);

    /* Theme switch row: [moon] ( switch ) [sun], knob side = active mode */
    lv_obj_t *moon_icon = lv_obj_create(card_disp);
    lv_obj_set_size(moon_icon, 28, 28);
    lv_obj_set_pos(moon_icon, 0, 60);
    lv_obj_set_style_bg_opa(moon_icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(moon_icon, 0, 0);
    lv_obj_set_style_pad_all(moon_icon, 0, 0);
    lv_obj_clear_flag(moon_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(moon_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(moon_icon, moon_icon_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    theme_switch = lv_switch_create(card_disp);
    lv_obj_set_size(theme_switch, 64, 32);
    lv_obj_set_pos(theme_switch, 36, 58);
    lv_obj_set_style_border_width(theme_switch, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(theme_switch, LV_RADIUS_CIRCLE, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(theme_switch, 3, LV_PART_KNOB | LV_STATE_DEFAULT);
    if (devos_theme_get_type() == DEVOS_THEME_LIGHT) {
        lv_obj_add_state(theme_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(theme_switch, theme_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *sun_icon = lv_obj_create(card_disp);
    lv_obj_set_size(sun_icon, 28, 28);
    lv_obj_set_pos(sun_icon, 108, 60);
    lv_obj_set_style_bg_opa(sun_icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sun_icon, 0, 0);
    lv_obj_set_style_pad_all(sun_icon, 0, 0);
    lv_obj_clear_flag(sun_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(sun_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sun_icon, sun_icon_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    /* Section 2: Power & Battery (INA226) */
    card_pwr = lv_obj_create(screen);
    lv_obj_set_size(card_pwr, (DEVOS_SCREEN_WIDTH - 48) / 2, 220);
    lv_obj_set_pos(card_pwr, (DEVOS_SCREEN_WIDTH - 48) / 2 + 16, 0);
    lv_obj_set_style_bg_color(card_pwr, p->surface, 0);
    lv_obj_set_style_border_color(card_pwr, p->surface_border, 0);
    lv_obj_set_style_border_width(card_pwr, 1, 0);
    lv_obj_set_style_radius(card_pwr, 6, 0);
    lv_obj_set_style_pad_all(card_pwr, 14, 0);

    lbl_p = lv_label_create(card_pwr);
    lv_label_set_text(lbl_p, "POWER & TELEMETRY (TI INA226)");
    lv_obj_set_style_text_font(lbl_p, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_p, p->accent_secondary, 0);

    lbl_pwr_info = lv_label_create(card_pwr);
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
    card_hw = lv_obj_create(screen);
    lv_obj_set_size(card_hw, DEVOS_SCREEN_WIDTH - 32, 200);
    lv_obj_set_pos(card_hw, 0, 236);
    lv_obj_set_style_bg_color(card_hw, p->surface, 0);
    lv_obj_set_style_border_color(card_hw, p->surface_border, 0);
    lv_obj_set_style_border_width(card_hw, 1, 0);
    lv_obj_set_style_radius(card_hw, 6, 0);
    lv_obj_set_style_pad_all(card_hw, 14, 0);

    lbl_h = lv_label_create(card_hw);
    lv_label_set_text(lbl_h, "SYSTEM HARDWARE & MEMORY ARCHITECTURE");
    lv_obj_set_style_text_font(lbl_h, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_h, p->accent_primary, 0);

    lbl_hw_info = lv_label_create(card_hw);
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

    devos_theme_add_listener(apply_theme, NULL);
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
