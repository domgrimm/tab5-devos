#include "devos_top_bar.h"
#include "devos_theme.h"
#include "devos_core.h"
#include "devos_config.h"
#include <stdio.h>

static lv_obj_t *top_bar_container = NULL;
static lv_obj_t *btn_home = NULL;
static lv_obj_t *lbl_home = NULL;
static lv_obj_t *lbl_wifi = NULL;
static lv_obj_t *box_ip = NULL;
static lv_obj_t *icon_tailscale = NULL;
static lv_obj_t *lbl_ip = NULL;
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

static void tailscale_icon_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_core_switch_app(DEVOS_APP_TAILSCALE);
}

static void tailscale_icon_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    const devos_palette_t *p = devos_theme_get();

    /* Tailscale 3x3 dot matrix logo (16x16 area):
     * Active dots (solid): middle row (r=1) and bottom center (r=2, c=1)
     * Inactive dots (faint): remainder of 3x3 grid
     */
    const int dot_size = 4;
    const int step = 6;

    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            bool active = (r == 1) || (r == 2 && c == 1);

            lv_draw_rect_dsc_t rect_dsc;
            lv_draw_rect_dsc_init(&rect_dsc);
            rect_dsc.radius = LV_RADIUS_CIRCLE;
            rect_dsc.bg_color = active ? p->accent_secondary : p->text_muted;
            rect_dsc.bg_opa = active ? LV_OPA_COVER : LV_OPA_40;
            rect_dsc.border_width = 0;

            lv_area_t dot_area;
            dot_area.x1 = coords.x1 + c * step;
            dot_area.y1 = coords.y1 + r * step;
            dot_area.x2 = dot_area.x1 + dot_size - 1;
            dot_area.y2 = dot_area.y1 + dot_size - 1;

            lv_draw_rect(layer, &rect_dsc, &dot_area);
        }
    }
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
    lv_obj_set_style_text_color(lbl_ip, p->accent_secondary, 0);
    if (icon_tailscale) {
        lv_obj_invalidate(icon_tailscale);
    }

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
    lv_obj_set_style_pad_left(top_bar_container, 12, 0);
    lv_obj_set_style_pad_right(top_bar_container, 12, 0);
    lv_obj_set_style_pad_top(top_bar_container, 4, 0);
    lv_obj_set_style_pad_bottom(top_bar_container, 4, 0);
    lv_obj_clear_flag(top_bar_container, LV_OBJ_FLAG_SCROLLABLE);

    /* 1. devOS Home Trigger */
    btn_home = lv_button_create(top_bar_container);
    lv_obj_set_size(btn_home, 84, 28);
    lv_obj_align(btn_home, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_home, p->surface_active, 0);
    lv_obj_set_style_border_color(btn_home, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_home, 1, 0);
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

    /* 3. Local Network IP + Optional Tailscale Status Icon */
    box_ip = lv_obj_create(top_bar_container);
    lv_obj_set_size(box_ip, LV_SIZE_CONTENT, 24);
    lv_obj_align(box_ip, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(box_ip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box_ip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(box_ip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box_ip, 0, 0);
    lv_obj_set_style_pad_all(box_ip, 0, 0);
    lv_obj_set_style_pad_column(box_ip, 8, 0);
    lv_obj_clear_flag(box_ip, LV_OBJ_FLAG_SCROLLABLE);

    /* Tailscale 3x3 Dot Matrix Icon (16x16) */
    icon_tailscale = lv_obj_create(box_ip);
    lv_obj_set_size(icon_tailscale, 16, 16);
    lv_obj_set_style_bg_opa(icon_tailscale, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(icon_tailscale, 0, 0);
    lv_obj_set_style_pad_all(icon_tailscale, 0, 0);
    lv_obj_clear_flag(icon_tailscale, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(icon_tailscale, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(icon_tailscale, 10);
    lv_obj_add_event_cb(icon_tailscale, tailscale_icon_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(icon_tailscale, tailscale_icon_click_cb, LV_EVENT_CLICKED, NULL);

    /* Local IP Label */
    lbl_ip = lv_label_create(box_ip);
    lv_label_set_text(lbl_ip, "IP: 10.2.132.54");
    lv_obj_set_style_text_color(lbl_ip, p->accent_secondary, 0);
    lv_obj_set_style_text_font(lbl_ip, &lv_font_montserrat_14, 0);

    const devos_telemetry_t *init_t = devos_telemetry_get();
    if (!init_t || !init_t->tailscale_online) {
        lv_obj_add_flag(icon_tailscale, LV_OBJ_FLAG_HIDDEN);
    }

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

    /* Tailscale Icon Visibility next to Local IP */
    if (icon_tailscale) {
        if (t->tailscale_online) {
            lv_obj_remove_flag(icon_tailscale, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(icon_tailscale, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Local Network IP (Always shown, whether Tailscale is connected or not) */
    if (t->local_ip[0] != '\0') {
        snprintf(buf, sizeof(buf), "IP: %s", t->local_ip);
    } else if (t->wifi_connected) {
        snprintf(buf, sizeof(buf), "IP: DHCP...");
    } else {
        snprintf(buf, sizeof(buf), "IP: Offline");
    }
    lv_label_set_text(lbl_ip, buf);

    /* Battery */
    snprintf(buf, sizeof(buf), "%d%%%s", t->battery_percent, t->battery_charging ? " " LV_SYMBOL_CHARGE : "");
    lv_label_set_text(lbl_battery, buf);

    /* Clock */
    snprintf(buf, sizeof(buf), "%02d:%02d", t->rtc_hour, t->rtc_min);
    lv_label_set_text(lbl_clock, buf);
}
