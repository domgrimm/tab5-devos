/* Tailscale: status, enrolment and peers for the Tab5's tailnet client
 * (components/devos_tailnet, MicroLink on the device).
 *
 * Keyboard first (AGENTS.md invariant 9):
 *   Peers    Up/Down, PgUp/PgDn or 1-9 select a peer (accent border), Enter SSH
 *            to it, P ping it, Esc deselect (then Home).
 *   Anywhere C connect/disconnect, K auth key, N device name, F forget.
 *   Buttons  Tab / Aa+Tab walk the header buttons (focus ring), Left/Right
 *            move along them, Enter/Space press, Down / Esc / Tab past the
 *            end go back to the peers.
 *   Dialogs  open with the text field (or the default button) focused;
 *            Enter saves / confirms, Esc cancels, Tab / Up / Down / Left /
 *            Right move between field and buttons; the forget dialog also
 *            takes Y / N. The on-screen keyboard only appears when no
 *            hardware keyboard is attached.
 */
#include "devos_toast.h"
#include "app_tailscale.h"
#include "devos_config.h"
#include "devos_icons.h"
#include "devos_tailnet.h"
#include "devos_wireguard.h"
#include "devos_theme.h"
#include "devos_focus.h"
#include "tab5_keyboard.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ROW_H   58
#define PAD     16

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;

static bool s_styles_ready = false;
static lv_style_t st_bg, st_card, st_title, st_text, st_muted, st_small, st_btn, st_btn_primary, st_btn_danger,
                  st_row, st_row_sel, st_ta, st_ta_focus, st_overlay, st_modal, st_kb, st_kb_btn, st_ok, st_warn,
                  st_err, st_section;

/* status card */
static lv_obj_t *lbl_state, *lbl_line1, *lbl_line2, *lbl_msg, *btn_toggle, *lbl_toggle;
static lv_obj_t *btn_key, *btn_name, *btn_forget;
/* peers */
static lv_obj_t *lbl_peers_h, *lbl_hint, *list_peers, *lbl_no_peers;
static lv_obj_t *rows[DEVOS_TS_MAX_PEERS], *row_name[DEVOS_TS_MAX_PEERS], *row_sub[DEVOS_TS_MAX_PEERS],
                *row_ping[DEVOS_TS_MAX_PEERS];
/* dialogs */
static lv_obj_t *overlay, *kb, *dlg_key, *ta_key, *dlg_name, *ta_name, *dlg_forget, *btn_forget_ok;

/* keyboard focus: header button row, one set per dialog (devos_focus) */
static devos_focus_t s_hdr, s_f_key, s_f_name, s_f_forget;
static devos_focus_t *s_dlg_f;          /* the open dialog's, NULL = none */
static bool s_via_key;                  /* inside handle_key (vs a tap) */

static int s_sel = -1;
static uint32_t s_seen_gen = UINT32_MAX;

static void refresh_ui(void);

/* ======================================================================== */
/* Styles                                                                   */
/* ======================================================================== */
static void restyle(const devos_palette_t *p)
{
    lv_style_set_bg_color(&st_bg, p->bg);
    lv_style_set_bg_color(&st_card, p->surface);
    lv_style_set_border_color(&st_card, p->surface_border);
    lv_style_set_text_color(&st_title, p->text_primary);
    lv_style_set_text_color(&st_text, p->text_primary);
    lv_style_set_text_color(&st_muted, p->text_secondary);
    lv_style_set_text_color(&st_small, p->text_muted);
    lv_style_set_text_color(&st_section, p->accent_primary);
    lv_style_set_bg_color(&st_btn, p->surface_active);
    lv_style_set_border_color(&st_btn, p->surface_border);
    lv_style_set_text_color(&st_btn, p->text_primary);
    lv_style_set_bg_color(&st_btn_primary, p->accent_primary);
    lv_style_set_text_color(&st_btn_primary, p->bg);
    lv_style_set_text_color(&st_btn_danger, p->accent_danger);
    lv_style_set_bg_color(&st_row, p->surface);
    lv_style_set_border_color(&st_row, p->surface_border);
    lv_style_set_border_color(&st_row_sel, p->accent_primary);
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
    lv_style_set_text_color(&st_ok, p->accent_secondary);
    lv_style_set_text_color(&st_warn, p->accent_warning);
    lv_style_set_text_color(&st_err, p->accent_danger);
}

static void styles_init(void)
{
    if (s_styles_ready) return;
    lv_style_t *all[] = { &st_bg, &st_card, &st_title, &st_text, &st_muted, &st_small, &st_btn, &st_btn_primary,
                          &st_btn_danger, &st_row, &st_row_sel, &st_ta, &st_ta_focus, &st_overlay, &st_modal,
                          &st_kb, &st_kb_btn, &st_ok, &st_warn, &st_err, &st_section };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) lv_style_init(all[i]);
    lv_style_set_bg_opa(&st_bg, LV_OPA_COVER);
    lv_style_set_radius(&st_bg, 0);
    lv_style_set_border_width(&st_bg, 0);
    lv_style_set_pad_all(&st_bg, PAD);
    lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
    lv_style_set_border_width(&st_card, 1);
    lv_style_set_radius(&st_card, 8);
    lv_style_set_pad_all(&st_card, 14);
    lv_style_set_text_font(&st_title, &lv_font_montserrat_20);
    lv_style_set_text_font(&st_text, &lv_font_montserrat_14);
    lv_style_set_text_font(&st_muted, &lv_font_montserrat_14);
    lv_style_set_text_font(&st_small, &lv_font_montserrat_12);
    lv_style_set_text_font(&st_section, &lv_font_montserrat_12);
    lv_style_set_text_letter_space(&st_section, 1);
    lv_style_set_bg_opa(&st_btn, LV_OPA_COVER);
    lv_style_set_border_width(&st_btn, 1);
    lv_style_set_radius(&st_btn, 6);
    lv_style_set_shadow_width(&st_btn, 0);
    lv_style_set_pad_hor(&st_btn, 14);
    lv_style_set_pad_ver(&st_btn, 8);
    lv_style_set_text_font(&st_btn, &lv_font_montserrat_14);
    lv_style_set_border_width(&st_btn_primary, 0);
    lv_style_set_bg_opa(&st_row, LV_OPA_COVER);
    lv_style_set_border_width(&st_row, 1);
    lv_style_set_radius(&st_row, 6);
    lv_style_set_shadow_width(&st_row, 0);
    lv_style_set_pad_hor(&st_row, 12);
    lv_style_set_pad_ver(&st_row, 0);
    lv_style_set_border_width(&st_row_sel, 2);
    lv_style_set_bg_opa(&st_ta, LV_OPA_COVER);
    lv_style_set_border_width(&st_ta, 1);
    lv_style_set_radius(&st_ta, 6);
    lv_style_set_pad_all(&st_ta, 8);
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
    lv_style_set_pad_all(&st_modal, 18);
    lv_style_set_pad_row(&st_modal, 10);
    lv_style_set_bg_opa(&st_kb, LV_OPA_COVER);
    lv_style_set_border_width(&st_kb, 0);
    lv_style_set_radius(&st_kb, 0);
    lv_style_set_bg_opa(&st_kb_btn, LV_OPA_COVER);
    lv_style_set_radius(&st_kb_btn, 6);
    lv_style_set_border_width(&st_kb_btn, 0);
    lv_style_set_shadow_width(&st_kb_btn, 0);
    lv_style_set_text_font(&st_kb_btn, &lv_font_montserrat_18);
    lv_style_set_text_font(&st_ok, &lv_font_montserrat_14);
    lv_style_set_text_font(&st_warn, &lv_font_montserrat_14);
    lv_style_set_text_font(&st_err, &lv_font_montserrat_14);
    restyle(devos_theme_get());
    s_styles_ready = true;
}

static lv_obj_t *mk_label(lv_obj_t *parent, lv_style_t *st, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_add_style(l, st, 0);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *txt, lv_style_t *extra, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_add_style(b, &st_btn, 0);
    if (extra) lv_obj_add_style(b, extra, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void set_text(lv_obj_t *lbl, const char *txt)
{
    if (lbl && strcmp(lv_label_get_text(lbl), txt) != 0) lv_label_set_text(lbl, txt);
}

static void set_style_one(lv_obj_t *obj, lv_style_t *want, lv_style_t *const *options, int n)
{
    for (int i = 0; i < n; i++) {
        if (options[i] != want) lv_obj_remove_style(obj, options[i], 0);
    }
    lv_obj_add_style(obj, want, 0);
}

/* ======================================================================== */
/* Actions                                                                  */
/* ======================================================================== */
static bool state_is_active(devos_ts_state_t st)
{
    return st == DEVOS_TS_WAIT_WIFI || st == DEVOS_TS_CONNECTING || st == DEVOS_TS_REGISTERING ||
           st == DEVOS_TS_CONNECTED || st == DEVOS_TS_RECONNECTING;
}

static void open_key_dialog(void);

/* Tailscale and a WireGuard tunnel can't run together (UDP 51820, routes). */
static bool s_wg_blocked;
static bool wg_blocks(void)
{
    s_wg_blocked = devos_wg_active();
    return s_wg_blocked;
}

static void toggle_conn(void)
{
    devos_ts_info_t info;
    devos_tailnet_get_info(&info);
    if (state_is_active(info.state)) {
        devos_tailnet_disconnect();
    } else if (wg_blocks()) {
        /* message shown by refresh_ui */
    } else if (!info.registered && !info.has_auth_key) {
        open_key_dialog();
    } else {
        devos_tailnet_connect();
    }
    refresh_ui();
}

static void toggle_cb(lv_event_t *e) { LV_UNUSED(e); toggle_conn(); }

static void peer_ssh(int idx)
{
    devos_ts_peer_t p;
    if (devos_tailnet_get_peer(idx, &p) != 0) return;
    if (!devos_core_open_with("terminal", "ssh", p.ip)) {
        devos_toast_show("The Terminal is switched off (Settings > Apps)", DEVOS_TOAST_WARN, 3000);
    }
}

/* The header button row has the keyboard (focus ring showing). */
static bool hdr_active(void)
{
    return devos_focus_get(&s_hdr) && s_hdr.ring;
}

static void select_peer(int idx)
{
    if (hdr_active()) devos_focus_clear(&s_hdr);        /* keys go to the peers again */
    s_sel = idx;
    s_seen_gen = UINT32_MAX;
    refresh_ui();
    if (idx >= 0 && idx < DEVOS_TS_MAX_PEERS && rows[idx]) lv_obj_scroll_to_view(rows[idx], LV_ANIM_ON);
}

static void row_cb(lv_event_t *e) { select_peer((int)(intptr_t)lv_event_get_user_data(e)); }
static void ssh_cb(lv_event_t *e) { peer_ssh((int)(intptr_t)lv_event_get_user_data(e)); }
static void ping_cb(lv_event_t *e)
{
    devos_tailnet_ping((int)(intptr_t)lv_event_get_user_data(e));
    refresh_ui();
}

/* ======================================================================== */
/* Dialogs                                                                  */
/* ======================================================================== */
static bool dialog_open(void)
{
    return overlay && !lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

static void close_dialogs(void)
{
    if (!overlay) return;
    if (s_dlg_f) devos_focus_clear(s_dlg_f);
    s_dlg_f = NULL;
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dlg_key, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dlg_name, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dlg_forget, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(ta_key, "");
}

/* Open dlg with `first` (its text field or default button) focused. The
 * header's focus is left alone, so it is where it was once the dialog closes. */
static void show_dialog(lv_obj_t *dlg, devos_focus_t *f, lv_obj_t *first)
{
    close_dialogs();
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(dlg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay);
    s_dlg_f = f;
    devos_focus_set(f, first);
    if (!s_via_key) {                                   /* opened by a tap: no ring */
        f->ring = false;
        lv_obj_remove_state(first, LV_STATE_FOCUS_KEY);
    }
    bool ta = lv_obj_check_type(first, &lv_textarea_class);
    if (ta) lv_keyboard_set_textarea(kb, first);
    if (ta && !tab5_keyboard_is_connected()) lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void open_key_dialog(void) { show_dialog(dlg_key, &s_f_key, ta_key); }
static void open_forget_dialog(void) { show_dialog(dlg_forget, &s_f_forget, btn_forget_ok); }

static void open_name_dialog(void)
{
    char name[DEVOS_TS_HOSTNAME_MAX];
    devos_tailnet_get_hostname(name, sizeof(name));
    lv_textarea_set_text(ta_name, name);
    show_dialog(dlg_name, &s_f_name, ta_name);
}

static void key_submit(void)
{
    const char *k = lv_textarea_get_text(ta_key);
    if (!k || strncmp(k, "tskey-", 6) != 0) return;     /* keep the dialog open */
    devos_tailnet_set_auth_key(k);
    close_dialogs();
    if (!wg_blocks()) devos_tailnet_connect();
    refresh_ui();
}

static void name_submit(void)
{
    devos_tailnet_set_hostname(lv_textarea_get_text(ta_name));
    close_dialogs();
    devos_ts_info_t info;
    devos_tailnet_get_info(&info);
    if (state_is_active(info.state)) {                   /* reconnect under the new name */
        devos_tailnet_disconnect();
        devos_tailnet_connect();
    }
    refresh_ui();
}

static void key_btn_cb(lv_event_t *e) { LV_UNUSED(e); open_key_dialog(); }
static void name_btn_cb(lv_event_t *e) { LV_UNUSED(e); open_name_dialog(); }
static void forget_btn_cb(lv_event_t *e) { LV_UNUSED(e); open_forget_dialog(); }
static void key_ok_cb(lv_event_t *e) { LV_UNUSED(e); key_submit(); }
static void name_ok_cb(lv_event_t *e) { LV_UNUSED(e); name_submit(); }
static void cancel_cb(lv_event_t *e) { LV_UNUSED(e); close_dialogs(); }
static void forget_ok_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    close_dialogs();
    devos_tailnet_forget();
    refresh_ui();
}

/* Tap on a text field: the on-screen keyboard, unless a real one is attached. */
static void ta_click_cb(lv_event_t *e)
{
    if (tab5_keyboard_is_connected()) return;
    lv_keyboard_set_textarea(kb, lv_event_get_target(e));
    lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL) { close_dialogs(); return; }
    if (code != LV_EVENT_READY) return;
    if (!lv_obj_has_flag(dlg_key, LV_OBJ_FLAG_HIDDEN)) key_submit();
    else if (!lv_obj_has_flag(dlg_name, LV_OBJ_FLAG_HIDDEN)) name_submit();
}

static lv_obj_t *mk_dialog(int w)
{
    lv_obj_t *d = lv_obj_create(overlay);
    lv_obj_remove_style_all(d);
    lv_obj_add_style(d, &st_modal, 0);
    lv_obj_set_size(d, w, LV_SIZE_CONTENT);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_flex_flow(d, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    return d;
}

static lv_obj_t *mk_ta(lv_obj_t *parent, const char *placeholder, int w, int max)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_add_style(ta, &st_ta, 0);
    lv_obj_add_style(ta, &st_ta_focus, LV_STATE_FOCUSED);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_max_length(ta, max);
    lv_obj_set_width(ta, w);
    lv_obj_add_event_cb(ta, ta_click_cb, LV_EVENT_CLICKED, NULL);
    return ta;
}

/* Button row with the dialog's key hint on the left. */
static lv_obj_t *mk_btn_row(lv_obj_t *parent, const char *hint)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);     /* don't clip the focus ring */
    lv_obj_t *l = mk_label(row, &st_small, hint);
    lv_obj_set_flex_grow(l, 1);
    return row;
}

static void build_dialogs(void)
{
    overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(overlay);
    lv_obj_add_style(overlay, &st_overlay, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(overlay, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(overlay, -PAD, -PAD);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    dlg_key = mk_dialog(760);
    lv_obj_t *t = mk_label(dlg_key, &st_title, "Tailscale auth key");
    LV_UNUSED(t);
    lv_obj_t *l = mk_label(dlg_key, &st_muted,
                           "Create one at login.tailscale.com/admin/settings/keys (on any computer or phone) "
                           "and type it here. It is used once to add this Tab5 to your tailnet and then "
                           "erased; the device keeps its own node key.\n"
                           "Or save the key in a file named ts_key in the SD card root and reboot.");
    lv_obj_set_width(l, 720);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    ta_key = mk_ta(dlg_key, "tskey-auth-...", 720, DEVOS_TS_KEY_MAX - 1);
    lv_obj_t *row = mk_btn_row(dlg_key, "Enter save & connect  |  Tab move  |  Esc cancel");
    lv_obj_t *b_cancel = mk_btn(row, "Cancel", NULL, cancel_cb, NULL);
    lv_obj_t *b_ok = mk_btn(row, LV_SYMBOL_OK "  Save & connect", &st_btn_primary, key_ok_cb, NULL);
    devos_focus_init(&s_f_key);
    devos_focus_add(&s_f_key, ta_key);
    devos_focus_add(&s_f_key, b_cancel);
    devos_focus_add(&s_f_key, b_ok);

    dlg_name = mk_dialog(560);
    mk_label(dlg_name, &st_title, "Device name");
    l = mk_label(dlg_name, &st_muted, "How this Tab5 appears on your tailnet (letters, digits and -).");
    lv_obj_set_width(l, 520);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    ta_name = mk_ta(dlg_name, "devos-tab5", 520, DEVOS_TS_HOSTNAME_MAX - 1);
    row = mk_btn_row(dlg_name, "Enter save  |  Tab move  |  Esc cancel");
    b_cancel = mk_btn(row, "Cancel", NULL, cancel_cb, NULL);
    b_ok = mk_btn(row, LV_SYMBOL_OK "  Save", &st_btn_primary, name_ok_cb, NULL);
    devos_focus_init(&s_f_name);
    devos_focus_add(&s_f_name, ta_name);
    devos_focus_add(&s_f_name, b_cancel);
    devos_focus_add(&s_f_name, b_ok);

    dlg_forget = mk_dialog(600);
    mk_label(dlg_forget, &st_title, "Forget this device?");
    l = mk_label(dlg_forget, &st_muted,
                 "Disconnects and erases this Tab5's Tailscale keys and cached peers. To use Tailscale "
                 "again you will need a new auth key. Also remove the old machine from the admin console.");
    lv_obj_set_width(l, 560);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    row = mk_btn_row(dlg_forget, "Enter / Y forget  |  Esc / N cancel");
    b_cancel = mk_btn(row, "Cancel", NULL, cancel_cb, NULL);
    btn_forget_ok = mk_btn(row, LV_SYMBOL_TRASH "  Forget", &st_btn_danger, forget_ok_cb, NULL);
    devos_focus_init(&s_f_forget);
    devos_focus_add(&s_f_forget, b_cancel);
    devos_focus_add(&s_f_forget, btn_forget_ok);

    kb = lv_keyboard_create(overlay);
    lv_obj_add_style(kb, &st_kb, 0);
    lv_obj_add_style(kb, &st_kb_btn, LV_PART_ITEMS);
    lv_obj_set_size(kb, DEVOS_SCREEN_WIDTH, 280);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

/* ======================================================================== */
/* Refresh                                                                  */
/* ======================================================================== */
static void fmt_ago(char *buf, size_t len, int s)
{
    if (s < 0) snprintf(buf, len, "not seen yet");
    else if (s < 90) snprintf(buf, len, "seen %ds ago", s);
    else if (s < 5400) snprintf(buf, len, "seen %dm ago", s / 60);
    else snprintf(buf, len, "seen %dh ago", s / 3600);
}

static void refresh_ui(void)
{
    if (!screen) return;
    uint32_t gen = devos_tailnet_generation();
    s_seen_gen = gen;

    devos_ts_info_t info;
    devos_tailnet_get_info(&info);
    char buf[256];

    /* State headline */
    lv_style_t *st_opts[] = { &st_ok, &st_warn, &st_err, &st_muted };
    lv_style_t *st = &st_muted;
    switch (info.state) {
    case DEVOS_TS_CONNECTED:  st = &st_ok; break;
    case DEVOS_TS_ERROR:      st = &st_err; break;
    case DEVOS_TS_OFF:
    case DEVOS_TS_NEEDS_KEY:  st = &st_muted; break;
    default:                  st = &st_warn; break;
    }
    snprintf(buf, sizeof(buf), LV_SYMBOL_LOOP "  Tailscale  -  %s", devos_tailnet_state_text(info.state));
    set_text(lbl_state, buf);
    set_style_one(lbl_state, st, st_opts, 4);
    lv_obj_set_style_text_font(lbl_state, &lv_font_montserrat_20, 0);

    set_text(lbl_toggle, state_is_active(info.state) ? LV_SYMBOL_POWER "  Disconnect"
                         : (!info.registered && !info.has_auth_key) ? LV_SYMBOL_PLUS "  Set up"
                                                                     : LV_SYMBOL_POWER "  Connect");

    /* Line 1: this device */
    if (info.ip[0]) {
        snprintf(buf, sizeof(buf), "This device: %s  |  %s%s%s", info.hostname, info.ip,
                 info.domain[0] ? "  |  MagicDNS: " : "", info.domain);
    } else {
        snprintf(buf, sizeof(buf), "This device: %s  |  %s", info.hostname,
                 info.registered ? "enrolled" : "not enrolled");
    }
    set_text(lbl_line1, buf);

    /* Line 2: relay / peers / key */
    char key[48] = "";
    if (info.key_expired) {
        snprintf(key, sizeof(key), "  |  node key EXPIRED");
    } else if (info.key_expiry > 0) {
        time_t tt = (time_t)info.key_expiry;
        struct tm tm;
        gmtime_r(&tt, &tm);
        snprintf(key, sizeof(key), "  |  key expires %04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    }
    if (info.state == DEVOS_TS_CONNECTED || info.state == DEVOS_TS_RECONNECTING) {
        snprintf(buf, sizeof(buf), "Relay: %s%s  |  Peers: %d (%d direct)%s", info.derp[0] ? info.derp : "-",
                 info.derp_connected ? "" : " (not connected)", info.peer_count, info.peers_direct, key);
    } else if (info.state == DEVOS_TS_NEEDS_KEY) {
        snprintf(buf, sizeof(buf), "Add this Tab5 to your tailnet with an auth key: press C (or tap Set up).");
    } else if (info.state == DEVOS_TS_OFF) {
        snprintf(buf, sizeof(buf), "Tailscale is off. Local network connections work as normal.");
    } else {
        snprintf(buf, sizeof(buf), "Working in the background; you can keep using the Tab5.");
    }
    set_text(lbl_line2, buf);

    /* Message: error / login URL */
    if (s_wg_blocked && !devos_wg_active()) s_wg_blocked = false;
    if (s_wg_blocked) {
        set_text(lbl_msg, "A WireGuard tunnel is running. Disconnect it in the WireGuard app first - "
                          "the two can't run at the same time.");
        lv_obj_remove_flag(lbl_msg, LV_OBJ_FLAG_HIDDEN);
    } else if (info.last_error[0]) {
        snprintf(buf, sizeof(buf), "%s%s%s", info.last_error, info.login_url[0] ? "\nLogin URL: " : "",
                 info.login_url);
        set_text(lbl_msg, buf);
        lv_obj_remove_flag(lbl_msg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_msg, LV_OBJ_FLAG_HIDDEN);
    }

    /* Peers */
    int n = devos_tailnet_peer_count();
    if (s_sel >= n) s_sel = n - 1;
    snprintf(buf, sizeof(buf), "PEERS (%d)", n);
    set_text(lbl_peers_h, buf);
    if (n == 0) {
        set_text(lbl_no_peers, info.state == DEVOS_TS_CONNECTED ? "No other devices on this tailnet yet."
                               : "Peers appear here once connected.");
        lv_obj_remove_flag(lbl_no_peers, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_no_peers, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < DEVOS_TS_MAX_PEERS; i++) {
        devos_ts_peer_t p;
        if (i >= n || devos_tailnet_get_peer(i, &p) != 0) {
            lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
        if (i == s_sel) lv_obj_add_style(rows[i], &st_row_sel, 0);
        else lv_obj_remove_style(rows[i], &st_row_sel, 0);

        snprintf(buf, sizeof(buf), "%d  %s   %s", i + 1, p.name[0] ? p.name : p.fqdn, p.ip);
        set_text(row_name[i], buf);
        char ago[32];
        fmt_ago(ago, sizeof(ago), p.last_seen_s);
        snprintf(buf, sizeof(buf), "%s  |  %s  |  %s", p.direct ? "direct" : "via relay", ago, p.fqdn);
        set_text(row_sub[i], buf);
        lv_style_t *sub_opts[] = { &st_ok, &st_small };
        set_style_one(row_sub[i], p.direct ? &st_ok : &st_small, sub_opts, 2);
        lv_obj_set_style_text_font(row_sub[i], &lv_font_montserrat_12, 0);

        if (p.ping_ms >= 0) snprintf(buf, sizeof(buf), "%d ms", p.ping_ms);
        else if (p.ping_ms == -2) snprintf(buf, sizeof(buf), "timeout");
        else if (p.ping_ms == -3) snprintf(buf, sizeof(buf), "pinging...");
        else buf[0] = '\0';
        set_text(row_ping[i], buf);
        lv_style_t *ping_opts[] = { &st_ok, &st_err, &st_muted };
        set_style_one(row_ping[i], p.ping_ms >= 0 ? &st_ok : p.ping_ms == -2 ? &st_err : &st_muted, ping_opts, 3);
    }
}

/* Key hint for whichever region has the keyboard. */
static void update_hint(void)
{
    if (!lbl_hint) return;
    set_text(lbl_hint, hdr_active()
             ? "Left/Right, Tab choose  |  Enter press  |  Down or Esc back to peers  |  C / K / N / F work too"
             : "Up/Down, 1-9 select  |  Enter SSH  |  P ping  |  C connect  |  K auth key  |  N name  |  "
               "F forget  |  Tab buttons  |  Esc back");
}

static void poll_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!screen || lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN)) return;
    if (devos_tailnet_generation() != s_seen_gen) refresh_ui();
    update_hint();                                      /* a tap hides the ring */
}

/* ======================================================================== */
/* Build                                                                    */
/* ======================================================================== */
static void apply_theme(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    if (!s_styles_ready) return;
    restyle(p);
    lv_obj_report_style_change(NULL);
}

static void tailscale_init(void)
{
    styles_init();

    screen = lv_obj_create(lv_screen_active());
    app_descriptor.screen = screen;
    lv_obj_remove_style_all(screen);
    lv_obj_add_style(screen, &st_bg, 0);
    lv_obj_set_size(screen, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(screen, 0, DEVOS_TOP_BAR_HEIGHT);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);

    const int w = DEVOS_SCREEN_WIDTH - 2 * PAD;

    /* Status card */
    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &st_card, 0);
    lv_obj_set_size(card, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = lv_obj_create(card);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, lv_pct(100), 40);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(top, LV_OBJ_FLAG_OVERFLOW_VISIBLE);     /* don't clip the focus ring */
    lbl_state = mk_label(top, &st_title, "Tailscale");
    lv_obj_align(lbl_state, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *btns = lv_obj_create(top);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 8, 0);
    lv_obj_align(btns, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(btns, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    btn_key = mk_btn(btns, LV_SYMBOL_EDIT "  Auth key", NULL, key_btn_cb, NULL);
    btn_name = mk_btn(btns, LV_SYMBOL_SETTINGS "  Device name", NULL, name_btn_cb, NULL);
    btn_forget = mk_btn(btns, LV_SYMBOL_TRASH "  Forget", &st_btn_danger, forget_btn_cb, NULL);
    btn_toggle = mk_btn(btns, LV_SYMBOL_POWER "  Connect", &st_btn_primary, toggle_cb, NULL);
    lbl_toggle = lv_obj_get_child(btn_toggle, 0);
    devos_focus_init(&s_hdr);
    devos_focus_add(&s_hdr, btn_key);
    devos_focus_add(&s_hdr, btn_name);
    devos_focus_add(&s_hdr, btn_forget);
    devos_focus_add(&s_hdr, btn_toggle);

    lbl_line1 = mk_label(card, &st_text, "");
    lbl_line2 = mk_label(card, &st_muted, "");
    lbl_msg = mk_label(card, &st_err, "");
    lv_obj_set_width(lbl_msg, lv_pct(100));
    lv_label_set_long_mode(lbl_msg, LV_LABEL_LONG_WRAP);

    /* Peers */
    lv_obj_t *head = lv_obj_create(screen);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, w, 30);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lbl_peers_h = mk_label(head, &st_section, "PEERS");
    lv_obj_align(lbl_peers_h, LV_ALIGN_LEFT_MID, 2, 0);
    lbl_hint = mk_label(head, &st_small, "");
    lv_obj_align(lbl_hint, LV_ALIGN_RIGHT_MID, 0, 0);
    update_hint();

    list_peers = lv_obj_create(screen);
    lv_obj_remove_style_all(list_peers);
    lv_obj_set_width(list_peers, w);
    lv_obj_set_flex_grow(list_peers, 1);
    lv_obj_set_flex_flow(list_peers, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_peers, 6, 0);
    lv_obj_set_scroll_dir(list_peers, LV_DIR_VER);

    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 10, 0);

    lbl_no_peers = mk_label(list_peers, &st_muted, "");
    for (int i = 0; i < DEVOS_TS_MAX_PEERS; i++) {
        lv_obj_t *r = rows[i] = lv_button_create(list_peers);
        lv_obj_remove_style_all(r);
        lv_obj_add_style(r, &st_row, 0);
        lv_obj_set_size(r, lv_pct(100), ROW_H);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        row_name[i] = mk_label(r, &st_text, "");
        lv_obj_align(row_name[i], LV_ALIGN_TOP_LEFT, 0, 9);
        row_sub[i] = mk_label(r, &st_small, "");
        lv_obj_align(row_sub[i], LV_ALIGN_TOP_LEFT, 0, 31);
        lv_obj_t *b_ssh = mk_btn(r, LV_SYMBOL_POWER "  SSH", &st_btn_primary, ssh_cb, (void *)(intptr_t)i);
        lv_obj_align(b_ssh, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_t *b_ping = mk_btn(r, "Ping", NULL, ping_cb, (void *)(intptr_t)i);
        lv_obj_align_to(b_ping, b_ssh, LV_ALIGN_OUT_LEFT_MID, -8, 0);
        row_ping[i] = mk_label(r, &st_muted, "");
        lv_obj_align(row_ping[i], LV_ALIGN_RIGHT_MID, -210, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
    }

    build_dialogs();
    devos_theme_add_listener(apply_theme, NULL);
    lv_timer_create(poll_cb, 500, NULL);
    refresh_ui();
}

static void tailscale_show(void)
{
    s_seen_gen = UINT32_MAX;
    refresh_ui();
    update_hint();
}

static void tailscale_hide(void)
{
    close_dialogs();
    devos_focus_clear(&s_hdr);
}

/* ======================================================================== */
/* Keyboard                                                                 */
/* ======================================================================== */
/* Left/Right along a dialog's button row (never onto its text field). */
static void btn_row_step(devos_focus_t *f, int dir)
{
    lv_obj_t *from = devos_focus_get(f);
    devos_focus_move(f, dir);
    lv_obj_t *to = devos_focus_get(f);
    if (to && lv_obj_check_type(to, &lv_textarea_class)) devos_focus_set(f, from);
}

static bool dialog_key(uint32_t key, uint8_t mods)
{
    if (tab5_keyboard_is_connected()) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    if (key == LV_KEY_ESC) { close_dialogs(); return true; }
    devos_focus_t *f = s_dlg_f;
    if (!f) return true;
    bool plain = !(mods & (DEVOS_MOD_CTRL | DEVOS_MOD_FN | DEVOS_MOD_ALT));
    if (f == &s_f_forget && plain) {
        if (key == 'y' || key == 'Y') { forget_ok_cb(NULL); return true; }
        if (key == 'n' || key == 'N') { close_dialogs(); return true; }
    }
    lv_obj_t *ta = f == &s_f_key ? ta_key : f == &s_f_name ? ta_name : NULL;
    lv_obj_t *cur = devos_focus_get(f);
    if (cur && cur != ta && (key == LV_KEY_LEFT || key == LV_KEY_RIGHT)) {
        btn_row_step(f, key == LV_KEY_RIGHT ? 1 : -1);
        return true;
    }
    /* typing while a button has the focus goes to the text field */
    if (ta && cur != ta && plain && ((key > ' ' && key <= 126) || key == '\b' || key == LV_KEY_DEL)) {
        devos_focus_set(f, ta);
        cur = ta;
    }
    if (cur && cur == ta && key == LV_KEY_DEL) {        /* forward delete */
        lv_textarea_delete_char_forward(ta);
        return true;
    }
    if (devos_focus_key(f, key, mods)) return true;
    if ((key == '\r' || key == '\n') && ta && devos_focus_get(f) == ta) {     /* Enter in the field */
        if (f == &s_f_key) key_submit();
        else name_submit();
    }
    return true;                                        /* modal: nothing leaks through */
}

/* Keys while the header button row has the focus ring; false = not used. */
static bool header_key(uint32_t key, uint8_t mods)
{
    lv_obj_t *cur = devos_focus_get(&s_hdr);
    bool back = (mods & DEVOS_MOD_SHIFT) != 0;
    if (key == LV_KEY_ESC || key == LV_KEY_DOWN ||
        (key == '\t' && cur == (back ? btn_key : btn_toggle))) {
        devos_focus_clear(&s_hdr);
        if (key != LV_KEY_ESC && s_sel < 0 && devos_tailnet_peer_count() > 0) select_peer(0);
        return true;
    }
    if (key == LV_KEY_UP) return true;
    if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        devos_focus_move(&s_hdr, key == LV_KEY_RIGHT ? 1 : -1);
        return true;
    }
    if (key == '\t' || key == '\r' || key == '\n' || key == ' ') return devos_focus_key(&s_hdr, key, mods);
    return false;                                       /* letters: the shortcuts below */
}

static bool handle_key(uint32_t key, uint8_t mods)
{
    if (dialog_open()) return dialog_key(key, mods);
    if (mods & (DEVOS_MOD_FN | DEVOS_MOD_CTRL | DEVOS_MOD_ALT)) return false;
    if (hdr_active() && header_key(key, mods)) return true;

    int n = devos_tailnet_peer_count();
    switch (key) {
    case 'c': case 'C': toggle_conn(); return true;
    case 'k': case 'K': open_key_dialog(); return true;
    case 'n': case 'N': open_name_dialog(); return true;
    case 'f': case 'F': open_forget_dialog(); return true;
    case 'p': case 'P':
        if (s_sel >= 0) { devos_tailnet_ping(s_sel); refresh_ui(); }
        return true;
    case '\t':                                          /* into the header buttons */
        devos_focus_clear(&s_hdr);
        devos_focus_move(&s_hdr, (mods & DEVOS_MOD_SHIFT) ? -1 : 1);
        return true;
    case LV_KEY_DOWN:
        if (n > 0) select_peer(s_sel < 0 ? 0 : (s_sel + 1 < n ? s_sel + 1 : s_sel));
        return true;
    case LV_KEY_UP:
        if (n > 0) select_peer(s_sel <= 0 ? 0 : s_sel - 1);
        return true;
    case DEVOS_KEY_PGDN:                                /* Sym+Down: a screenful */
        if (n > 0) select_peer(s_sel < 0 ? 0 : (s_sel + 6 < n ? s_sel + 6 : n - 1));
        return true;
    case DEVOS_KEY_PGUP:
        if (n > 0) select_peer(s_sel > 6 ? s_sel - 6 : 0);
        return true;
    case '\r': case '\n':
        if (s_sel >= 0) { peer_ssh(s_sel); return true; }
        return false;
    case LV_KEY_ESC:
        if (s_sel >= 0) { select_peer(-1); return true; }
        return false;                                   /* Home */
    default:
        break;
    }
    if (key >= '1' && key <= '9' && (int)(key - '1') < n) {
        select_peer((int)(key - '1'));
        return true;
    }
    return false;
}

static bool tailscale_handle_key(uint32_t key, uint8_t mods)
{
    s_via_key = true;
    bool used = handle_key(key, mods);
    s_via_key = false;
    update_hint();
    return used;
}

static int tailscale_telemetry_lines(char lines[3][64])
{
    devos_ts_info_t info;
    devos_tailnet_get_info(&info);
    if (info.state != DEVOS_TS_CONNECTED) {
        snprintf(lines[0], sizeof(lines[0]), "* %s", devos_tailnet_state_text(info.state));
        snprintf(lines[1], sizeof(lines[1]), "* %s", info.registered ? info.hostname : "Not enrolled");
        snprintf(lines[2], sizeof(lines[2]), "* LAN routing only");
        return 3;
    }
    snprintf(lines[0], sizeof(lines[0]), "* Peers: %d (%d direct)", info.peer_count, info.peers_direct);
    snprintf(lines[1], sizeof(lines[1]), "* Relay: %s", info.derp[0] ? info.derp : "-");
    snprintf(lines[2], sizeof(lines[2]), "* IP: %s", info.ip);
    return 3;
}

/* Sym+S sheet (devos_shortcuts.h) */
static const char *tailscale_shortcuts(void)
{
    return
        "Peers\n"
        "Up / Down, 1 ... 9\tPick a peer\n"
        "Enter\tSSH to it in the Terminal\n"
        "P\tPing it\n"
        "Anywhere here\n"
        "C\tConnect / disconnect\n"
        "K\tAuth key\n"
        "N\tThis device's name on the tailnet\n"
        "F\tForget this device (log out)\n"
        "Tab\tThe buttons along the top (Down / Esc: back)\n"
        "Esc\tDeselect, then Home\n";
}

devos_app_descriptor_t *app_tailscale_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_TAILSCALE;
    app_descriptor.uid = "tailscale";
    app_descriptor.icon = LV_SYMBOL_LOOP;
    app_descriptor.draw_icon = devos_icon_tailscale;
    app_descriptor.category = "network";
    app_descriptor.name = "Tailscale";
    app_descriptor.title = "Tailscale";
    app_descriptor.subtitle = "Private network (MicroLink)";
    app_descriptor.screen = screen;
    app_descriptor.init = tailscale_init;
    app_descriptor.show = tailscale_show;
    app_descriptor.hide = tailscale_hide;
    app_descriptor.handle_key = tailscale_handle_key;
    app_descriptor.get_telemetry_lines = tailscale_telemetry_lines;
    app_descriptor.get_shortcuts = tailscale_shortcuts;
    return &app_descriptor;
}
