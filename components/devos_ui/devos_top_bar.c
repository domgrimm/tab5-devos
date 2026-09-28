#include "devos_top_bar.h"
#include "devos_theme.h"
#include "devos_icons.h"
#include "devos_core.h"
#include "devos_config.h"
#include "devos_fileshare.h"
#include <stdio.h>

static lv_obj_t *top_bar_container = NULL;
static lv_obj_t *btn_home = NULL;
static lv_obj_t *lbl_home = NULL;
static lv_obj_t *lbl_wifi = NULL;
static lv_obj_t *box_ip = NULL;
static lv_obj_t *icon_tailscale = NULL;
static lv_obj_t *icon_wireguard = NULL;
static lv_obj_t *lbl_ip = NULL;
static lv_obj_t *lbl_battery = NULL;
static lv_obj_t *lbl_share = NULL;         /* the SD card is shared (Settings > File Sharing) */
static lv_obj_t *lbl_clock = NULL;

static void home_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_core_switch_app(DEVOS_APP_LAUNCHER);
}

static void wifi_label_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_core_switch_app(DEVOS_APP_SETTINGS);
}

static void tailscale_icon_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_core_switch_app(DEVOS_APP_TAILSCALE);
}

static void wireguard_icon_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_core_switch_app(DEVOS_APP_WIREGUARD);
}

/* The Tailscale / WireGuard marks next to the IP (devos_icons, as on the
 * launcher tiles). */
static void wireguard_icon_draw_cb(lv_event_t *e)
{
    lv_area_t c;
    lv_obj_get_coords(lv_event_get_current_target(e), &c);
    devos_icon_wireguard(lv_event_get_layer(e), &c, lv_color_white());
}

static void tailscale_icon_draw_cb(lv_event_t *e)
{
    lv_area_t c;
    lv_obj_get_coords(lv_event_get_current_target(e), &c);
    devos_icon_tailscale(lv_event_get_layer(e), &c, devos_theme_get()->accent_secondary);
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

    lv_obj_set_style_text_color(lbl_battery, p->accent_secondary, 0);
    lv_obj_set_style_text_color(lbl_clock, p->text_primary, 0);
    lv_obj_set_style_text_color(lbl_share, p->accent_warning, 0);
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
    lv_obj_set_size(btn_home, 88, 30);
    /* Touches this close to the panel edge are reported well below where the
     * finger is (logged taps on this button landed at y~43-64 vs a 5-33 px
     * button), so extend the hit area generously; it spills a little into the
     * top-left of the app area, which is where "go home" belongs anyway. */
    lv_obj_set_ext_click_area(btn_home, 28);
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
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI " --");
    lv_label_set_long_mode(lbl_wifi, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_wifi, 360);
    lv_obj_align(lbl_wifi, LV_ALIGN_LEFT_MID, 100, 0);
    lv_obj_set_style_text_color(lbl_wifi, p->text_secondary, 0);
    lv_obj_set_style_text_font(lbl_wifi, &lv_font_montserrat_12, 0);
    lv_obj_add_flag(lbl_wifi, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(lbl_wifi, 16);
    lv_obj_add_event_cb(lbl_wifi, wifi_label_click_cb, LV_EVENT_CLICKED, NULL);

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
    lv_obj_set_ext_click_area(icon_tailscale, 16);
    lv_obj_add_event_cb(icon_tailscale, tailscale_icon_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(icon_tailscale, tailscale_icon_click_cb, LV_EVENT_CLICKED, NULL);

    /* WireGuard mark (16x16), shown while a tunnel is up */
    icon_wireguard = lv_obj_create(box_ip);
    lv_obj_set_size(icon_wireguard, 16, 16);
    lv_obj_set_style_bg_opa(icon_wireguard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(icon_wireguard, 0, 0);
    lv_obj_set_style_pad_all(icon_wireguard, 0, 0);
    lv_obj_clear_flag(icon_wireguard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(icon_wireguard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(icon_wireguard, 16);
    lv_obj_add_event_cb(icon_wireguard, wireguard_icon_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(icon_wireguard, wireguard_icon_click_cb, LV_EVENT_CLICKED, NULL);

    /* Local IP Label */
    lbl_ip = lv_label_create(box_ip);
    lv_label_set_text(lbl_ip, "IP: Offline");
    lv_obj_set_style_text_color(lbl_ip, p->accent_secondary, 0);
    lv_obj_set_style_text_font(lbl_ip, &lv_font_montserrat_14, 0);

    const devos_telemetry_t *init_t = devos_telemetry_get();
    if (!init_t || !init_t->tailscale_online) {
        lv_obj_add_flag(icon_tailscale, LV_OBJ_FLAG_HIDDEN);
    }
    if (!init_t || !init_t->wireguard_online) {
        lv_obj_add_flag(icon_wireguard, LV_OBJ_FLAG_HIDDEN);
    }

    /* 4. Clock (Far Right) */
    lbl_clock = lv_label_create(top_bar_container);
    lv_label_set_text(lbl_clock, "--:--");
    lv_obj_align(lbl_clock, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_color(lbl_clock, p->text_primary, 0);
    lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_14, 0);

    /* 5. Battery Status */
    lbl_battery = lv_label_create(top_bar_container);
    lv_label_set_text(lbl_battery, "--%");
    lv_obj_align_to(lbl_battery, lbl_clock, LV_ALIGN_OUT_LEFT_MID, -22, 0);
    lv_obj_set_style_text_color(lbl_battery, p->accent_secondary, 0);
    lv_obj_set_style_text_font(lbl_battery, &lv_font_montserrat_14, 0);

    /* 6. File sharing on: amber SD card left of the battery; tap for Settings */
    lbl_share = lv_label_create(top_bar_container);
    lv_label_set_text(lbl_share, LV_SYMBOL_SD_CARD " Shared");
    lv_obj_set_style_text_color(lbl_share, p->accent_warning, 0);
    lv_obj_set_style_text_font(lbl_share, &lv_font_montserrat_14, 0);
    lv_obj_add_flag(lbl_share, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_ext_click_area(lbl_share, 16);
    lv_obj_add_event_cb(lbl_share, wifi_label_click_cb, LV_EVENT_CLICKED, NULL);

    /* Register theme listener */
    devos_theme_add_listener(on_theme_change, NULL);

    return top_bar_container;
}

void devos_top_bar_update(void)
{
    if (!top_bar_container) return;

    const devos_telemetry_t *t = devos_telemetry_get();
    const devos_palette_t *p = devos_theme_get();
    char buf[80];

    /* Wi-Fi (states mirror devos_wifi_state_t: 0 off, 1 idle, 2 connecting,
     * 3 connected, 4 failed). Tap opens Settings. */
    lv_color_t wifi_col = p->text_secondary;
    switch (t->wifi_state) {
    case 3:
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %s  %d dBm", t->wifi_ssid, t->wifi_rssi);
        break;
    case 2:
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " Connecting to %s...", t->wifi_ssid);
        wifi_col = p->accent_warning;
        break;
    case 4:
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " Wi-Fi: connection failed");
        wifi_col = p->accent_danger;
        break;
    case 0:
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " Wi-Fi unavailable");
        wifi_col = p->text_muted;
        break;
    default:
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " Wi-Fi: not connected");
        wifi_col = p->text_muted;
        break;
    }
    lv_label_set_text(lbl_wifi, buf);
    lv_obj_set_style_text_color(lbl_wifi, wifi_col, 0);

    /* Tailscale Icon Visibility next to Local IP */
    if (icon_tailscale) {
        if (t->tailscale_online) {
            lv_obj_remove_flag(icon_tailscale, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(icon_tailscale, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* WireGuard mark next to the IP while a tunnel is up */
    if (icon_wireguard) {
        if (t->wireguard_online) lv_obj_remove_flag(icon_wireguard, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(icon_wireguard, LV_OBJ_FLAG_HIDDEN);
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
    lv_color_t bat_col = p->accent_secondary;
    if (!t->battery_valid) {
        snprintf(buf, sizeof(buf), "--%%");
        bat_col = p->text_muted;
    } else if (!t->battery_present) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_USB " USB");
    } else {
        const char *sym = t->battery_percent >= 90 ? LV_SYMBOL_BATTERY_FULL
                        : t->battery_percent >= 65 ? LV_SYMBOL_BATTERY_3
                        : t->battery_percent >= 40 ? LV_SYMBOL_BATTERY_2
                        : t->battery_percent >= 15 ? LV_SYMBOL_BATTERY_1
                        : LV_SYMBOL_BATTERY_EMPTY;
        snprintf(buf, sizeof(buf), "%s %d%%%s", sym, t->battery_percent,
                 t->battery_charging ? " " LV_SYMBOL_CHARGE : "");
        if (t->battery_percent < 15 && !t->battery_charging) bat_col = p->accent_danger;
    }
    lv_label_set_text(lbl_battery, buf);
    lv_obj_set_style_text_color(lbl_battery, bat_col, 0);

    /* Clock */
    if (t->time_valid) {
        snprintf(buf, sizeof(buf), "%02d:%02d", t->rtc_hour, t->rtc_min);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    lv_label_set_text(lbl_clock, buf);
    /* Battery sits left of the clock; re-anchor as both widths change. */
    lv_obj_align_to(lbl_battery, lbl_clock, LV_ALIGN_OUT_LEFT_MID, -22, 0);

    if (devos_fileshare_running()) {
        lv_obj_remove_flag(lbl_share, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align_to(lbl_share, lbl_battery, LV_ALIGN_OUT_LEFT_MID, -22, 0);
    } else {
        lv_obj_add_flag(lbl_share, LV_OBJ_FLAG_HIDDEN);
    }
}
