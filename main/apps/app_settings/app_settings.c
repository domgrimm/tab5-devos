/* Settings: Wi-Fi, display, power, date & time, system info.
 *
 * Every value shown here is live (devos_net, devos_sysmon, devos_power, the
 * BSP); nothing is hard-coded. Layout: a section list on the left, one panel
 * per section on the right. Styling uses shared lv_style_t objects that are
 * re-coloured on theme change (lv_obj_report_style_change), so new widgets
 * follow the theme without per-widget bookkeeping.
 *
 * Keyboard: Up/Down pick a section; Tab/Right moves into the Wi-Fi network
 * list (Up/Down/Enter), Left/Esc goes back; S rescans. In the password dialog
 * the physical keyboard types, Enter connects, Esc cancels. An on-screen
 * keyboard covers the touch-only case.
 */
#include "app_settings.h"
#include "devos_config.h"
#include "devos_theme.h"
#include "devos_power.h"
#include "devos_ota.h"
#include "devos_net.h"
#include "devos_sysmon.h"
#include "bsp_tab5.h"
#include "tab5_keyboard.h"
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "sdkconfig.h"
#endif

enum { SEC_WIFI = 0, SEC_DISPLAY, SEC_POWER, SEC_TIME, SEC_SYSTEM, SEC_COUNT };

static const char *const s_sec_labels[SEC_COUNT] = {
    LV_SYMBOL_WIFI "   Wi-Fi",
    LV_SYMBOL_EYE_OPEN "   Display",
    LV_SYMBOL_BATTERY_FULL "   Power",
    LV_SYMBOL_BELL "   Date & Time",
    LV_SYMBOL_SETTINGS "   System",
};

#define NAV_W        240
#define CONTENT_W    (DEVOS_SCREEN_WIDTH - NAV_W)
#define PANEL_PAD    20
#define PANEL_W      (CONTENT_W - 2 * PANEL_PAD)
#define PANEL_H      (DEVOS_CONTENT_HEIGHT - 2 * PANEL_PAD)

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;

/* ---- styles ---- */
static bool s_styles_ready = false;
static lv_style_t st_screen, st_nav, st_nav_btn, st_nav_sel, st_card, st_header, st_title,
                  st_text, st_muted, st_btn, st_btn_primary, st_btn_danger, st_row, st_row_sel,
                  st_ta, st_ta_focus, st_overlay, st_modal, st_kb, st_kb_btn, st_track,
                  st_indicator, st_knob, st_ok, st_warn, st_err, st_dd_list, st_dd_sel;

/* ---- navigation ---- */
static lv_obj_t *nav_btns[SEC_COUNT];
static lv_obj_t *panels[SEC_COUNT];
static int s_section = SEC_WIFI;
static bool s_focus_list = false;      /* keyboard focus in the Wi-Fi network list */

/* ---- Wi-Fi panel ---- */
static lv_obj_t *lbl_wifi_state, *lbl_wifi_detail, *lbl_wifi_error;
static lv_obj_t *btn_wifi_disconnect, *btn_wifi_scan, *lbl_scan_btn;
static lv_obj_t *list_avail, *lbl_avail_hdr, *list_saved;
static devos_wifi_ap_t s_aps[DEVOS_WIFI_MAX_SCAN];
static int s_ap_count = 0;
static lv_obj_t *s_ap_rows[DEVOS_WIFI_MAX_SCAN];
static int s_sel_row = 0;
static uint32_t s_last_gen = 0xFFFFFFFFu;
static char s_last_list_key[80] = "";
static devos_wifi_saved_t s_saved[DEVOS_WIFI_MAX_SAVED];
static int s_saved_count = 0;

/* ---- password / add-network dialog ---- */
static lv_obj_t *overlay, *modal, *lbl_modal_title, *ta_ssid, *ta_pass, *cb_show, *kb;
static lv_obj_t *s_focused_ta = NULL;
static bool s_modal_hidden_net = false;
static char s_modal_ssid[33];

/* ---- other panels ---- */
static lv_obj_t *theme_switch, *slider_bright, *lbl_bright, *dd_dim, *dd_sleep;
static lv_obj_t *lbl_bat_pct, *bar_bat, *lbl_bat_status, *lbl_bat_detail, *lbl_pwr_state;
static lv_obj_t *lbl_clock_big, *lbl_clock_date, *lbl_clock_src, *dd_tz;
static lv_obj_t *lbl_sys_device, *lbl_sys_mem, *lbl_fw, *lbl_ota, *lbl_ota_btn;

static const uint32_t s_dim_opts_s[] = { 30, 60, 120, 300, 600, 0 };
static const char *s_dim_opts_txt = "30 seconds\n1 minute\n2 minutes\n5 minutes\n10 minutes\nNever";
static const uint32_t s_sleep_opts_s[] = { 60, 300, 600, 1800, 3600, 0 };
static const char *s_sleep_opts_txt = "1 minute\n5 minutes\n10 minutes\n30 minutes\n1 hour\nNever";

/* ======================================================================== */
static void set_text(lv_obj_t *lbl, const char *txt)
{
    /* Only touch the label when the text changes: every redraw costs a
     * full-frame rotation on this panel. */
    if (lbl && strcmp(lv_label_get_text(lbl), txt) != 0) lv_label_set_text(lbl, txt);
}

static void restyle(const devos_palette_t *p)
{
    lv_style_set_bg_color(&st_screen, p->bg);
    lv_style_set_bg_color(&st_nav, p->bg_alt);
    lv_style_set_border_color(&st_nav, p->surface_border);
    lv_style_set_text_color(&st_nav_btn, p->text_secondary);
    lv_style_set_bg_color(&st_nav_sel, p->surface_active);
    lv_style_set_text_color(&st_nav_sel, p->accent_primary);
    lv_style_set_border_color(&st_nav_sel, p->accent_primary);
    lv_style_set_bg_color(&st_card, p->surface);
    lv_style_set_border_color(&st_card, p->surface_border);
    lv_style_set_text_color(&st_header, p->accent_primary);
    lv_style_set_text_color(&st_title, p->text_primary);
    lv_style_set_text_color(&st_text, p->text_primary);
    lv_style_set_text_color(&st_muted, p->text_secondary);
    lv_style_set_bg_color(&st_btn, p->surface_active);
    lv_style_set_border_color(&st_btn, p->surface_border);
    lv_style_set_text_color(&st_btn, p->text_primary);
    lv_style_set_bg_color(&st_btn_primary, p->accent_primary);
    lv_style_set_text_color(&st_btn_primary, p->bg);
    lv_style_set_text_color(&st_btn_danger, p->accent_danger);
    lv_style_set_bg_color(&st_row, p->surface_active);
    lv_style_set_text_color(&st_row, p->text_primary);
    lv_style_set_border_color(&st_row_sel, p->accent_warning);
    lv_style_set_bg_color(&st_row_sel, p->surface_active);
    lv_style_set_bg_color(&st_ta, p->bg_alt);
    lv_style_set_border_color(&st_ta, p->surface_border);
    lv_style_set_text_color(&st_ta, p->text_primary);
    lv_style_set_border_color(&st_ta_focus, p->accent_primary);
    lv_style_set_bg_color(&st_modal, p->surface);
    lv_style_set_border_color(&st_modal, p->accent_primary);
    lv_style_set_bg_color(&st_kb, p->bg_alt);
    lv_style_set_bg_color(&st_kb_btn, p->surface_active);
    lv_style_set_text_color(&st_kb_btn, p->text_primary);
    lv_style_set_bg_color(&st_track, p->surface_active);
    lv_style_set_bg_color(&st_indicator, p->accent_primary);
    lv_style_set_bg_color(&st_knob, p->text_primary);
    lv_style_set_text_color(&st_ok, p->accent_secondary);
    lv_style_set_text_color(&st_warn, p->accent_warning);
    lv_style_set_text_color(&st_err, p->accent_danger);
    lv_style_set_bg_color(&st_dd_list, p->surface);
    lv_style_set_border_color(&st_dd_list, p->surface_border);
    lv_style_set_text_color(&st_dd_list, p->text_primary);
    lv_style_set_bg_color(&st_dd_sel, p->surface_active);
    lv_style_set_text_color(&st_dd_sel, p->accent_primary);
}

static void styles_init(void)
{
    if (s_styles_ready) return;
    lv_style_t *all[] = { &st_screen, &st_nav, &st_nav_btn, &st_nav_sel, &st_card, &st_header,
                          &st_title, &st_text, &st_muted, &st_btn, &st_btn_primary, &st_btn_danger,
                          &st_row, &st_row_sel, &st_ta, &st_ta_focus, &st_overlay, &st_modal, &st_kb,
                          &st_kb_btn, &st_track, &st_indicator, &st_knob, &st_ok, &st_warn, &st_err,
                          &st_dd_list, &st_dd_sel };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) lv_style_init(all[i]);

    lv_style_set_bg_opa(&st_screen, LV_OPA_COVER);
    lv_style_set_radius(&st_screen, 0);
    lv_style_set_border_width(&st_screen, 0);
    lv_style_set_pad_all(&st_screen, 0);

    lv_style_set_bg_opa(&st_nav, LV_OPA_COVER);
    lv_style_set_radius(&st_nav, 0);
    lv_style_set_border_width(&st_nav, 1);
    lv_style_set_border_side(&st_nav, LV_BORDER_SIDE_RIGHT);
    lv_style_set_pad_all(&st_nav, 12);
    lv_style_set_pad_row(&st_nav, 6);

    lv_style_set_bg_opa(&st_nav_btn, LV_OPA_TRANSP);
    lv_style_set_radius(&st_nav_btn, 6);
    lv_style_set_border_width(&st_nav_btn, 0);
    lv_style_set_shadow_width(&st_nav_btn, 0);
    lv_style_set_pad_left(&st_nav_btn, 14);
    lv_style_set_text_font(&st_nav_btn, &lv_font_montserrat_16);

    lv_style_set_bg_opa(&st_nav_sel, LV_OPA_COVER);
    lv_style_set_border_width(&st_nav_sel, 3);
    lv_style_set_border_side(&st_nav_sel, LV_BORDER_SIDE_LEFT);

    lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
    lv_style_set_border_width(&st_card, 1);
    lv_style_set_radius(&st_card, 8);
    lv_style_set_pad_all(&st_card, 16);

    lv_style_set_text_font(&st_header, &lv_font_montserrat_14);
    lv_style_set_text_letter_space(&st_header, 1);
    lv_style_set_text_font(&st_title, &lv_font_montserrat_22);
    lv_style_set_text_font(&st_text, &lv_font_montserrat_14);
    lv_style_set_text_font(&st_muted, &lv_font_montserrat_12);

    lv_style_set_bg_opa(&st_btn, LV_OPA_COVER);
    lv_style_set_border_width(&st_btn, 1);
    lv_style_set_radius(&st_btn, 6);
    lv_style_set_shadow_width(&st_btn, 0);
    lv_style_set_pad_hor(&st_btn, 14);
    lv_style_set_pad_ver(&st_btn, 8);
    lv_style_set_text_font(&st_btn, &lv_font_montserrat_14);
    lv_style_set_border_width(&st_btn_primary, 0);

    lv_style_set_bg_opa(&st_row, LV_OPA_TRANSP);
    lv_style_set_radius(&st_row, 6);
    lv_style_set_border_width(&st_row, 0);
    lv_style_set_shadow_width(&st_row, 0);
    lv_style_set_pad_hor(&st_row, 10);
    lv_style_set_pad_ver(&st_row, 6);
    lv_style_set_border_width(&st_row_sel, 2);
    lv_style_set_bg_opa(&st_row_sel, LV_OPA_COVER);

    lv_style_set_bg_opa(&st_ta, LV_OPA_COVER);
    lv_style_set_border_width(&st_ta, 1);
    lv_style_set_radius(&st_ta, 6);
    lv_style_set_pad_all(&st_ta, 10);
    lv_style_set_text_font(&st_ta, &lv_font_montserrat_16);
    lv_style_set_border_width(&st_ta_focus, 2);

    lv_style_set_bg_color(&st_overlay, lv_color_black());
    lv_style_set_bg_opa(&st_overlay, LV_OPA_50);
    lv_style_set_border_width(&st_overlay, 0);
    lv_style_set_radius(&st_overlay, 0);
    lv_style_set_pad_all(&st_overlay, 0);

    lv_style_set_bg_opa(&st_modal, LV_OPA_COVER);
    lv_style_set_border_width(&st_modal, 1);
    lv_style_set_radius(&st_modal, 10);
    lv_style_set_pad_all(&st_modal, 20);

    lv_style_set_bg_opa(&st_kb, LV_OPA_COVER);
    lv_style_set_border_width(&st_kb, 0);
    lv_style_set_radius(&st_kb, 0);
    lv_style_set_bg_opa(&st_kb_btn, LV_OPA_COVER);
    lv_style_set_radius(&st_kb_btn, 6);
    lv_style_set_border_width(&st_kb_btn, 0);
    lv_style_set_shadow_width(&st_kb_btn, 0);
    lv_style_set_text_font(&st_kb_btn, &lv_font_montserrat_18);

    lv_style_set_bg_opa(&st_track, LV_OPA_COVER);
    lv_style_set_bg_opa(&st_indicator, LV_OPA_COVER);
    lv_style_set_bg_opa(&st_knob, LV_OPA_COVER);

    lv_style_set_text_font(&st_ok, &lv_font_montserrat_14);
    lv_style_set_text_font(&st_warn, &lv_font_montserrat_14);
    lv_style_set_text_font(&st_err, &lv_font_montserrat_14);

    lv_style_set_bg_opa(&st_dd_list, LV_OPA_COVER);
    lv_style_set_border_width(&st_dd_list, 1);
    lv_style_set_text_font(&st_dd_list, &lv_font_montserrat_14);
    lv_style_set_bg_opa(&st_dd_sel, LV_OPA_COVER);

    restyle(devos_theme_get());
    s_styles_ready = true;
}

/* ---- small widget helpers ---- */
static lv_obj_t *mk_card(lv_obj_t *parent, int x, int y, int w, int h, const char *header)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_add_style(c, &st_card, 0);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    if (header) {
        lv_obj_t *l = lv_label_create(c);
        lv_obj_add_style(l, &st_header, 0);
        lv_label_set_text(l, header);
    }
    return c;
}

static lv_obj_t *mk_label(lv_obj_t *parent, lv_style_t *st, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_add_style(l, st, 0);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *txt, lv_style_t *extra, lv_event_cb_t cb, void *ud,
                        lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_add_style(b, &st_btn, 0);
    if (extra) lv_obj_add_style(b, extra, 0);
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (lbl_out) *lbl_out = l;
    return b;
}

static void style_dropdown(lv_obj_t *dd)
{
    lv_obj_add_style(dd, &st_ta, 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (list) {
        lv_obj_add_style(list, &st_dd_list, 0);
        lv_obj_add_style(list, &st_dd_sel, LV_PART_SELECTED | LV_STATE_CHECKED);
        lv_obj_add_style(list, &st_dd_sel, LV_PART_SELECTED | LV_STATE_PRESSED);
    }
}

static void style_track(lv_obj_t *o)
{
    lv_obj_add_style(o, &st_track, LV_PART_MAIN);
    lv_obj_add_style(o, &st_indicator, LV_PART_INDICATOR);
    lv_obj_add_style(o, &st_knob, LV_PART_KNOB);
}

/* ======================================================================== */
/* Navigation                                                               */
/* ======================================================================== */
static void refresh_visible(void);

static void select_section(int sec)
{
    if (sec < 0) sec = SEC_COUNT - 1;
    if (sec >= SEC_COUNT) sec = 0;
    s_section = sec;
    s_focus_list = false;
    for (int i = 0; i < SEC_COUNT; i++) {
        if (i == sec) {
            lv_obj_add_state(nav_btns[i], LV_STATE_CHECKED);
            lv_obj_remove_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_state(nav_btns[i], LV_STATE_CHECKED);
            lv_obj_add_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    bool visible = screen && !lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN);
    if (sec == SEC_WIFI && visible && !devos_net_wifi_scan_busy()) {
        devos_net_wifi_scan_start();   /* fresh list whenever Wi-Fi is opened */
    }
    refresh_visible();
}

static void nav_btn_cb(lv_event_t *e)
{
    select_section((int)(intptr_t)lv_event_get_user_data(e));
}

/* ======================================================================== */
/* Wi-Fi                                                                    */
/* ======================================================================== */
static const char *auth_text(uint8_t authmode)
{
    switch (authmode) {
    case 0:  return "Open";
    case 1:  return "WEP";
    case 2:  return "WPA";
    case 3:  return "WPA2";
    case 4:  return "WPA/WPA2";
    case 6:  return "WPA3";
    case 7:  return "WPA2/WPA3";
    default: return "Secured";
    }
}

/* Signal bars drawn from the live palette; bar count in user data. */
static void bars_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_current_target(e);
    int bars = (int)(intptr_t)lv_event_get_user_data(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const devos_palette_t *p = devos_theme_get();
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = 1;
    for (int i = 0; i < 4; i++) {
        int h = 5 + i * 4;
        lv_area_t r = { a.x1 + i * 6, a.y2 - h, a.x1 + i * 6 + 3, a.y2 };
        d.bg_color = (i < bars) ? (bars <= 1 ? p->accent_warning : p->accent_secondary) : p->text_muted;
        d.bg_opa = (i < bars) ? LV_OPA_COVER : LV_OPA_30;
        lv_draw_rect(layer, &d, &r);
    }
}

static void open_modal(const char *ssid, bool hidden_net);

static void connect_or_prompt(const devos_wifi_ap_t *ap)
{
    devos_wifi_status_t st;
    devos_net_wifi_get_status(&st);
    if (st.state == DEVOS_WIFI_STATE_CONNECTED && strcmp(st.ssid, ap->ssid) == 0) return;
    /* Re-prompt if the saved password was just rejected. */
    bool bad_pw = st.state == DEVOS_WIFI_STATE_FAILED && strcmp(st.ssid, ap->ssid) == 0 &&
                  strstr(st.last_error, "password") != NULL;
    if (!bad_pw && (devos_net_wifi_is_saved(ap->ssid) || ap->authmode == 0)) {
        devos_net_wifi_connect(ap->ssid, NULL);
    } else {
        open_modal(ap->ssid, false);
    }
}

static void ap_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_ap_count) {
        s_sel_row = idx;
        connect_or_prompt(&s_aps[idx]);
    }
}

static void saved_connect_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_saved_count) devos_net_wifi_connect(s_saved[idx].ssid, NULL);
}

static void saved_forget_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_saved_count) {
        devos_net_wifi_forget(s_saved[idx].ssid);
        s_last_list_key[0] = '\0';   /* force a list rebuild */
    }
}

static void add_network_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    open_modal("", true);
}

static void scan_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_net_wifi_scan_start();
}

static void disconnect_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_net_wifi_disconnect();
}

static void highlight_row(void)
{
    for (int i = 0; i < s_ap_count; i++) {
        if (!s_ap_rows[i]) continue;
        if (s_focus_list && i == s_sel_row) {
            lv_obj_add_state(s_ap_rows[i], LV_STATE_CHECKED);
            lv_obj_scroll_to_view(s_ap_rows[i], LV_ANIM_ON);
        } else {
            lv_obj_remove_state(s_ap_rows[i], LV_STATE_CHECKED);
        }
    }
}

static void rebuild_lists(const devos_wifi_status_t *st)
{
    s_ap_count = devos_net_wifi_scan_results(s_aps, DEVOS_WIFI_MAX_SCAN);
    s_saved_count = devos_net_wifi_saved_list(s_saved, DEVOS_WIFI_MAX_SAVED);

    /* Available networks */
    lv_obj_clean(list_avail);
    memset(s_ap_rows, 0, sizeof(s_ap_rows));
    if (s_ap_count == 0) {
        mk_label(list_avail, &st_muted, devos_net_wifi_scan_busy() ? "Scanning..." : "No networks found. Tap Scan to search again.");
    }
    for (int i = 0; i < s_ap_count; i++) {
        const devos_wifi_ap_t *ap = &s_aps[i];
        lv_obj_t *row = lv_button_create(list_avail);
        lv_obj_add_style(row, &st_row, 0);
        lv_obj_add_style(row, &st_row_sel, LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_size(row, lv_pct(100), 48);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 14, 0);
        lv_obj_add_event_cb(row, ap_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_ap_rows[i] = row;

        lv_obj_t *bars = lv_obj_create(row);
        lv_obj_remove_style_all(bars);
        lv_obj_set_size(bars, 22, 18);
        lv_obj_remove_flag(bars, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(bars, bars_draw_cb, LV_EVENT_DRAW_MAIN,
                            (void *)(intptr_t)devos_net_wifi_signal_bars(ap->rssi));

        lv_obj_t *name = mk_label(row, &st_text, ap->ssid);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(name, 1);

        bool connected = st->state == DEVOS_WIFI_STATE_CONNECTED && strcmp(st->ssid, ap->ssid) == 0;
        bool connecting = st->state == DEVOS_WIFI_STATE_CONNECTING && strcmp(st->ssid, ap->ssid) == 0;
        if (connected || connecting) {
            mk_label(row, connected ? &st_ok : &st_warn, connected ? "Connected" : "Connecting...");
        } else if (devos_net_wifi_is_saved(ap->ssid)) {
            mk_label(row, &st_muted, "Saved");
        }
        char meta[32];
        snprintf(meta, sizeof(meta), "%s  %d dBm", auth_text(ap->authmode), ap->rssi);
        mk_label(row, &st_muted, meta);
    }
    if (s_sel_row >= s_ap_count) s_sel_row = s_ap_count ? s_ap_count - 1 : 0;
    highlight_row();

    /* Saved networks */
    lv_obj_clean(list_saved);
    if (s_saved_count == 0) {
        mk_label(list_saved, &st_muted, "No saved networks yet.\nNetworks are remembered after\nthey connect successfully.");
    }
    for (int i = 0; i < s_saved_count; i++) {
        lv_obj_t *row = lv_obj_create(list_saved);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 44);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = mk_label(row, &st_text, s_saved[i].ssid);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(name, 1);

        bool connected = st->state == DEVOS_WIFI_STATE_CONNECTED && strcmp(st->ssid, s_saved[i].ssid) == 0;
        if (connected) {
            mk_label(row, &st_ok, "Connected");
        } else {
            mk_btn(row, "Connect", NULL, saved_connect_cb, (void *)(intptr_t)i, NULL);
        }
        mk_btn(row, LV_SYMBOL_TRASH, &st_btn_danger, saved_forget_cb, (void *)(intptr_t)i, NULL);
    }
}

static void refresh_wifi(void)
{
    devos_wifi_status_t st;
    devos_net_wifi_get_status(&st);
    char buf[160];

    switch (st.state) {
    case DEVOS_WIFI_STATE_CONNECTED:
        snprintf(buf, sizeof(buf), "Connected to %s", st.ssid);
        break;
    case DEVOS_WIFI_STATE_CONNECTING:
        snprintf(buf, sizeof(buf), "Connecting to %s...", st.ssid);
        break;
    case DEVOS_WIFI_STATE_FAILED:
        snprintf(buf, sizeof(buf), "Couldn't connect to %s", st.ssid);
        break;
    case DEVOS_WIFI_STATE_OFF:
        snprintf(buf, sizeof(buf), "Wi-Fi unavailable");
        break;
    default:
        snprintf(buf, sizeof(buf), "Not connected");
        break;
    }
    set_text(lbl_wifi_state, buf);

    if (st.state == DEVOS_WIFI_STATE_CONNECTED) {
        static const char *const quality[] = { "Very weak", "Weak", "Fair", "Good", "Excellent" };
        snprintf(buf, sizeof(buf), "IP %s   |   Gateway %s   |   DNS %s   |   Signal %d dBm (%s)",
                 st.ip, st.gateway[0] ? st.gateway : "-", st.dns[0] ? st.dns : "-", st.rssi,
                 quality[devos_net_wifi_signal_bars(st.rssi)]);
    } else if (st.state == DEVOS_WIFI_STATE_OFF) {
        snprintf(buf, sizeof(buf), "%s", st.last_error[0] ? st.last_error : "The Wi-Fi co-processor is not responding.");
    } else if (st.state == DEVOS_WIFI_STATE_CONNECTING) {
        snprintf(buf, sizeof(buf), "Waiting for the access point and an IP address (DHCP).");
    } else {
        snprintf(buf, sizeof(buf), "Choose a network below. Saved networks reconnect automatically.");
    }
    set_text(lbl_wifi_detail, buf);

    bool show_err = st.last_error[0] && st.state != DEVOS_WIFI_STATE_CONNECTED &&
                    st.state != DEVOS_WIFI_STATE_OFF;
    set_text(lbl_wifi_error, show_err ? st.last_error : "");

    if (st.state == DEVOS_WIFI_STATE_CONNECTED || st.state == DEVOS_WIFI_STATE_CONNECTING) {
        lv_obj_remove_flag(btn_wifi_disconnect, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(btn_wifi_disconnect, LV_OBJ_FLAG_HIDDEN);
    }
    bool busy = devos_net_wifi_scan_busy();
    set_text(lbl_scan_btn, busy ? "Scanning..." : LV_SYMBOL_REFRESH "  Scan");
    if (st.state == DEVOS_WIFI_STATE_OFF) {
        lv_obj_add_state(btn_wifi_scan, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(btn_wifi_scan, LV_STATE_DISABLED);
    }

    int saved_n = devos_net_wifi_saved_list(s_saved, DEVOS_WIFI_MAX_SAVED);
    uint32_t gen = devos_net_wifi_scan_generation();
    snprintf(buf, sizeof(buf), "AVAILABLE NETWORKS (%d)", s_ap_count);
    set_text(lbl_avail_hdr, buf);

    /* Rebuild the lists only when something they show has changed. */
    char key[80];
    snprintf(key, sizeof(key), "%d|%.33s|%d|%d", (int)st.state, st.ssid, saved_n, busy && s_ap_count == 0);
    if (gen != s_last_gen || strcmp(key, s_last_list_key) != 0) {
        s_last_gen = gen;
        snprintf(s_last_list_key, sizeof(s_last_list_key), "%s", key);
        rebuild_lists(&st);
        snprintf(buf, sizeof(buf), "AVAILABLE NETWORKS (%d)", s_ap_count);
        set_text(lbl_avail_hdr, buf);
    }
}

/* ---- password / add-network dialog ---- */
static bool modal_open(void)
{
    return overlay && !lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

static void focus_ta(lv_obj_t *ta)
{
    s_focused_ta = ta;
    if (ta_ssid) lv_obj_remove_state(ta_ssid, LV_STATE_FOCUSED);
    lv_obj_remove_state(ta_pass, LV_STATE_FOCUSED);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(kb, ta);
}

static void close_modal(void)
{
    if (!overlay) return;
    lv_textarea_set_text(ta_pass, "");     /* don't keep the password around */
    lv_textarea_set_text(ta_ssid, "");
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    s_focused_ta = NULL;
}

static void submit_modal(void)
{
    const char *ssid = s_modal_hidden_net ? lv_textarea_get_text(ta_ssid) : s_modal_ssid;
    const char *pass = lv_textarea_get_text(ta_pass);
    if (!ssid || !ssid[0]) {
        focus_ta(ta_ssid);
        return;
    }
    size_t pl = strlen(pass);
    if (pl > 0 && pl < 8) {
        set_text(lbl_modal_title, "Password must be at least 8 characters");
        return;
    }
    devos_net_wifi_connect(ssid, pass);
    close_modal();
    s_last_list_key[0] = '\0';
    refresh_wifi();
}

static void modal_connect_cb(lv_event_t *e) { LV_UNUSED(e); submit_modal(); }
static void modal_cancel_cb(lv_event_t *e) { LV_UNUSED(e); close_modal(); }

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        if (s_modal_hidden_net && s_focused_ta == ta_ssid) {
            focus_ta(ta_pass);
        } else {
            submit_modal();
        }
    } else if (code == LV_EVENT_CANCEL) {
        close_modal();
    }
}

static void ta_click_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    focus_ta(ta);
    lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);   /* bring the OSK back */
}

static void show_pw_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_textarea_set_password_mode(ta_pass, !lv_obj_has_state(cb_show, LV_STATE_CHECKED));
}

static void open_modal(const char *ssid, bool hidden_net)
{
    s_modal_hidden_net = hidden_net;
    snprintf(s_modal_ssid, sizeof(s_modal_ssid), "%s", ssid);
    char title[64];
    if (hidden_net) {
        snprintf(title, sizeof(title), "Add a network");
        lv_obj_remove_flag(ta_ssid, LV_OBJ_FLAG_HIDDEN);
    } else {
        snprintf(title, sizeof(title), "Password for %s", ssid);
        lv_obj_add_flag(ta_ssid, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(lbl_modal_title, title);
    lv_textarea_set_text(ta_pass, "");
    lv_textarea_set_password_mode(ta_pass, true);
    lv_obj_remove_state(cb_show, LV_STATE_CHECKED);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay);
    /* Touch users get the on-screen keyboard; it hides on the first physical key. */
    if (tab5_keyboard_is_connected()) {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
    focus_ta(hidden_net ? ta_ssid : ta_pass);
}

static void build_modal(void)
{
    overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(overlay);
    lv_obj_add_style(overlay, &st_overlay, 0);
    lv_obj_set_size(overlay, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);   /* swallow taps behind the dialog */
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    modal = lv_obj_create(overlay);
    lv_obj_remove_style_all(modal);
    lv_obj_add_style(modal, &st_modal, 0);
    lv_obj_set_size(modal, 640, LV_SIZE_CONTENT);
    lv_obj_align(modal, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(modal, 12, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lbl_modal_title = mk_label(modal, &st_title, "Password");
    lv_obj_set_style_text_font(lbl_modal_title, &lv_font_montserrat_20, 0);

    ta_ssid = lv_textarea_create(modal);
    lv_obj_add_style(ta_ssid, &st_ta, 0);
    lv_obj_add_style(ta_ssid, &st_ta_focus, LV_STATE_FOCUSED);
    lv_textarea_set_one_line(ta_ssid, true);
    lv_textarea_set_max_length(ta_ssid, 32);
    lv_textarea_set_placeholder_text(ta_ssid, "Network name (SSID)");
    lv_obj_set_width(ta_ssid, lv_pct(100));
    lv_obj_add_event_cb(ta_ssid, ta_click_cb, LV_EVENT_CLICKED, NULL);

    ta_pass = lv_textarea_create(modal);
    lv_obj_add_style(ta_pass, &st_ta, 0);
    lv_obj_add_style(ta_pass, &st_ta_focus, LV_STATE_FOCUSED);
    lv_textarea_set_one_line(ta_pass, true);
    lv_textarea_set_max_length(ta_pass, 64);
    lv_textarea_set_password_mode(ta_pass, true);
    lv_textarea_set_placeholder_text(ta_pass, "Password (leave empty for an open network)");
    lv_obj_set_width(ta_pass, lv_pct(100));
    lv_obj_add_event_cb(ta_pass, ta_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *row = lv_obj_create(modal);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);

    cb_show = lv_checkbox_create(row);
    lv_checkbox_set_text(cb_show, "Show password");
    lv_obj_add_style(cb_show, &st_text, 0);
    lv_obj_add_event_cb(cb_show, show_pw_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_flex_grow(cb_show, 1);

    mk_btn(row, "Cancel", NULL, modal_cancel_cb, NULL, NULL);
    mk_btn(row, LV_SYMBOL_OK "  Connect", &st_btn_primary, modal_connect_cb, NULL, NULL);

    kb = lv_keyboard_create(overlay);
    lv_obj_add_style(kb, &st_kb, 0);
    lv_obj_add_style(kb, &st_kb_btn, LV_PART_ITEMS);
    lv_obj_set_size(kb, DEVOS_SCREEN_WIDTH, 290);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_CANCEL, NULL);
}

static void build_wifi_panel(lv_obj_t *pn)
{
    lv_obj_t *c = mk_card(pn, 0, 0, PANEL_W, 124, NULL);
    lbl_wifi_state = mk_label(c, &st_title, "Not connected");
    lbl_wifi_detail = mk_label(c, &st_muted, "");
    lv_obj_set_pos(lbl_wifi_detail, 0, 36);
    lv_obj_set_width(lbl_wifi_detail, PANEL_W - 300);
    lv_label_set_long_mode(lbl_wifi_detail, LV_LABEL_LONG_WRAP);
    lbl_wifi_error = mk_label(c, &st_err, "");
    lv_obj_set_pos(lbl_wifi_error, 0, 66);

    btn_wifi_scan = mk_btn(c, LV_SYMBOL_REFRESH "  Scan", NULL, scan_btn_cb, NULL, &lbl_scan_btn);
    lv_obj_align(btn_wifi_scan, LV_ALIGN_TOP_RIGHT, 0, 0);
    btn_wifi_disconnect = mk_btn(c, LV_SYMBOL_CLOSE "  Disconnect", &st_btn_danger, disconnect_btn_cb, NULL, NULL);
    lv_obj_align(btn_wifi_disconnect, LV_ALIGN_TOP_RIGHT, -150, 0);

    const int list_y = 124 + 16;
    const int list_h = PANEL_H - list_y;
    const int left_w = 596;

    lv_obj_t *ca = mk_card(pn, 0, list_y, left_w, list_h, NULL);
    lbl_avail_hdr = mk_label(ca, &st_header, "AVAILABLE NETWORKS");
    list_avail = lv_obj_create(ca);
    lv_obj_remove_style_all(list_avail);
    lv_obj_set_pos(list_avail, 0, 28);
    lv_obj_set_size(list_avail, left_w - 32, list_h - 32 - 28);
    lv_obj_set_flex_flow(list_avail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_avail, 4, 0);
    lv_obj_set_scroll_dir(list_avail, LV_DIR_VER);

    lv_obj_t *cs = mk_card(pn, left_w + 16, list_y, PANEL_W - left_w - 16, list_h, "SAVED NETWORKS");
    list_saved = lv_obj_create(cs);
    lv_obj_remove_style_all(list_saved);
    lv_obj_set_pos(list_saved, 0, 28);
    lv_obj_set_size(list_saved, PANEL_W - left_w - 16 - 32, list_h - 32 - 28 - 52);
    lv_obj_set_flex_flow(list_saved, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_saved, 4, 0);
    lv_obj_set_scroll_dir(list_saved, LV_DIR_VER);

    lv_obj_t *add = mk_btn(cs, LV_SYMBOL_PLUS "  Add network...", NULL, add_network_cb, NULL, NULL);
    lv_obj_align(add, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

/* ======================================================================== */
/* Display                                                                  */
/* ======================================================================== */
static void theme_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    devos_theme_set(lv_obj_has_state(sw, LV_STATE_CHECKED) ? DEVOS_THEME_LIGHT : DEVOS_THEME_DARK);
}

static void bright_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    devos_power_set_user_brightness(lv_slider_get_value(s));
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", devos_power_user_brightness());
    set_text(lbl_bright, buf);
}

static void timeout_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    uint32_t di = lv_dropdown_get_selected(dd_dim);
    uint32_t si = lv_dropdown_get_selected(dd_sleep);
    devos_power_set_timeouts(s_dim_opts_s[di], s_sleep_opts_s[si]);
}

static int opt_index(const uint32_t *opts, int n, uint32_t v, int fallback)
{
    for (int i = 0; i < n; i++) if (opts[i] == v) return i;
    return fallback;
}

/* Sun / moon icons read the live palette at draw time. */
static void sun_icon_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const devos_palette_t *p = devos_theme_get();
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    static const int8_t dirs[8][2] = { {0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1} };
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = p->accent_warning;
    ld.width = 2;
    ld.round_end = 1;
    for (int i = 0; i < 8; i++) {
        ld.p1.x = cx + dirs[i][0] * 8;  ld.p1.y = cy + dirs[i][1] * 8;
        ld.p2.x = cx + dirs[i][0] * 11; ld.p2.y = cy + dirs[i][1] * 11;
        lv_draw_line(layer, &ld);
    }
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = LV_RADIUS_CIRCLE;
    rd.bg_color = p->accent_warning;
    lv_area_t core = { cx - 5, cy - 5, cx + 5, cy + 5 };
    lv_draw_rect(layer, &rd, &core);
}

static void moon_icon_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const devos_palette_t *p = devos_theme_get();
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = LV_RADIUS_CIRCLE;
    rd.bg_color = p->text_primary;
    lv_area_t disc = { cx - 7, cy - 7, cx + 7, cy + 7 };
    lv_draw_rect(layer, &rd, &disc);
    rd.bg_color = p->surface;
    lv_area_t cut = { cx - 3, cy - 9, cx + 9, cy + 3 };
    lv_draw_rect(layer, &rd, &cut);
}

static lv_obj_t *mk_icon(lv_obj_t *parent, int x, int y, lv_event_cb_t draw)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 28, 28);
    lv_obj_set_pos(o, x, y);
    lv_obj_add_event_cb(o, draw, LV_EVENT_DRAW_MAIN, NULL);
    return o;
}

static void build_display_panel(lv_obj_t *pn)
{
    lv_obj_t *c = mk_card(pn, 0, 0, PANEL_W, 132, "APPEARANCE");
    lv_obj_t *d = mk_label(c, &st_muted, "Dark Cyberdeck for indoors, High-Contrast Light for sunlight.  Sym+T toggles from anywhere.");
    lv_obj_set_pos(d, 0, 26);
    mk_icon(c, 0, 62, moon_icon_draw_cb);
    theme_switch = lv_switch_create(c);
    lv_obj_set_size(theme_switch, 64, 32);
    lv_obj_set_pos(theme_switch, 38, 60);
    style_track(theme_switch);
    if (devos_theme_get_type() == DEVOS_THEME_LIGHT) lv_obj_add_state(theme_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(theme_switch, theme_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    mk_icon(c, 112, 62, sun_icon_draw_cb);

    c = mk_card(pn, 0, 148, PANEL_W, 132, "BRIGHTNESS");
    d = mk_label(c, &st_muted, "Backlight level.  Sym+- / Sym++ adjust it from anywhere.");
    lv_obj_set_pos(d, 0, 26);
    slider_bright = lv_slider_create(c);
    style_track(slider_bright);
    lv_slider_set_range(slider_bright, 5, 100);
    lv_obj_set_size(slider_bright, PANEL_W - 160, 14);
    lv_obj_set_pos(slider_bright, 8, 72);
    lv_obj_add_event_cb(slider_bright, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lbl_bright = mk_label(c, &st_title, "100%");
    lv_obj_align(lbl_bright, LV_ALIGN_TOP_RIGHT, 0, 58);

    c = mk_card(pn, 0, 296, PANEL_W, 150, "SCREEN TIMEOUT");
    d = mk_label(c, &st_muted, "Any key or touch wakes the screen.");
    lv_obj_set_pos(d, 0, 26);
    lv_obj_t *l = mk_label(c, &st_text, "Dim after");
    lv_obj_set_pos(l, 0, 70);
    dd_dim = lv_dropdown_create(c);
    lv_dropdown_set_options(dd_dim, s_dim_opts_txt);
    lv_obj_set_width(dd_dim, 220);
    lv_obj_set_pos(dd_dim, 100, 58);
    style_dropdown(dd_dim);
    lv_obj_add_event_cb(dd_dim, timeout_cb, LV_EVENT_VALUE_CHANGED, NULL);
    l = mk_label(c, &st_text, "Turn off after");
    lv_obj_set_pos(l, 380, 70);
    dd_sleep = lv_dropdown_create(c);
    lv_dropdown_set_options(dd_sleep, s_sleep_opts_txt);
    lv_obj_set_width(dd_sleep, 220);
    lv_obj_set_pos(dd_sleep, 510, 58);
    style_dropdown(dd_sleep);
    lv_obj_add_event_cb(dd_sleep, timeout_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void refresh_display(void)
{
    char buf[16];
    if (!lv_slider_is_dragged(slider_bright)) {
        lv_slider_set_value(slider_bright, devos_power_user_brightness(), LV_ANIM_OFF);
    }
    snprintf(buf, sizeof(buf), "%d%%", devos_power_user_brightness());
    set_text(lbl_bright, buf);
    uint32_t di = (uint32_t)opt_index(s_dim_opts_s, 6, devos_power_dim_after_s(), 2);
    uint32_t si = (uint32_t)opt_index(s_sleep_opts_s, 6, devos_power_sleep_after_s(), 2);
    if (lv_dropdown_get_selected(dd_dim) != di) lv_dropdown_set_selected(dd_dim, di);
    if (lv_dropdown_get_selected(dd_sleep) != si) lv_dropdown_set_selected(dd_sleep, si);
    bool light = devos_theme_get_type() == DEVOS_THEME_LIGHT;
    if (light != lv_obj_has_state(theme_switch, LV_STATE_CHECKED)) {
        if (light) lv_obj_add_state(theme_switch, LV_STATE_CHECKED);
        else lv_obj_remove_state(theme_switch, LV_STATE_CHECKED);
    }
}

/* ======================================================================== */
/* Power                                                                    */
/* ======================================================================== */
static void sleep_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_power_sleep_now();
}

static void build_power_panel(lv_obj_t *pn)
{
    lv_obj_t *c = mk_card(pn, 0, 0, PANEL_W, 260, "BATTERY");
    lbl_bat_pct = mk_label(c, &st_title, "--%");
    lv_obj_set_style_text_font(lbl_bat_pct, &lv_font_montserrat_28, 0);
    lv_obj_set_pos(lbl_bat_pct, 0, 30);
    bar_bat = lv_bar_create(c);
    lv_obj_add_style(bar_bat, &st_track, LV_PART_MAIN);
    lv_obj_add_style(bar_bat, &st_indicator, LV_PART_INDICATOR);
    lv_obj_set_size(bar_bat, 420, 16);
    lv_obj_set_pos(bar_bat, 120, 42);
    lv_bar_set_range(bar_bat, 0, 100);
    lbl_bat_status = mk_label(c, &st_text, "");
    lv_obj_set_pos(lbl_bat_status, 0, 84);
    lbl_bat_detail = mk_label(c, &st_muted, "");
    lv_obj_set_pos(lbl_bat_detail, 0, 114);
    lv_obj_set_style_text_font(lbl_bat_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_line_space(lbl_bat_detail, 6, 0);

    c = mk_card(pn, 0, 276, PANEL_W, 150, "POWER STATE");
    lbl_pwr_state = mk_label(c, &st_text, "");
    lv_obj_set_pos(lbl_pwr_state, 0, 30);
    lv_obj_t *b = mk_btn(c, LV_SYMBOL_POWER "  Sleep now", NULL, sleep_btn_cb, NULL, NULL);
    lv_obj_set_pos(b, 0, 66);
    lv_obj_t *h = mk_label(c, &st_muted, "Turns the backlight off. Any key or touch wakes it.");
    lv_obj_set_pos(h, 170, 76);
}

static void refresh_power(void)
{
    const devos_telemetry_t *t = devos_telemetry_get();
    char buf[200];
    if (!t->battery_valid) {
        set_text(lbl_bat_pct, "--%");
        set_text(lbl_bat_status, "Power monitor (INA226) is not responding.");
        set_text(lbl_bat_detail, "");
        lv_bar_set_value(bar_bat, 0, LV_ANIM_OFF);
    } else if (!t->battery_present) {
        set_text(lbl_bat_pct, LV_SYMBOL_USB);
        set_text(lbl_bat_status, "Running on USB power - no battery pack detected.");
        snprintf(buf, sizeof(buf), "Bus voltage %.2f V", t->battery_voltage_mv / 1000.0f);
        set_text(lbl_bat_detail, buf);
        lv_bar_set_value(bar_bat, 0, LV_ANIM_OFF);
    } else {
        snprintf(buf, sizeof(buf), "%d%%", t->battery_percent);
        set_text(lbl_bat_pct, buf);
        lv_bar_set_value(bar_bat, t->battery_percent, LV_ANIM_OFF);
        if (t->battery_charging) {
            snprintf(buf, sizeof(buf), LV_SYMBOL_CHARGE "  Charging");
        } else if (t->runtime_minutes_left > 0) {
            snprintf(buf, sizeof(buf), "On battery  -  about %dh %02dm remaining at the current draw",
                     t->runtime_minutes_left / 60, t->runtime_minutes_left % 60);
        } else {
            snprintf(buf, sizeof(buf), "On battery");
        }
        set_text(lbl_bat_status, buf);
        snprintf(buf, sizeof(buf), "Voltage  %.2f V   (%.2f V per cell)\nCurrent  %+d mA  (+ = into the battery)\n"
                                   "Power    %.2f W\nCharger signal (CHG_STAT)  %s",
                 t->battery_voltage_mv / 1000.0f, t->battery_voltage_mv / 2000.0f,
                 (int)t->battery_current_ma, t->battery_power_mw / 1000.0f,
                 t->charger_signal ? "high" : "low");
        set_text(lbl_bat_detail, buf);
    }
    snprintf(buf, sizeof(buf), "Mode: %s   |   Idle: %u s   |   Backlight: %d%%",
             devos_power_mode_text(), (unsigned)devos_power_idle_s(),
             devos_power_user_brightness() * devos_power_brightness() / 100);
    set_text(lbl_pwr_state, buf);
}

/* ======================================================================== */
/* Date & Time                                                              */
/* ======================================================================== */
static void tz_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_sysmon_set_timezone_index((int)lv_dropdown_get_selected(dd_tz));
}

static void build_time_panel(lv_obj_t *pn)
{
    lv_obj_t *c = mk_card(pn, 0, 0, PANEL_W, 200, "CLOCK");
    lbl_clock_big = mk_label(c, &st_title, "--:--:--");
    lv_obj_set_style_text_font(lbl_clock_big, &lv_font_montserrat_28, 0);
    lv_obj_set_pos(lbl_clock_big, 0, 32);
    lbl_clock_date = mk_label(c, &st_text, "");
    lv_obj_set_pos(lbl_clock_date, 0, 76);
    lbl_clock_src = mk_label(c, &st_muted, "");
    lv_obj_set_pos(lbl_clock_src, 0, 110);
    lv_obj_set_width(lbl_clock_src, PANEL_W - 32);
    lv_label_set_long_mode(lbl_clock_src, LV_LABEL_LONG_WRAP);

    c = mk_card(pn, 0, 216, PANEL_W, 130, "TIME ZONE");
    lv_obj_t *d = mk_label(c, &st_muted, "Daylight saving is applied automatically where the zone observes it.");
    lv_obj_set_pos(d, 0, 26);
    dd_tz = lv_dropdown_create(c);
    int n = 0;
    const devos_timezone_t *z = devos_sysmon_timezones(&n);
    static char opts[1400];
    opts[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        int w = snprintf(opts + used, sizeof(opts) - used, "%s%s", i ? "\n" : "", z[i].name);
        if (w < 0 || (size_t)w >= sizeof(opts) - used) break;
        used += (size_t)w;
    }
    lv_dropdown_set_options(dd_tz, opts);
    lv_obj_set_width(dd_tz, 420);
    lv_obj_set_pos(dd_tz, 0, 56);
    style_dropdown(dd_tz);
    lv_dropdown_set_selected(dd_tz, (uint32_t)devos_sysmon_timezone_index());
    lv_obj_add_event_cb(dd_tz, tz_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void refresh_time(void)
{
    const devos_telemetry_t *t = devos_telemetry_get();
    char buf[160];
    if (t->time_valid) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t->rtc_hour, t->rtc_min, t->rtc_sec);
        set_text(lbl_clock_big, buf);
        set_text(lbl_clock_date, t->rtc_date_str);
    } else {
        set_text(lbl_clock_big, "--:--:--");
        set_text(lbl_clock_date, "Clock not set");
    }
    switch (devos_sysmon_time_source()) {
    case DEVOS_TIME_NTP: {
        time_t ls = devos_sysmon_last_sync();
        struct tm lt;
        localtime_r(&ls, &lt);
        snprintf(buf, sizeof(buf), "Synced from the internet (pool.ntp.org) at %02d:%02d. "
                                   "The hardware clock keeps time while offline.", lt.tm_hour, lt.tm_min);
        break;
    }
    case DEVOS_TIME_RTC:
        snprintf(buf, sizeof(buf), "Restored from the hardware clock (RTC). It will sync from the internet once Wi-Fi is connected.");
        break;
    default:
        snprintf(buf, sizeof(buf), "Not set. Connect to Wi-Fi and the clock will set itself.");
        break;
    }
    set_text(lbl_clock_src, buf);
    if (lv_dropdown_get_selected(dd_tz) != (uint32_t)devos_sysmon_timezone_index()) {
        lv_dropdown_set_selected(dd_tz, (uint32_t)devos_sysmon_timezone_index());
    }
}

/* ======================================================================== */
/* System                                                                   */
/* ======================================================================== */
static void ota_check_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (devos_ota_has_update()) {
        devos_ota_apply();
        return;
    }
    set_text(lbl_ota, "Checking feed...");
    devos_ota_check();
}

static void build_system_panel(lv_obj_t *pn)
{
    lv_obj_t *c = mk_card(pn, 0, 0, PANEL_W, 230, "DEVICE");
    lbl_sys_device = mk_label(c, &st_text, "");
    lv_obj_set_pos(lbl_sys_device, 0, 30);
    lv_obj_set_style_text_line_space(lbl_sys_device, 6, 0);

    c = mk_card(pn, 0, 246, PANEL_W, 150, "MEMORY & STORAGE");
    lbl_sys_mem = mk_label(c, &st_text, "");
    lv_obj_set_pos(lbl_sys_mem, 0, 30);
    lv_obj_set_style_text_line_space(lbl_sys_mem, 6, 0);

    c = mk_card(pn, 0, 412, PANEL_W, 120, "FIRMWARE UPDATE");
    lbl_fw = mk_label(c, &st_text, DEVOS_VERSION_STR);
    lv_obj_set_pos(lbl_fw, 0, 30);
    lv_obj_t *b = mk_btn(c, LV_SYMBOL_REFRESH "  Check for updates", NULL, ota_check_cb, NULL, &lbl_ota_btn);
    lv_obj_set_pos(b, 0, 58);
    lbl_ota = mk_label(c, &st_muted, "");
    lv_obj_set_pos(lbl_ota, 240, 68);
}

static void refresh_system(void)
{
    const devos_telemetry_t *t = devos_telemetry_get();
    char buf[512];
    char soc[96];
#ifdef ESP_PLATFORM
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    snprintf(soc, sizeof(soc), "ESP32-P4 rev %d.%d, %d cores @ %d MHz, ESP-IDF %s",
             ci.revision / 100, ci.revision % 100, ci.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
             esp_get_idf_version());
#else
    snprintf(soc, sizeof(soc), "Desktop simulator (SDL2)");
#endif
    snprintf(buf, sizeof(buf),
             "Firmware    %s (%s), built %s\n"
             "SoC         %s\n"
             "Wi-Fi       ESP32-C6 co-processor via ESP-Hosted (SDIO)\n"
             "Display     %s, 720 x 1280 MIPI-DSI (shown 1280 x 720)\n"
             "Keyboard    %s\n"
             "Uptime      %luh %02lum %02lus",
             DEVOS_VERSION_STR, DEVOS_BUILD_CODENAME, __DATE__, soc, bsp_tab5_panel_name(),
             tab5_keyboard_is_connected() ? "Tab5 keyboard connected" : "not detected",
             (unsigned long)(t->uptime_s / 3600), (unsigned long)((t->uptime_s / 60) % 60),
             (unsigned long)(t->uptime_s % 60));
    set_text(lbl_sys_device, buf);

    char sd[96];
    if (!t->sd_mounted) {
        snprintf(sd, sizeof(sd), "not inserted");
    } else if (t->sd_total_mb == 0) {
        snprintf(sd, sizeof(sd), "mounted at %s (reading size...)", TAB5_SD_MOUNT_POINT);
    } else {
        snprintf(sd, sizeof(sd), "%.1f GB free of %.1f GB (mounted at %s)",
                 t->sd_free_mb / 1024.0f, t->sd_total_mb / 1024.0f, TAB5_SD_MOUNT_POINT);
    }
    snprintf(buf, sizeof(buf),
             "PSRAM         %.1f MB free of %.1f MB\n"
             "Internal RAM  %u KB free (lowest since boot %u KB)\n"
             "SD card       %s",
             t->free_psram_kb / 1024.0f, t->psram_total_kb / 1024.0f,
             (unsigned)t->free_sram_kb, (unsigned)t->sram_min_free_kb, sd);
    set_text(lbl_sys_mem, buf);

    set_text(lbl_ota, devos_ota_update_text());
    set_text(lbl_ota_btn, devos_ota_has_update() ? LV_SYMBOL_DOWNLOAD "  Install update"
                                                 : LV_SYMBOL_REFRESH "  Check for updates");
}

/* ======================================================================== */
static void refresh_visible(void)
{
    if (!screen || lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN)) return;
    switch (s_section) {
    case SEC_WIFI:    refresh_wifi(); break;
    case SEC_DISPLAY: refresh_display(); break;
    case SEC_POWER:   refresh_power(); break;
    case SEC_TIME:    refresh_time(); break;
    case SEC_SYSTEM:  refresh_system(); break;
    default: break;
    }
}

static void settings_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    refresh_visible();
}

static void apply_theme(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    if (!s_styles_ready) return;
    restyle(p);
    lv_obj_report_style_change(NULL);
    s_last_list_key[0] = '\0';   /* rebuild lists (bar colours) */
    refresh_visible();
}

static void settings_init(void)
{
    styles_init();

    screen = lv_obj_create(lv_screen_active());
    app_descriptor.screen = screen;
    lv_obj_remove_style_all(screen);
    lv_obj_add_style(screen, &st_screen, 0);
    lv_obj_set_size(screen, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(screen, 0, DEVOS_TOP_BAR_HEIGHT);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);

    /* Section list */
    lv_obj_t *nav = lv_obj_create(screen);
    lv_obj_remove_style_all(nav);
    lv_obj_add_style(nav, &st_nav, 0);
    lv_obj_set_size(nav, NAV_W, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(nav, 0, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(nav, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = mk_label(nav, &st_title, "Settings");
    lv_obj_set_style_pad_left(title, 8, 0);
    lv_obj_set_style_pad_bottom(title, 10, 0);
    for (int i = 0; i < SEC_COUNT; i++) {
        lv_obj_t *b = lv_button_create(nav);
        lv_obj_add_style(b, &st_nav_btn, 0);
        lv_obj_add_style(b, &st_nav_sel, LV_STATE_CHECKED);
        lv_obj_set_size(b, lv_pct(100), 50);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, s_sec_labels[i]);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(b, nav_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        nav_btns[i] = b;
    }

    /* Panels */
    for (int i = 0; i < SEC_COUNT; i++) {
        lv_obj_t *pn = lv_obj_create(screen);
        lv_obj_remove_style_all(pn);
        lv_obj_set_pos(pn, NAV_W + PANEL_PAD, PANEL_PAD);
        lv_obj_set_size(pn, PANEL_W, PANEL_H);
        lv_obj_remove_flag(pn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(pn, LV_OBJ_FLAG_HIDDEN);
        panels[i] = pn;
    }
    build_wifi_panel(panels[SEC_WIFI]);
    build_display_panel(panels[SEC_DISPLAY]);
    build_power_panel(panels[SEC_POWER]);
    build_time_panel(panels[SEC_TIME]);
    build_system_panel(panels[SEC_SYSTEM]);
    build_modal();

    devos_theme_add_listener(apply_theme, NULL);
    lv_timer_create(settings_timer_cb, 500, NULL);
    select_section(SEC_WIFI);
}

static void settings_show(void)
{
    select_section(s_section);
}

static void settings_hide(void)
{
    close_modal();
}

/* ---- keyboard ---- */
static bool modal_handle_key(uint32_t key, uint8_t mods)
{
    if (tab5_keyboard_is_connected()) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    if (key == LV_KEY_ESC) { close_modal(); return true; }
    if (key == '\r' || key == '\n') {
        if (s_modal_hidden_net && s_focused_ta == ta_ssid) focus_ta(ta_pass);
        else submit_modal();
        return true;
    }
    if (key == '\t' && s_modal_hidden_net) {
        focus_ta(s_focused_ta == ta_ssid ? ta_pass : ta_ssid);
        return true;
    }
    if (!s_focused_ta) return true;
    if (key == '\b') { lv_textarea_delete_char(s_focused_ta); return true; }
    if (key == LV_KEY_DEL) { lv_textarea_delete_char_forward(s_focused_ta); return true; }
    if (key == LV_KEY_LEFT) { lv_textarea_cursor_left(s_focused_ta); return true; }
    if (key == LV_KEY_RIGHT) { lv_textarea_cursor_right(s_focused_ta); return true; }
    if (key >= 32 && key <= 126 && !(mods & (DEVOS_MOD_CTRL | DEVOS_MOD_FN))) {
        lv_textarea_add_char(s_focused_ta, (char)key);
    }
    return true;   /* the dialog owns the keyboard */
}

static bool settings_handle_key(uint32_t key, uint8_t mods)
{
    if (modal_open()) return modal_handle_key(key, mods);
    if (mods & DEVOS_MOD_FN) return false;   /* global Sym shortcuts */

    if (s_focus_list && s_section == SEC_WIFI) {
        if (key == LV_KEY_UP) {
            if (s_sel_row > 0) s_sel_row--;
            highlight_row();
            return true;
        }
        if (key == LV_KEY_DOWN) {
            if (s_sel_row < s_ap_count - 1) s_sel_row++;
            highlight_row();
            return true;
        }
        if (key == '\r' || key == '\n') {
            if (s_sel_row < s_ap_count) connect_or_prompt(&s_aps[s_sel_row]);
            return true;
        }
        if (key == LV_KEY_LEFT || key == LV_KEY_ESC) {
            s_focus_list = false;
            highlight_row();
            return true;
        }
    } else {
        if (key == LV_KEY_UP) { select_section(s_section - 1); return true; }
        if (key == LV_KEY_DOWN) { select_section(s_section + 1); return true; }
        if (s_section == SEC_WIFI &&
            (key == '\t' || key == LV_KEY_RIGHT || key == '\r' || key == '\n') && s_ap_count > 0) {
            s_focus_list = true;
            highlight_row();
            return true;
        }
    }
    if (s_section == SEC_WIFI && (key == 's' || key == 'S')) {
        devos_net_wifi_scan_start();
        return true;
    }
    return false;   /* Esc falls through to the core (back to Home) */
}

/* ---- launcher tile ---- */
static int settings_telemetry_lines(char lines[3][64])
{
    const devos_telemetry_t *t = devos_telemetry_get();
    if (t->wifi_state == DEVOS_WIFI_STATE_CONNECTED) {
        snprintf(lines[0], sizeof(lines[0]), "* Wi-Fi: %.40s", t->wifi_ssid);
    } else {
        snprintf(lines[0], sizeof(lines[0]), "* Wi-Fi: %s", devos_net_wifi_state_text((devos_wifi_state_t)t->wifi_state));
    }
    if (!t->battery_valid) {
        snprintf(lines[1], sizeof(lines[1]), "* Battery: n/a");
    } else if (!t->battery_present) {
        snprintf(lines[1], sizeof(lines[1]), "* USB power");
    } else {
        snprintf(lines[1], sizeof(lines[1]), "* Battery %d%%%s", t->battery_percent,
                 t->battery_charging ? " (charging)" : "");
    }
    snprintf(lines[2], sizeof(lines[2]), "* Theme: %s", devos_theme_is_dark() ? "Dark" : "Light");
    return 3;
}

devos_app_descriptor_t *app_settings_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_SETTINGS;
    app_descriptor.uid = "settings";
    app_descriptor.icon = LV_SYMBOL_SETTINGS;
    app_descriptor.category = "system";
    app_descriptor.name = "Settings";
    app_descriptor.title = "Settings";
    app_descriptor.subtitle = "Wi-Fi, display, power & system";
    app_descriptor.screen = screen;
    app_descriptor.init = settings_init;
    app_descriptor.show = settings_show;
    app_descriptor.hide = settings_hide;
    app_descriptor.handle_key = settings_handle_key;
    app_descriptor.get_telemetry_lines = settings_telemetry_lines;

    return &app_descriptor;
}
