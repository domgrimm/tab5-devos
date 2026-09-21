#include "devos_top_bar.h"
#include "devos_theme.h"
#include "devos_core.h"
#include "devos_config.h"
#include <stdio.h>

static lv_obj_t *top_bar_container = NULL;
static lv_obj_t *btn_home = NULL;
static lv_obj_t *lbl_home = NULL;
static lv_obj_t *lbl_wifi = NULL;
static lv_obj_t *lbl_tailscale = NULL;
static lv_obj_t *btn_theme = NULL;
static lv_obj_t *lbl_theme = NULL;
static lv_obj_t *lbl_battery = NULL;
static lv_obj_t *lbl_clock = NULL;

static void home_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_core_switch_app(DEVOS_APP_LAUNCHER);
}

static void theme_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_theme_toggle();
}

static void on_theme_change(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    if (!top_bar_container) return;

    lv_obj_set_style_bg_color(top_bar_container, p->top_bar_bg, 0);
    lv_obj_set_style_border_color(top_bar_container, p->surface_border, 0);

    /* Update button and label colors */
    lv_obj_set_style_bg_color(btn_home, p->surface_active, 0);
    lv_obj_set_style_text_color(lbl_home, p->accent_primary, 0);

    lv_obj_set_style_text_color(lbl_wifi, p->text_secondary, 0);
    lv_obj_set_style_text_color(lbl_tailscale, p->accent_secondary, 0);

    lv_obj_set_style_bg_color(btn_theme, p->surface, 0);
    lv_obj_set_style_border_color(btn_theme, p->surface_border, 0);
    lv_obj_set_style_text_color(lbl_theme, p->text_primary, 0);
    lv_label_set_text(lbl_theme, devos_theme_is_dark() ? (LV_SYMBOL_EYE_CLOSE " Dark") : (LV_SYMBOL_EYE_OPEN " Light"));

    lv_obj_set_style_text_color(lbl_battery, p->accent_secondary, 0);
    lv_obj_set_style_text_color(lbl_clock, p->text_primary, 0);
}

lv_obj_t *devos_top_bar_create(lv_obj_t *parent)
{
    const devos_palette_t *p = devos_theme_get();

    top_bar_container = lv_obj_create(parent);
    lv_obj_set_size(top_bar_container, DEVOS_SCREEN_WIDTH, DEVOS_TOP_BAR_HEIGHT);
    lv_obj_set_pos(top_bar_container, 0, 0);
    lv_obj_set_style_bg_color(top_bar_container, p->top_bar_bg, 0);
    lv_obj_set_style_border_color(top_bar_container, p->surface_border, 0);
    lv_obj_set_style_border_width(top_bar_container, 1, 0);
    lv_obj_set_style_border_side(top_bar_container, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(top_bar_container, 0, 0);
    lv_obj_set_style_pad_left(top_bar_container, 14, 0);
    lv_obj_set_style_pad_right(top_bar_container, 14, 0);
    lv_obj_set_style_pad_top(top_bar_container, 2, 0);
    lv_obj_set_style_pad_bottom(top_bar_container, 2, 0);
    lv_obj_clear_flag(top_bar_container, LV_OBJ_FLAG_SCROLLABLE);

    /* 1. Home / Logo Button */
    btn_home = lv_button_create(top_bar_container);
    lv_obj_set_size(btn_home, 90, 28);
    lv_obj_align(btn_home, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_home, p->surface_active, 0);
    lv_obj_set_style_radius(btn_home, 4, 0);
    lv_obj_set_style_pad_all(btn_home, 0, 0);
    lv_obj_add_event_cb(btn_home, home_btn_cb, LV_EVENT_CLICKED, NULL);

    lbl_home = lv_label_create(btn_home);
    lv_label_set_text(lbl_home, LV_SYMBOL_HOME " devOS");
    lv_obj_center(lbl_home);
    lv_obj_set_style_text_color(lbl_home, p->accent_primary, 0);
    lv_obj_set_style_text_font(lbl_home, &lv_font_montserrat_14, 0);

    /* 2. Wi-Fi Status */
    lbl_wifi = lv_label_create(top_bar_container);
    lv_label_set_text(lbl_wifi, "WiFi: DevNet -58dBm");
    lv_obj_align(lbl_wifi, LV_ALIGN_LEFT_MID, 110, 0);
    lv_obj_set_style_text_color(lbl_wifi, p->text_secondary, 0);
    lv_obj_set_style_text_font(lbl_wifi, &lv_font_montserrat_12, 0);

    /* 3. Tailscale IP */
    lbl_tailscale = lv_label_create(top_bar_container);
    lv_label_set_text(lbl_tailscale, LV_SYMBOL_BULLET " Tailscale: 100.77.11.92");
    lv_obj_align(lbl_tailscale, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(lbl_tailscale, p->accent_secondary, 0);
    lv_obj_set_style_text_font(lbl_tailscale, &lv_font_montserrat_14, 0);

    /* 4. Clock (Far Right) */
    lbl_clock = lv_label_create(top_bar_container);
    lv_label_set_text(lbl_clock, "14:28");
    lv_obj_align(lbl_clock, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_color(lbl_clock, p->text_primary, 0);
    lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_14, 0);

    /* 5. Battery Status */
    lbl_battery = lv_label_create(top_bar_container);
    lv_label_set_text(lbl_battery, "94% " LV_SYMBOL_CHARGE);
    lv_obj_align_to(lbl_battery, lbl_clock, LV_ALIGN_OUT_LEFT_MID, -22, 0);
    lv_obj_set_style_text_color(lbl_battery, p->accent_secondary, 0);
    lv_obj_set_style_text_font(lbl_battery, &lv_font_montserrat_14, 0);

    /* 6. Theme Toggle Button */
    btn_theme = lv_button_create(top_bar_container);
    lv_obj_set_size(btn_theme, 84, 26);
    lv_obj_align_to(btn_theme, lbl_battery, LV_ALIGN_OUT_LEFT_MID, -20, 0);
    lv_obj_set_style_bg_color(btn_theme, p->surface, 0);
    lv_obj_set_style_border_color(btn_theme, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_theme, 1, 0);
    lv_obj_set_style_radius(btn_theme, 4, 0);
    lv_obj_set_style_pad_all(btn_theme, 0, 0);
    lv_obj_add_event_cb(btn_theme, theme_btn_cb, LV_EVENT_CLICKED, NULL);

    lbl_theme = lv_label_create(btn_theme);
    lv_label_set_text(lbl_theme, devos_theme_is_dark() ? (LV_SYMBOL_EYE_CLOSE " Dark") : (LV_SYMBOL_EYE_OPEN " Light"));
    lv_obj_center(lbl_theme);
    lv_obj_set_style_text_color(lbl_theme, p->text_primary, 0);
    lv_obj_set_style_text_font(lbl_theme, &lv_font_montserrat_12, 0);

    /* Register theme listener */
    devos_theme_add_listener(on_theme_change, NULL);

    return top_bar_container;
}

void devos_top_bar_update(void)
{
    if (!top_bar_container) return;

    const devos_telemetry_t *t = devos_telemetry_get();

    /* Wi-Fi */
    char buf[64];
    if (t->wifi_connected) {
        snprintf(buf, sizeof(buf), "WiFi: %s (%ddBm)", t->wifi_ssid, t->wifi_rssi);
    } else {
        snprintf(buf, sizeof(buf), "WiFi: Disconnected");
    }
    lv_label_set_text(lbl_wifi, buf);

    /* Tailscale */
    if (t->tailscale_online) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_BULLET " Tailscale: %s", t->tailscale_ip);
    } else {
        snprintf(buf, sizeof(buf), "- Tailscale: Offline");
    }
    lv_label_set_text(lbl_tailscale, buf);

    /* Battery */
    snprintf(buf, sizeof(buf), "%d%%%s", t->battery_percent, t->battery_charging ? " " LV_SYMBOL_CHARGE : "");
    lv_label_set_text(lbl_battery, buf);

    /* Clock */
    snprintf(buf, sizeof(buf), "%02d:%02d", t->rtc_hour, t->rtc_min);
    lv_label_set_text(lbl_clock, buf);
}
