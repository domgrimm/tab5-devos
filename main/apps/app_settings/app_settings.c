/* Settings: Wi-Fi, file sharing, display, keyboard, power, date & time, apps,
 * system info.
 *
 * Every value shown here is live (devos_net, devos_sysmon, devos_power, the
 * BSP); nothing is hard-coded. Layout: a section list on the left, one panel
 * per section on the right. Styling uses shared lv_style_t objects that are
 * re-coloured on theme change (lv_obj_report_style_change), so new widgets
 * follow the theme without per-widget bookkeeping.
 *
 * Keyboard (AGENTS.md invariant 9): the section list has focus first -
 * Up/Down pick a section, Tab/Right/Enter move into its panel, Esc goes Home.
 * In a panel the controls use devos_focus (Up/Down/Tab move, Left/Right change,
 * Space toggles, Enter presses / opens a list) and Esc goes back to the list.
 * The Wi-Fi panel has three regions (Tab cycles): the Disconnect / Scan
 * buttons, the available networks (Up/Down, Enter connects) and the saved
 * networks (Enter connects, D/Del forgets, "Add network..." below them);
 * S scans, A adds a network, X disconnects. In the dialog the physical
 * keyboard types into the focused field, Tab moves, Enter connects / saves,
 * Esc cancels. The on-screen keyboard only appears without a keyboard.
 * File Sharing: one switch (Space or Enter) for devos_fileshare, the SD card
 * as a web page; the address and password show while it's on.
 * Apps: one switch per app (devos_core's boot mask) with the memory it took
 * at start; Space switches, Enter restarts to apply, Esc leaves (the changes
 * still apply next start).
 * Each panel shows its keys in a hint line at the bottom.
 */
#include "app_settings.h"
#include "devos_config.h"
#include "devos_fileshare.h"
#include "devos_focus.h"
#include "devos_cmdpal.h"
#include "devos_widgets.h"
#include "devos_icons.h"
#include "devos_theme.h"
#include "devos_power.h"
#include "devos_powerdlg.h"
#include "devos_ota.h"
#include "devos_net.h"
#include "devos_storage.h"
#include "devos_sysmon.h"
#include "bsp_tab5.h"
#include "tab5_keyboard.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "sdkconfig.h"
#endif

enum { SEC_WIFI = 0, SEC_SHARE, SEC_DISPLAY, SEC_KEYBOARD, SEC_POWER, SEC_TIME, SEC_APPS, SEC_SYSTEM, SEC_COUNT };

/* Section names for the "section" intent: devos_core_open_with("settings",
 * "section", "power") opens Settings at Power (the top bar, the palette). */
static const char *const s_sec_names[SEC_COUNT] = {
    "wifi", "share", "display", "keyboard", "power", "time", "apps", "system",
};

static const char *const s_sec_labels[SEC_COUNT] = {
    LV_SYMBOL_WIFI "   Wi-Fi",
    LV_SYMBOL_SD_CARD "   File Sharing",
    LV_SYMBOL_EYE_OPEN "   Display",
    LV_SYMBOL_KEYBOARD "   Keyboard",
    LV_SYMBOL_BATTERY_FULL "   Power",
    LV_SYMBOL_BELL "   Date & Time",
    LV_SYMBOL_LIST "   Apps",
    LV_SYMBOL_SETTINGS "   System",
};

#define NAV_W        240
#define CONTENT_W    (DEVOS_SCREEN_WIDTH - NAV_W)
#define PANEL_PAD    20
#define PANEL_W      (CONTENT_W - 2 * PANEL_PAD)
#define PANEL_H      (DEVOS_CONTENT_HEIGHT - 2 * PANEL_PAD)
#define HINT_H       24     /* key hint line at the bottom of each panel */

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
static lv_obj_t *lbl_hint[SEC_COUNT];
static int s_section = SEC_WIFI;
/* Keyboard focus is in the section list or (s_in_panel) in the section's
 * panel. Each panel's controls are a devos_focus list; the Wi-Fi network
 * lists draw their own selection. */
static bool s_in_panel = false;
static devos_focus_t s_nav_focus;          /* focus ring on the section list */
static devos_focus_t s_pf[SEC_COUNT];      /* each panel's controls */

/* ---- Wi-Fi panel ---- */
/* Regions, in Tab order: Disconnect / Scan, available networks, saved
 * networks (whose last entry is the "Add network..." button). */
enum { WF_TOP = 0, WF_AVAIL, WF_SAVED, WF_COUNT };
static int s_wifi_zone = WF_AVAIL;
static lv_obj_t *lbl_wifi_state, *lbl_wifi_detail, *lbl_wifi_error;
static lv_obj_t *btn_wifi_disconnect, *btn_wifi_scan, *lbl_scan_btn, *btn_wifi_add;
static lv_obj_t *list_avail, *lbl_avail_hdr, *list_saved;
static devos_focus_t s_wifi_add_focus;     /* ring on "Add network..." */
static devos_wifi_ap_t s_aps[DEVOS_WIFI_MAX_SCAN];
static int s_ap_count = 0;
static lv_obj_t *s_ap_rows[DEVOS_WIFI_MAX_SCAN];
static int s_sel_row = 0;
static uint32_t s_last_gen = 0xFFFFFFFFu;
static char s_last_list_key[80] = "";
static devos_wifi_saved_t s_saved[DEVOS_WIFI_MAX_SAVED];
static int s_saved_count = 0;
static lv_obj_t *s_saved_rows[DEVOS_WIFI_MAX_SAVED];
static int s_sel_saved = 0;                /* == s_saved_count: "Add network..." */

/* ---- password / add-network dialog ---- */
static lv_obj_t *overlay, *modal, *lbl_modal_title, *ta_ssid, *ta_pass, *cb_show, *kb, *lbl_modal_ok,
                *lbl_modal_hint;
static enum { MODAL_WIFI, MODAL_FEED } s_modal_mode = MODAL_WIFI;
static devos_focus_t s_mf;                 /* the dialog's fields and buttons */
static bool s_modal_hidden_net = false;
static char s_modal_ssid[33];

/* ---- other panels ---- */
static lv_obj_t *theme_switch, *slider_bright, *lbl_bright, *dd_dim, *dd_sleep;
static lv_obj_t *sw_kbd_custom, *lbl_kbd_state, *card_light[2], *dd_light[2], *slider_light[2],
                *lbl_light_pct[2], *cb_light_caps[2], *swatch_light[2];
static lv_obj_t *lbl_bat_pct, *bar_bat, *lbl_bat_status, *lbl_bat_detail, *lbl_pwr_state;
static lv_obj_t *lbl_clock_big, *lbl_clock_date, *lbl_clock_src, *dd_tz;
static lv_obj_t *sw_share, *lbl_share_state, *lbl_share_url, *lbl_share_pw, *lbl_share_more, *lbl_share_count,
                *lbl_share_last, *lbl_share_sd;
static lv_style_t *s_share_state_style;
static lv_obj_t *lbl_sys_device, *lbl_sys_mem, *lbl_fw, *lbl_ota, *lbl_ota_btn, *btn_ota, *bar_ota, *lbl_feed;

/* ---- Apps panel: one row per app, built on first view (the list is
 * complete only after every app has registered) ---- */
typedef struct {
    devos_app_descriptor_t *app;
    lv_obj_t *sw, *lbl_cost, *lbl_state;
    lv_style_t *state_style;
} app_row_t;
static app_row_t s_app_rows[DEVOS_MAX_APPS];
static int s_app_rows_n = -1;
static lv_obj_t *list_apps, *lbl_apps_banner, *lbl_apps_total;
static lv_style_t *s_banner_style = &st_muted;
static char s_apps_note[96];

static const uint32_t s_dim_opts_s[] = { 30, 60, 120, 300, 600, 0 };
static const char *s_dim_opts_txt = "30 seconds\n1 minute\n2 minutes\n5 minutes\n10 minutes\nNever";
static const uint32_t s_sleep_opts_s[] = { 60, 300, 600, 1800, 3600, 0 };
/* Keyboard light choices: Off, Theme accent, Battery, then fixed colours. */
static const char *s_light_opts_txt = "Off\nTheme accent\nBattery\nCyan\nGreen\nBlue\nPurple\nPink\nRed\nAmber\nWhite";
static const uint32_t s_light_rgb[] = { 0x00E5FF, 0x00E676, 0x2979FF, 0xB060FF, 0xFF4081, 0xFF3030, 0xFFB300, 0xFFFFFF };
#define LIGHT_FIXED_FIRST 3
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

/* Switches fill with the accent only when on. */
static void style_switch(lv_obj_t *o)
{
    lv_obj_add_style(o, &st_track, LV_PART_MAIN);
    lv_obj_add_style(o, &st_indicator, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_style(o, &st_knob, LV_PART_KNOB);
}

/* Refresh a dropdown from live state, but not while its list is open (the
 * user is browsing it). */
static void sync_dropdown(lv_obj_t *dd, uint32_t sel)
{
    if (!lv_dropdown_is_open(dd) && lv_dropdown_get_selected(dd) != sel) lv_dropdown_set_selected(dd, sel);
}

/* ======================================================================== */
/* Keyboard focus                                                           */
/* ======================================================================== */
static void update_hint(void);
static void wifi_highlight(void);

static bool obj_usable(lv_obj_t *o)
{
    return o && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_state(o, LV_STATE_DISABLED);
}

/* A control that got hidden / disabled under the focus (Disconnect, the OTA
 * button while busy): drop it with its ring. */
static void focus_drop_unusable(devos_focus_t *f)
{
    if (devos_focus_get(f) && !obj_usable(devos_focus_get(f))) devos_focus_clear(f);
}

/* Keep devos_focus in step with a dropdown list opened / closed by touch. */
static void focus_sync_dropdown(devos_focus_t *f)
{
    lv_obj_t *o = devos_focus_get(f);
    if (!o || !lv_obj_check_type(o, &lv_dropdown_class)) return;
    bool open = lv_dropdown_is_open(o);
    if (open && !f->dd_open) f->dd_orig = lv_dropdown_get_selected(o);
    f->dd_open = open;
}

/* Left / Right walk a row of buttons (they have no value to change). */
static bool focus_button_row_key(devos_focus_t *f, uint32_t key)
{
    lv_obj_t *o = devos_focus_get(f);
    if ((key != LV_KEY_LEFT && key != LV_KEY_RIGHT) || !o || !lv_obj_check_type(o, &lv_button_class)) return false;
    devos_focus_move(f, key == LV_KEY_RIGHT ? 1 : -1);
    return true;
}

/* A tap on a panel control moves the keyboard focus into that panel. */
static void panel_pressed_cb(lv_event_t *e)
{
    int sec = (int)(intptr_t)lv_event_get_user_data(e);
    if (sec != s_section) return;
    s_in_panel = true;
    devos_focus_clear(&s_nav_focus);
    if (sec == SEC_WIFI) {
        if (lv_event_get_current_target(e) == btn_wifi_add) {
            s_wifi_zone = WF_SAVED;
            s_sel_saved = s_saved_count;
        } else {
            s_wifi_zone = WF_TOP;
        }
        wifi_highlight();
    }
    update_hint();
}

/* Register a panel control (registration order = focus order). */
static void focus_add(int sec, devos_focus_t *f, lv_obj_t *o)
{
    devos_focus_add(f, o);
    lv_obj_add_event_cb(o, panel_pressed_cb, LV_EVENT_PRESSED, (void *)(intptr_t)sec);
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
    s_in_panel = false;
    for (int i = 0; i < SEC_COUNT; i++) devos_focus_clear(&s_pf[i]);
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
    wifi_highlight();
    update_hint();
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

/* Draw the keyboard selection: the selected row of the focused list, or the
 * ring on a button (Disconnect / Scan / Add network...). */
static void wifi_highlight(void)
{
    bool on = s_in_panel && s_section == SEC_WIFI;
    for (int i = 0; i < s_ap_count; i++) {
        if (!s_ap_rows[i]) continue;
        if (on && s_wifi_zone == WF_AVAIL && i == s_sel_row) {
            lv_obj_add_state(s_ap_rows[i], LV_STATE_CHECKED);
            lv_obj_scroll_to_view(s_ap_rows[i], LV_ANIM_ON);
        } else {
            lv_obj_remove_state(s_ap_rows[i], LV_STATE_CHECKED);
        }
    }
    for (int i = 0; i < s_saved_count; i++) {
        if (!s_saved_rows[i]) continue;
        if (on && s_wifi_zone == WF_SAVED && i == s_sel_saved) {
            lv_obj_add_state(s_saved_rows[i], LV_STATE_CHECKED);
            lv_obj_scroll_to_view(s_saved_rows[i], LV_ANIM_ON);
        } else {
            lv_obj_remove_state(s_saved_rows[i], LV_STATE_CHECKED);
        }
    }
    if (!(on && s_wifi_zone == WF_TOP)) devos_focus_clear(&s_pf[SEC_WIFI]);
    if (on && s_wifi_zone == WF_SAVED && s_sel_saved >= s_saved_count) {
        if (!devos_focus_get(&s_wifi_add_focus)) devos_focus_set(&s_wifi_add_focus, btn_wifi_add);
    } else {
        devos_focus_clear(&s_wifi_add_focus);
    }
}

/* A tap in a network list puts the keyboard focus there too. */
static void wifi_touch_zone(int zone)
{
    s_in_panel = true;
    devos_focus_clear(&s_nav_focus);
    s_wifi_zone = zone;
    wifi_highlight();
    update_hint();
}

static void wifi_connect_saved(int idx)
{
    if (idx < 0 || idx >= s_saved_count) return;
    devos_wifi_status_t st;
    devos_net_wifi_get_status(&st);
    if (st.state == DEVOS_WIFI_STATE_CONNECTED && strcmp(st.ssid, s_saved[idx].ssid) == 0) return;
    devos_net_wifi_connect(s_saved[idx].ssid, NULL);
}

static void wifi_forget(int idx)
{
    if (idx < 0 || idx >= s_saved_count) return;
    /* the selection stays on the next row, or the one above for the last */
    if (s_sel_saved == idx && idx == s_saved_count - 1 && idx > 0) s_sel_saved--;
    devos_net_wifi_forget(s_saved[idx].ssid);
    s_last_list_key[0] = '\0';   /* rebuild the lists on the next refresh */
}

static void ap_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_ap_count) {
        s_sel_row = idx;
        wifi_touch_zone(WF_AVAIL);
        connect_or_prompt(&s_aps[idx]);
    }
}

static void saved_connect_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_saved_count) {
        s_sel_saved = idx;
        wifi_touch_zone(WF_SAVED);
        wifi_connect_saved(idx);
    }
}

static void saved_forget_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_saved_count) {
        s_sel_saved = idx;
        wifi_touch_zone(WF_SAVED);
        wifi_forget(idx);
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

static void rebuild_lists(const devos_wifi_status_t *st)
{
    /* Keep the keyboard selection on the same network when a new scan
     * re-sorts the list. */
    char sel_ssid[sizeof(s_aps[0].ssid)];
    snprintf(sel_ssid, sizeof(sel_ssid), "%s", s_sel_row < s_ap_count ? s_aps[s_sel_row].ssid : "");
    s_ap_count = devos_net_wifi_scan_results(s_aps, DEVOS_WIFI_MAX_SCAN);
    s_saved_count = devos_net_wifi_saved_list(s_saved, DEVOS_WIFI_MAX_SAVED);
    for (int i = 0; sel_ssid[0] && i < s_ap_count; i++) {
        if (strcmp(s_aps[i].ssid, sel_ssid) == 0) {
            s_sel_row = i;
            break;
        }
    }

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

    /* Saved networks */
    lv_obj_clean(list_saved);
    memset(s_saved_rows, 0, sizeof(s_saved_rows));
    if (s_saved_count == 0) {
        mk_label(list_saved, &st_muted, "No saved networks yet.\nNetworks are remembered after\nthey connect successfully.");
    }
    for (int i = 0; i < s_saved_count; i++) {
        lv_obj_t *row = lv_obj_create(list_saved);
        lv_obj_remove_style_all(row);
        lv_obj_add_style(row, &st_row, 0);
        lv_obj_add_style(row, &st_row_sel, LV_STATE_CHECKED);
        s_saved_rows[i] = row;
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
    if (s_sel_saved > s_saved_count) s_sel_saved = s_saved_count;
    wifi_highlight();
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

/* The on-screen keyboard types into the focused text field. */
static void modal_sync_kb(void)
{
    lv_obj_t *o = devos_focus_get(&s_mf);
    if (o && lv_obj_check_type(o, &lv_textarea_class)) lv_keyboard_set_textarea(kb, o);
}

static void modal_focus(lv_obj_t *o)
{
    devos_focus_set(&s_mf, o);
    modal_sync_kb();
}

/* The OSK is only for when no keyboard is attached. */
static void modal_show_osk(void)
{
    if (tab5_keyboard_is_connected()) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void close_modal(void)
{
    if (!overlay) return;
    lv_textarea_set_text(ta_pass, "");     /* don't keep the password around */
    lv_textarea_set_text(ta_ssid, "");
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    devos_focus_clear(&s_mf);              /* keys go back to the panel */
}

static void submit_modal(void)
{
    if (s_modal_mode == MODAL_FEED) {
        if (devos_ota_set_feed(lv_textarea_get_text(ta_pass)) != 0) {
            set_text(lbl_modal_title, "Enter a full URL, e.g. http://host:8090/devos-manifest.json");
            return;
        }
        close_modal();
        return;
    }
    const char *ssid = s_modal_hidden_net ? lv_textarea_get_text(ta_ssid) : s_modal_ssid;
    const char *pass = lv_textarea_get_text(ta_pass);
    if (!ssid || !ssid[0]) {
        modal_focus(ta_ssid);
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
        if (s_modal_hidden_net && devos_focus_get(&s_mf) == ta_ssid) {
            modal_focus(ta_pass);
        } else {
            submit_modal();
        }
    } else if (code == LV_EVENT_CANCEL) {
        close_modal();
    }
}

static void ta_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);          /* devos_focus moved the focus to the tapped field */
    modal_sync_kb();
    modal_show_osk();      /* bring the OSK back (touch-only case) */
}

static void show_pw_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_textarea_set_password_mode(ta_pass, !lv_obj_has_state(cb_show, LV_STATE_CHECKED));
}

static void open_modal(const char *ssid, bool hidden_net)
{
    s_modal_mode = MODAL_WIFI;
    s_modal_hidden_net = hidden_net;
    snprintf(s_modal_ssid, sizeof(s_modal_ssid), "%s", ssid);
    lv_obj_remove_flag(cb_show, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_max_length(ta_pass, 64);
    lv_textarea_set_placeholder_text(ta_pass, "Password (leave empty for an open network)");
    lv_label_set_text(lbl_modal_ok, LV_SYMBOL_OK "  Connect");
    lv_label_set_text(lbl_modal_hint, hidden_net ? "Tab  next field   Enter  next field / connect   Esc  cancel"
                                                 : "Tab  next field   Enter  connect   Esc  cancel");
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
    modal_show_osk();
    modal_focus(hidden_net ? ta_ssid : ta_pass);
}

static void open_feed_modal(void)
{
    s_modal_mode = MODAL_FEED;
    s_modal_hidden_net = false;
    lv_label_set_text(lbl_modal_title, "Firmware update feed (manifest URL)");
    lv_obj_add_flag(ta_ssid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(cb_show, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_password_mode(ta_pass, false);
    lv_textarea_set_max_length(ta_pass, DEVOS_OTA_FEED_MAX - 1);
    lv_textarea_set_placeholder_text(ta_pass, "http://host:8090/devos-manifest.json");
    char feed[DEVOS_OTA_FEED_MAX];
    devos_ota_get_feed(feed, sizeof(feed));
    lv_textarea_set_text(ta_pass, feed);
    lv_label_set_text(lbl_modal_ok, LV_SYMBOL_OK "  Save");
    lv_label_set_text(lbl_modal_hint, "Tab  next   Enter  save   Esc  cancel");
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay);
    modal_show_osk();
    modal_focus(ta_pass);
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
    devos_focus_add(&s_mf, ta_ssid);

    ta_pass = lv_textarea_create(modal);
    lv_obj_add_style(ta_pass, &st_ta, 0);
    lv_obj_add_style(ta_pass, &st_ta_focus, LV_STATE_FOCUSED);
    lv_textarea_set_one_line(ta_pass, true);
    lv_textarea_set_max_length(ta_pass, 64);
    lv_textarea_set_password_mode(ta_pass, true);
    lv_textarea_set_placeholder_text(ta_pass, "Password (leave empty for an open network)");
    lv_obj_set_width(ta_pass, lv_pct(100));
    lv_obj_add_event_cb(ta_pass, ta_click_cb, LV_EVENT_CLICKED, NULL);
    devos_focus_add(&s_mf, ta_pass);

    lv_obj_t *row = lv_obj_create(modal);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_pad_all(row, 6, 0);   /* room for the focus rings */
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    cb_show = lv_checkbox_create(row);
    lv_checkbox_set_text(cb_show, "Show password");
    lv_obj_add_style(cb_show, &st_text, 0);
    lv_obj_add_event_cb(cb_show, show_pw_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_flex_grow(cb_show, 1);
    devos_focus_add(&s_mf, cb_show);

    devos_focus_add(&s_mf, mk_btn(row, "Cancel", NULL, modal_cancel_cb, NULL, NULL));
    devos_focus_add(&s_mf, mk_btn(row, LV_SYMBOL_OK "  Connect", &st_btn_primary, modal_connect_cb, NULL,
                                  &lbl_modal_ok));

    lbl_modal_hint = mk_label(modal, &st_muted, "");

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
    focus_add(SEC_WIFI, &s_pf[SEC_WIFI], btn_wifi_disconnect);
    focus_add(SEC_WIFI, &s_pf[SEC_WIFI], btn_wifi_scan);

    const int list_y = 124 + 16;
    const int list_h = PANEL_H - list_y - HINT_H;
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

    btn_wifi_add = mk_btn(cs, LV_SYMBOL_PLUS "  Add network...", NULL, add_network_cb, NULL, NULL);
    lv_obj_align(btn_wifi_add, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    focus_add(SEC_WIFI, &s_wifi_add_focus, btn_wifi_add);
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
    style_switch(theme_switch);
    if (devos_theme_get_type() == DEVOS_THEME_LIGHT) lv_obj_add_state(theme_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(theme_switch, theme_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    focus_add(SEC_DISPLAY, &s_pf[SEC_DISPLAY], theme_switch);
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
    focus_add(SEC_DISPLAY, &s_pf[SEC_DISPLAY], slider_bright);
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
    focus_add(SEC_DISPLAY, &s_pf[SEC_DISPLAY], dd_dim);
    l = mk_label(c, &st_text, "Turn off after");
    lv_obj_set_pos(l, 380, 70);
    dd_sleep = lv_dropdown_create(c);
    lv_dropdown_set_options(dd_sleep, s_sleep_opts_txt);
    lv_obj_set_width(dd_sleep, 220);
    lv_obj_set_pos(dd_sleep, 510, 58);
    style_dropdown(dd_sleep);
    lv_obj_add_event_cb(dd_sleep, timeout_cb, LV_EVENT_VALUE_CHANGED, NULL);
    focus_add(SEC_DISPLAY, &s_pf[SEC_DISPLAY], dd_sleep);
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
    sync_dropdown(dd_dim, di);
    sync_dropdown(dd_sleep, si);
    bool light = devos_theme_get_type() == DEVOS_THEME_LIGHT;
    if (light != lv_obj_has_state(theme_switch, LV_STATE_CHECKED)) {
        if (light) lv_obj_add_state(theme_switch, LV_STATE_CHECKED);
        else lv_obj_remove_state(theme_switch, LV_STATE_CHECKED);
    }
}

/* ======================================================================== */
/* Keyboard                                                                 */
/* ======================================================================== */
static int light_opt_index(const tab5_kbd_light_t *l)
{
    if (l->source == TAB5_KBD_LIGHT_ACCENT) return 1;
    if (l->source == TAB5_KBD_LIGHT_BATTERY) return 2;
    if (l->source == TAB5_KBD_LIGHT_COLOUR) {
        for (unsigned i = 0; i < sizeof(s_light_rgb) / sizeof(s_light_rgb[0]); i++) {
            if (s_light_rgb[i] == l->rgb) return LIGHT_FIXED_FIRST + (int)i;
        }
        return LIGHT_FIXED_FIRST;
    }
    return 0;
}

/* Widgets -> driver. The sliders save when released, not on every step. */
static void kbd_light_cb(lv_event_t *e)
{
    bool save = !(lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED &&
                  (lv_event_get_target(e) == slider_light[0] || lv_event_get_target(e) == slider_light[1]));
    tab5_kbd_lights_t cfg;
    tab5_keyboard_get_lights(&cfg);
    cfg.custom = lv_obj_has_state(sw_kbd_custom, LV_STATE_CHECKED);
    for (int i = 0; i < 2; i++) {
        tab5_kbd_light_t *l = &cfg.light[i];
        int o = (int)lv_dropdown_get_selected(dd_light[i]);
        if (o == 0) l->source = TAB5_KBD_LIGHT_OFF;
        else if (o == 1) l->source = TAB5_KBD_LIGHT_ACCENT;
        else if (o == 2) l->source = TAB5_KBD_LIGHT_BATTERY;
        else {
            l->source = TAB5_KBD_LIGHT_COLOUR;
            l->rgb = s_light_rgb[o - LIGHT_FIXED_FIRST];
        }
        l->brightness = (uint8_t)lv_slider_get_value(slider_light[i]);
        l->caps_lock = lv_obj_has_state(cb_light_caps[i], LV_STATE_CHECKED);
    }
    tab5_keyboard_set_lights(&cfg, save);
}

static void build_light_card(lv_obj_t *pn, int i, int y)
{
    lv_obj_t *c = mk_card(pn, 0, y, PANEL_W, 150, i == 0 ? "LEFT LIGHT" : "RIGHT LIGHT");
    card_light[i] = c;
    swatch_light[i] = lv_obj_create(c);
    lv_obj_remove_style_all(swatch_light[i]);
    lv_obj_set_size(swatch_light[i], 30, 30);
    lv_obj_set_pos(swatch_light[i], 0, 42);
    lv_obj_set_style_radius(swatch_light[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(swatch_light[i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(swatch_light[i], 2, 0);
    lv_obj_set_style_border_opa(swatch_light[i], LV_OPA_40, 0);
    dd_light[i] = lv_dropdown_create(c);
    lv_dropdown_set_options(dd_light[i], s_light_opts_txt);
    lv_obj_set_width(dd_light[i], 220);
    lv_obj_set_pos(dd_light[i], 44, 36);
    style_dropdown(dd_light[i]);
    lv_obj_add_event_cb(dd_light[i], kbd_light_cb, LV_EVENT_VALUE_CHANGED, NULL);
    focus_add(SEC_KEYBOARD, &s_pf[SEC_KEYBOARD], dd_light[i]);
    lv_obj_t *l = mk_label(c, &st_text, "Brightness");
    lv_obj_set_pos(l, 300, 48);
    slider_light[i] = lv_slider_create(c);
    style_track(slider_light[i]);
    lv_slider_set_range(slider_light[i], 5, 100);
    lv_obj_set_size(slider_light[i], PANEL_W - 540, 14);
    lv_obj_set_pos(slider_light[i], 410, 50);
    lv_obj_add_event_cb(slider_light[i], kbd_light_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider_light[i], kbd_light_cb, LV_EVENT_RELEASED, NULL);
    focus_add(SEC_KEYBOARD, &s_pf[SEC_KEYBOARD], slider_light[i]);
    lbl_light_pct[i] = mk_label(c, &st_title, "20%");
    lv_obj_align(lbl_light_pct[i], LV_ALIGN_TOP_RIGHT, 0, 38);
    cb_light_caps[i] = lv_checkbox_create(c);
    lv_checkbox_set_text(cb_light_caps[i], "Caps lock: turn amber while caps lock is on");
    lv_obj_add_style(cb_light_caps[i], &st_text, 0);
    lv_obj_add_style(cb_light_caps[i], &st_indicator, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_pos(cb_light_caps[i], 0, 94);
    lv_obj_add_event_cb(cb_light_caps[i], kbd_light_cb, LV_EVENT_VALUE_CHANGED, NULL);
    focus_add(SEC_KEYBOARD, &s_pf[SEC_KEYBOARD], cb_light_caps[i]);
}

static void build_keyboard_panel(lv_obj_t *pn)
{
    lv_obj_t *c = mk_card(pn, 0, 0, PANEL_W, 120, "KEYBOARD LIGHTS");
    lv_obj_t *d = mk_label(c, &st_muted, "Off: the keyboard shows its own status.  On: choose each light below.");
    lv_obj_set_pos(d, 0, 26);
    sw_kbd_custom = lv_switch_create(c);
    lv_obj_set_size(sw_kbd_custom, 64, 32);
    lv_obj_set_pos(sw_kbd_custom, 0, 60);
    style_switch(sw_kbd_custom);
    lv_obj_add_event_cb(sw_kbd_custom, kbd_light_cb, LV_EVENT_VALUE_CHANGED, NULL);
    focus_add(SEC_KEYBOARD, &s_pf[SEC_KEYBOARD], sw_kbd_custom);
    lv_obj_t *l = mk_label(c, &st_text, "Set the lights from devOS");
    lv_obj_set_pos(l, 80, 66);
    lbl_kbd_state = mk_label(c, &st_muted, "");
    lv_obj_align(lbl_kbd_state, LV_ALIGN_TOP_RIGHT, 0, 66);

    build_light_card(pn, 0, 136);
    build_light_card(pn, 1, 302);

    c = mk_card(pn, 0, 468, PANEL_W, 110, "BATTERY COLOURS");
    d = mk_label(c, &st_muted, "Green from 50%, amber from 20%, red below that (blinking under 10%), blue while "
                               "charging or on USB power.  The lights switch off while the screen sleeps.");
    lv_obj_set_width(d, PANEL_W - 40);
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(d, 0, 30);
}

static void set_checked(lv_obj_t *o, bool on)
{
    if (on != lv_obj_has_state(o, LV_STATE_CHECKED)) {
        if (on) lv_obj_add_state(o, LV_STATE_CHECKED);
        else lv_obj_remove_state(o, LV_STATE_CHECKED);
    }
}

static void refresh_keyboard(void)
{
    tab5_kbd_lights_t cfg;
    tab5_keyboard_get_lights(&cfg);
    set_checked(sw_kbd_custom, cfg.custom);
    set_text(lbl_kbd_state, tab5_keyboard_is_connected() ? "Keyboard connected" : "Keyboard not detected");
    for (int i = 0; i < 2; i++) {
        const tab5_kbd_light_t *l = &cfg.light[i];
        uint32_t o = (uint32_t)light_opt_index(l);
        sync_dropdown(dd_light[i], o);
        if (!lv_slider_is_dragged(slider_light[i])) {
            lv_slider_set_value(slider_light[i], l->brightness < 5 ? 5 : l->brightness, LV_ANIM_OFF);
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", (int)lv_slider_get_value(slider_light[i]));
        set_text(lbl_light_pct[i], buf);
        set_checked(cb_light_caps[i], l->caps_lock);
        uint32_t rgb = cfg.custom ? tab5_keyboard_light_colour(i) : 0;
        lv_obj_set_style_bg_color(swatch_light[i], rgb ? lv_color_hex(rgb) : devos_theme_get()->surface_active, 0);
        lv_obj_set_style_border_color(swatch_light[i], devos_theme_get()->text_secondary, 0);
        /* greyed out while the keyboard drives its own lights */
        lv_obj_set_style_opa(card_light[i], cfg.custom ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_obj_t *w[] = {dd_light[i], slider_light[i], cb_light_caps[i]};
        for (unsigned k = 0; k < 3; k++) {
            if (cfg.custom) lv_obj_remove_state(w[k], LV_STATE_DISABLED);
            else lv_obj_add_state(w[k], LV_STATE_DISABLED);
        }
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

static void restart_btn_cb(lv_event_t *e);
static void shutdown_btn_cb(lv_event_t *e);

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
    lv_obj_t *b = mk_btn(c, LV_SYMBOL_PAUSE "  Sleep", NULL, sleep_btn_cb, NULL, NULL);
    lv_obj_set_pos(b, 0, 66);
    focus_add(SEC_POWER, &s_pf[SEC_POWER], b);

    lv_obj_t *rb = mk_btn(c, LV_SYMBOL_REFRESH "  Restart", NULL, restart_btn_cb, NULL, NULL);
    lv_obj_align_to(rb, b, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
    focus_add(SEC_POWER, &s_pf[SEC_POWER], rb);

    lv_obj_t *sb = mk_btn(c, LV_SYMBOL_POWER "  Shutdown", NULL, shutdown_btn_cb, NULL, NULL);
    lv_obj_align_to(sb, rb, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
    focus_add(SEC_POWER, &s_pf[SEC_POWER], sb);
    lv_obj_t *h = mk_label(c, &st_muted, "Turns the backlight off. Any key or touch wakes it.");
    lv_obj_set_pos(h, 0, 110);
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
    focus_add(SEC_TIME, &s_pf[SEC_TIME], dd_tz);
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
    sync_dropdown(dd_tz, (uint32_t)devos_sysmon_timezone_index());
}

/* ======================================================================== */
/* System                                                                   */
/* ======================================================================== */
static void ota_check_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (devos_ota_busy()) return;
    if (devos_ota_has_update()) devos_ota_apply();
    else devos_ota_check();
}

static void ota_feed_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!devos_ota_busy()) open_feed_modal();
}

/* ---- Restart / shut down, asking first ----
 * The dialog itself lives in devos_ui (devos_powerdlg) so the global
 * Sym+Shift+R / Sym+Shift+Q shortcuts raise the very same one. */
static void restart_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_powerdlg_restart();
}

static void shutdown_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_powerdlg_shutdown();
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

    c = mk_card(pn, 0, 412, PANEL_W, 176, "FIRMWARE UPDATE");
    char fw[96];
    snprintf(fw, sizeof(fw), "%s   (%s)", DEVOS_VERSION_STR, devos_ota_build_text());
    lbl_fw = mk_label(c, &st_text, fw);
    lv_obj_set_pos(lbl_fw, 0, 30);
    btn_ota = mk_btn(c, LV_SYMBOL_REFRESH "  Check for updates", NULL, ota_check_cb, NULL, &lbl_ota_btn);
    lv_obj_set_pos(btn_ota, 0, 58);
    lv_obj_set_width(btn_ota, 210);
    focus_add(SEC_SYSTEM, &s_pf[SEC_SYSTEM], btn_ota);
    lv_obj_t *b = mk_btn(c, LV_SYMBOL_EDIT "  Feed...", NULL, ota_feed_cb, NULL, NULL);
    lv_obj_set_pos(b, 222, 58);
    focus_add(SEC_SYSTEM, &s_pf[SEC_SYSTEM], b);
    lbl_ota = mk_label(c, &st_muted, "");
    lv_obj_set_pos(lbl_ota, 0, 104);
    lv_obj_set_width(lbl_ota, PANEL_W - 40);
    lv_label_set_long_mode(lbl_ota, LV_LABEL_LONG_DOT);
    bar_ota = lv_bar_create(c);
    lv_obj_set_size(bar_ota, PANEL_W - 40, 8);
    lv_obj_set_pos(bar_ota, 0, 130);
    lv_bar_set_range(bar_ota, 0, 100);
    lv_obj_add_flag(bar_ota, LV_OBJ_FLAG_HIDDEN);
    lbl_feed = mk_label(c, &st_muted, "");
    lv_obj_set_pos(lbl_feed, 0, 130);
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
             "Firmware    %s (%s), %s\n"
             "SoC         %s\n"
             "Wi-Fi       ESP32-C6 co-processor via ESP-Hosted (SDIO)\n"
             "Display     %s, 720 x 1280 MIPI-DSI (shown 1280 x 720)\n"
             "Keyboard    %s\n"
             "Uptime      %luh %02lum %02lus",
             DEVOS_VERSION_STR, DEVOS_BUILD_CODENAME, devos_ota_build_text(), soc, bsp_tab5_panel_name(),
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

    char ota[320];
    const char *notes = devos_ota_notes();
    snprintf(ota, sizeof(ota), "%s%s%s", devos_ota_update_text(),
             devos_ota_has_update() && notes[0] ? "  -  " : "", devos_ota_has_update() ? notes : "");
    set_text(lbl_ota, ota);
    int pct = devos_ota_progress();
    if (pct >= 0) {
        lv_bar_set_value(bar_ota, pct, LV_ANIM_OFF);
        lv_obj_remove_flag(bar_ota, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_feed, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bar_ota, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(lbl_feed, LV_OBJ_FLAG_HIDDEN);
    }
    char feed[DEVOS_OTA_FEED_MAX + 8];
    devos_ota_get_feed(feed + 6, sizeof(feed) - 6);
    memcpy(feed, "Feed: ", 6);
    set_text(lbl_feed, feed);
    if (devos_ota_busy()) lv_obj_add_state(btn_ota, LV_STATE_DISABLED);
    else lv_obj_remove_state(btn_ota, LV_STATE_DISABLED);
    set_text(lbl_ota_btn, devos_ota_busy() ? LV_SYMBOL_REFRESH "  Working..."
                          : devos_ota_has_update() ? LV_SYMBOL_DOWNLOAD "  Install update"
                                                 : LV_SYMBOL_REFRESH "  Check for updates");
}

/* ======================================================================== */
/* Apps                                                                     */
/* ======================================================================== */
static void fmt_bytes(char *out, size_t cap, int32_t b)
{
    if (b >= 1024 * 1024) snprintf(out, cap, "%.1f MB", b / (1024.0 * 1024.0));
    else snprintf(out, cap, "%ld KB", (long)((b + 1023) / 1024));
}

/* Give a label one of the text styles; *cur remembers which, so the
 * 500 ms refresh doesn't restyle (and redraw) it every time. */
static void swap_style(lv_obj_t *o, lv_style_t **cur, lv_style_t *want)
{
    if (*cur == want) return;
    if (*cur) lv_obj_remove_style(o, *cur, 0);
    lv_obj_add_style(o, want, 0);
    *cur = want;
}

static void app_switch_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_app_rows_n) return;
    bool on = lv_obj_has_state(s_app_rows[i].sw, LV_STATE_CHECKED);
    devos_core_set_app_enabled_next(s_app_rows[i].app->uid, on);
    s_apps_note[0] = '\0';
    refresh_visible();
}

static void build_apps_panel(lv_obj_t *pn)
{
    lv_obj_t *c = mk_card(pn, 0, 0, PANEL_W, PANEL_H - HINT_H - 8, "APPS");
    lv_obj_t *l = mk_label(c, &st_muted,
                           "A switched-off app doesn't start at all - no screen, no background work - so its "
                           "memory stays free. Changes apply after a restart.");
    lv_obj_set_pos(l, 0, 24);
    lv_obj_set_width(l, PANEL_W - 34);
    lbl_apps_banner = mk_label(c, &st_muted, "");
    lv_obj_set_pos(lbl_apps_banner, 0, 50);
    lv_obj_set_width(lbl_apps_banner, PANEL_W - 34);
    lv_label_set_long_mode(lbl_apps_banner, LV_LABEL_LONG_DOT);

    int inner_h = PANEL_H - HINT_H - 8 - 32;
    list_apps = lv_obj_create(c);
    lv_obj_remove_style_all(list_apps);
    lv_obj_set_pos(list_apps, 0, 80);
    lv_obj_set_size(list_apps, PANEL_W - 34, inner_h - 80 - 30);
    lv_obj_set_flex_flow(list_apps, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_apps, 2, 0);
    lv_obj_set_scrollbar_mode(list_apps, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(list_apps, LV_DIR_VER);

    lbl_apps_total = mk_label(c, &st_muted, "");
    lv_obj_set_pos(lbl_apps_total, 0, inner_h - 20);
    lv_obj_set_width(lbl_apps_total, PANEL_W - 34);
    lv_label_set_long_mode(lbl_apps_total, LV_LABEL_LONG_DOT);
}

static void build_app_rows(void)
{
    lv_obj_clean(list_apps);
    devos_focus_init(&s_pf[SEC_APPS]);
    s_app_rows_n = 0;
    int n = devos_core_known_app_count();
    for (int i = 0; i < n && s_app_rows_n < DEVOS_MAX_APPS; i++) {
        devos_app_descriptor_t *app = devos_core_known_app_at(i);
        if (!app || !app->uid) continue;
        app_row_t *r = &s_app_rows[s_app_rows_n];
        r->app = app;
        lv_obj_t *row = lv_obj_create(list_apps);
        lv_obj_remove_style_all(row);
        lv_obj_add_style(row, &st_row, 0);
        lv_obj_set_size(row, lv_pct(100), 54);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        r->sw = lv_switch_create(row);
        style_switch(r->sw);
        lv_obj_set_size(r->sw, 46, 24);
        lv_obj_align(r->sw, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(r->sw, app_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)s_app_rows_n);
        if (devos_core_app_required(app->uid)) {
            lv_obj_add_state(r->sw, LV_STATE_CHECKED | LV_STATE_DISABLED);
        } else {
            focus_add(SEC_APPS, &s_pf[SEC_APPS], r->sw);
        }

        lv_obj_t *ic = devos_icon_create(row, 18);
        lv_obj_add_style(ic, &st_text, 0);
        lv_obj_set_pos(ic, 62, 2);
        devos_icon_set_app(ic, app);
        lv_obj_t *l = mk_label(row, &st_text, app->name ? app->name : app->uid);
        lv_obj_set_pos(l, 88, 2);
        l = mk_label(row, &st_muted, app->subtitle ? app->subtitle : "");
        lv_obj_set_pos(l, 88, 24);
        lv_obj_set_width(l, 480);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);

        r->lbl_cost = mk_label(row, &st_text, "");
        lv_obj_align(r->lbl_cost, LV_ALIGN_TOP_RIGHT, 0, 2);
        r->lbl_state = mk_label(row, &st_muted, "");
        r->state_style = &st_muted;
        lv_obj_align(r->lbl_state, LV_ALIGN_TOP_RIGHT, 0, 24);
        s_app_rows_n++;
    }
}

static void refresh_apps(void)
{
    if (s_app_rows_n != devos_core_known_app_count()) build_app_rows();
    int64_t sum_sram = 0, sum_psram = 0;
    int on_now = 0;
    char buf[160], a[24], b[24];
    for (int i = 0; i < s_app_rows_n; i++) {
        app_row_t *r = &s_app_rows[i];
        const char *uid = r->app->uid;
        bool req = devos_core_app_required(uid), now = devos_core_app_enabled(uid),
             next = devos_core_app_enabled_next(uid);
        if (!req && lv_obj_has_state(r->sw, LV_STATE_CHECKED) != next) {
            if (next) lv_obj_add_state(r->sw, LV_STATE_CHECKED);
            else lv_obj_remove_state(r->sw, LV_STATE_CHECKED);
        }

        int32_t sram = 0, psram = 0;
        bool this_boot = false;
        if (devos_core_app_cost(uid, &sram, &psram, &this_boot)) {
            fmt_bytes(a, sizeof(a), sram);
            fmt_bytes(b, sizeof(b), psram);
#ifdef ESP_PLATFORM
            snprintf(buf, sizeof(buf), "%sRAM %s   PSRAM %s", this_boot ? "" : "last start: ", a, b);
#else
            snprintf(buf, sizeof(buf), "%sheap %s (simulator)", this_boot ? "" : "last start: ", b);
#endif
            if (now && this_boot) {
                sum_sram += sram;
                sum_psram += psram;
            }
        } else {
            snprintf(buf, sizeof(buf), now ? "under 1 KB" : "not measured yet");
        }
        set_text(r->lbl_cost, buf);
        if (now) on_now++;

        const char *st;
        lv_style_t *sty = &st_muted;
        if (req) st = "Always on";
        else if (now && !next) { st = "Off after restart"; sty = &st_warn; }
        else if (!now && next) { st = "On after restart"; sty = &st_warn; }
        else st = now ? "On" : "Off";
        set_text(r->lbl_state, st);
        swap_style(r->lbl_state, &r->state_style, sty);
    }

    bool pending = devos_core_apps_restart_pending();
    devos_apps_boot_t kind = devos_core_apps_boot_kind();
    lv_style_t *sty = &st_muted;
    if (pending) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING "  Restart required: Enter restarts now. "
                                   "Esc leaves without restarting - the changes apply next start.");
        sty = &st_warn;
    } else if (s_apps_note[0]) {
        snprintf(buf, sizeof(buf), "%s", s_apps_note);
    } else if (kind == DEVOS_APPS_BOOT_REVERTED) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING "  The last change stopped devOS from starting, "
                                   "so the switches before it were put back.");
        sty = &st_err;
    } else if (kind == DEVOS_APPS_BOOT_SAFE) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING "  Safe start: every app was switched back on.");
        sty = &st_warn;
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_OK "  Running as switched. Safe start (every app on): hold "
                                   "a finger on the screen while the Tab5 powers on.");
    }
    set_text(lbl_apps_banner, buf);
    swap_style(lbl_apps_banner, &s_banner_style, sty);

    const devos_telemetry_t *t = devos_telemetry_get();
    fmt_bytes(a, sizeof(a), (int32_t)sum_sram);
    fmt_bytes(b, sizeof(b), (int32_t)(sum_psram > INT32_MAX ? INT32_MAX : sum_psram));
#ifdef ESP_PLATFORM
    snprintf(buf, sizeof(buf), "%d of %d on, taking about %s RAM and %s PSRAM at start.   "
             "Free now: %u KB RAM (lowest %u KB), %.1f MB PSRAM.",
             on_now, s_app_rows_n, a, b, (unsigned)t->free_sram_kb, (unsigned)t->sram_min_free_kb,
             t->free_psram_kb / 1024.0f);
#else
    LV_UNUSED(t);
    snprintf(buf, sizeof(buf), "%d of %d on, taking about %s of heap at start (simulator: no RAM split).",
             on_now, s_app_rows_n, b);
#endif
    set_text(lbl_apps_total, buf);
}

/* Enter in the Apps panel */
static void apps_restart(void)
{
    if (!devos_core_apps_restart_pending()) {
        snprintf(s_apps_note, sizeof(s_apps_note),
                 LV_SYMBOL_OK "  Nothing to apply: every switch matches what's running.");
        refresh_apps();
        return;
    }
    set_text(lbl_apps_banner, LV_SYMBOL_REFRESH "  Restarting...");
    lv_refr_now(NULL);
    devos_core_restart();
}

/* ======================================================================== */
/* File Sharing                                                             */
/* ======================================================================== */
static void refresh_share(void);

static void share_toggle(bool on)
{
    if (on) devos_fileshare_start();
    else devos_fileshare_stop();
    refresh_share();
}

static void share_switch_cb(lv_event_t *e)
{
    share_toggle(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void build_share_panel(lv_obj_t *pn)
{
    lv_obj_t *c = mk_card(pn, 0, 0, PANEL_W, 318, "SHARE THE SD CARD");
    lv_obj_t *d = mk_label(c, &st_muted, "Add, download, rename and delete files on the SD card from a browser on "
                                         "a computer or phone on the same network. It asks for the password below - "
                                         "a new one each time sharing starts. Sharing is off again after a restart.");
    lv_obj_set_width(d, PANEL_W - 40);
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(d, 0, 26);
    sw_share = lv_switch_create(c);
    lv_obj_set_size(sw_share, 64, 32);
    lv_obj_set_pos(sw_share, 0, 76);
    style_switch(sw_share);
    lv_obj_add_event_cb(sw_share, share_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    focus_add(SEC_SHARE, &s_pf[SEC_SHARE], sw_share);
    lv_obj_t *l = mk_label(c, &st_text, "Share over the network");
    lv_obj_set_pos(l, 80, 83);
    lbl_share_state = mk_label(c, &st_muted, "");
    lv_obj_align(lbl_share_state, LV_ALIGN_TOP_RIGHT, 0, 83);
    s_share_state_style = &st_muted;

    lbl_share_url = mk_label(c, &st_title, "");
    lv_obj_set_style_text_font(lbl_share_url, &lv_font_montserrat_28, 0);
    lv_obj_set_pos(lbl_share_url, 0, 132);
    lbl_share_pw = mk_label(c, &st_title, "");
    lv_obj_set_pos(lbl_share_pw, 0, 180);
    lbl_share_more = mk_label(c, &st_muted, "");
    lv_obj_set_width(lbl_share_more, PANEL_W - 40);
    lv_label_set_long_mode(lbl_share_more, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(lbl_share_more, 4, 0);
    lv_obj_set_pos(lbl_share_more, 0, 222);

    c = mk_card(pn, 0, 334, PANEL_W, 116, "ACTIVITY");
    lbl_share_count = mk_label(c, &st_text, "");
    lv_obj_set_pos(lbl_share_count, 0, 30);
    lbl_share_last = mk_label(c, &st_muted, "");
    lv_obj_set_width(lbl_share_last, PANEL_W - 40);
    lv_label_set_long_mode(lbl_share_last, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(lbl_share_last, 0, 60);

    c = mk_card(pn, 0, 466, PANEL_W, 84, "SD CARD");
    lbl_share_sd = mk_label(c, &st_text, "");
    lv_obj_set_pos(lbl_share_sd, 0, 30);
}

static void fmt_size64(char *out, size_t cap, uint64_t b)
{
    if (b >= 1024ull * 1024 * 1024) snprintf(out, cap, "%.1f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024 * 1024) snprintf(out, cap, "%.1f MB", b / (1024.0 * 1024));
    else snprintf(out, cap, "%llu KB", (unsigned long long)((b + 1023) / 1024));
}

static void refresh_share(void)
{
    devos_fileshare_status_t st;
    devos_fileshare_status(&st);
    const devos_telemetry_t *t = devos_telemetry_get();
    bool on = st.running || st.password[0];         /* switched on (the port may still be opening) */
    set_checked(sw_share, on);

    char buf[320], port[8] = "";
    if (st.port != 80) snprintf(port, sizeof(port), ":%d", st.port);
    lv_style_t *state_style = &st_muted;
    if (!on && st.error[0]) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING "  %s", st.error);
        state_style = &st_err;
    } else if (!on) {
        snprintf(buf, sizeof(buf), "Off");
    } else if (!st.running) {
        snprintf(buf, sizeof(buf), "Starting...");
        state_style = &st_warn;
    } else if (st.clients > 0) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_UPLOAD "  Sharing - %d connection%s", st.clients, st.clients == 1 ? "" : "s");
        state_style = &st_ok;
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_OK "  Sharing");
        state_style = &st_ok;
    }
    set_text(lbl_share_state, buf);
    swap_style(lbl_share_state, &s_share_state_style, state_style);

    /* where to point the browser: the Wi-Fi address, else a tunnel's */
    const char *ip = t->local_ip[0] ? t->local_ip
                   : t->tailscale_online && t->tailscale_ip[0] ? t->tailscale_ip
                   : t->wireguard_online && t->wireguard_ip[0] ? t->wireguard_ip : NULL;
    if (!on) {
        set_text(lbl_share_url, devos_storage_is_mounted() ? "Not sharing" : "No SD card");
        set_text(lbl_share_pw, "");
        set_text(lbl_share_more, "Switch it on to see the address and password here.");
    } else if (!ip) {
        set_text(lbl_share_url, "Connect to Wi-Fi first");
        snprintf(buf, sizeof(buf), "Password   %s", st.password);
        set_text(lbl_share_pw, buf);
        set_text(lbl_share_more, "The address shows here once the Tab5 is on a network.");
    } else {
        snprintf(buf, sizeof(buf), "http://%s%s", ip, port);
        set_text(lbl_share_url, buf);
        snprintf(buf, sizeof(buf), "Password   %s      (any user name)", st.password);
        set_text(lbl_share_pw, buf);
        char alt[120] = "";
        size_t n = 0;
        if (t->tailscale_online && t->tailscale_ip[0] && ip != t->tailscale_ip) {
            n += (size_t)snprintf(alt + n, sizeof(alt) - n, "Over Tailscale: http://%s%s   ", t->tailscale_ip, port);
        }
        if (n < sizeof(alt) && t->wireguard_online && t->wireguard_ip[0] && ip != t->wireguard_ip) {
            snprintf(alt + n, sizeof(alt) - n, "Over WireGuard: http://%s%s", t->wireguard_ip, port);
        }
        snprintf(buf, sizeof(buf), "%s%sFrom a terminal:  curl -u devos:%s 'http://%s%s/api/list?path=/'",
                 alt, alt[0] ? "\n" : "", st.password, ip, port);
        set_text(lbl_share_more, buf);
    }

    char in[16], out[16];
    fmt_size64(in, sizeof(in), st.bytes_in);
    fmt_size64(out, sizeof(out), st.bytes_out);
    snprintf(buf, sizeof(buf), "%u uploaded (%s)     %u downloaded (%s)     %u deleted     %u requests",
             (unsigned)st.uploads, in, (unsigned)st.downloads, out, (unsigned)st.deletes, (unsigned)st.requests);
    set_text(lbl_share_count, buf);
    if (st.last[0]) {
        long ago = (long)(time(NULL) - st.last_time);
        char when[24];
        if (ago < 60) snprintf(when, sizeof(when), "%ld s ago", ago < 0 ? 0 : ago);
        else if (ago < 3600) snprintf(when, sizeof(when), "%ld min ago", ago / 60);
        else snprintf(when, sizeof(when), "%ld h ago", ago / 3600);
        snprintf(buf, sizeof(buf), "Last: %s   from %s, %s", st.last, st.last_client, when);
    } else {
        snprintf(buf, sizeof(buf), "Nothing yet");
    }
    set_text(lbl_share_last, buf);

    if (!devos_storage_is_mounted()) {
        snprintf(buf, sizeof(buf), "No SD card: insert one (FAT32) and restart to share it.");
    } else if (devos_storage_get_total_mb() == 0) {
        snprintf(buf, sizeof(buf), "Mounted at %s", TAB5_SD_MOUNT_POINT);
    } else {
        snprintf(buf, sizeof(buf), "%.1f GB free of %.1f GB", devos_storage_get_free_mb() / 1024.0f,
                 devos_storage_get_total_mb() / 1024.0f);
    }
    set_text(lbl_share_sd, buf);
}

/* ======================================================================== */
static void refresh_visible(void)
{
    if (!screen || lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN)) return;
    switch (s_section) {
    case SEC_WIFI:    refresh_wifi(); break;
    case SEC_SHARE:   refresh_share(); break;
    case SEC_DISPLAY: refresh_display(); break;
    case SEC_KEYBOARD: refresh_keyboard(); break;
    case SEC_POWER:   refresh_power(); break;
    case SEC_TIME:    refresh_time(); break;
    case SEC_APPS:    refresh_apps(); break;
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

/* ---- command palette (Sym + Space) ---- */
static void open_section_cmd(void *ud)
{
    devos_core_open_with("settings", "section", (const char *)ud);
}

static const char *share_label(void *ud)
{
    LV_UNUSED(ud);
    return devos_fileshare_running() ? "Stop sharing the SD card" : "Share the SD card (File Sharing)";
}

static void share_cmd(void *ud)
{
    LV_UNUSED(ud);
    if (devos_fileshare_running()) {
        share_toggle(false);
        return;
    }
    share_toggle(true);
    devos_core_open_with("settings", "section", "share");     /* its address and password */
}

static void update_cmd(void *ud)
{
    LV_UNUSED(ud);
    if (!devos_ota_busy() && !devos_ota_has_update()) devos_ota_check();
    devos_core_open_with("settings", "section", "system");
}

static const devos_command_t s_palette_cmds[] = {
    { .title = "Wi-Fi settings", .keywords = "wifi wireless network ssid password internet connect",
      .hint = "Settings", .icon = LV_SYMBOL_WIFI, .run = open_section_cmd, .ud = (void *)"wifi" },
    { .title = "File Sharing", .keywords = "share sharing sd card files web browser upload download",
      .hint = "Settings", .icon = LV_SYMBOL_SD_CARD, .label = share_label, .run = share_cmd },
    { .title = "Display settings", .keywords = "display screen brightness dim sleep timeout theme",
      .hint = "Settings", .icon = LV_SYMBOL_EYE_OPEN, .run = open_section_cmd, .ud = (void *)"display" },
    { .title = "Keyboard settings", .keywords = "keyboard lights backlight caps colour color",
      .hint = "Settings", .icon = LV_SYMBOL_KEYBOARD, .run = open_section_cmd, .ud = (void *)"keyboard" },
    { .title = "Power and battery", .keywords = "power battery charge charging sleep",
      .hint = "Settings", .icon = LV_SYMBOL_BATTERY_FULL, .run = open_section_cmd, .ud = (void *)"power" },
    { .title = "Date and time", .keywords = "date time clock timezone zone ntp rtc",
      .hint = "Settings", .icon = LV_SYMBOL_BELL, .run = open_section_cmd, .ud = (void *)"time" },
    { .title = "Switch apps on or off", .keywords = "apps enable disable memory boot mask",
      .hint = "Settings", .icon = LV_SYMBOL_LIST, .run = open_section_cmd, .ud = (void *)"apps" },
    { .title = "System and firmware", .keywords = "system about version build firmware device",
      .hint = "Settings", .icon = LV_SYMBOL_SETTINGS, .run = open_section_cmd, .ud = (void *)"system" },
    { .title = "Check for updates", .keywords = "update upgrade ota firmware install",
      .hint = "Settings", .icon = LV_SYMBOL_DOWNLOAD, .run = update_cmd },
};

/* A restart now would cut an update off halfway. */
static const char *ota_restart_check(void)
{
    devos_ota_state_t st = devos_ota_state();
    if (st == DEVOS_OTA_DOWNLOADING || st == DEVOS_OTA_VERIFYING) return "An update is downloading";
    if (st == DEVOS_OTA_WRITING) return "An update is being written to flash";
    return NULL;
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

    devos_focus_init(&s_nav_focus);
    devos_focus_init(&s_wifi_add_focus);
    devos_focus_init(&s_mf);
    for (int i = 0; i < SEC_COUNT; i++) devos_focus_init(&s_pf[i]);

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
        devos_focus_add(&s_nav_focus, b);
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
    build_share_panel(panels[SEC_SHARE]);
    build_display_panel(panels[SEC_DISPLAY]);
    build_keyboard_panel(panels[SEC_KEYBOARD]);
    build_power_panel(panels[SEC_POWER]);
    build_time_panel(panels[SEC_TIME]);
    build_apps_panel(panels[SEC_APPS]);
    build_system_panel(panels[SEC_SYSTEM]);
    for (int i = 0; i < SEC_COUNT; i++) {
        lbl_hint[i] = mk_label(panels[i], &st_muted, "");
        lv_obj_align(lbl_hint[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    build_modal();

    devos_theme_add_listener(apply_theme, NULL);
    lv_timer_create(settings_timer_cb, 500, NULL);
    select_section(SEC_WIFI);

    for (size_t i = 0; i < sizeof(s_palette_cmds) / sizeof(s_palette_cmds[0]); i++) {
        devos_cmdpal_add(&s_palette_cmds[i]);
    }
    devos_core_add_restart_check(ota_restart_check);
}

static void settings_show(void)
{
    /* "section": open at that section (top bar taps, the command palette) */
    char action[16], arg[16];
    if (devos_core_take_intent("settings", action, sizeof(action), arg, sizeof(arg)) && !strcmp(action, "section")) {
        for (int i = 0; i < SEC_COUNT; i++) {
            if (!strcmp(arg, s_sec_names[i])) {
                close_modal();
                s_section = i;
                break;
            }
        }
    }
    select_section(s_section);
    devos_focus_set(&s_nav_focus, nav_btns[s_section]);   /* keys start in the section list */
}

static void settings_hide(void)
{
    if (!screen) return;
    close_modal();
    devos_powerdlg_close();
    /* don't leave a dropdown list open on the top layer */
    for (int i = 0; i < SEC_COUNT; i++) devos_focus_clear(&s_pf[i]);
    lv_obj_t *dds[] = { dd_dim, dd_sleep, dd_light[0], dd_light[1], dd_tz };
    for (size_t i = 0; i < sizeof(dds) / sizeof(dds[0]); i++) lv_dropdown_close(dds[i]);
}

/* ---- keyboard ---- */
static void update_hint(void)
{
    if (!lbl_hint[s_section]) return;
    const char *h;
    if (!s_in_panel) {
        h = s_section == SEC_WIFI ? "Up / Down  section   Tab / Enter  networks   S  scan   A  add network   Esc  home"
                                  : "Up / Down  section   Tab / Enter  controls   Esc  home";
    } else if (s_section != SEC_WIFI && s_pf[s_section].dd_open) {
        h = "Up / Down  choose   Enter  confirm   Esc  cancel";
    } else {
        switch (s_section) {
        case SEC_WIFI:
            if (s_wifi_zone == WF_TOP) {
                h = "Left / Right  move   Enter  press   Tab  networks   S  scan   A  add network   Esc  sections";
            } else if (s_wifi_zone == WF_AVAIL) {
                h = "Up / Down  select   Enter  connect   Tab / Right  saved   S  scan   A  add network   "
                    "X  disconnect   Esc  sections";
            } else if (s_sel_saved >= s_saved_count) {
                h = "Up / Down  select   Enter  add a network   Tab  buttons   Left  available   Esc  sections";
            } else {
                h = "Up / Down  select   Enter  connect   D / Del  forget   Tab  buttons   Left  available   "
                    "Esc  sections";
            }
            break;
        case SEC_DISPLAY:
        case SEC_KEYBOARD:
            h = "Up / Down  move   Left / Right  change   Space  toggle   Enter  open list   Esc  sections";
            break;
        case SEC_SHARE:
            h = "Space / Enter  share on / off   Esc  sections";
            break;
        case SEC_POWER:
            h = "Enter  sleep now   Esc  sections";
            break;
        case SEC_TIME:
            h = "Left / Right  change   Enter  open the list   Esc  sections";
            break;
        case SEC_APPS:
            h = "Up / Down  move   Space  switch on / off   Enter  restart now   "
                "Esc  sections (changes still apply next start)";
            break;
        default:
            h = "Up / Down / Left / Right  move   Enter  press   Esc  sections";
            break;
        }
    }
    set_text(lbl_hint[s_section], h);
}

/* ---- Wi-Fi regions ---- */
static bool wifi_zone_ok(int z)
{
    if (z == WF_TOP) return obj_usable(btn_wifi_disconnect) || obj_usable(btn_wifi_scan);
    if (z == WF_AVAIL) return s_ap_count > 0;
    return true;    /* the saved list always ends in "Add network..." */
}

static void wifi_set_zone(int z)
{
    s_wifi_zone = z;
    devos_focus_clear(&s_pf[SEC_WIFI]);
    devos_focus_clear(&s_wifi_add_focus);
    if (z == WF_TOP) devos_focus_first(&s_pf[SEC_WIFI]);
    wifi_highlight();
}

/* Tab / Aa+Tab: the next / previous region that has something in it. */
static void wifi_step_zone(int dir)
{
    int z = s_wifi_zone;
    for (int k = 0; k < WF_COUNT; k++) {
        z = (z + dir + WF_COUNT) % WF_COUNT;
        if (wifi_zone_ok(z)) break;
    }
    wifi_set_zone(z);
}

static int list_step(uint32_t key)
{
    if (key == LV_KEY_DOWN) return 1;
    if (key == LV_KEY_UP) return -1;
    if (key == DEVOS_KEY_PGDN) return 8;
    if (key == DEVOS_KEY_PGUP) return -8;
    return 0;
}

static void leave_panel(void);

static bool wifi_handle_key(uint32_t key, uint8_t mods)
{
    devos_focus_t *f = &s_pf[SEC_WIFI];
    bool enter = key == '\r' || key == '\n';
    int step = list_step(key);

    if (key == '\t') {
        wifi_step_zone((mods & DEVOS_MOD_SHIFT) ? -1 : 1);
        return true;
    }
    if (key == LV_KEY_ESC) return false;              /* back to the section list */
    /* The region emptied (rescan) or its button went away: show where the
     * focus is now before acting on anything. */
    focus_drop_unusable(f);
    if (!wifi_zone_ok(s_wifi_zone)) {
        wifi_step_zone(1);
        return true;
    }
    if (s_wifi_zone == WF_TOP && !devos_focus_get(f)) {
        devos_focus_first(f);
        return true;
    }

    switch (s_wifi_zone) {
    case WF_TOP:
        if (key == LV_KEY_UP || key == LV_KEY_DOWN || key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
            devos_focus_move(f, (key == LV_KEY_DOWN || key == LV_KEY_RIGHT) ? 1 : -1);
            return true;
        }
        return devos_focus_key(f, key, mods);          /* Enter / Space press */
    case WF_AVAIL:
        if (step) {
            s_sel_row = LV_CLAMP(0, s_sel_row + step, s_ap_count - 1);
            wifi_highlight();
            return true;
        }
        if (enter) {
            if (s_sel_row < s_ap_count) connect_or_prompt(&s_aps[s_sel_row]);
            return true;
        }
        if (key == LV_KEY_RIGHT) {
            wifi_set_zone(WF_SAVED);
            return true;
        }
        if (key == LV_KEY_LEFT) {
            leave_panel();
            return true;
        }
        return false;
    default:    /* WF_SAVED: rows 0..n-1, then n = "Add network..." */
        if (step) {
            s_sel_saved = LV_CLAMP(0, s_sel_saved + step, s_saved_count);
            wifi_highlight();
            return true;
        }
        if (enter) {
            if (s_sel_saved >= s_saved_count) open_modal("", true);
            else wifi_connect_saved(s_sel_saved);
            return true;
        }
        if (key == 'd' || key == 'D' || key == LV_KEY_DEL) {
            wifi_forget(s_sel_saved);
            return true;
        }
        if (key == LV_KEY_LEFT) {
            if (wifi_zone_ok(WF_AVAIL)) wifi_set_zone(WF_AVAIL);
            else leave_panel();
            return true;
        }
        return false;
    }
}

/* Wi-Fi letter shortcuts, from the section list or the panel. */
static bool wifi_shortcut_key(uint32_t key)
{
    if (s_section != SEC_WIFI) return false;
    if (key == 's' || key == 'S') {
        devos_net_wifi_scan_start();
        return true;
    }
    if (key == 'a' || key == 'A') {
        open_modal("", true);
        return true;
    }
    if ((key == 'x' || key == 'X') && obj_usable(btn_wifi_disconnect)) {
        devos_net_wifi_disconnect();
        return true;
    }
    return false;
}

/* ---- section list <-> panel ---- */
static void enter_panel(int dir)
{
    s_in_panel = true;
    devos_focus_clear(&s_nav_focus);
    if (s_section == SEC_WIFI) {
        s_wifi_zone = WF_AVAIL;                       /* the network list first */
        if (dir < 0 || !wifi_zone_ok(WF_AVAIL)) wifi_step_zone(dir < 0 ? -1 : 1);
        else wifi_set_zone(WF_AVAIL);
    } else {
        devos_focus_t *f = &s_pf[s_section];
        devos_focus_clear(f);
        devos_focus_move(f, dir < 0 ? -1 : 1);        /* first (or last) control */
    }
    update_hint();
}

static void leave_panel(void)
{
    s_in_panel = false;
    devos_focus_clear(&s_pf[s_section]);
    wifi_highlight();
    devos_focus_set(&s_nav_focus, nav_btns[s_section]);
    update_hint();
}

static bool nav_handle_key(uint32_t key, uint8_t mods)
{
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        select_section(s_section + (key == LV_KEY_DOWN ? 1 : -1));
        devos_focus_set(&s_nav_focus, nav_btns[s_section]);
        return true;
    }
    if (key == '\t' || key == LV_KEY_RIGHT || key == '\r' || key == '\n') {
        enter_panel((key == '\t' && (mods & DEVOS_MOD_SHIFT)) ? -1 : 1);
        return true;
    }
    return false;   /* Esc falls through to the core (back to Home) */
}

static bool panel_handle_key(uint32_t key, uint8_t mods)
{
    if (s_section == SEC_APPS && (key == '\r' || key == '\n')) {
        apps_restart();
        return true;
    }
    if (s_section == SEC_SHARE && (key == '\r' || key == '\n')) {   /* Enter flips the switch too */
        devos_focus_set(&s_pf[SEC_SHARE], sw_share);
        share_toggle(!lv_obj_has_state(sw_share, LV_STATE_CHECKED));
        return true;
    }
    if (s_section == SEC_WIFI) {
        if (wifi_handle_key(key, mods)) return true;
    } else {
        devos_focus_t *f = &s_pf[s_section];
        focus_drop_unusable(f);
        focus_sync_dropdown(f);
        if (devos_focus_key(f, key, mods) || focus_button_row_key(f, key)) {
            /* the whole row, not just its switch */
            if (s_section == SEC_APPS && devos_focus_get(f)) {
                lv_obj_scroll_to_view(lv_obj_get_parent(devos_focus_get(f)), LV_ANIM_OFF);
            }
            return true;
        }
    }
    if (key == LV_KEY_ESC) {
        leave_panel();
        return true;
    }
    return false;
}

static bool modal_handle_key(uint32_t key, uint8_t mods)
{
    if (tab5_keyboard_is_connected()) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    bool enter = key == '\r' || key == '\n';
    lv_obj_t *o = devos_focus_get(&s_mf);
    if (enter && o == cb_show) {           /* Enter confirms; Space ticks the box */
        submit_modal();
        return true;
    }
    if (devos_focus_key(&s_mf, key, mods)) {   /* typing, Tab, Up/Down, buttons, checkbox */
        if (modal_open()) modal_sync_kb();
        return true;
    }
    if (key == LV_KEY_ESC) {
        close_modal();
    } else if (enter) {                    /* Enter in a text field */
        if (s_modal_hidden_net && o == ta_ssid) modal_focus(ta_pass);
        else submit_modal();
    } else {
        focus_button_row_key(&s_mf, key);  /* Left / Right between Cancel and OK */
    }
    return true;   /* the dialog owns the keyboard */
}

static bool settings_handle_key(uint32_t key, uint8_t mods)
{
    if (modal_open()) return modal_handle_key(key, mods);
    if (mods & (DEVOS_MOD_FN | DEVOS_MOD_CTRL | DEVOS_MOD_ALT)) return false;   /* global shortcuts */

    bool used = wifi_shortcut_key(key) || (s_in_panel ? panel_handle_key(key, mods) : nav_handle_key(key, mods));
    if (used) {
        refresh_visible();     /* show the effect now (e.g. light controls enabled) */
        update_hint();
    }
    return used;
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
    if (devos_fileshare_running()) snprintf(lines[2], sizeof(lines[2]), "* Sharing the SD card");
    else snprintf(lines[2], sizeof(lines[2]), "* Theme: %s", devos_theme_is_dark() ? "Dark" : "Light");
    return 3;
}

/* Sym+S sheet (devos_shortcuts.h) */
static const char *settings_shortcuts(void)
{
    return
        "Sections\n"
        "Up / Down\tPick a section\n"
        "Enter / Tab / Right\tInto its controls\n"
        "Esc\tBack to the sections, then Home\n"
        "Wi-Fi\n"
        "S\tScan\n"
        "A\tAdd a network (hidden ones too)\n"
        "X\tDisconnect\n"
        "Enter\tConnect to the picked network\n"
        "D / Del\tForget a saved network\n"
        "Tab\tButtons, available networks, saved networks\n"
        "The other sections\n"
        "Left / Right\tChange the focused setting\n"
        "Space\tFlip a switch\n"
        "Enter\tOpen a list, press a button (Apps: restart now)\n"
        "Power\n"
        "Sleep / Restart / Shutdown\tButtons there; ask first (Enter confirms, Esc cancels)\n";
}

devos_app_descriptor_t *app_settings_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_SETTINGS;
    app_descriptor.uid = "settings";
    app_descriptor.icon = LV_SYMBOL_SETTINGS;
    app_descriptor.category = "system";
    app_descriptor.name = "Settings";
    app_descriptor.title = "Settings";
    app_descriptor.subtitle = "Wi-Fi, file sharing, display & system";
    app_descriptor.screen = screen;
    app_descriptor.init = settings_init;
    app_descriptor.show = settings_show;
    app_descriptor.hide = settings_hide;
    app_descriptor.handle_key = settings_handle_key;
    app_descriptor.get_telemetry_lines = settings_telemetry_lines;
    app_descriptor.get_shortcuts = settings_shortcuts;

    return &app_descriptor;
}
