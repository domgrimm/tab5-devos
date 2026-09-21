#include "app_opendev.h"
#include "devos_config.h"
#include "devos_theme.h"
#include "devos_agent_viewport.h"
#include "opendev_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPENDEV_SESS_BTNS OPENDEV_MAX_SESSIONS

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;
static devos_agent_viewport_t *viewport = NULL;

/* Left: status + sessions */
static lv_obj_t *lbl_title = NULL;
static lv_obj_t *status_card = NULL;
static lv_obj_t *lbl_target = NULL;
static lv_obj_t *lbl_status = NULL;
static lv_obj_t *btn_connect = NULL;
static lv_obj_t *lbl_connect = NULL;
static lv_obj_t *btn_new = NULL;
static lv_obj_t *lbl_new = NULL;
static lv_obj_t *sess_btns[OPENDEV_SESS_BTNS] = {NULL};
static lv_obj_t *sess_lbls[OPENDEV_SESS_BTNS] = {NULL};

/* Center: chat + input */
static lv_obj_t *chat_scroll = NULL;
static lv_obj_t *input_bar = NULL;
static lv_obj_t *ta = NULL;
static lv_obj_t *btn_send = NULL;
static lv_obj_t *lbl_send = NULL;
static bool think_expanded = true;

/* Right: session info + diffs */
static lv_obj_t *lbl_rg = NULL;
static lv_obj_t *lbl_info = NULL;
static lv_obj_t *btn_diff = NULL;
static lv_obj_t *lbl_diff_btn = NULL;
static lv_obj_t *lbl_diff = NULL;

/* Permission modal */
static lv_obj_t *modal_perm = NULL;
static lv_obj_t *lbl_perm_title = NULL;
static lv_obj_t *lbl_perm_desc = NULL;
static lv_obj_t *perm_btns[3] = {NULL};
static lv_obj_t *perm_lbls[3] = {NULL};

/* Server modal */
static lv_obj_t *modal_srv = NULL;
static lv_obj_t *lbl_srv_title = NULL;
static lv_obj_t *ta_host = NULL;
static lv_obj_t *ta_port = NULL;
static lv_obj_t *srv_focus = NULL;
static lv_obj_t *lbl_srv_save = NULL;

static uint32_t s_seen_gen = 0;
static lv_timer_t *poll_timer = NULL;

static void refresh_all(void);
static void apply_theme(const devos_palette_t *p, void *user_data);
static void srv_focus_paint(void);

/* ------------------------------------------------------------------ theme */
static void apply_theme(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    if (!screen) return;
    lv_color_t on_accent =
        devos_theme_is_dark() ? lv_color_black() : lv_color_white();

    lv_obj_set_style_bg_color(screen, p->bg, 0);

    if (lbl_title) lv_obj_set_style_text_color(lbl_title, p->text_secondary, 0);
    if (status_card) {
        lv_obj_set_style_bg_color(status_card, p->surface, 0);
        lv_obj_set_style_border_color(status_card, p->surface_border, 0);
    }
    if (lbl_target) lv_obj_set_style_text_color(lbl_target, p->text_primary, 0);
    if (lbl_status) lv_obj_set_style_text_color(lbl_status, p->accent_secondary, 0);
    if (btn_connect) {
        lv_obj_set_style_bg_color(btn_connect, p->surface_active, 0);
        lv_obj_set_style_border_color(btn_connect, p->accent_primary, 0);
    }
    if (lbl_connect) lv_obj_set_style_text_color(lbl_connect, p->accent_primary, 0);
    if (btn_new) {
        lv_obj_set_style_bg_color(btn_new, p->surface, 0);
        lv_obj_set_style_border_color(btn_new, p->surface_border, 0);
    }
    if (lbl_new) lv_obj_set_style_text_color(lbl_new, p->text_primary, 0);
    for (int i = 0; i < OPENDEV_SESS_BTNS; i++) {
        if (sess_btns[i]) {
            lv_obj_set_style_bg_color(sess_btns[i], p->surface, 0);
            lv_obj_set_style_border_color(sess_btns[i], p->surface_border, 0);
        }
        if (sess_lbls[i]) {
            lv_obj_set_style_text_color(sess_lbls[i], p->text_primary, 0);
        }
    }

    if (chat_scroll) lv_obj_set_style_bg_color(chat_scroll, p->bg, 0);
    if (input_bar) {
        lv_obj_set_style_bg_color(input_bar, p->surface, 0);
        lv_obj_set_style_border_color(input_bar, p->surface_border, 0);
    }
    if (ta) {
        lv_obj_set_style_bg_color(ta, p->bg_alt, 0);
        lv_obj_set_style_border_color(ta, p->surface_border, 0);
        lv_obj_set_style_text_color(ta, p->text_primary, 0);
    }
    if (btn_send) lv_obj_set_style_bg_color(btn_send, p->accent_primary, 0);
    if (lbl_send) lv_obj_set_style_text_color(lbl_send, on_accent, 0);

    if (lbl_rg) lv_obj_set_style_text_color(lbl_rg, p->text_secondary, 0);
    if (lbl_info) lv_obj_set_style_text_color(lbl_info, p->text_primary, 0);
    if (btn_diff) {
        lv_obj_set_style_bg_color(btn_diff, p->surface_active, 0);
        lv_obj_set_style_border_color(btn_diff, p->surface_border, 0);
    }
    if (lbl_diff_btn) {
        lv_obj_set_style_text_color(lbl_diff_btn, p->text_primary, 0);
    }
    if (lbl_diff) lv_obj_set_style_text_color(lbl_diff, p->text_secondary, 0);

    if (modal_perm) {
        lv_obj_set_style_bg_color(modal_perm, p->surface, 0);
        lv_obj_set_style_border_color(modal_perm, p->accent_warning, 0);
    }
    if (lbl_perm_title) {
        lv_obj_set_style_text_color(lbl_perm_title, p->accent_warning, 0);
    }
    for (int i = 0; i < 3; i++) {
        if (perm_btns[i]) {
            lv_obj_set_style_bg_color(perm_btns[i],
                i == 1 ? p->accent_danger
                       : i == 2 ? p->surface_active : p->accent_secondary,
                0);
        }
        if (perm_lbls[i]) {
            lv_obj_set_style_text_color(perm_lbls[i],
                i == 1 ? lv_color_white()
                       : i == 2 ? p->text_primary
                                : devos_theme_is_dark() ? lv_color_black()
                                                        : lv_color_white(),
                0);
        }
    }
    if (lbl_perm_desc) {
        lv_obj_set_style_text_color(lbl_perm_desc, p->text_primary, 0);
    }
    if (modal_srv) {
        lv_obj_set_style_bg_color(modal_srv, p->surface, 0);
        lv_obj_set_style_border_color(modal_srv, p->accent_primary, 0);
    }
    if (lbl_srv_title) {
        lv_obj_set_style_text_color(lbl_srv_title, p->accent_primary, 0);
    }
    if (lbl_srv_save) {
        lv_obj_set_style_text_color(lbl_srv_save, on_accent, 0);
    }
    if (ta_host) {
        lv_obj_set_style_bg_color(ta_host, p->code_bg, 0);
        lv_obj_set_style_text_color(ta_host, p->text_primary, 0);
        lv_obj_set_style_border_color(ta_host, p->surface_border, 0);
    }
    if (ta_port) {
        lv_obj_set_style_bg_color(ta_port, p->code_bg, 0);
        lv_obj_set_style_text_color(ta_port, p->text_primary, 0);
        lv_obj_set_style_border_color(ta_port, p->surface_border, 0);
    }
    srv_focus_paint();

    refresh_all();
}

/* --------------------------------------------------------------- refresh */
static lv_obj_t *chat_card(const devos_palette_t *p, lv_color_t border)
{
    lv_obj_t *card = lv_obj_create(chat_scroll);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, p->surface, 0);
    lv_obj_set_style_border_color(card, border, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    return card;
}

static void chat_text(lv_obj_t *card, const char *text,
                      lv_color_t color, const lv_font_t *font)
{
    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
}

static void toggle_think_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    think_expanded = !think_expanded;
    refresh_all();
}

static void refresh_all(void)
{
    if (!screen) return;
    const devos_palette_t *p = devos_theme_get();
    char host[OPENDEV_HOST_MAX];
    int port = 0;
    opendev_mode_t mode = OPENDEV_MODE_CODE;
    opendev_client_get_config(host, sizeof(host), &port, &mode, NULL, 0);

    /* Left: status */
    char buf[160];
    snprintf(buf, sizeof(buf), "%s:%d%s", host, port,
             mode == OPENDEV_MODE_CHAMBER ? " [chamber]" : "");
    if (lbl_target) lv_label_set_text(lbl_target, buf);
    if (lbl_status) {
        lv_label_set_text(lbl_status, opendev_client_status_text());
        lv_obj_set_style_text_color(lbl_status,
            opendev_client_status() == OPENDEV_UP ? p->accent_secondary
                                                  : p->text_secondary,
            0);
    }

    /* Left: sessions */
    int n = opendev_client_session_count();
    int active = opendev_client_active();
    for (int i = 0; i < OPENDEV_SESS_BTNS; i++) {
        if (!sess_btns[i]) continue;
        if (i >= n) {
            lv_obj_add_flag(sess_btns[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(sess_btns[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(sess_btns[i], 4, 178 + i * 52);
        const opendev_session_t *s = opendev_client_session(i);
        snprintf(buf, sizeof(buf), "%s%s\n%.12s%s", s->busy ? LV_SYMBOL_BULLET " " : "",
                 s->title[0] ? s->title : "(untitled)", s->id,
                 i == active ? "  [active]" : "");
        lv_label_set_text(sess_lbls[i], buf);
        if (i == active) {
            lv_obj_set_style_bg_color(sess_btns[i], p->surface_active, 0);
            lv_obj_set_style_border_color(sess_btns[i], p->accent_primary, 0);
            lv_obj_set_style_text_color(sess_lbls[i], p->accent_primary, 0);
        } else {
            lv_obj_set_style_bg_color(sess_btns[i], p->surface, 0);
            lv_obj_set_style_border_color(sess_btns[i], p->surface_border, 0);
            lv_obj_set_style_text_color(sess_lbls[i], p->text_primary, 0);
        }
    }

    /* Center: chat blocks */
    if (chat_scroll) {
        lv_obj_clean(chat_scroll);
        int nb = opendev_client_block_count();
        /* ponytail: one collapsible card for the whole thinking trace */
        bool has_think = false;
        for (int i = 0; i < nb; i++) {
            const opendev_block_t *b = opendev_client_block(i);
            if (b && b->kind == OPENDEV_KIND_THINK && b->text[0]) {
                has_think = true;
                break;
            }
        }
        if (has_think) {
            lv_obj_t *box = chat_card(p, p->thinking_border);
            lv_obj_set_style_bg_color(box, p->thinking_bg, 0);
            lv_obj_add_event_cb(box, toggle_think_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_t *arrow = lv_label_create(box);
            lv_label_set_text(arrow, think_expanded
                              ? LV_SYMBOL_DOWN " Thinking"
                              : LV_SYMBOL_RIGHT " Thinking (collapsed)");
            lv_obj_set_style_text_color(arrow, p->accent_primary, 0);
            lv_obj_set_style_text_font(arrow, &lv_font_montserrat_14, 0);
            if (think_expanded) {
                lv_obj_t *body = lv_obj_create(box);
                lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
                lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(body, 0, 0);
                lv_obj_set_style_pad_all(body, 0, 0);
                for (int i = 0; i < nb; i++) {
                    const opendev_block_t *b = opendev_client_block(i);
                    if (b && b->kind == OPENDEV_KIND_THINK) {
                        chat_text(body, b->text, p->text_secondary,
                                  &lv_font_montserrat_12);
                    }
                }
            }
        }
        for (int i = 0; i < nb; i++) {
            const opendev_block_t *b = opendev_client_block(i);
            if (!b || b->kind == OPENDEV_KIND_THINK) continue;
            if (b->kind == OPENDEV_KIND_TOOL) {
                lv_obj_t *card = chat_card(p, p->tool_card_border);
                lv_obj_set_style_bg_color(card, p->tool_card_bg, 0);
                chat_text(card, b->text, p->accent_secondary,
                          &lv_font_montserrat_12);
            } else {
                bool user = b->role == OPENDEV_ROLE_USER;
                lv_obj_t *card = chat_card(p, user ? p->accent_primary
                                                  : p->surface_border);
                if (user) {
                    lv_obj_set_style_bg_color(card, p->surface_active, 0);
                }
                chat_text(card, b->text, p->text_primary,
                          &lv_font_montserrat_14);
            }
        }
        if (nb == 0) {
            lv_obj_t *card = chat_card(p, p->surface_border);
            const char *hint =
                opendev_client_status() == OPENDEV_UP
                ? "Connected. Pick a session or press + New, then type below."
                : "No server link. Tap the server row or Connect, "
                  "or start `opencode serve --port 4096 --hostname 0.0.0.0`.";
            chat_text(card, hint, p->text_secondary, &lv_font_montserrat_14);
        }
        lv_obj_scroll_to_y(chat_scroll, LV_COORD_MAX, LV_ANIM_OFF);
    }

    /* Right: session info + diff */
    if (lbl_info) {
        int ai = opendev_client_active();
        const opendev_session_t *s =
            ai >= 0 ? opendev_client_session(ai) : NULL;
        if (s) {
            snprintf(buf, sizeof(buf), "ID: %s\nModel: %s\nState: %s",
                     s->id, s->model[0] ? s->model : "-",
                     s->busy ? "busy" : "idle");
            if (s->busy) {
                strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
                strncat(buf, LV_SYMBOL_BULLET, sizeof(buf) - strlen(buf) - 1);
            }
        } else {
            snprintf(buf, sizeof(buf), "No session selected");
        }
        lv_label_set_text(lbl_info, buf);
    }
    if (lbl_diff) lv_label_set_text(lbl_diff, opendev_client_diff_text());

    /* Permission modal */
    opendev_permission_t perm;
    if (opendev_client_permission_pending(&perm)) {
        if (modal_perm) {
            snprintf(buf, sizeof(buf), "Approve tool?\n%s", perm.text);
            lv_label_set_text(lbl_perm_desc, buf);
            lv_obj_remove_flag(modal_perm, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(modal_perm);
        }
    } else if (modal_perm) {
        lv_obj_add_flag(modal_perm, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------------------------------------------------------- events */
static void sess_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    opendev_client_select(idx);
    refresh_all();
}

static void new_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    opendev_client_new_session();
    refresh_all();
}

static void connect_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    opendev_client_reconnect();
    refresh_all();
}

static void send_current(void)
{
    if (!ta) return;
    const char *t = lv_textarea_get_text(ta);
    if (t && *t) {
        opendev_client_send(t);
        lv_textarea_set_text(ta, "");
        refresh_all();
    }
}

static void send_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    send_current();
}

static void perm_btn_cb(lv_event_t *e)
{
    int ans = (int)(intptr_t)lv_event_get_user_data(e);
    /* ponytail: 1 = once (Y), 2 = always (A), 0 = reject (N) */
    opendev_client_answer_permission(ans > 0, ans == 2);
    refresh_all();
}

static void diff_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    opendev_client_fetch_diff();
    refresh_all();
}

static void srv_focus_paint(void)
{
    const devos_palette_t *p = devos_theme_get();
    if (ta_host) {
        lv_obj_set_style_border_color(ta_host,
            (srv_focus == ta_host) ? p->accent_primary : p->surface_border, 0);
    }
    if (ta_port) {
        lv_obj_set_style_border_color(ta_port,
            (srv_focus == ta_port) ? p->accent_primary : p->surface_border, 0);
    }
}

static void srv_row_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (modal_srv && ta_host && ta_port) {
        char host[OPENDEV_HOST_MAX];
        int port = 0;
        opendev_client_get_config(host, sizeof(host), &port, NULL, NULL, 0);
        lv_textarea_set_text(ta_host, host);
        char pb[16];
        snprintf(pb, sizeof(pb), "%d", port);
        lv_textarea_set_text(ta_port, pb);
        srv_focus = ta_host;
        srv_focus_paint();
        lv_obj_remove_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(modal_srv);
    }
}

static void srv_save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    bool ok = false;
    if (ta_host && ta_port) {
        const char *h = lv_textarea_get_text(ta_host);
        int port = atoi(lv_textarea_get_text(ta_port));
        /* ponytail: chamber pairing pasted into the host field just works */
        if (h && strncmp(h, "openchamber://", 14) == 0) {
            ok = opendev_client_pair(h) == 0;
        } else if (h && *h && port > 0) {
            ok = opendev_client_set_server(h, port) == 0;
        }
    }
    /* ponytail: invalid input keeps the modal open instead of vanishing */
    if (ok && modal_srv) lv_obj_add_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
    refresh_all();
}

static void srv_cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (modal_srv) lv_obj_add_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
}

static void poll_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!screen || lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN)) return;
    opendev_client_poll();
    uint32_t g = opendev_client_generation();
    if (g != s_seen_gen) {
        s_seen_gen = g;
        refresh_all();
    }
}

/* ----------------------------------------------------------------- input */
static bool modal_open(void)
{
    return (modal_perm && !lv_obj_has_flag(modal_perm, LV_OBJ_FLAG_HIDDEN)) ||
           (modal_srv && !lv_obj_has_flag(modal_srv, LV_OBJ_FLAG_HIDDEN));
}

static bool opendev_handle_key(uint32_t key, uint8_t modifiers)
{
    if (modifiers & DEVOS_MOD_FN) {
        if (key == 'f' || key == 'F') {
            devos_agent_viewport_toggle_focus(viewport);
            return true;
        } else if (key == '[') {
            devos_agent_viewport_toggle_left(viewport);
            return true;
        } else if (key == ']') {
            devos_agent_viewport_toggle_right(viewport);
            return true;
        }
    }

    /* Permission modal: Y once, A always, N reject */
    if (modal_perm && !lv_obj_has_flag(modal_perm, LV_OBJ_FLAG_HIDDEN)) {
        if (key == 'y' || key == 'Y') {
            opendev_client_answer_permission(true, false);
            refresh_all();
            return true;
        }
        if (key == 'a' || key == 'A') {
            opendev_client_answer_permission(true, true);
            refresh_all();
            return true;
        }
        if (key == 'n' || key == 'N') {
            opendev_client_answer_permission(false, false);
            refresh_all();
            return true;
        }
        if (key == LV_KEY_ESC) {
            opendev_client_answer_permission(false, false);
            refresh_all();
            return true;
        }
        return true;
    }

    /* Server modal: Tab cycles host/port, Enter saves */
    if (modal_srv && !lv_obj_has_flag(modal_srv, LV_OBJ_FLAG_HIDDEN)) {
        if (!srv_focus) srv_focus = ta_host;
        if (key == LV_KEY_ESC) {
            lv_obj_add_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
            return true;
        }
        if (key == '\r' || key == '\n') {
            srv_save_cb(NULL);
            return true;
        }
        if (key == '\t') {
            srv_focus = (srv_focus == ta_host) ? ta_port : ta_host;
            srv_focus_paint();
            return true;
        }
        if (key == '\b' || key == 0x7F) {
            if (srv_focus) lv_textarea_delete_char(srv_focus);
            return true;
        }
        if (key == LV_KEY_LEFT) {
            if (srv_focus) lv_textarea_cursor_left(srv_focus);
            return true;
        }
        if (key == LV_KEY_RIGHT) {
            if (srv_focus) lv_textarea_cursor_right(srv_focus);
            return true;
        }
        if (key >= 32 && key <= 126) {
            if (srv_focus) lv_textarea_add_char(srv_focus, (char)key);
            return true;
        }
        return true;
    }

    /* Main input (Ctrl+C aborts the running turn) */
    if (key == 0x03) {
        opendev_client_abort();
        refresh_all();
        return true;
    }
    if (!ta || modal_open()) return false;
    if (key == '\b' || key == 0x7F) {
        lv_textarea_delete_char(ta);
        return true;
    }
    if (key == LV_KEY_LEFT) {
        lv_textarea_cursor_left(ta);
        return true;
    }
    if (key == LV_KEY_RIGHT) {
        lv_textarea_cursor_right(ta);
        return true;
    }
    if (key == '\r' || key == '\n') {
        send_current();
        return true;
    }
    if (key >= 32 && key <= 126) {
        lv_textarea_add_char(ta, (char)key);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ init */
static void opendev_init(void)
{
    const devos_palette_t *p = devos_theme_get();
    opendev_client_init();

    screen = lv_obj_create(lv_screen_active());
    app_descriptor.screen = screen;
    lv_obj_set_size(screen, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(screen, 0, DEVOS_TOP_BAR_HEIGHT);
    lv_obj_set_style_bg_color(screen, p->bg, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);

    viewport = devos_agent_viewport_create(screen);

    lv_obj_t *left_panel = devos_agent_viewport_get_left(viewport);
    lv_obj_t *center_panel = devos_agent_viewport_get_center(viewport);
    lv_obj_t *right_panel = devos_agent_viewport_get_right(viewport);

    /* Left: title + status card */
    lbl_title = lv_label_create(left_panel);
    lv_label_set_text(lbl_title, "OPENCODE / OPENCHAMBER");
    lv_obj_set_pos(lbl_title, 4, 4);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_title, p->text_secondary, 0);

    status_card = lv_obj_create(left_panel);
    lv_obj_set_size(status_card, DEVOS_PANE_LEFT_WIDTH - 28, 138);
    lv_obj_set_pos(status_card, 4, 28);
    lv_obj_set_style_bg_color(status_card, p->surface, 0);
    lv_obj_set_style_border_color(status_card, p->surface_border, 0);
    lv_obj_set_style_border_width(status_card, 1, 0);
    lv_obj_set_style_radius(status_card, 4, 0);
    lv_obj_set_style_pad_all(status_card, 8, 0);
    lv_obj_clear_flag(status_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(status_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(status_card, srv_row_cb, LV_EVENT_CLICKED, NULL);

    lbl_target = lv_label_create(status_card);
    lv_label_set_text(lbl_target, "...");
    lv_obj_set_pos(lbl_target, 0, 0);
    lv_obj_set_style_text_font(lbl_target, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_target, p->text_primary, 0);

    lbl_status = lv_label_create(status_card);
    lv_label_set_text(lbl_status, "Offline");
    lv_obj_set_pos(lbl_status, 0, 22);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_status, p->text_secondary, 0);

    btn_connect = lv_button_create(status_card);
    lv_obj_set_size(btn_connect, 104, 28);
    lv_obj_set_pos(btn_connect, 0, 48);
    lv_obj_set_style_bg_color(btn_connect, p->surface_active, 0);
    lv_obj_set_style_border_color(btn_connect, p->accent_primary, 0);
    lv_obj_set_style_border_width(btn_connect, 1, 0);
    lv_obj_set_style_radius(btn_connect, 4, 0);
    lv_obj_add_event_cb(btn_connect, connect_btn_cb, LV_EVENT_CLICKED, NULL);

    lbl_connect = lv_label_create(btn_connect);
    lv_label_set_text(lbl_connect, LV_SYMBOL_REFRESH " Link");
    lv_obj_center(lbl_connect);
    lv_obj_set_style_text_font(lbl_connect, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_connect, p->accent_primary, 0);

    btn_new = lv_button_create(status_card);
    lv_obj_set_size(btn_new, 104, 28);
    lv_obj_set_pos(btn_new, 112, 48);
    lv_obj_set_style_bg_color(btn_new, p->surface, 0);
    lv_obj_set_style_border_color(btn_new, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_new, 1, 0);
    lv_obj_set_style_radius(btn_new, 4, 0);
    lv_obj_add_event_cb(btn_new, new_btn_cb, LV_EVENT_CLICKED, NULL);

    lbl_new = lv_label_create(btn_new);
    lv_label_set_text(lbl_new, LV_SYMBOL_PLUS " New");
    lv_obj_center(lbl_new);
    lv_obj_set_style_text_font(lbl_new, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_new, p->text_primary, 0);

    /* Left: session buttons (pool) */
    for (int i = 0; i < OPENDEV_SESS_BTNS; i++) {
        sess_btns[i] = lv_button_create(left_panel);
        lv_obj_set_size(sess_btns[i], DEVOS_PANE_LEFT_WIDTH - 28, 46);
        lv_obj_set_pos(sess_btns[i], 4, 178 + i * 52);
        lv_obj_set_style_bg_color(sess_btns[i], p->surface, 0);
        lv_obj_set_style_border_color(sess_btns[i], p->surface_border, 0);
        lv_obj_set_style_border_width(sess_btns[i], 1, 0);
        lv_obj_set_style_radius(sess_btns[i], 4, 0);
        lv_obj_add_flag(sess_btns[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(sess_btns[i], sess_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        sess_lbls[i] = lv_label_create(sess_btns[i]);
        lv_obj_align(sess_lbls[i], LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_size(sess_lbls[i], DEVOS_PANE_LEFT_WIDTH - 44, 40);
        lv_label_set_long_mode(sess_lbls[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(sess_lbls[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(sess_lbls[i], p->text_primary, 0);
    }

    /* Center: chat stream */
    chat_scroll = lv_obj_create(center_panel);
    lv_obj_set_size(chat_scroll, lv_pct(100), DEVOS_CONTENT_HEIGHT - 56);
    lv_obj_set_pos(chat_scroll, 0, 0);
    lv_obj_set_style_bg_color(chat_scroll, p->bg, 0);
    lv_obj_set_style_border_width(chat_scroll, 0, 0);
    lv_obj_set_style_pad_all(chat_scroll, 8, 0);

    /* Center: input bar */
    input_bar = lv_obj_create(center_panel);
    lv_obj_set_size(input_bar, lv_pct(100), 50);
    lv_obj_set_pos(input_bar, 0, DEVOS_CONTENT_HEIGHT - 50);
    lv_obj_set_style_bg_color(input_bar, p->surface, 0);
    lv_obj_set_style_border_color(input_bar, p->surface_border, 0);
    lv_obj_set_style_border_width(input_bar, 1, 0);
    lv_obj_set_style_border_side(input_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(input_bar, 0, 0);
    lv_obj_set_style_pad_all(input_bar, 6, 0);
    lv_obj_clear_flag(input_bar, LV_OBJ_FLAG_SCROLLABLE);

    ta = lv_textarea_create(input_bar);
    lv_textarea_set_placeholder_text(ta, "Prompt (Enter sends, Ctrl+C stops)...");
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_size(ta, lv_pct(82), 38);
    lv_obj_align(ta, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(ta, p->bg_alt, 0);
    lv_obj_set_style_border_color(ta, p->surface_border, 0);
    lv_obj_set_style_text_color(ta, p->text_primary, 0);

    btn_send = lv_button_create(input_bar);
    lv_obj_set_size(btn_send, lv_pct(16), 38);
    lv_obj_align(btn_send, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_send, p->accent_primary, 0);
    lv_obj_add_event_cb(btn_send, send_btn_cb, LV_EVENT_CLICKED, NULL);

    lbl_send = lv_label_create(btn_send);
    lv_label_set_text(lbl_send, "Send " LV_SYMBOL_RIGHT);
    lv_obj_center(lbl_send);
    lv_obj_set_style_text_color(lbl_send,
        devos_theme_is_dark() ? lv_color_black() : lv_color_white(), 0);

    /* Right: session info + diffs */
    lbl_rg = lv_label_create(right_panel);
    lv_label_set_text(lbl_rg, "SESSION (Fn+])");
    lv_obj_set_pos(lbl_rg, 4, 4);
    lv_obj_set_style_text_font(lbl_rg, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_rg, p->text_secondary, 0);

    lbl_info = lv_label_create(right_panel);
    lv_label_set_text(lbl_info, "No session selected");
    lv_obj_set_pos(lbl_info, 4, 28);
    lv_obj_set_size(lbl_info, DEVOS_PANE_RIGHT_WIDTH - 36, 90);
    lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl_info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_info, p->text_primary, 0);

    btn_diff = lv_button_create(right_panel);
    lv_obj_set_size(btn_diff, DEVOS_PANE_RIGHT_WIDTH - 28, 32);
    lv_obj_set_pos(btn_diff, 4, 124);
    lv_obj_set_style_bg_color(btn_diff, p->surface_active, 0);
    lv_obj_set_style_border_color(btn_diff, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_diff, 1, 0);
    lv_obj_set_style_radius(btn_diff, 4, 0);
    lv_obj_add_event_cb(btn_diff, diff_btn_cb, LV_EVENT_CLICKED, NULL);

    lbl_diff_btn = lv_label_create(btn_diff);
    lv_label_set_text(lbl_diff_btn, LV_SYMBOL_REFRESH " Diffs");
    lv_obj_center(lbl_diff_btn);
    lv_obj_set_style_text_font(lbl_diff_btn, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_diff_btn, p->text_primary, 0);

    lbl_diff = lv_label_create(right_panel);
    lv_label_set_text(lbl_diff, "");
    lv_obj_set_pos(lbl_diff, 4, 164);
    lv_obj_set_size(lbl_diff, DEVOS_PANE_RIGHT_WIDTH - 36, 380);
    lv_label_set_long_mode(lbl_diff, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl_diff, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_diff, p->text_secondary, 0);

    /* Permission modal */
    modal_perm = lv_obj_create(screen);
    lv_obj_set_size(modal_perm, 460, 190);
    lv_obj_align(modal_perm, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(modal_perm, p->surface, 0);
    lv_obj_set_style_border_color(modal_perm, p->accent_warning, 0);
    lv_obj_set_style_border_width(modal_perm, 2, 0);
    lv_obj_set_style_radius(modal_perm, 8, 0);
    lv_obj_set_style_pad_all(modal_perm, 16, 0);
    lv_obj_add_flag(modal_perm, LV_OBJ_FLAG_HIDDEN);

    lbl_perm_title = lv_label_create(modal_perm);
    lv_label_set_text(lbl_perm_title, LV_SYMBOL_WARNING " Tool Permission");
    lv_obj_set_style_text_font(lbl_perm_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_perm_title, p->accent_warning, 0);

    lbl_perm_desc = lv_label_create(modal_perm);
    lv_label_set_text(lbl_perm_desc, "");
    lv_obj_set_pos(lbl_perm_desc, 0, 36);
    lv_obj_set_size(lbl_perm_desc, 428, 60);
    lv_label_set_long_mode(lbl_perm_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl_perm_desc, p->text_primary, 0);

    const char *ynames[3] = {"[Y] Once", "[N] Deny", "[A] Always"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = perm_btns[i] = lv_button_create(modal_perm);
        lv_obj_set_size(b, 130, 36);
        lv_obj_set_pos(b, i * 142, 110);
        lv_obj_set_style_bg_color(b, i == 1 ? p->accent_danger
                                  : i == 2   ? p->surface_active
                                             : p->accent_secondary,
                                  0);
        lv_obj_add_event_cb(b, perm_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(i == 0 ? 1 : i == 1 ? 0 : 2));
        lv_obj_t *l = perm_lbls[i] = lv_label_create(b);
        lv_label_set_text(l, ynames[i]);
        lv_obj_center(l);
        /* ponytail: Deny needs white on red; accent buttons track theme */
        lv_obj_set_style_text_color(l,
            i == 1 ? lv_color_white()
                   : i == 2 ? p->text_primary
                            : devos_theme_is_dark() ? lv_color_black()
                                                    : lv_color_white(),
            0);
    }

    /* Server modal */
    modal_srv = lv_obj_create(screen);
    lv_obj_set_size(modal_srv, 480, 210);
    lv_obj_align(modal_srv, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(modal_srv, p->surface, 0);
    lv_obj_set_style_border_color(modal_srv, p->accent_primary, 0);
    lv_obj_set_style_border_width(modal_srv, 2, 0);
    lv_obj_set_style_radius(modal_srv, 8, 0);
    lv_obj_set_style_pad_all(modal_srv, 16, 0);
    lv_obj_clear_flag(modal_srv, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);

    lbl_srv_title = lv_label_create(modal_srv);
    lv_label_set_text(lbl_srv_title, LV_SYMBOL_SETTINGS " Server (or paste openchamber:// URI)");
    lv_obj_set_style_text_font(lbl_srv_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_srv_title, p->accent_primary, 0);

    ta_host = lv_textarea_create(modal_srv);
    lv_textarea_set_placeholder_text(ta_host, "host or openchamber:// URI");
    lv_textarea_set_one_line(ta_host, true);
    lv_obj_set_size(ta_host, 448, 36);
    lv_obj_set_pos(ta_host, 0, 40);
    lv_obj_set_style_bg_color(ta_host, p->code_bg, 0);
    lv_obj_set_style_text_color(ta_host, p->text_primary, 0);
    lv_obj_set_style_border_color(ta_host, p->surface_border, 0);
    lv_obj_set_style_border_width(ta_host, 1, 0);
    lv_obj_set_style_radius(ta_host, 4, 0);
    lv_obj_set_style_pad_all(ta_host, 8, 0);

    ta_port = lv_textarea_create(modal_srv);
    lv_textarea_set_placeholder_text(ta_port, "4096");
    lv_textarea_set_one_line(ta_port, true);
    lv_obj_set_size(ta_port, 140, 36);
    lv_obj_set_pos(ta_port, 0, 84);
    lv_obj_set_style_bg_color(ta_port, p->code_bg, 0);
    lv_obj_set_style_text_color(ta_port, p->text_primary, 0);
    lv_obj_set_style_border_color(ta_port, p->surface_border, 0);
    lv_obj_set_style_border_width(ta_port, 1, 0);
    lv_obj_set_style_radius(ta_port, 4, 0);
    lv_obj_set_style_pad_all(ta_port, 8, 0);

    lv_obj_t *btn_srv_save = lv_button_create(modal_srv);
    lv_obj_set_size(btn_srv_save, 130, 34);
    lv_obj_set_pos(btn_srv_save, 190, 132);
    lv_obj_set_style_bg_color(btn_srv_save, p->accent_primary, 0);
    lv_obj_set_style_radius(btn_srv_save, 4, 0);
    lv_obj_add_event_cb(btn_srv_save, srv_save_cb, LV_EVENT_CLICKED, NULL);
    lbl_srv_save = lv_label_create(btn_srv_save);
    lv_label_set_text(lbl_srv_save, LV_SYMBOL_OK " Save");
    lv_obj_center(lbl_srv_save);
    lv_obj_set_style_text_color(lbl_srv_save,
        devos_theme_is_dark() ? lv_color_black() : lv_color_white(), 0);

    lv_obj_t *btn_srv_cancel = lv_button_create(modal_srv);
    lv_obj_set_size(btn_srv_cancel, 110, 34);
    lv_obj_set_pos(btn_srv_cancel, 330, 132);
    lv_obj_set_style_bg_color(btn_srv_cancel, p->surface, 0);
    lv_obj_set_style_border_color(btn_srv_cancel, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_srv_cancel, 1, 0);
    lv_obj_set_style_radius(btn_srv_cancel, 4, 0);
    lv_obj_add_event_cb(btn_srv_cancel, srv_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_srv_cancel = lv_label_create(btn_srv_cancel);
    lv_label_set_text(lbl_srv_cancel, "Cancel");
    lv_obj_center(lbl_srv_cancel);
    lv_obj_set_style_text_color(lbl_srv_cancel, p->text_primary, 0);

    devos_theme_add_listener(apply_theme, NULL);
    poll_timer = lv_timer_create(poll_cb, 100, NULL);
    refresh_all();
}

static void opendev_show(void)
{
    if (opendev_client_status() == OPENDEV_UP) {
        opendev_client_refresh_sessions();
    }
    refresh_all();
}

static void opendev_hide(void)
{
    if (modal_perm) lv_obj_add_flag(modal_perm, LV_OBJ_FLAG_HIDDEN);
    if (modal_srv) lv_obj_add_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
}

devos_app_descriptor_t *app_opendev_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_OPENDEV;
    app_descriptor.name = "OpenDev";
    app_descriptor.title = "OpenDev / OpenChamber";
    app_descriptor.subtitle = "Remote AI Coding Agent";
    app_descriptor.screen = screen;
    app_descriptor.init = opendev_init;
    app_descriptor.show = opendev_show;
    app_descriptor.hide = opendev_hide;
    app_descriptor.handle_key = opendev_handle_key;

    return &app_descriptor;
}
