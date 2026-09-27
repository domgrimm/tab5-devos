/* Terminal: multi-session SSH client with a real VT100/xterm terminal.
 *
 * Each SSH session (components/libssh2_port) gets a devos_vterm emulator;
 * a poll timer feeds every session's output into its emulator (background
 * sessions keep their state) and the active one is drawn as a character grid
 * in Nimbus Mono 14 (8 x 16 px cells, 159 x 40 full width, 126 x 40 with the
 * sidebar). Sidebar: sessions, saved hosts, device key. Dialogs: connect /
 * edit saved host, password, host-key trust (TOFU), device key, confirm.
 *
 * Keyboard (AGENTS.md invariant 9). Focus is on the shell or on the side
 * panel; the shell owns every key it can use, so the panel is reached by Sym.
 *   Shell:   everything goes to the remote (Ctrl+letter -> control code,
 *            Alt+key -> ESC prefix, arrows honour application-cursor mode,
 *            Esc, Tab, Backspace = DEL). Local: Sym+L focus the panel (opens
 *            it if hidden), Sym+N new connection, Sym+K device key,
 *            Sym+Up/Down scrollback, Sym+Left/Right = Home/End, Alt+1..8
 *            switch session. No session: Enter new connection, Tab / Up /
 *            Down go to the panel, Esc Home. Closed / failed: Enter reconnects.
 *   Panel:   Up/Down select, Tab / Aa+Tab next / previous section, Enter open
 *            (session: switch to it; saved host: connect; + New; Device key),
 *            N new, E edit saved host, D / Del disconnect or remove a session
 *            / delete a saved host (asks first unless nothing is lost), K
 *            device key, Esc back to the shell, Sym+L hide the panel.
 *   Dialogs: open focused on the first field / default button (devos_focus);
 *            Tab / Up / Down move, Left / Right pick the sign-in method or
 *            move along a button row, Space ticks, Enter submits (or presses
 *            the focused button), Esc cancels. Host key: Y trust, N reject.
 * The shell cursor is drawn hollow while the panel or a dialog has focus.
 */
#include "app_terminal.h"
#include "devos_config.h"
#include "devos_icons.h"
#include "devos_focus.h"
#include "devos_theme.h"
#include "devos_vterm.h"
#include "libssh2_port.h"
#include "tab5_keyboard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_nimbus_mono_14);

#define CELL_W        8
#define CELL_H        16
#define SIDEBAR_W     DEVOS_PANE_LEFT_WIDTH
#define HEADER_H      32
#define TERM_PAD      4
#define SCROLLBACK    1000

typedef struct {
    devos_vterm_t *vt;
    int view_offset;               /* lines scrolled back (0 = live) */
    uint32_t sb_seen;              /* devos_vterm_scrolled_total() last seen */
    ssh_session_state_t last_state;
    /* connection parameters, for reconnect (password is never kept) */
    char alias[SSH_MAX_ALIAS_LEN];
    char host[SSH_MAX_HOST_LEN];
    int port;
    char user[SSH_MAX_USER_LEN];
    ssh_auth_type_t auth;
    char keypath[SSH_MAX_PATH_LEN];
} term_sess_t;

static term_sess_t s_ts[SSH_MAX_SESSIONS];

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;

/* styles (recoloured on theme change) */
static bool s_styles_ready = false;
static lv_style_t st_bg, st_sidebar, st_header, st_title, st_text, st_muted, st_small, st_btn, st_btn_primary,
                  st_btn_danger, st_row, st_row_active, st_ta, st_ta_focus, st_overlay, st_modal, st_kb,
                  st_kb_btn, st_err, st_ok, st_warn, st_mono, st_ring;

/* layout */
static lv_obj_t *sidebar, *list_sessions, *list_hosts, *term_area, *header, *lbl_header_title,
                *lbl_header_info, *term_view, *lbl_empty;
static bool s_sidebar_visible = true;
static int s_cols = 126, s_rows = 40;
static int s_active = 0;           /* active session id (0 = none) */
static char s_list_key[256] = "";
static char s_hosts_key[64] = "";

/* side panel keyboard focus: a custom list over these objects */
static lv_obj_t *side_scroll, *btn_new, *btn_devkey, *lbl_panel_hint;
static lv_obj_t *s_sess_row[SSH_MAX_SESSIONS];     /* by session id - 1 */
static lv_obj_t *s_host_row[SSH_MAX_BOOKMARKS];    /* by saved-host index */
static bool s_panel_focus = false;                 /* keys go to the panel, not the shell */
enum { PI_NONE, PI_NEW, PI_SESSION, PI_HOST, PI_DEVKEY };
static int s_psel_kind = PI_NONE, s_psel_idx = 0, s_psel_pos = 0;

#define HINT_SHELL "Sym+L  panel     Sym+N  new\nSym+K  device key\nSym+Up/Down  scroll back\nAlt+1..8  switch session"
#define HINT_PANEL "Up/Down  select    Tab  section\nEnter  open    N  new    E  edit\nD  disconnect / delete\nEsc  shell     Sym+L  hide panel"

/* dialogs */
static lv_obj_t *overlay, *kb;
static lv_obj_t *dlg_connect, *lbl_connect_title, *ta_host, *ta_port, *ta_user, *ta_alias, *ta_pass, *ta_keypath,
                *cb_save, *btn_auth[3], *lbl_pass_hint, *row_pass, *row_key, *lbl_connect_err, *row_connect_btns,
                *lbl_connect_btn, *lbl_connect_hint;
static lv_obj_t *dlg_password, *lbl_pw_title, *ta_pw;
static lv_obj_t *dlg_hostkey, *lbl_hk_body, *btn_hk_trust;
static lv_obj_t *dlg_devkey, *lbl_dk_body, *btn_dk_close, *btn_dk_create, *btn_dk_install;
static lv_obj_t *dlg_confirm, *lbl_confirm_body, *btn_confirm_ok, *lbl_confirm_ok;
static devos_focus_t s_f_connect, s_f_pw, s_f_hk, s_f_dk, s_f_confirm;
static int s_auth_choice = 0;      /* 0 password, 1 device key, 2 key file */
static int s_hostkey_for = 0;      /* session id the host-key dialog is for */

/* connect dialog editing a saved host (index into s_hosts, -1 = new connection) */
static int s_edit_idx = -1;
static ssh_bookmark_t s_edit_orig;

/* what the confirm dialog will do */
enum { CONFIRM_NONE, CONFIRM_DISCONNECT, CONFIRM_DELETE_HOST };
static struct {
    int kind;
    int arg;                       /* session id / saved-host index */
    ssh_bookmark_t bm;             /* the saved host, to check it is still the same one */
} s_confirm;

#define HINT_CONNECT "Tab / Up / Down  next field     Left / Right  sign-in method     Space  tick     " \
                     "Enter  connect     Esc  cancel"
#define HINT_EDIT    "Tab / Up / Down  next field     Left / Right  sign-in method     Enter  save     Esc  cancel"

/* pending connect waiting for a password */
static struct {
    bool active;
    int reuse_id;                  /* reconnect into this terminal (0 = new) */
    ssh_bookmark_t bm;
} s_pending;

static EXT_RAM_BSS_ATTR ssh_bookmark_t s_hosts[SSH_MAX_BOOKMARKS];
static int s_host_count = 0;

static void refresh_sidebar(bool force);
static void refresh_header(void);
static void close_dialogs(void);
static bool dialog_open(void);
static void set_panel_focus(bool on);
static void panel_apply_sel(void);
static void show_dialog(lv_obj_t *dlg, devos_focus_t *f, lv_obj_t *first);

/* ======================================================================== */
/* Styles                                                                   */
/* ======================================================================== */
static void restyle(const devos_palette_t *p)
{
    lv_style_set_bg_color(&st_bg, p->bg);
    lv_style_set_bg_color(&st_sidebar, p->bg_alt);
    lv_style_set_border_color(&st_sidebar, p->surface_border);
    lv_style_set_bg_color(&st_header, p->surface);
    lv_style_set_border_color(&st_header, p->surface_border);
    lv_style_set_text_color(&st_title, p->accent_primary);
    lv_style_set_text_color(&st_text, p->text_primary);
    lv_style_set_text_color(&st_muted, p->text_secondary);
    lv_style_set_text_color(&st_small, p->text_muted);
    lv_style_set_bg_color(&st_btn, p->surface_active);
    lv_style_set_border_color(&st_btn, p->surface_border);
    lv_style_set_text_color(&st_btn, p->text_primary);
    lv_style_set_bg_color(&st_btn_primary, p->accent_primary);
    lv_style_set_text_color(&st_btn_primary, p->bg);
    lv_style_set_text_color(&st_btn_danger, p->accent_danger);
    lv_style_set_bg_color(&st_row, p->surface_active);
    lv_style_set_text_color(&st_row, p->text_primary);
    lv_style_set_bg_color(&st_row_active, p->surface_active);
    lv_style_set_border_color(&st_row_active, p->accent_primary);
    lv_style_set_bg_color(&st_ta, p->bg_alt);
    lv_style_set_border_color(&st_ta, p->surface_border);
    lv_style_set_text_color(&st_ta, p->text_primary);
    lv_style_set_border_color(&st_ta_focus, p->accent_primary);
    lv_style_set_bg_color(&st_modal, p->surface);
    lv_style_set_border_color(&st_modal, p->accent_primary);
    lv_style_set_bg_color(&st_kb, p->bg_alt);
    lv_style_set_bg_color(&st_kb_btn, p->surface_active);
    lv_style_set_text_color(&st_kb_btn, p->text_primary);
    lv_style_set_text_color(&st_err, p->accent_danger);
    lv_style_set_text_color(&st_ok, p->accent_secondary);
    lv_style_set_text_color(&st_warn, p->accent_warning);
    lv_style_set_text_color(&st_mono, p->text_primary);
    lv_style_set_outline_color(&st_ring, p->accent_primary);
}

static void styles_init(void)
{
    if (s_styles_ready) return;
    lv_style_t *all[] = { &st_bg, &st_sidebar, &st_header, &st_title, &st_text, &st_muted, &st_small, &st_btn,
                          &st_btn_primary, &st_btn_danger, &st_row, &st_row_active, &st_ta, &st_ta_focus,
                          &st_overlay, &st_modal, &st_kb, &st_kb_btn, &st_err, &st_ok, &st_warn, &st_mono,
                          &st_ring };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) lv_style_init(all[i]);
    lv_style_set_bg_opa(&st_bg, LV_OPA_COVER);
    lv_style_set_radius(&st_bg, 0);
    lv_style_set_border_width(&st_bg, 0);
    lv_style_set_pad_all(&st_bg, 0);
    lv_style_set_bg_opa(&st_sidebar, LV_OPA_COVER);
    lv_style_set_radius(&st_sidebar, 0);
    lv_style_set_border_width(&st_sidebar, 1);
    lv_style_set_border_side(&st_sidebar, LV_BORDER_SIDE_RIGHT);
    lv_style_set_pad_all(&st_sidebar, 10);
    lv_style_set_pad_row(&st_sidebar, 6);
    lv_style_set_bg_opa(&st_header, LV_OPA_COVER);
    lv_style_set_radius(&st_header, 0);
    lv_style_set_border_width(&st_header, 1);
    lv_style_set_border_side(&st_header, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_pad_hor(&st_header, 8);
    lv_style_set_pad_ver(&st_header, 0);
    lv_style_set_text_font(&st_title, &lv_font_montserrat_12);
    lv_style_set_text_letter_space(&st_title, 1);
    lv_style_set_text_font(&st_text, &lv_font_montserrat_14);
    lv_style_set_text_font(&st_muted, &lv_font_montserrat_12);
    lv_style_set_text_font(&st_small, &lv_font_montserrat_12);
    lv_style_set_bg_opa(&st_btn, LV_OPA_COVER);
    lv_style_set_border_width(&st_btn, 1);
    lv_style_set_radius(&st_btn, 6);
    lv_style_set_shadow_width(&st_btn, 0);
    lv_style_set_pad_hor(&st_btn, 12);
    lv_style_set_pad_ver(&st_btn, 7);
    lv_style_set_text_font(&st_btn, &lv_font_montserrat_14);
    lv_style_set_border_width(&st_btn_primary, 0);
    lv_style_set_bg_opa(&st_row, LV_OPA_TRANSP);
    lv_style_set_radius(&st_row, 6);
    lv_style_set_border_width(&st_row, 0);
    lv_style_set_shadow_width(&st_row, 0);
    lv_style_set_pad_all(&st_row, 6);
    lv_style_set_bg_opa(&st_row_active, LV_OPA_COVER);
    lv_style_set_border_width(&st_row_active, 2);
    lv_style_set_border_side(&st_row_active, LV_BORDER_SIDE_LEFT);
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
    lv_style_set_text_font(&st_err, &lv_font_montserrat_12);
    lv_style_set_text_font(&st_ok, &lv_font_montserrat_12);
    lv_style_set_text_font(&st_warn, &lv_font_montserrat_12);
    lv_style_set_text_font(&st_mono, &lv_font_nimbus_mono_14);
    /* panel selection ring (same look as devos_focus); fits the 4 px row gap */
    lv_style_set_outline_width(&st_ring, 2);
    lv_style_set_outline_pad(&st_ring, 2);
    lv_style_set_outline_opa(&st_ring, LV_OPA_COVER);
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

/* ======================================================================== */
/* Sessions & emulators                                                     */
/* ======================================================================== */
static term_sess_t *ts_for(int id)
{
    return (id >= 1 && id <= SSH_MAX_SESSIONS) ? &s_ts[id - 1] : NULL;
}

static void vt_reply_cb(const char *data, size_t len, void *user)
{
    ssh_port_send((int)(intptr_t)user, data, len);
}

static void vt_puts(term_sess_t *t, const char *s)
{
    if (t && t->vt) devos_vterm_feed(t->vt, s, strlen(s));
}

static void term_invalidate_all(void)
{
    if (term_view) lv_obj_invalidate(term_view);
}

static void compute_grid(void)
{
    int w = DEVOS_SCREEN_WIDTH - (s_sidebar_visible ? SIDEBAR_W : 0);
    int h = DEVOS_CONTENT_HEIGHT - HEADER_H;
    s_cols = (w - 2 * TERM_PAD) / CELL_W;
    s_rows = (h - 2 * TERM_PAD) / CELL_H;
    if (s_cols > DEVOS_VT_MAX_COLS) s_cols = DEVOS_VT_MAX_COLS;
}

static void apply_grid_to_sessions(void)
{
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (!s_ts[i].vt) continue;
        devos_vterm_resize(s_ts[i].vt, s_cols, s_rows);
        ssh_session_t *s = ssh_port_get_session(i + 1);
        if (s && s->state == SSH_SESSION_CONNECTED) ssh_port_resize_pty(i + 1, (uint16_t)s_cols, (uint16_t)s_rows);
    }
}

static void layout(void)
{
    compute_grid();
    int x = s_sidebar_visible ? SIDEBAR_W : 0;
    if (s_sidebar_visible) lv_obj_remove_flag(sidebar, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(sidebar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(term_area, x, 0);
    lv_obj_set_size(term_area, DEVOS_SCREEN_WIDTH - x, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_width(lbl_header_title, DEVOS_SCREEN_WIDTH - x - 44 - 300);
    apply_grid_to_sessions();
    term_invalidate_all();
    refresh_header();
}

/* Show / hide the panel; the PTYs follow the new grid (TIOCSWINSZ). */
static void set_sidebar_visible(bool on)
{
    if (s_sidebar_visible == on) return;
    s_sidebar_visible = on;
    if (!on) set_panel_focus(false);
    layout();
}

void app_terminal_toggle_sidebar(void)
{
    set_sidebar_visible(!s_sidebar_visible);
}

void app_terminal_resize_pty(uint16_t cols, uint16_t rows)
{
    if (s_active) ssh_port_resize_pty(s_active, cols, rows);
}

static void switch_session(int id)
{
    term_sess_t *t = ts_for(id);
    if (!t || !t->vt) return;
    s_active = id;
    t->view_offset = 0;
    term_invalidate_all();
    refresh_sidebar(true);
    refresh_header();
}

/* Start (or restart into `reuse_id`) a session from connection parameters. */
static int start_session(const ssh_bookmark_t *bm, const char *password, int reuse_id)
{
    term_sess_t keep = { 0 };
    if (reuse_id) {
        term_sess_t *old = ts_for(reuse_id);
        if (old) keep = *old;
        memset(old, 0, sizeof(*old));
        ssh_port_close_session(reuse_id);        /* frees the finished slot */
    }
    const char *cred = bm->auth_type == SSH_AUTH_KEY ? bm->key_path : password;
    int id = ssh_port_create_session(bm->alias, bm->host, bm->port, bm->user, bm->auth_type, cred,
                                     (uint16_t)s_cols, (uint16_t)s_rows);
    if (id <= 0) {
        if (keep.vt) devos_vterm_destroy(keep.vt);
        return -1;
    }
    term_sess_t *t = ts_for(id);
    if (t->vt && t->vt != keep.vt) devos_vterm_destroy(t->vt);
    memset(t, 0, sizeof(*t));
    t->vt = keep.vt ? keep.vt : devos_vterm_create(s_cols, s_rows, SCROLLBACK);
    if (!t->vt) {
        ssh_port_close_session(id);
        return -1;
    }
    devos_vterm_resize(t->vt, s_cols, s_rows);
    devos_vterm_set_output_cb(t->vt, vt_reply_cb, (void *)(intptr_t)id);
    t->last_state = SSH_SESSION_DISCONNECTED;
    snprintf(t->alias, sizeof(t->alias), "%.*s", (int)sizeof(t->alias) - 1, bm->alias[0] ? bm->alias : bm->host);
    snprintf(t->host, sizeof(t->host), "%s", bm->host);
    t->port = bm->port > 0 ? bm->port : 22;
    snprintf(t->user, sizeof(t->user), "%s", bm->user);
    t->auth = bm->auth_type;
    snprintf(t->keypath, sizeof(t->keypath), "%s", bm->key_path);
    char line[256];
    snprintf(line, sizeof(line), "\r\n\033[0;36mConnecting to %.60s@%.100s:%d ...\033[0m\r\n", t->user, t->host, t->port);
    vt_puts(t, line);
    switch_session(id);
    set_panel_focus(false);                     /* type into the new session */
    return id;
}

/* Poll all sessions: feed output, track state, surface prompts. */
static void on_state_change(int id, term_sess_t *t, ssh_session_t *s)
{
    char line[300];
    switch (s->state) {
    case SSH_SESSION_ERROR:
        snprintf(line, sizeof(line), "\r\n\033[1;31m%s\033[0m\r\n\033[0;33mPress Enter to retry.\033[0m\r\n",
                 s->last_error[0] ? s->last_error : "Connection failed");
        vt_puts(t, line);
        break;
    case SSH_SESSION_CLOSED:
        vt_puts(t, "\033[0;33mPress Enter to reconnect.\033[0m\r\n");
        break;
    case SSH_SESSION_HOSTKEY_PROMPT:
        if (!dlg_hostkey || lv_obj_has_flag(dlg_hostkey, LV_OBJ_FLAG_HIDDEN)) {
            close_dialogs();
            s_hostkey_for = id;
            snprintf(line, sizeof(line),
                     "The authenticity of %.60s (port %d) can't be established.\n\n"
                     "%s key fingerprint:\n%s\n\n"
                     "Trust this host and remember its key in /sdcard/.ssh/known_hosts?",
                     s->host, s->port, s->hostkey_type, s->hostkey_fp);
            lv_label_set_text(lbl_hk_body, line);
            show_dialog(dlg_hostkey, &s_f_hk, btn_hk_trust);   /* Enter = Trust, as before */
            if (id != s_active) switch_session(id);
        }
        break;
    default:
        break;
    }
}

static void poll_cb(lv_timer_t *tm)
{
    LV_UNUSED(tm);
    static char buf[4096];
    bool visible = screen && !lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN);
    bool states_changed = false;

    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        term_sess_t *t = &s_ts[i];
        if (!t->vt) continue;
        int id = i + 1;
        ssh_session_t *s = ssh_port_get_session(id);
        /* Drain this session (bounded per tick so the UI stays responsive). */
        for (int n = 0; n < 16; n++) {
            int got = ssh_port_recv(id, buf, sizeof(buf));
            if (got <= 0) break;
            devos_vterm_feed(t->vt, buf, (size_t)got);
        }
        /* Keep a scrolled-back view anchored as new lines arrive. */
        uint32_t total = devos_vterm_scrolled_total(t->vt);
        if (t->view_offset > 0 && total != t->sb_seen) {
            t->view_offset += (int)(total - t->sb_seen);
            int max = devos_vterm_scrollback_lines(t->vt);
            if (t->view_offset > max) t->view_offset = max;
        }
        t->sb_seen = total;

        if (s && s->state != t->last_state) {
            t->last_state = s->state;
            states_changed = true;
            on_state_change(id, t, s);
        }
    }

    /* Redraw only the rows that changed in the active session. */
    term_sess_t *a = ts_for(s_active);
    if (visible && a && a->vt && term_view) {
        static uint8_t dirty[64];
        if (devos_vterm_take_dirty(a->vt, dirty, (int)sizeof(dirty))) {
            if (a->view_offset > 0) {
                term_invalidate_all();
            } else {
                lv_area_t c;
                lv_obj_get_coords(term_view, &c);
                int rows = devos_vterm_rows(a->vt);
                for (int r = 0; r < rows && r < (int)sizeof(dirty); r++) {
                    if (!dirty[r]) continue;
                    lv_area_t ar = { c.x1, c.y1 + TERM_PAD + r * CELL_H, c.x2, c.y1 + TERM_PAD + (r + 1) * CELL_H - 1 };
                    lv_obj_invalidate_area(term_view, &ar);
                }
            }
        }
    }
    if (states_changed || visible) {
        refresh_sidebar(states_changed);
        if (visible) refresh_header();
    }

    /* Launcher tile telemetry */
    int live = 0;
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        ssh_session_t *s = ssh_port_get_session(i + 1);
        if (s && s->state == SSH_SESSION_CONNECTED) live++;
    }
    const devos_telemetry_t *cur = devos_telemetry_get();
    char host[32];
    ssh_session_t *as = ssh_port_get_session(s_active);
    if (as && s_ts[s_active - 1].vt) snprintf(host, sizeof(host), "%.12s@%.18s", as->user, as->host);
    else snprintf(host, sizeof(host), "No session");
    if (cur->terminal_sessions != live || strcmp(cur->terminal_host, host) != 0) {
        devos_telemetry_t t2;
        memcpy(&t2, cur, sizeof(t2));
        t2.terminal_sessions = (uint8_t)live;
        snprintf(t2.terminal_host, sizeof(t2.terminal_host), "%s", host);
        devos_telemetry_update(&t2);
    }
}

/* ======================================================================== */
/* Terminal grid renderer                                                   */
/* ======================================================================== */
static lv_color_t xterm_color(uint8_t idx, const devos_palette_t *p)
{
    if (idx < 16) return p->ansi[idx];
    if (idx < 232) {
        static const uint8_t lv[6] = { 0, 95, 135, 175, 215, 255 };
        int i = idx - 16;
        return lv_color_make(lv[i / 36], lv[(i / 6) % 6], lv[i % 6]);
    }
    uint8_t v = (uint8_t)(8 + (idx - 232) * 10);
    return lv_color_make(v, v, v);
}

/* Nimbus Mono 14 covers ASCII + Latin-1; map common TUI glyphs to ASCII. */
static uint32_t glyph_for(uint16_t ch)
{
    if (ch == 0) return ' ';
    if ((ch >= 0x20 && ch < 0x7F) || (ch >= 0xA0 && ch <= 0xFF)) return ch;
    if (ch >= 0x2500 && ch <= 0x257F) {
        switch (ch) {
        case 0x2500: case 0x2501: case 0x2504: case 0x2505: case 0x2508: case 0x2509:
        case 0x254C: case 0x254D: case 0x2574: case 0x2576: case 0x2578: case 0x257A:
            return '-';
        case 0x2550: return '=';
        case 0x2502: case 0x2503: case 0x2506: case 0x2507: case 0x250A: case 0x250B:
        case 0x254E: case 0x254F: case 0x2551: case 0x2575: case 0x2577: case 0x2579: case 0x257B:
            return '|';
        case 0x2571: return '/';
        case 0x2572: return '\\';
        case 0x2573: return 'X';
        default: return '+';
        }
    }
    if (ch >= 0x2580 && ch <= 0x259F) return '#';
    if (ch >= 0x23BA && ch <= 0x23BD) return '-';
    switch (ch) {
    case 0x2190: case 0x25C0: case 0x25C4: case 0x2039: return '<';
    case 0x2192: case 0x25B6: case 0x25BA: case 0x203A: return '>';
    case 0x2191: case 0x25B2: return '^';
    case 0x2193: case 0x25BC: return 'v';
    case 0x2022: case 0x25CF: case 0x25C6: case 0x2605: return '*';
    case 0x25CB: case 0x25E6: return 'o';
    case 0x2026: return '.';
    case 0x2018: case 0x2019: case 0x201B: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
    case 0x2713: case 0x2714: return 'v';
    case 0x2717: case 0x2718: return 'x';
    case 0x2264: return '<';
    case 0x2265: return '>';
    case 0x2260: return '#';
    case 0x03C0: return 'p';
    case 0x2409: case 0x240A: case 0x240B: case 0x240C: case 0x240D: case 0x2424: return ' ';
    default: return '?';
    }
}

static int utf8_put(char *dst, uint32_t cp)
{
    if (cp < 0x80) { dst[0] = (char)cp; return 1; }
    dst[0] = (char)(0xC0 | (cp >> 6));
    dst[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
}

static void resolve_colors(const devos_vt_cell_t *c, const devos_palette_t *p, lv_color_t dfg, lv_color_t dbg,
                           lv_color_t *fg, lv_color_t *bg, bool *bg_default)
{
    uint8_t fi = c->fg;
    if ((c->attr & VT_ATTR_BOLD) && fi < 8) fi += 8;                 /* bold = bright */
    lv_color_t f = (c->attr & VT_ATTR_FG_DEFAULT) ? dfg : xterm_color(fi, p);
    bool bdef = (c->attr & VT_ATTR_BG_DEFAULT) != 0;
    lv_color_t b = bdef ? dbg : xterm_color(c->bg, p);
    if (c->attr & VT_ATTR_REVERSE) {
        lv_color_t tmp = f; f = b; b = tmp;
        bdef = false;
    }
    if (c->attr & VT_ATTR_DIM) f = lv_color_mix(f, b, 150);
    *fg = f;
    *bg = b;
    *bg_default = bdef;
}

static void term_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_current_target(e);
    term_sess_t *t = ts_for(s_active);
    if (!t || !t->vt) return;
    const devos_palette_t *p = devos_theme_get();
    lv_color_t dfg = p->text_primary, dbg = p->code_bg;

    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int cols = devos_vterm_cols(t->vt), rows = devos_vterm_rows(t->vt);
    int x0 = a.x1 + TERM_PAD, y0 = a.y1 + TERM_PAD;
    const lv_area_t *clip = &layer->_clip_area;
    int r0 = (clip->y1 - y0) / CELL_H, r1 = (clip->y2 - y0) / CELL_H;
    if (r0 < 0) r0 = 0;
    if (r1 > rows - 1) r1 = rows - 1;

    int ccol = 0, crow = 0;
    bool cvis = false;
    devos_vterm_cursor(t->vt, &ccol, &crow, &cvis);
    ssh_session_t *s = ssh_port_get_session(s_active);
    bool show_cursor = cvis && t->view_offset == 0 && s && s->state == SSH_SESSION_CONNECTED;
    bool hollow = s_panel_focus || dialog_open();          /* keys don't go to the shell */

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_opa = LV_OPA_COVER;
    lv_draw_rect_dsc_t hd;                                  /* hollow cursor */
    lv_draw_rect_dsc_init(&hd);
    hd.bg_opa = LV_OPA_TRANSP;
    hd.border_width = 1;
    hd.border_opa = LV_OPA_COVER;
    hd.border_color = p->accent_primary;
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &lv_font_nimbus_mono_14;
    ld.flag = LV_TEXT_FLAG_EXPAND;
    ld.text_local = 1;
    char txt[DEVOS_VT_MAX_COLS * 2 + 1];

    for (int r = r0; r <= r1; r++) {
        const devos_vt_cell_t *row = devos_vterm_view_row(t->vt, r, t->view_offset);
        if (!row) continue;
        int y = y0 + r * CELL_H;
        int c = 0;
        while (c < cols) {
            lv_color_t fg, bg;
            bool bdef;
            resolve_colors(&row[c], p, dfg, dbg, &fg, &bg, &bdef);
            uint8_t ul = row[c].attr & VT_ATTR_UNDERLINE;
            int start = c, tl = 0;
            bool blank = true;
            while (c < cols) {
                lv_color_t f2, b2;
                bool bd2;
                resolve_colors(&row[c], p, dfg, dbg, &f2, &b2, &bd2);
                if (c > start && (!lv_color_eq(f2, fg) || !lv_color_eq(b2, bg) || bd2 != bdef ||
                                  (row[c].attr & VT_ATTR_UNDERLINE) != ul)) break;
                uint32_t g = glyph_for(row[c].ch);
                if (g != ' ') blank = false;
                tl += utf8_put(txt + tl, g);
                c++;
            }
            txt[tl] = '\0';
            if (!bdef) {
                rd.bg_color = bg;
                lv_area_t br = { x0 + start * CELL_W, y, x0 + c * CELL_W - 1, y + CELL_H - 1 };
                lv_draw_rect(layer, &rd, &br);
            }
            if (!blank || ul) {
                ld.color = fg;
                ld.decor = ul ? LV_TEXT_DECOR_UNDERLINE : LV_TEXT_DECOR_NONE;
                ld.text = txt;
                lv_area_t tr = { x0 + start * CELL_W, y, x0 + c * CELL_W + CELL_W, y + CELL_H - 1 };
                lv_draw_label(layer, &ld, &tr);
            }
        }
        if (show_cursor && r == crow && ccol < cols && hollow) {
            lv_area_t cr = { x0 + ccol * CELL_W, y, x0 + (ccol + 1) * CELL_W - 1, y + CELL_H - 1 };
            lv_draw_rect(layer, &hd, &cr);
        } else if (show_cursor && r == crow && ccol < cols) {
            rd.bg_color = p->accent_primary;
            lv_area_t cr = { x0 + ccol * CELL_W, y, x0 + (ccol + 1) * CELL_W - 1, y + CELL_H - 1 };
            lv_draw_rect(layer, &rd, &cr);
            char cc[3];
            int n = utf8_put(cc, glyph_for(row[ccol].ch));
            cc[n] = '\0';
            if (cc[0] != ' ') {
                ld.color = dbg;
                ld.decor = LV_TEXT_DECOR_NONE;
                ld.text = cc;
                lv_area_t ct = { cr.x1, y, cr.x1 + 2 * CELL_W, y + CELL_H - 1 };
                lv_draw_label(layer, &ld, &ct);
            }
        }
    }
}

/* Touch: drag vertically to scroll back through history. */
static void term_touch_cb(lv_event_t *e)
{
    static int32_t last_y = 0;
    static int acc = 0;
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    if (code == LV_EVENT_PRESSED) {
        last_y = pt.y;
        acc = 0;
        if (s_panel_focus) set_panel_focus(false);          /* tapping the shell focuses it */
        return;
    }
    if (code != LV_EVENT_PRESSING) return;
    term_sess_t *t = ts_for(s_active);
    if (!t || !t->vt) return;
    acc += pt.y - last_y;
    last_y = pt.y;
    int lines = acc / CELL_H;
    if (!lines) return;
    acc -= lines * CELL_H;
    int max = devos_vterm_scrollback_lines(t->vt);
    int v = t->view_offset + lines;                          /* drag down = older */
    if (v < 0) v = 0;
    if (v > max) v = max;
    if (v != t->view_offset) {
        t->view_offset = v;
        term_invalidate_all();
        refresh_header();
    }
}

/* ======================================================================== */
/* Sidebar / header                                                         */
/* ======================================================================== */
static const char *state_text(ssh_session_state_t st)
{
    switch (st) {
    case SSH_SESSION_CONNECTING:     return "Connecting...";
    case SSH_SESSION_AUTHENTICATING: return "Authenticating...";
    case SSH_SESSION_HOSTKEY_PROMPT: return "Verify host key";
    case SSH_SESSION_CONNECTED:      return "Connected";
    case SSH_SESSION_CLOSED:         return "Closed";
    case SSH_SESSION_ERROR:          return "Failed";
    default:                         return "";
    }
}

static lv_style_t *state_style(ssh_session_state_t st)
{
    switch (st) {
    case SSH_SESSION_CONNECTED: return &st_ok;
    case SSH_SESSION_ERROR:     return &st_err;
    case SSH_SESSION_CLOSED:    return &st_small;
    default:                    return &st_warn;
    }
}

static bool state_live(ssh_session_state_t st)
{
    return st == SSH_SESSION_CONNECTED || st == SSH_SESSION_CONNECTING || st == SSH_SESSION_AUTHENTICATING ||
           st == SSH_SESSION_HOSTKEY_PROMPT;
}

static void session_row_cb(lv_event_t *e)
{
    switch_session((int)(intptr_t)lv_event_get_user_data(e));
    set_panel_focus(false);
}

/* Disconnect a live session; remove a finished one (terminal + scrollback). */
static void session_close(int id)
{
    ssh_session_t *s = ssh_port_get_session(id);
    term_sess_t *t = ts_for(id);
    if (!s || !t) return;
    if (state_live(s->state)) {
        ssh_port_close_session(id);                         /* disconnect; row stays until removed */
        return;
    }
    ssh_port_close_session(id);                             /* free the slot */
    if (t->vt) devos_vterm_destroy(t->vt);
    memset(t, 0, sizeof(*t));
    if (s_active == id) {
        s_active = 0;
        for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
            if (s_ts[i].vt) { s_active = i + 1; break; }
        }
    }
    term_invalidate_all();
    refresh_sidebar(true);
    refresh_header();
}

static void session_close_cb(lv_event_t *e)
{
    session_close((int)(intptr_t)lv_event_get_user_data(e));
}

static void open_connect_dialog(const char *host);
static void open_edit_dialog(int idx);
static void open_confirm(int kind, int arg, const ssh_bookmark_t *bm, const char *body, const char *ok);
static void new_btn_cb(lv_event_t *e) { LV_UNUSED(e); open_connect_dialog(NULL); }

static void open_password_dialog(const ssh_bookmark_t *bm, int reuse_id);

static void connect_bookmark(const ssh_bookmark_t *bm, int reuse_id)
{
    if (bm->auth_type == SSH_AUTH_PASSWORD) {
        open_password_dialog(bm, reuse_id);
    } else {
        start_session(bm, NULL, reuse_id);
    }
}

static void host_row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < s_host_count) connect_bookmark(&s_hosts[i], 0);
}

static void host_delete(int i)
{
    ssh_port_delete_bookmark(i);
    s_hosts_key[0] = '\0';
    refresh_sidebar(true);
}

static void host_delete_cb(lv_event_t *e)
{
    host_delete((int)(intptr_t)lv_event_get_user_data(e));
}

static void open_devkey_dialog(void);
static void devkey_btn_cb(lv_event_t *e) { LV_UNUSED(e); open_devkey_dialog(); }

/* Section title (+ optional "+ New" button) and its list. Both are padded
 * by 4 px so the panel selection ring isn't clipped. */
static lv_obj_t *mk_section(lv_obj_t *parent, const char *title, lv_event_cb_t add_cb, lv_obj_t **add_btn)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 36);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = mk_label(row, &st_title, title);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    if (add_cb) {
        lv_obj_t *b = mk_btn(row, LV_SYMBOL_PLUS " New", NULL, add_cb, NULL);
        lv_obj_set_style_pad_ver(b, 4, 0);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_style(b, &st_ring, LV_STATE_FOCUS_KEY);
        if (add_btn) *add_btn = b;
    }
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    return list;
}

static void refresh_sidebar(bool force)
{
    if (!list_sessions) return;
    bool rebuilt = false;
    /* Sessions: rebuild when ids / states / errors change */
    char key[256];
    int kl = 0;
    key[0] = '\0';
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (!s_ts[i].vt) continue;
        ssh_session_t *s = ssh_port_get_session(i + 1);
        kl += snprintf(key + kl, sizeof(key) - (size_t)kl, "%d:%d:%d,", i, (int)s->state, s_active == i + 1);
        if (kl >= (int)sizeof(key) - 16) break;
    }
    if (force || strcmp(key, s_list_key) != 0) {
        snprintf(s_list_key, sizeof(s_list_key), "%s", key);
        memset(s_sess_row, 0, sizeof(s_sess_row));
        lv_obj_clean(list_sessions);
        rebuilt = true;
        bool any = false;
        for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
            term_sess_t *t = &s_ts[i];
            if (!t->vt) continue;
            any = true;
            ssh_session_t *s = ssh_port_get_session(i + 1);
            lv_obj_t *row = lv_button_create(list_sessions);
            s_sess_row[i] = row;
            lv_obj_add_style(row, &st_row, 0);
            if (s_active == i + 1) lv_obj_add_style(row, &st_row_active, 0);
            lv_obj_add_style(row, &st_ring, LV_STATE_FOCUS_KEY);
            lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_add_event_cb(row, session_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));
            char buf[160];
            snprintf(buf, sizeof(buf), "%d  %.28s", i + 1, t->alias);
            lv_obj_t *l1 = mk_label(row, &st_text, buf);
            lv_label_set_long_mode(l1, LV_LABEL_LONG_DOT);
            lv_obj_set_width(l1, SIDEBAR_W - 70);
            snprintf(buf, sizeof(buf), "%.16s@%.24s", t->user, t->host);
            lv_obj_t *l2 = mk_label(row, &st_muted, buf);
            lv_obj_set_pos(l2, 0, 20);
            lv_label_set_long_mode(l2, LV_LABEL_LONG_DOT);
            lv_obj_set_width(l2, SIDEBAR_W - 70);
            lv_obj_t *l3 = mk_label(row, state_style(s->state), state_text(s->state));
            lv_obj_set_pos(l3, 0, 38);
            bool live = state_live(s->state);
            lv_obj_t *x = mk_btn(row, live ? LV_SYMBOL_POWER : LV_SYMBOL_CLOSE, &st_btn_danger, session_close_cb,
                                 (void *)(intptr_t)(i + 1));
            lv_obj_set_style_pad_all(x, 6, 0);
            lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
        }
        if (!any) mk_label(list_sessions, &st_small, "No sessions yet.");
    }

    /* Saved hosts: read the SD file only when asked (show, save, delete) -
     * this runs on every 20 ms poll while the terminal is visible */
    static EXT_RAM_BSS_ATTR ssh_bookmark_t tmp[SSH_MAX_BOOKMARKS];
    int n = 0;
    if (force) ssh_port_load_bookmarks(tmp, SSH_MAX_BOOKMARKS, &n);
    char hk[64];
    snprintf(hk, sizeof(hk), "%d:%.48s", n, n ? tmp[n - 1].host : "");
    if (force) {
        snprintf(s_hosts_key, sizeof(s_hosts_key), "%s", hk);
        memcpy(s_hosts, tmp, sizeof(s_hosts));
        s_host_count = n;
        memset(s_host_row, 0, sizeof(s_host_row));
        lv_obj_clean(list_hosts);
        rebuilt = true;
        if (n == 0) mk_label(list_hosts, &st_small, "None yet: tick \"Save\" when connecting.");
        for (int i = 0; i < n; i++) {
            lv_obj_t *row = lv_button_create(list_hosts);
            s_host_row[i] = row;
            lv_obj_add_style(row, &st_row, 0);
            lv_obj_add_style(row, &st_ring, LV_STATE_FOCUS_KEY);
            lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_add_event_cb(row, host_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            char buf[160];
            lv_obj_t *l1 = mk_label(row, &st_text, s_hosts[i].alias[0] ? s_hosts[i].alias : s_hosts[i].host);
            lv_label_set_long_mode(l1, LV_LABEL_LONG_DOT);
            lv_obj_set_width(l1, SIDEBAR_W - 70);
            snprintf(buf, sizeof(buf), "%.16s@%.24s  (%s)", s_hosts[i].user, s_hosts[i].host,
                     s_hosts[i].auth_type == SSH_AUTH_DEVICE_KEY ? "device key"
                     : s_hosts[i].auth_type == SSH_AUTH_KEY ? "key file" : "password");
            lv_obj_t *l2 = mk_label(row, &st_muted, buf);
            lv_obj_set_pos(l2, 0, 20);
            lv_label_set_long_mode(l2, LV_LABEL_LONG_DOT);
            lv_obj_set_width(l2, SIDEBAR_W - 70);
            lv_obj_t *x = mk_btn(row, LV_SYMBOL_TRASH, &st_btn_danger, host_delete_cb, (void *)(intptr_t)i);
            lv_obj_set_style_pad_all(x, 6, 0);
            lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }
    if (rebuilt) panel_apply_sel();                         /* rows were recreated */
}

/* ---- side panel keyboard focus: a custom list (AGENTS.md invariant 9) ---- */
typedef struct {
    int kind;
    int idx;                       /* session id / saved-host index */
    lv_obj_t *obj;
} pitem_t;
#define PANEL_MAX (2 + SSH_MAX_SESSIONS + SSH_MAX_BOOKMARKS)

/* The panel's selectable items in reading order. */
static int panel_items(pitem_t *it)
{
    int n = 0;
    if (btn_new) it[n++] = (pitem_t){ PI_NEW, 0, btn_new };
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (s_sess_row[i]) it[n++] = (pitem_t){ PI_SESSION, i + 1, s_sess_row[i] };
    }
    for (int i = 0; i < s_host_count && i < SSH_MAX_BOOKMARKS; i++) {
        if (s_host_row[i]) it[n++] = (pitem_t){ PI_HOST, i, s_host_row[i] };
    }
    if (btn_devkey) it[n++] = (pitem_t){ PI_DEVKEY, 0, btn_devkey };
    return n;
}

static void panel_select(const pitem_t *it, int pos)
{
    s_psel_kind = it[pos].kind;
    s_psel_idx = it[pos].idx;
    s_psel_pos = pos;
}

/* Index of the selection in it[] (n > 0). First visit: the active session,
 * else the first saved host, else "+ New". If the item is gone (session
 * removed, host deleted), whatever now sits at its position. */
static int panel_cur(const pitem_t *it, int n)
{
    if (s_psel_kind == PI_NONE) {
        int pick = -1;
        for (int i = 0; i < n && pick < 0; i++) {
            if (it[i].kind == PI_SESSION && it[i].idx == s_active) pick = i;
        }
        for (int i = 0; i < n && pick < 0; i++) {
            if (it[i].kind == PI_HOST) pick = i;
        }
        panel_select(it, pick < 0 ? 0 : pick);
        return s_psel_pos;
    }
    for (int i = 0; i < n; i++) {
        if (it[i].kind == s_psel_kind && it[i].idx == s_psel_idx) {
            s_psel_pos = i;
            return i;
        }
    }
    panel_select(it, s_psel_pos < 0 ? 0 : s_psel_pos < n ? s_psel_pos : n - 1);
    return s_psel_pos;
}

static pitem_t panel_sel_item(void)
{
    pitem_t it[PANEL_MAX];
    int n = panel_items(it);
    if (!n) return (pitem_t){ PI_NONE, 0, NULL };
    return it[panel_cur(it, n)];
}

/* Scroll the panel so o (and its ring) is visible. The rows sit in
 * non-scrolling lists, so lv_obj_scroll_to_view() would only bring the
 * whole list into view. */
static void panel_scroll_into_view(lv_obj_t *o)
{
    if (!side_scroll) return;
    lv_obj_update_layout(side_scroll);                      /* rows may be brand new */
    lv_area_t a, v;
    lv_obj_get_coords(o, &a);
    lv_obj_get_coords(side_scroll, &v);
    const int32_t m = 6;                                    /* ring + a little air */
    int32_t dy = 0;
    if (a.y1 - m < v.y1) dy = v.y1 - (a.y1 - m);            /* above: content moves down */
    else if (a.y2 + m > v.y2) dy = v.y2 - (a.y2 + m);       /* below: content moves up */
    if (dy) lv_obj_scroll_by_bounded(side_scroll, 0, dy, LV_ANIM_OFF);
}

/* Draw the selection ring (only while the panel has keyboard focus). */
static void panel_apply_sel(void)
{
    pitem_t it[PANEL_MAX];
    int n = panel_items(it);
    for (int i = 0; i < n; i++) lv_obj_remove_state(it[i].obj, LV_STATE_FOCUS_KEY);
    if (!s_panel_focus || !n) return;
    lv_obj_t *o = it[panel_cur(it, n)].obj;
    lv_obj_add_state(o, LV_STATE_FOCUS_KEY);
    panel_scroll_into_view(o);
}

/* Move keyboard focus between the shell and the panel (opened if hidden). */
static void set_panel_focus(bool on)
{
    if (on && !s_sidebar_visible) set_sidebar_visible(true);
    if (s_panel_focus != on) {
        s_panel_focus = on;
        term_invalidate_all();                              /* cursor: solid <-> hollow */
    }
    if (lbl_panel_hint) set_text(lbl_panel_hint, on ? HINT_PANEL : HINT_SHELL);
    panel_apply_sel();
    refresh_header();
}

static void panel_move(int dir)
{
    pitem_t it[PANEL_MAX];
    int n = panel_items(it);
    if (!n) return;
    panel_select(it, (panel_cur(it, n) + dir + n) % n);
    panel_apply_sel();
}

static int panel_section_of(int kind)
{
    return kind == PI_HOST ? 1 : kind == PI_DEVKEY ? 2 : 0;
}

/* Tab / Aa+Tab: first item of the next / previous section. */
static void panel_section(int dir)
{
    pitem_t it[PANEL_MAX];
    int n = panel_items(it);
    if (!n) return;
    int cur = panel_cur(it, n), starts[3] = { 0, 0, 0 }, m = 0, j = 0;
    for (int i = 0; i < n && m < 3; i++) {
        if (i == 0 || panel_section_of(it[i].kind) != panel_section_of(it[i - 1].kind)) starts[m++] = i;
    }
    for (int k = 0; k < m; k++) {
        if (starts[k] <= cur) j = k;
    }
    panel_select(it, starts[((j + dir) % m + m) % m]);
    panel_apply_sel();
}

/* Enter: switch to a session, connect a saved host, or press the button. */
static void panel_activate(void)
{
    pitem_t sel = panel_sel_item();
    switch (sel.kind) {
    case PI_NEW:
        open_connect_dialog(NULL);
        break;
    case PI_SESSION:
        switch_session(sel.idx);
        set_panel_focus(false);
        break;
    case PI_HOST:
        if (sel.idx < s_host_count) connect_bookmark(&s_hosts[sel.idx], 0);
        break;
    case PI_DEVKEY:
        open_devkey_dialog();
        break;
    default:
        break;
    }
}

/* D / Del: disconnect a live session or delete a saved host (both ask first);
 * a finished session is removed straight away, like its X button. */
static void panel_delete(void)
{
    pitem_t sel = panel_sel_item();
    char body[320];
    if (sel.kind == PI_SESSION) {
        ssh_session_t *s = ssh_port_get_session(sel.idx);
        term_sess_t *t = ts_for(sel.idx);
        if (!s || !t || !t->vt) return;
        if (!state_live(s->state)) {
            session_close(sel.idx);
            return;
        }
        snprintf(body, sizeof(body),
                 "Disconnect %.40s (%.30s@%.60s)?\n\nThe remote shell and anything running in it will end.",
                 t->alias, t->user, t->host);
        open_confirm(CONFIRM_DISCONNECT, sel.idx, NULL, body, LV_SYMBOL_POWER "  Disconnect");
    } else if (sel.kind == PI_HOST && sel.idx < s_host_count) {
        const ssh_bookmark_t *bm = &s_hosts[sel.idx];
        snprintf(body, sizeof(body), "Delete the saved host %.40s (%.30s@%.60s)?",
                 bm->alias[0] ? bm->alias : bm->host, bm->user, bm->host);
        open_confirm(CONFIRM_DELETE_HOST, sel.idx, bm, body, LV_SYMBOL_TRASH "  Delete");
    }
}

/* Keys while the panel has focus; false = not for the panel (scrollback). */
static bool panel_handle_key(uint32_t key, uint8_t mods)
{
    if (key == DEVOS_KEY_PGUP || key == DEVOS_KEY_PGDN) return false;
    if (key == LV_KEY_ESC) {
        set_panel_focus(false);
        return true;
    }
    if (mods & (DEVOS_MOD_CTRL | DEVOS_MOD_FN | DEVOS_MOD_ALT)) return true;
    switch (key) {
    case LV_KEY_UP:
        panel_move(-1);
        break;
    case LV_KEY_DOWN:
        panel_move(1);
        break;
    case '\t':
        panel_section((mods & DEVOS_MOD_SHIFT) ? -1 : 1);
        break;
    case '\r': case '\n': case ' ':
        panel_activate();
        break;
    case 'n': case 'N':
        open_connect_dialog(NULL);
        break;
    case 'e': case 'E': {
        pitem_t sel = panel_sel_item();
        if (sel.kind == PI_HOST) open_edit_dialog(sel.idx);
        break;
    }
    case 'd': case 'D': case LV_KEY_DEL:
        panel_delete();
        break;
    case 'k': case 'K':
        open_devkey_dialog();
        break;
    default:
        break;                                              /* panel focused: nothing reaches the shell */
    }
    return true;
}

static void refresh_header(void)
{
    if (!lbl_header_title) return;
    term_sess_t *t = ts_for(s_active);
    ssh_session_t *s = ssh_port_get_session(s_active);
    char buf[200];
    if (!t || !t->vt || !s) {
        set_text(lbl_header_title, "Terminal");
        lv_obj_remove_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        const char *title = devos_vterm_title(t->vt);
        snprintf(buf, sizeof(buf), "%.24s@%.40s   %s%s%.60s", t->user, t->host, state_text(s->state),
                 title[0] ? "   |   " : "", title);
        set_text(lbl_header_title, buf);
        lv_obj_add_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    }
    if (t && t->vt && t->view_offset > 0) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_UP " %d lines back (Sym+Down)  |  %dx%d", t->view_offset, s_cols,
                 s_rows);
    } else {
        snprintf(buf, sizeof(buf), "%s  |  %dx%d", s_panel_focus ? "Esc  back to shell" : "Sym+L  panel", s_cols,
                 s_rows);
    }
    set_text(lbl_header_info, buf);
}

/* ======================================================================== */
/* Dialogs                                                                  */
/* ======================================================================== */
static bool dialog_open(void)
{
    return overlay && !lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

/* The dialog on screen (at most one) and its focus list. */
static lv_obj_t *visible_dialog(devos_focus_t **f)
{
    struct {
        lv_obj_t *dlg;
        devos_focus_t *f;
    } all[] = {
        { dlg_hostkey, &s_f_hk }, { dlg_confirm, &s_f_confirm }, { dlg_password, &s_f_pw },
        { dlg_connect, &s_f_connect }, { dlg_devkey, &s_f_dk },
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (all[i].dlg && !lv_obj_has_flag(all[i].dlg, LV_OBJ_FLAG_HIDDEN)) {
            if (f) *f = all[i].f;
            return all[i].dlg;
        }
    }
    return NULL;
}

/* The on-screen keyboard follows the focused text field, and only appears
 * when no hardware keyboard is attached. */
static void sync_kb(devos_focus_t *f)
{
    if (!dialog_open()) return;
    lv_obj_t *o = devos_focus_get(f);
    bool ta = o && lv_obj_check_type(o, &lv_textarea_class);
    if (ta) lv_keyboard_set_textarea(kb, o);
    if (ta && !tab5_keyboard_is_connected()) lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void close_dialogs(void)
{
    if (!overlay) return;
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dlg_connect, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dlg_password, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dlg_hostkey, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dlg_devkey, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dlg_confirm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(ta_pass, "");
    lv_textarea_set_text(ta_pw, "");
    devos_focus_t *all[] = { &s_f_connect, &s_f_pw, &s_f_hk, &s_f_dk, &s_f_confirm };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) devos_focus_clear(all[i]);
    s_pending.active = false;
    s_edit_idx = -1;
    s_confirm.kind = CONFIRM_NONE;
}

/* Open a dialog with keyboard focus on `first` (NULL = its first control).
 * Closing returns focus to wherever it was (shell or panel). */
static void show_dialog(lv_obj_t *dlg, devos_focus_t *f, lv_obj_t *first)
{
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(dlg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay);
    devos_focus_clear(f);
    if (first) devos_focus_set(f, first);
    else devos_focus_first(f);
    sync_kb(f);
}

static void ta_click_cb(lv_event_t *e)
{
    lv_keyboard_set_textarea(kb, lv_event_get_target(e));       /* devos_focus moved the focus */
    if (!tab5_keyboard_is_connected()) lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *mk_ta(lv_obj_t *parent, const char *placeholder, int w, bool password)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_add_style(ta, &st_ta, 0);
    lv_obj_add_style(ta, &st_ta_focus, LV_STATE_FOCUSED);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_password_mode(ta, password);
    lv_obj_set_width(ta, w);
    lv_obj_add_event_cb(ta, ta_click_cb, LV_EVENT_CLICKED, NULL);
    return ta;
}

static lv_obj_t *mk_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t *mk_dialog(int w)
{
    lv_obj_t *d = lv_obj_create(overlay);
    lv_obj_remove_style_all(d);
    lv_obj_add_style(d, &st_modal, 0);
    lv_obj_set_size(d, w, LV_SIZE_CONTENT);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_flex_flow(d, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    return d;
}

/* Key hint line at the bottom of a dialog. */
static lv_obj_t *mk_hint(lv_obj_t *dlg, const char *txt)
{
    lv_obj_t *l = mk_label(dlg, &st_small, txt);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

static bool bm_equal(const ssh_bookmark_t *a, const ssh_bookmark_t *b)
{
    return strcmp(a->alias, b->alias) == 0 && strcmp(a->host, b->host) == 0 && strcmp(a->user, b->user) == 0 &&
           a->port == b->port && a->auth_type == b->auth_type && strcmp(a->key_path, b->key_path) == 0;
}

/* ---- connect / edit saved host dialog ---- */
static void set_auth_choice(int c)
{
    s_auth_choice = c;
    for (int i = 0; i < 3; i++) {
        if (i == c) lv_obj_add_style(btn_auth[i], &st_btn_primary, 0);
        else lv_obj_remove_style(btn_auth[i], &st_btn_primary, 0);
    }
    /* editing a saved host: passwords are never saved, so no password field */
    if (c == 2 || s_edit_idx >= 0) lv_obj_add_flag(row_pass, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(row_pass, LV_OBJ_FLAG_HIDDEN);
    if (c == 2) lv_obj_remove_flag(row_key, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(row_key, LV_OBJ_FLAG_HIDDEN);
    set_text(lbl_pass_hint, c == 1 ? "Device key (password optional, used as fallback)" : "Password");
}

static void auth_btn_cb(lv_event_t *e)
{
    set_auth_choice((int)(intptr_t)lv_event_get_user_data(e));
}

static void connect_dialog_mode(bool edit)
{
    set_text(lbl_connect_title, edit ? "Edit saved host" : "New SSH connection");
    set_text(lbl_connect_btn, edit ? LV_SYMBOL_SAVE "  Save" : LV_SYMBOL_OK "  Connect");
    set_text(lbl_connect_hint, edit ? HINT_EDIT : HINT_CONNECT);
    set_text(lbl_connect_err, "");
    if (edit) lv_obj_add_flag(cb_save, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(cb_save, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_align(row_connect_btns, edit ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
}

static void open_connect_dialog(const char *host)
{
    close_dialogs();
    lv_textarea_set_text(ta_host, host ? host : "");
    lv_textarea_set_text(ta_port, "22");
    lv_textarea_set_text(ta_alias, "");
    lv_textarea_set_text(ta_pass, "");
    char keys[1][SSH_MAX_PATH_LEN];
    int nk = 0;
    if (!lv_textarea_get_text(ta_keypath)[0] && ssh_port_scan_keys(keys, 1, &nk) == 0 && nk > 0) {
        lv_textarea_set_text(ta_keypath, keys[0]);
    }
    connect_dialog_mode(false);
    set_auth_choice(ssh_port_devkey_exists() ? 1 : 0);
    show_dialog(dlg_connect, &s_f_connect, host && host[0] ? ta_user : ta_host);
}

/* E on a saved host in the panel. */
static void open_edit_dialog(int idx)
{
    if (idx < 0 || idx >= s_host_count) return;
    close_dialogs();
    s_edit_idx = idx;
    s_edit_orig = s_hosts[idx];
    const ssh_bookmark_t *bm = &s_edit_orig;
    char port[12];
    snprintf(port, sizeof(port), "%d", bm->port > 0 ? bm->port : 22);
    lv_textarea_set_text(ta_host, bm->host);
    lv_textarea_set_text(ta_port, port);
    lv_textarea_set_text(ta_user, bm->user);
    lv_textarea_set_text(ta_alias, bm->alias);
    lv_textarea_set_text(ta_pass, "");
    if (bm->key_path[0]) lv_textarea_set_text(ta_keypath, bm->key_path);
    connect_dialog_mode(true);
    set_auth_choice(bm->auth_type == SSH_AUTH_DEVICE_KEY ? 1 : bm->auth_type == SSH_AUTH_KEY ? 2 : 0);
    show_dialog(dlg_connect, &s_f_connect, ta_host);
}

/* Replace the saved host being edited. ssh_port_save_bookmark() updates an
 * entry with the same name or user@host:port in place; otherwise the old
 * entry is deleted and the new one appended. */
static int save_edited_host(const ssh_bookmark_t *bm)
{
    const ssh_bookmark_t *o = &s_edit_orig;
    int idx = -1;
    for (int i = 0; i < s_host_count && idx < 0; i++) {
        if (bm_equal(&s_hosts[i], o)) idx = i;
    }
    bool in_place = (o->alias[0] && strcmp(o->alias, bm->alias) == 0) ||
                    (strcmp(o->host, bm->host) == 0 && strcmp(o->user, bm->user) == 0 && o->port == bm->port);
    if (idx >= 0 && !in_place) ssh_port_delete_bookmark(idx);
    s_hosts_key[0] = '\0';
    return ssh_port_save_bookmark(bm);
}

static void connect_error(const char *msg, lv_obj_t *field)
{
    set_text(lbl_connect_err, msg);
    if (field) {
        devos_focus_set(&s_f_connect, field);
        sync_kb(&s_f_connect);
    }
}

static void connect_submit(void)
{
    bool edit = s_edit_idx >= 0;
    ssh_bookmark_t bm;
    memset(&bm, 0, sizeof(bm));
    snprintf(bm.host, sizeof(bm.host), "%s", lv_textarea_get_text(ta_host));
    snprintf(bm.user, sizeof(bm.user), "%s", lv_textarea_get_text(ta_user));
    snprintf(bm.alias, sizeof(bm.alias), "%s", lv_textarea_get_text(ta_alias));
    bm.port = atoi(lv_textarea_get_text(ta_port));
    if (bm.port <= 0 || bm.port > 65535) bm.port = 22;
    bm.auth_type = s_auth_choice == 1 ? SSH_AUTH_DEVICE_KEY : s_auth_choice == 2 ? SSH_AUTH_KEY : SSH_AUTH_PASSWORD;
    snprintf(bm.key_path, sizeof(bm.key_path), "%s", s_auth_choice == 2 ? lv_textarea_get_text(ta_keypath) : "");
    if (!bm.host[0]) { connect_error("Enter a host name or IP address.", ta_host); return; }
    if (!bm.user[0]) { connect_error("Enter a user name.", ta_user); return; }
    if (!edit && bm.auth_type == SSH_AUTH_PASSWORD && !lv_textarea_get_text(ta_pass)[0]) {
        connect_error("Enter the password (or choose Device key).", ta_pass);
        return;
    }
    if (bm.auth_type == SSH_AUTH_KEY && !bm.key_path[0]) { connect_error("Enter the key file path.", ta_keypath); return; }
    if (edit) {
        if (save_edited_host(&bm) != 0) { connect_error("Could not save to the SD card.", NULL); return; }
        close_dialogs();                                    /* focus back to the panel */
        refresh_sidebar(true);
        for (int i = 0; i < s_host_count; i++) {           /* keep the edited host selected */
            if (s_psel_kind == PI_HOST && bm_equal(&s_hosts[i], &bm)) { s_psel_idx = i; break; }
        }
        panel_apply_sel();
        return;
    }
    if (bm.auth_type == SSH_AUTH_DEVICE_KEY && !ssh_port_devkey_exists()) {
        connect_error("No device key yet: press Esc, then Sym+K to create one.", NULL);
        return;
    }
    if (lv_obj_has_state(cb_save, LV_STATE_CHECKED)) {
        ssh_port_save_bookmark(&bm);                        /* never stores the password */
        s_hosts_key[0] = '\0';
    }
    char pw[SSH_MAX_PASSWORD_LEN];
    snprintf(pw, sizeof(pw), "%s", lv_textarea_get_text(ta_pass));
    close_dialogs();
    if (start_session(&bm, pw, 0) < 0) {
        vt_puts(ts_for(s_active), "\r\n\033[1;31mAll 8 session slots are in use.\033[0m\r\n");
    }
    memset(pw, 0, sizeof(pw));
    refresh_sidebar(true);
}

static void connect_btn_cb(lv_event_t *e) { LV_UNUSED(e); connect_submit(); }
static void cancel_btn_cb(lv_event_t *e) { LV_UNUSED(e); close_dialogs(); }

/* ---- password dialog ---- */
static void open_password_dialog(const ssh_bookmark_t *bm, int reuse_id)
{
    close_dialogs();
    s_pending.active = true;
    s_pending.reuse_id = reuse_id;
    s_pending.bm = *bm;
    char t[200];
    snprintf(t, sizeof(t), "Password for %.40s@%.80s", bm->user, bm->host);
    lv_label_set_text(lbl_pw_title, t);
    lv_textarea_set_text(ta_pw, "");
    show_dialog(dlg_password, &s_f_pw, ta_pw);
}

static void password_submit(void)
{
    if (!s_pending.active) return;
    ssh_bookmark_t bm = s_pending.bm;
    int reuse = s_pending.reuse_id;
    char pw[SSH_MAX_PASSWORD_LEN];
    snprintf(pw, sizeof(pw), "%s", lv_textarea_get_text(ta_pw));
    close_dialogs();
    start_session(&bm, pw, reuse);
    memset(pw, 0, sizeof(pw));
    refresh_sidebar(true);
}

static void pw_ok_cb(lv_event_t *e) { LV_UNUSED(e); password_submit(); }

/* ---- host key dialog ---- */
static void hostkey_answer(bool trust)
{
    int id = s_hostkey_for;
    s_hostkey_for = 0;
    close_dialogs();
    if (id) ssh_port_hostkey_answer(id, trust);
}

static void hk_trust_cb(lv_event_t *e) { LV_UNUSED(e); hostkey_answer(true); }
static void hk_reject_cb(lv_event_t *e) { LV_UNUSED(e); hostkey_answer(false); }

/* ---- device key dialog ---- */
static void refresh_devkey_dialog(void)
{
    char pub[300];
    if (ssh_port_devkey_exists() && ssh_port_devkey_public(pub, sizeof(pub)) == 0) {
        char body[640];
        snprintf(body, sizeof(body),
                 "This device's public key (ECDSA P-256):\n\n%s\n\n"
                 "Add it to ~/.ssh/authorized_keys on a server, or connect once with a password and "
                 "press Install to add it to that server for you.", pub);
        lv_label_set_text(lbl_dk_body, body);
        lv_obj_add_flag(btn_dk_create, LV_OBJ_FLAG_HIDDEN);
        ssh_session_t *s = ssh_port_get_session(s_active);
        if (s && s->state == SSH_SESSION_CONNECTED) lv_obj_remove_flag(btn_dk_install, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(btn_dk_install, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(lbl_dk_body,
                          "No device key yet.\n\nCreate one to log in without typing passwords. The private key "
                          "stays in this device's flash (NVS); only the public half is shown here.\n\n"
                          "Ed25519 keys are not supported by this build. RSA or ECDSA keys in PEM format "
                          "copied to /sdcard/.ssh/ can be used with \"Key file\".");
        lv_obj_remove_flag(btn_dk_create, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(btn_dk_install, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Default button: the action on offer (Install / Create), else Close. */
static lv_obj_t *dk_default_btn(void)
{
    if (!lv_obj_has_flag(btn_dk_install, LV_OBJ_FLAG_HIDDEN)) return btn_dk_install;
    if (!lv_obj_has_flag(btn_dk_create, LV_OBJ_FLAG_HIDDEN)) return btn_dk_create;
    return btn_dk_close;
}

static void open_devkey_dialog(void)
{
    close_dialogs();
    refresh_devkey_dialog();
    show_dialog(dlg_devkey, &s_f_dk, dk_default_btn());
}

static void dk_create_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_label_set_text(lbl_dk_body, "Generating key...");
    lv_refr_now(NULL);
    if (ssh_port_devkey_generate() != 0) {
        lv_label_set_text(lbl_dk_body, "Key generation failed. See the serial log.");
        return;
    }
    refresh_devkey_dialog();
    devos_focus_set(&s_f_dk, dk_default_btn());            /* Create is hidden now */
}

static void dk_install_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    char pub[300], cmd[1024];
    if (ssh_port_devkey_public(pub, sizeof(pub)) != 0 || !s_active) return;
    snprintf(cmd, sizeof(cmd),
             "mkdir -p ~/.ssh && chmod 700 ~/.ssh && grep -qxF '%s' ~/.ssh/authorized_keys 2>/dev/null || "
             "echo '%s' >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys && "
             "echo 'devOS: device key installed'\r", pub, pub);
    ssh_port_send(s_active, cmd, strlen(cmd));
    close_dialogs();
}

/* ---- confirm dialog (disconnect a session, delete a saved host) ---- */
static void open_confirm(int kind, int arg, const ssh_bookmark_t *bm, const char *body, const char *ok)
{
    close_dialogs();
    s_confirm.kind = kind;
    s_confirm.arg = arg;
    if (bm) s_confirm.bm = *bm;
    else memset(&s_confirm.bm, 0, sizeof(s_confirm.bm));
    lv_label_set_text(lbl_confirm_body, body);
    lv_label_set_text(lbl_confirm_ok, ok);
    show_dialog(dlg_confirm, &s_f_confirm, btn_confirm_ok);   /* Enter confirms */
}

static void confirm_ok_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int kind = s_confirm.kind, arg = s_confirm.arg;
    ssh_bookmark_t bm = s_confirm.bm;
    close_dialogs();
    if (kind == CONFIRM_DISCONNECT) {
        ssh_session_t *s = ssh_port_get_session(arg);
        if (s && state_live(s->state)) session_close(arg);   /* not if it ended meanwhile */
    } else if (kind == CONFIRM_DELETE_HOST) {
        if (arg >= 0 && arg < s_host_count && bm_equal(&s_hosts[arg], &bm)) host_delete(arg);
    }
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL) { close_dialogs(); return; }
    if (code != LV_EVENT_READY) return;
    if (!lv_obj_has_flag(dlg_password, LV_OBJ_FLAG_HIDDEN)) password_submit();
    else if (!lv_obj_has_flag(dlg_connect, LV_OBJ_FLAG_HIDDEN)) connect_submit();
}

static void build_dialogs(void)
{
    overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(overlay);
    lv_obj_add_style(overlay, &st_overlay, 0);
    lv_obj_set_size(overlay, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    devos_focus_t *fs[] = { &s_f_connect, &s_f_pw, &s_f_hk, &s_f_dk, &s_f_confirm };
    for (size_t i = 0; i < sizeof(fs) / sizeof(fs[0]); i++) devos_focus_init(fs[i]);

    /* connect / edit saved host */
    dlg_connect = mk_dialog(720);
    lbl_connect_title = mk_label(dlg_connect, &st_text, "New SSH connection");
    lv_obj_set_style_text_font(lbl_connect_title, &lv_font_montserrat_20, 0);
    lv_obj_t *row = mk_row(dlg_connect);
    ta_host = mk_ta(row, "Host or IP (e.g. 192.168.1.20 or box.local)", 560, false);
    ta_port = mk_ta(row, "Port", 100, false);
    lv_textarea_set_accepted_chars(ta_port, "0123456789");
    lv_textarea_set_max_length(ta_port, 5);
    row = mk_row(dlg_connect);
    ta_user = mk_ta(row, "User (e.g. root)", 330, false);
    ta_alias = mk_ta(row, "Name (optional)", 330, false);
    row = mk_row(dlg_connect);
    mk_label(row, &st_muted, "Sign in with");
    static const char *const auth_txt[3] = { "Password", "Device key", "Key file" };
    for (int i = 0; i < 3; i++) btn_auth[i] = mk_btn(row, auth_txt[i], NULL, auth_btn_cb, (void *)(intptr_t)i);
    row_pass = lv_obj_create(dlg_connect);
    lv_obj_remove_style_all(row_pass);
    lv_obj_set_size(row_pass, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row_pass, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row_pass, 4, 0);
    lbl_pass_hint = mk_label(row_pass, &st_muted, "Password");
    ta_pass = mk_ta(row_pass, "Password (not saved)", 680, true);
    lv_textarea_set_max_length(ta_pass, SSH_MAX_PASSWORD_LEN - 1);
    row_key = lv_obj_create(dlg_connect);
    lv_obj_remove_style_all(row_key);
    lv_obj_set_size(row_key, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row_key, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row_key, 4, 0);
    mk_label(row_key, &st_muted, "Private key file (RSA or ECDSA, PEM; put the .pub next to ECDSA keys)");
    ta_keypath = mk_ta(row_key, TAB5_SD_MOUNT_POINT "/.ssh/id_rsa", 680, false);
    lbl_connect_err = mk_label(dlg_connect, &st_err, "");
    row_connect_btns = mk_row(dlg_connect);
    cb_save = lv_checkbox_create(row_connect_btns);
    lv_checkbox_set_text(cb_save, "Save to saved hosts (password is never saved)");
    lv_obj_add_style(cb_save, &st_text, 0);
    lv_obj_add_state(cb_save, LV_STATE_CHECKED);
    lv_obj_set_flex_grow(cb_save, 1);
    lv_obj_t *cn_cancel = mk_btn(row_connect_btns, "Cancel", NULL, cancel_btn_cb, NULL);
    lv_obj_t *cn_ok = mk_btn(row_connect_btns, LV_SYMBOL_OK "  Connect", &st_btn_primary, connect_btn_cb, NULL);
    lbl_connect_btn = lv_obj_get_child(cn_ok, 0);
    lbl_connect_hint = mk_hint(dlg_connect, HINT_CONNECT);
    lv_obj_t *cn_order[] = { ta_host, ta_port, ta_user, ta_alias, btn_auth[0], btn_auth[1], btn_auth[2],
                             ta_pass, ta_keypath, cb_save, cn_cancel, cn_ok };
    for (size_t i = 0; i < sizeof(cn_order) / sizeof(cn_order[0]); i++) devos_focus_add(&s_f_connect, cn_order[i]);

    /* password */
    dlg_password = mk_dialog(620);
    lbl_pw_title = mk_label(dlg_password, &st_text, "Password");
    lv_obj_set_style_text_font(lbl_pw_title, &lv_font_montserrat_20, 0);
    ta_pw = mk_ta(dlg_password, "Password", 580, true);
    lv_textarea_set_max_length(ta_pw, SSH_MAX_PASSWORD_LEN - 1);
    row = mk_row(dlg_password);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *pw_cancel = mk_btn(row, "Cancel", NULL, cancel_btn_cb, NULL);
    lv_obj_t *pw_ok = mk_btn(row, LV_SYMBOL_OK "  Connect", &st_btn_primary, pw_ok_cb, NULL);
    mk_hint(dlg_password, "Enter  connect     Tab  move     Esc  cancel");
    devos_focus_add(&s_f_pw, ta_pw);
    devos_focus_add(&s_f_pw, pw_cancel);
    devos_focus_add(&s_f_pw, pw_ok);

    /* host key */
    dlg_hostkey = mk_dialog(700);
    lv_obj_t *t = mk_label(dlg_hostkey, &st_text, LV_SYMBOL_WARNING "  Unknown host key");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lbl_hk_body = mk_label(dlg_hostkey, &st_text, "");
    lv_obj_set_width(lbl_hk_body, 660);
    lv_label_set_long_mode(lbl_hk_body, LV_LABEL_LONG_WRAP);
    row = mk_row(dlg_hostkey);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *hk_reject = mk_btn(row, "Reject (N)", &st_btn_danger, hk_reject_cb, NULL);
    btn_hk_trust = mk_btn(row, LV_SYMBOL_OK "  Trust (Y)", &st_btn_primary, hk_trust_cb, NULL);
    mk_hint(dlg_hostkey, "Y  trust     N / Esc  reject     Left / Right / Tab  move     Enter  press the highlighted button");
    devos_focus_add(&s_f_hk, hk_reject);
    devos_focus_add(&s_f_hk, btn_hk_trust);

    /* device key */
    dlg_devkey = mk_dialog(760);
    t = mk_label(dlg_devkey, &st_text, "Device key");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lbl_dk_body = mk_label(dlg_devkey, &st_text, "");
    lv_obj_set_width(lbl_dk_body, 720);
    lv_label_set_long_mode(lbl_dk_body, LV_LABEL_LONG_WRAP);
    row = mk_row(dlg_devkey);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    btn_dk_close = mk_btn(row, "Close", NULL, cancel_btn_cb, NULL);
    btn_dk_create = mk_btn(row, LV_SYMBOL_PLUS "  Create key", &st_btn_primary, dk_create_cb, NULL);
    btn_dk_install = mk_btn(row, LV_SYMBOL_UPLOAD "  Install on this session's host", &st_btn_primary,
                            dk_install_cb, NULL);
    mk_hint(dlg_devkey, "Left / Right / Tab  move     Enter  press the highlighted button     Esc  close");
    devos_focus_add(&s_f_dk, btn_dk_close);
    devos_focus_add(&s_f_dk, btn_dk_create);
    devos_focus_add(&s_f_dk, btn_dk_install);

    /* confirm */
    dlg_confirm = mk_dialog(600);
    t = mk_label(dlg_confirm, &st_text, LV_SYMBOL_WARNING "  Please confirm");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lbl_confirm_body = mk_label(dlg_confirm, &st_text, "");
    lv_obj_set_width(lbl_confirm_body, 560);
    lv_label_set_long_mode(lbl_confirm_body, LV_LABEL_LONG_WRAP);
    row = mk_row(dlg_confirm);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *cf_cancel = mk_btn(row, "Cancel", NULL, cancel_btn_cb, NULL);
    btn_confirm_ok = mk_btn(row, "OK", &st_btn_danger, confirm_ok_cb, NULL);
    lbl_confirm_ok = lv_obj_get_child(btn_confirm_ok, 0);
    mk_hint(dlg_confirm, "Enter  confirm     Esc  cancel     Left / Right / Tab  move");
    devos_focus_add(&s_f_confirm, cf_cancel);
    devos_focus_add(&s_f_confirm, btn_confirm_ok);

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
/* Keyboard                                                                 */
/* ======================================================================== */
/* Dialogs are modal: every key is theirs. devos_focus does fields, buttons
 * and the checkbox; the app adds Esc, Enter-to-submit, the sign-in method
 * (Left / Right) and Left / Right along button rows. */
static bool dialog_handle_key(uint32_t key, uint8_t mods)
{
    if (tab5_keyboard_is_connected()) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    devos_focus_t *f = NULL;
    lv_obj_t *d = visible_dialog(&f);
    if (!d) {
        close_dialogs();
        return true;
    }
    bool plain = !(mods & (DEVOS_MOD_CTRL | DEVOS_MOD_FN | DEVOS_MOD_ALT));

    if (d == dlg_hostkey) {
        if (plain && (key == 'y' || key == 'Y')) { hostkey_answer(true); return true; }
        if ((plain && (key == 'n' || key == 'N')) || key == LV_KEY_ESC) { hostkey_answer(false); return true; }
    } else if (key == LV_KEY_ESC) {
        close_dialogs();
        return true;
    }

    lv_obj_t *o = devos_focus_get(f);
    if (d == dlg_connect && plain && (key == LV_KEY_LEFT || key == LV_KEY_RIGHT)) {
        for (int i = 0; i < 3; i++) {
            if (o != btn_auth[i]) continue;
            int c = i + (key == LV_KEY_RIGHT ? 1 : -1);
            if (c >= 0 && c < 3) {
                set_auth_choice(c);
                devos_focus_set(f, btn_auth[c]);
            }
            return true;
        }
    }
    /* Space ticks the checkbox; Enter on it confirms the dialog (key model) */
    if (d == dlg_connect && o == cb_save && (key == '\r' || key == '\n')) {
        connect_submit();
        return true;
    }
    if (devos_focus_key(f, key, mods)) {
        if (devos_focus_get(f) != o) sync_kb(f);
        return true;
    }
    if (key == '\r' || key == '\n') {                       /* Enter in a text field: submit */
        if (d == dlg_connect) connect_submit();
        else if (d == dlg_password) password_submit();
        return true;
    }
    o = devos_focus_get(f);
    if (plain && (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) && o && lv_obj_check_type(o, &lv_button_class)) {
        devos_focus_move(f, key == LV_KEY_RIGHT ? 1 : -1);  /* along a button row */
        return true;
    }
    return true;
}

static void send_str(const char *s)
{
    ssh_port_send(s_active, s, strlen(s));
}

static bool terminal_handle_key(uint32_t key, uint8_t mods)
{
    if (dialog_open()) return dialog_handle_key(key, mods);

    /* Local shortcuts (Sym = DEVOS_MOD_FN), from the shell and the panel */
    if (mods & DEVOS_MOD_FN) {
        if (key == 'l' || key == 'L') {
            if (s_panel_focus) set_sidebar_visible(false);  /* hide; focus back to the shell */
            else set_panel_focus(true);                     /* open the panel if hidden, focus it */
            return true;
        }
        if (key == 'n' || key == 'N') { open_connect_dialog(NULL); return true; }
        if (key == 'k' || key == 'K') { open_devkey_dialog(); return true; }
    }
    if ((mods & DEVOS_MOD_ALT) && key >= '1' && key <= '8') {
        term_sess_t *to = ts_for((int)(key - '0'));
        if (to && to->vt) {
            switch_session((int)(key - '0'));
            set_panel_focus(false);
        }
        return true;
    }

    if (s_panel_focus && panel_handle_key(key, mods)) return true;

    term_sess_t *t = ts_for(s_active);
    ssh_session_t *s = ssh_port_get_session(s_active);
    if (!t || !t->vt || !s) {
        if (key == '\r' || key == '\n') { open_connect_dialog(NULL); return true; }
        if (!(mods & (DEVOS_MOD_CTRL | DEVOS_MOD_FN | DEVOS_MOD_ALT)) &&
            (key == '\t' || key == LV_KEY_UP || key == LV_KEY_DOWN)) {
            set_panel_focus(true);                          /* no shell to type into */
            return true;
        }
        return false;                                       /* Esc -> Home */
    }

    /* Scrollback */
    if (key == DEVOS_KEY_PGUP || key == DEVOS_KEY_PGDN) {
        int max = devos_vterm_scrollback_lines(t->vt);
        int step = devos_vterm_rows(t->vt) - 2;
        int v = t->view_offset + (key == DEVOS_KEY_PGUP ? step : -step);
        if (v < 0) v = 0;
        if (v > max) v = max;
        t->view_offset = v;
        term_invalidate_all();
        refresh_header();
        return true;
    }

    if (s->state != SSH_SESSION_CONNECTED) {
        if (key == '\r' || key == '\n') {
            if (s->state == SSH_SESSION_ERROR || s->state == SSH_SESSION_CLOSED) {
                ssh_bookmark_t bm;
                memset(&bm, 0, sizeof(bm));
                snprintf(bm.alias, sizeof(bm.alias), "%s", t->alias);
                snprintf(bm.host, sizeof(bm.host), "%s", t->host);
                bm.port = t->port;
                snprintf(bm.user, sizeof(bm.user), "%s", t->user);
                bm.auth_type = t->auth;
                snprintf(bm.key_path, sizeof(bm.key_path), "%s", t->keypath);
                connect_bookmark(&bm, s_active);
            }
            return true;
        }
        return key != LV_KEY_ESC;                           /* Esc -> Home */
    }

    /* Any input returns a scrolled-back view to the live screen */
    if (t->view_offset) {
        t->view_offset = 0;
        term_invalidate_all();
        refresh_header();
    }

    char out[16];
    bool ctrl = mods & DEVOS_MOD_CTRL, alt = mods & DEVOS_MOD_ALT, shift = mods & DEVOS_MOD_SHIFT;
    if (key == LV_KEY_UP || key == LV_KEY_DOWN || key == LV_KEY_RIGHT || key == LV_KEY_LEFT) {
        char dir = key == LV_KEY_UP ? 'A' : key == LV_KEY_DOWN ? 'B' : key == LV_KEY_RIGHT ? 'C' : 'D';
        if ((mods & DEVOS_MOD_FN) && (dir == 'C' || dir == 'D')) {       /* Sym+Right/Left = End/Home */
            snprintf(out, sizeof(out), devos_vterm_app_cursor_keys(t->vt) ? "\033O%c" : "\033[%c",
                     dir == 'C' ? 'F' : 'H');
        } else {
            int m = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
            if (m > 1) snprintf(out, sizeof(out), "\033[1;%d%c", m, dir);
            else snprintf(out, sizeof(out), devos_vterm_app_cursor_keys(t->vt) ? "\033O%c" : "\033[%c", dir);
        }
        send_str(out);
        return true;
    }
    if (mods & DEVOS_MOD_FN) return false;                  /* other Sym chords are global */
    if (key == '\r' || key == '\n') { send_str("\r"); return true; }
    if (key == '\b') { send_str(alt ? "\033\177" : "\177"); return true; }
    if (key == '\t') { send_str(shift ? "\033[Z" : "\t"); return true; }
    if (key == LV_KEY_ESC) { send_str("\033"); return true; }
    if (key == LV_KEY_DEL) { send_str("\033[3~"); return true; }
    if (ctrl && key < 128) {
        char c = 0;
        if (key < 0x20) c = (char)key;                      /* already a control code */
        else if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) c = (char)(key & 0x1F);
        else if (key == ' ' || key == '@' || key == '2') c = 0;
        else if (key == '[') c = 0x1B;
        else if (key == '\\') c = 0x1C;
        else if (key == ']') c = 0x1D;
        else if (key == '^' || key == '6') c = 0x1E;
        else if (key == '_' || key == '-' || key == '/') c = 0x1F;
        else return true;
        ssh_port_send(s_active, &c, 1);
        return true;
    }
    if (key < 0x20) {                                       /* raw control code (simulator) */
        char c = (char)key;
        ssh_port_send(s_active, &c, 1);
        return true;
    }
    if (key < 0x100) {
        int n = 0;
        if (alt) out[n++] = 0x1B;                           /* Alt = Meta prefix */
        n += utf8_put(out + n, key);
        ssh_port_send(s_active, out, (size_t)n);
        return true;
    }
    return false;
}

/* ======================================================================== */
/* Lifecycle                                                                */
/* ======================================================================== */
static void toggle_btn_cb(lv_event_t *e) { LV_UNUSED(e); app_terminal_toggle_sidebar(); }

static void apply_theme(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    if (!s_styles_ready) return;
    restyle(p);
    lv_obj_report_style_change(NULL);
    if (term_view) lv_obj_set_style_bg_color(term_view, p->code_bg, 0);
    term_invalidate_all();
}

static void terminal_init(void)
{
    styles_init();
    const devos_palette_t *p = devos_theme_get();

    screen = lv_obj_create(lv_screen_active());
    app_descriptor.screen = screen;
    lv_obj_remove_style_all(screen);
    lv_obj_add_style(screen, &st_bg, 0);
    lv_obj_set_size(screen, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(screen, 0, DEVOS_TOP_BAR_HEIGHT);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);

    /* Sidebar: scrolling sections + a fixed key-hint footer */
    sidebar = lv_obj_create(screen);
    lv_obj_remove_style_all(sidebar);
    lv_obj_add_style(sidebar, &st_sidebar, 0);
    lv_obj_set_style_pad_all(sidebar, 6, 0);               /* + 4 in the sections = 10 */
    lv_obj_set_size(sidebar, SIDEBAR_W, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(sidebar, LV_OBJ_FLAG_SCROLLABLE);
    side_scroll = lv_obj_create(sidebar);
    lv_obj_remove_style_all(side_scroll);
    lv_obj_set_width(side_scroll, lv_pct(100));
    lv_obj_set_flex_grow(side_scroll, 1);
    lv_obj_set_flex_flow(side_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(side_scroll, 2, 0);
    lv_obj_set_scroll_dir(side_scroll, LV_DIR_VER);
    list_sessions = mk_section(side_scroll, "SESSIONS", new_btn_cb, &btn_new);
    list_hosts = mk_section(side_scroll, "SAVED HOSTS", NULL, NULL);
    lv_obj_t *dk_head = mk_section(side_scroll, "DEVICE KEY", NULL, NULL);
    btn_devkey = mk_btn(dk_head, LV_SYMBOL_EYE_OPEN "  Device key...", NULL, devkey_btn_cb, NULL);
    lv_obj_set_width(btn_devkey, lv_pct(100));
    lv_obj_add_style(btn_devkey, &st_ring, LV_STATE_FOCUS_KEY);
    lbl_panel_hint = mk_label(sidebar, &st_small, HINT_SHELL);
    lv_obj_set_width(lbl_panel_hint, lv_pct(100));
    lv_label_set_long_mode(lbl_panel_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_hor(lbl_panel_hint, 4, 0);
    lv_obj_set_style_pad_top(lbl_panel_hint, 4, 0);

    /* Terminal area: header + grid */
    term_area = lv_obj_create(screen);
    lv_obj_remove_style_all(term_area);
    lv_obj_remove_flag(term_area, LV_OBJ_FLAG_SCROLLABLE);

    header = lv_obj_create(term_area);
    lv_obj_remove_style_all(header);
    lv_obj_add_style(header, &st_header, 0);
    lv_obj_set_size(header, lv_pct(100), HEADER_H);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tb = mk_btn(header, LV_SYMBOL_LIST, NULL, toggle_btn_cb, NULL);
    lv_obj_set_style_pad_ver(tb, 3, 0);
    lv_obj_set_style_pad_hor(tb, 8, 0);
    lv_obj_align(tb, LV_ALIGN_LEFT_MID, 0, 0);
    lbl_header_title = mk_label(header, &st_text, "Terminal");
    lv_label_set_long_mode(lbl_header_title, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_header_title, LV_ALIGN_LEFT_MID, 44, 0);
    lbl_header_info = mk_label(header, &st_muted, "");
    lv_obj_align(lbl_header_info, LV_ALIGN_RIGHT_MID, 0, 0);

    term_view = lv_obj_create(term_area);
    lv_obj_remove_style_all(term_view);
    lv_obj_set_style_bg_opa(term_view, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(term_view, p->code_bg, 0);
    lv_obj_set_pos(term_view, 0, HEADER_H);
    lv_obj_set_size(term_view, lv_pct(100), DEVOS_CONTENT_HEIGHT - HEADER_H);
    lv_obj_remove_flag(term_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(term_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(term_view, term_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(term_view, term_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(term_view, term_touch_cb, LV_EVENT_PRESSING, NULL);

    lbl_empty = mk_label(term_view, &st_muted,
                         "No terminal session\n\n"
                         "Enter or Sym+N: new SSH connection.\n"
                         "Tab or Sym+L: saved hosts and your device key (side panel, Esc to leave).");
    lv_obj_set_style_text_align(lbl_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl_empty, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl_empty);

    build_dialogs();
    layout();
    devos_theme_add_listener(apply_theme, NULL);
    lv_timer_create(poll_cb, 20, NULL);
    refresh_sidebar(true);
}

static void terminal_show(void)
{
    /* Another app (Tailscale peer list) may ask for a session to a host. */
    const devos_telemetry_t *t = devos_telemetry_get();
    if (t->terminal_requested_host[0]) {
        char host[64];
        snprintf(host, sizeof(host), "%s", t->terminal_requested_host);
        devos_telemetry_t u;
        memcpy(&u, t, sizeof(u));
        u.terminal_requested_host[0] = '\0';
        devos_telemetry_update(&u);
        for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
            ssh_session_t *s = ssh_port_get_session(i + 1);
            if (s_ts[i].vt && s && s->state == SSH_SESSION_CONNECTED && strcmp(s->host, host) == 0) {
                switch_session(i + 1);
                return;
            }
        }
        open_connect_dialog(host);
    }
    term_invalidate_all();
    refresh_sidebar(true);
    refresh_header();
}

static void terminal_hide(void)
{
    /* Keep a pending host-key prompt visible for when we come back. */
    if (s_hostkey_for == 0) close_dialogs();
    set_panel_focus(false);                                 /* come back to the shell */
}

static int terminal_telemetry_lines(char lines[3][64])
{
    const devos_telemetry_t *t = devos_telemetry_get();
    snprintf(lines[0], sizeof(lines[0]), "* %d live session%s", t->terminal_sessions,
             t->terminal_sessions == 1 ? "" : "s");
    snprintf(lines[1], sizeof(lines[1]), "* %s", t->terminal_host[0] ? t->terminal_host : "No session");
    snprintf(lines[2], sizeof(lines[2]), "* SSH + xterm-256color");
    return 3;
}

devos_app_descriptor_t *app_terminal_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_TERMINAL;
    app_descriptor.uid = "terminal";
    app_descriptor.icon = LV_SYMBOL_POWER;
    app_descriptor.draw_icon = devos_icon_terminal;
    app_descriptor.category = "systems";
    app_descriptor.name = "Terminal";
    app_descriptor.title = "Terminal / SSH";
    app_descriptor.subtitle = "SSH client + VT100 terminal";
    app_descriptor.screen = screen;
    app_descriptor.init = terminal_init;
    app_descriptor.show = terminal_show;
    app_descriptor.hide = terminal_hide;
    app_descriptor.handle_key = terminal_handle_key;
    app_descriptor.get_telemetry_lines = terminal_telemetry_lines;
    return &app_descriptor;
}
