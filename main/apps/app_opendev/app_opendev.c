#include "app_opendev.h"
#include "devos_config.h"
#include "devos_theme.h"
#include "devos_agent_viewport.h"
#include "devos_mdview.h"
#include "devos_codeview.h"
#include "opendev_client.h"
#include "app_editor.h"
#include "bsp_tab5_camera.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_nimbus_mono_14);

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
static lv_obj_t *btn_note = NULL;
static lv_obj_t *lbl_note = NULL;
static lv_obj_t *btn_send = NULL;
static lv_obj_t *lbl_send = NULL;
static bool think_expanded = true;

/* Right: session info + diffs */
static lv_obj_t *lbl_rg = NULL;
static lv_obj_t *lbl_info = NULL;
static lv_obj_t *btn_diff = NULL;
static lv_obj_t *lbl_diff_btn = NULL;
static lv_obj_t *btn_plan = NULL;
static lv_obj_t *lbl_plan_btn = NULL;
static lv_obj_t *btn_save_diff = NULL;
static lv_obj_t *lbl_save_diff_btn = NULL;
static lv_obj_t *diff_scroll = NULL;

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
static lv_obj_t *ta_token = NULL;
static lv_obj_t *srv_focus = NULL;
static lv_obj_t *lbl_srv_save = NULL;
static lv_obj_t *btn_scan_qr = NULL;
static lv_obj_t *lbl_scan_qr = NULL;

/* Camera QR Scanner modal */
static lv_obj_t *modal_cam = NULL;
static lv_obj_t *cam_canvas = NULL;
static lv_obj_t *lbl_cam_title = NULL;
static lv_obj_t *lbl_cam_hint = NULL;
static lv_obj_t *btn_cam_cancel = NULL;
static lv_obj_t *lbl_cam_cancel = NULL;
static lv_obj_t *btn_cam_test = NULL;
static lv_obj_t *lbl_cam_test = NULL;
static char s_detected_qr[OPENDEV_HOST_MAX + OPENDEV_TOKEN_MAX + 64] = "";
static volatile bool s_qr_ready = false;

static uint32_t s_seen_gen = 0;
static lv_timer_t *poll_timer = NULL;

static void refresh_all(void);
static void apply_theme(const devos_palette_t *p, void *user_data);
static void srv_focus_paint(void);
static void srv_open(bool login);
static bool s_login_prompted = false;       /* sign-in dialog shown for this prompt */

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
    if (btn_note) {
        lv_obj_set_style_bg_color(btn_note, p->surface, 0);
        lv_obj_set_style_border_color(btn_note, p->surface_border, 0);
    }
    if (lbl_note) lv_obj_set_style_text_color(lbl_note, p->text_primary, 0);
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
    if (btn_plan) {
        lv_obj_set_style_bg_color(btn_plan, p->surface_active, 0);
        lv_obj_set_style_border_color(btn_plan, p->surface_border, 0);
    }
    if (lbl_plan_btn) {
        lv_obj_set_style_text_color(lbl_plan_btn, p->text_primary, 0);
    }
    if (btn_save_diff) {
        lv_obj_set_style_bg_color(btn_save_diff, p->surface_active, 0);
        lv_obj_set_style_border_color(btn_save_diff, p->surface_border, 0);
    }
    if (lbl_save_diff_btn) {
        lv_obj_set_style_text_color(lbl_save_diff_btn, p->text_primary, 0);
    }
    if (diff_scroll) {
        lv_obj_set_style_bg_color(diff_scroll, p->code_bg, 0);
        lv_obj_set_style_border_color(diff_scroll, p->surface_border, 0);
    }

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
    if (ta_token) {
        lv_obj_set_style_bg_color(ta_token, p->code_bg, 0);
        lv_obj_set_style_text_color(ta_token, p->text_primary, 0);
        lv_obj_set_style_border_color(ta_token, p->surface_border, 0);
    }
    if (btn_scan_qr) {
        lv_obj_set_style_bg_color(btn_scan_qr, p->surface_active, 0);
        lv_obj_set_style_border_color(btn_scan_qr, p->surface_border, 0);
    }
    if (lbl_scan_qr) lv_obj_set_style_text_color(lbl_scan_qr, p->text_primary, 0);
    if (modal_cam) {
        lv_obj_set_style_bg_color(modal_cam, p->surface, 0);
        lv_obj_set_style_border_color(modal_cam, p->accent_primary, 0);
    }
    if (lbl_cam_title) lv_obj_set_style_text_color(lbl_cam_title, p->accent_primary, 0);
    if (lbl_cam_hint) lv_obj_set_style_text_color(lbl_cam_hint, p->text_secondary, 0);
    if (cam_canvas) lv_obj_set_style_border_color(cam_canvas, p->surface_border, 0);
    if (btn_cam_cancel) {
        lv_obj_set_style_bg_color(btn_cam_cancel, p->surface, 0);
        lv_obj_set_style_border_color(btn_cam_cancel, p->surface_border, 0);
    }
    if (lbl_cam_cancel) lv_obj_set_style_text_color(lbl_cam_cancel, p->text_primary, 0);
    if (btn_cam_test) lv_obj_set_style_bg_color(btn_cam_test, p->accent_primary, 0);
    if (lbl_cam_test) lv_obj_set_style_text_color(lbl_cam_test, on_accent, 0);

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
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
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

static devos_codeview_t s_cv_pane, s_cv_full;
static lv_obj_t *modal_diff = NULL, *lbl_diff_title = NULL;
static EXT_RAM_BSS_ATTR char s_diff_display[OPENDEV_DIFF_MAX + 2048];

/* Summary of changed files on top of the unified diff. */
static const char *diff_display_text(void)
{
    const char *dt = opendev_client_diff_text();
    int nf = opendev_client_diff_file_count();
    if (!dt || !*dt) return "No diff yet: press Diff.";
    if (nf == 0) return dt;
    int add = 0, del = 0;
    for (int i = 0; i < nf; i++) {
        add += opendev_client_diff_file(i)->additions;
        del += opendev_client_diff_file(i)->deletions;
    }
    int n = snprintf(s_diff_display, sizeof(s_diff_display), "%d file%s changed, +%d -%d\n", nf, nf == 1 ? "" : "s",
                     add, del);
    for (int i = 0; i < nf && n < (int)sizeof(s_diff_display) - 200; i++) {
        const opendev_diff_file_t *f = opendev_client_diff_file(i);
        n += snprintf(s_diff_display + n, sizeof(s_diff_display) - (size_t)n, "  +%-4d -%-4d %s\n", f->additions,
                      f->deletions, f->path);
    }
    snprintf(s_diff_display + n, sizeof(s_diff_display) - (size_t)n, "\n%s", dt);
    return s_diff_display;
}

static void diff_full_close(void)
{
    if (modal_diff) lv_obj_add_flag(modal_diff, LV_OBJ_FLAG_HIDDEN);
}

static void diff_full_open(void)
{
    if (!modal_diff) return;
    int nf = opendev_client_diff_file_count();
    char t[80];
    snprintf(t, sizeof(t), "Diff  -  %d file%s  (Esc to close, arrows scroll)", nf, nf == 1 ? "" : "s");
    lv_label_set_text(lbl_diff_title, t);
    lv_obj_remove_flag(modal_diff, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(modal_diff);
    lv_obj_update_layout(modal_diff);
    devos_codeview_set(&s_cv_full, diff_display_text());
}

static void diff_pane_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    diff_full_open();
}

static void diff_close_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    diff_full_close();
}

static void build_diff_modal(const devos_palette_t *p)
{
    modal_diff = lv_obj_create(screen);
    lv_obj_set_size(modal_diff, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(modal_diff, 0, 0);
    lv_obj_set_style_bg_color(modal_diff, p->bg, 0);
    lv_obj_set_style_radius(modal_diff, 0, 0);
    lv_obj_set_style_border_width(modal_diff, 0, 0);
    lv_obj_set_style_pad_all(modal_diff, 8, 0);
    lv_obj_remove_flag(modal_diff, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal_diff, LV_OBJ_FLAG_HIDDEN);
    lbl_diff_title = lv_label_create(modal_diff);
    lv_obj_set_pos(lbl_diff_title, 4, 6);
    lv_obj_set_style_text_font(lbl_diff_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_diff_title, p->text_primary, 0);
    lv_obj_t *b = lv_button_create(modal_diff);
    lv_obj_set_size(b, 110, 32);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(b, p->surface_active, 0);
    lv_obj_add_event_cb(b, diff_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_CLOSE " Close");
    lv_obj_center(l);
    lv_obj_set_style_text_color(l, p->text_primary, 0);
    lv_obj_t *sc = lv_obj_create(modal_diff);
    lv_obj_set_pos(sc, 0, 40);
    lv_obj_set_size(sc, DEVOS_SCREEN_WIDTH - 16, DEVOS_CONTENT_HEIGHT - 56);
    lv_obj_set_style_bg_color(sc, p->code_bg, 0);
    lv_obj_set_style_border_color(sc, p->surface_border, 0);
    lv_obj_set_style_border_width(sc, 1, 0);
    lv_obj_set_style_radius(sc, 4, 0);
    devos_codeview_create(&s_cv_full, sc);
}

/* ---- chat ---- */
static int s_rendered_count = -1;
static uint32_t s_rendered_revs[OPENDEV_MAX_BLOCKS];
static lv_obj_t *s_last_card = NULL;        /* card of the last block (non-thinking) */
static uint32_t s_seen_blocks_gen = UINT32_MAX, s_seen_diff_gen = UINT32_MAX;

static void fill_block_card(lv_obj_t *card, const opendev_block_t *b, const devos_palette_t *p)
{
    lv_obj_clean(card);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    if (b->kind == OPENDEV_KIND_TOOL) {
        chat_text(card, b->text, p->accent_secondary, &lv_font_nimbus_mono_14);
    } else if (b->role == OPENDEV_ROLE_USER) {
        chat_text(card, b->text, p->text_primary, &lv_font_montserrat_14);
    } else {
        /* agent output renders through the shared markdown engine; absolute
         * blocks need a measured inner container + explicit card height */
        lv_obj_update_layout(card);
        lv_obj_t *inner = lv_obj_create(card);
        lv_obj_set_size(inner, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(inner, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(inner, 0, 0);
        lv_obj_set_style_pad_all(inner, 0, 0);
        lv_obj_remove_flag(inner, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_update_layout(inner);
        int endy = devos_md_render(inner, b->text);
        if (endy <= 0) {
            lv_obj_delete(inner);
            chat_text(card, b->text, p->text_primary, &lv_font_montserrat_14);
        } else {
            lv_obj_set_height(inner, endy);
            lv_obj_set_height(card, endy + 20);
        }
    }
}

static lv_obj_t *new_block_card(const opendev_block_t *b, const devos_palette_t *p)
{
    lv_obj_t *card;
    if (b->kind == OPENDEV_KIND_TOOL) {
        card = chat_card(p, p->tool_card_border);
        lv_obj_set_style_bg_color(card, p->tool_card_bg, 0);
    } else if (b->role == OPENDEV_ROLE_USER) {
        card = chat_card(p, p->accent_primary);
        lv_obj_set_style_bg_color(card, p->surface_active, 0);
    } else {
        card = chat_card(p, p->surface_border);
    }
    fill_block_card(card, b, p);
    return card;
}

static const char *s_last_hint = NULL;

static const char *chat_hint(void)
{
    if (opendev_client_messages_loading() && opendev_client_active() >= 0) return "Loading messages...";
    switch (opendev_client_status()) {
    case OPENDEV_UP:
        return opendev_client_active() >= 0 ? "No messages yet: type below and press Enter."
                                            : "Connected. Pick a session or press + New, then type below.";
    case OPENDEV_LOGIN:
        return "Sign in to OpenChamber: tap the server box and enter its password.";
    case OPENDEV_CONNECTING:
        return "Connecting...";
    default:
        return "No server link. Tap the server box to set the URL: an OpenChamber URL such as "
               "https://host.tail1234.ts.net, or http://<computer>:4096 for `opencode serve --hostname 0.0.0.0`.";
    }
}

static void refresh_chat(bool force)
{
    if (!chat_scroll) return;
    const devos_palette_t *p = devos_theme_get();
    int nb = opendev_client_block_count();
    bool near_bottom = lv_obj_get_scroll_bottom(chat_scroll) < 48;

    /* Streaming: only the last block changed -> re-render just that card. */
    if (!force && nb > 0 && nb == s_rendered_count && s_last_card) {
        bool only_last = true;
        for (int i = 0; i < nb - 1 && only_last; i++) {
            if (opendev_client_block(i)->rev != s_rendered_revs[i]) only_last = false;
        }
        const opendev_block_t *last = opendev_client_block(nb - 1);
        if (only_last && last->kind != OPENDEV_KIND_THINK) {
            if (last->rev != s_rendered_revs[nb - 1]) {
                fill_block_card(s_last_card, last, p);
                s_rendered_revs[nb - 1] = last->rev;
                if (near_bottom) {
                    lv_obj_update_layout(chat_scroll);
                    lv_obj_scroll_to_y(chat_scroll, LV_COORD_MAX, LV_ANIM_OFF);
                }
            }
            return;
        }
    }

    bool first = s_rendered_count <= 0;
    lv_obj_clean(chat_scroll);
    s_last_card = NULL;
    for (int i = 0; i < nb;) {
        const opendev_block_t *b = opendev_client_block(i);
        if (b->kind == OPENDEV_KIND_THINK) {
            /* consecutive thinking blocks collapse into one card */
            lv_obj_t *box = chat_card(p, p->thinking_border);
            lv_obj_set_style_bg_color(box, p->thinking_bg, 0);
            lv_obj_add_event_cb(box, toggle_think_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_t *arrow = lv_label_create(box);
            lv_label_set_text(arrow, think_expanded ? LV_SYMBOL_DOWN " Thinking" : LV_SYMBOL_RIGHT " Thinking (tap to expand)");
            lv_obj_set_style_text_color(arrow, p->accent_primary, 0);
            lv_obj_set_style_text_font(arrow, &lv_font_montserrat_14, 0);
            lv_obj_t *body = NULL;
            if (think_expanded) {
                body = lv_obj_create(box);
                lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
                lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(body, 0, 0);
                lv_obj_set_style_pad_all(body, 0, 0);
                lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
                lv_obj_set_y(body, 22);
            }
            while (i < nb && opendev_client_block(i)->kind == OPENDEV_KIND_THINK) {
                if (body) chat_text(body, opendev_client_block(i)->text, p->text_secondary, &lv_font_montserrat_12);
                s_rendered_revs[i] = opendev_client_block(i)->rev;
                i++;
            }
            continue;
        }
        lv_obj_t *card = new_block_card(b, p);
        s_rendered_revs[i] = b->rev;
        if (i == nb - 1) s_last_card = card;
        i++;
    }
    if (nb == 0) {
        lv_obj_t *card = chat_card(p, p->surface_border);
        s_last_hint = chat_hint();
        chat_text(card, s_last_hint, p->text_secondary, &lv_font_montserrat_14);
    }
    s_rendered_count = nb;
    if (near_bottom || first) {
        lv_obj_update_layout(chat_scroll);
        lv_obj_scroll_to_y(chat_scroll, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

static void refresh_meta(void)
{
    const devos_palette_t *p = devos_theme_get();
    char host[OPENDEV_HOST_MAX];
    int port = 0;
    opendev_mode_t mode = OPENDEV_MODE_CODE;
    opendev_client_get_config(host, sizeof(host), &port, &mode, NULL, 0);

    char buf[200];
    char url[OPENDEV_URL_MAX];
    opendev_client_get_url(url, sizeof(url));
    const char *shown = strstr(url, "://") ? strstr(url, "://") + 3 : url;
    snprintf(buf, sizeof(buf), "%s%s", mode == OPENDEV_MODE_CHAMBER ? "OpenChamber  " : "", shown);
    LV_UNUSED(host);
    LV_UNUSED(port);
    if (lbl_target && strcmp(lv_label_get_text(lbl_target), buf) != 0) lv_label_set_text(lbl_target, buf);
    if (lbl_status) {
        snprintf(buf, sizeof(buf), "%s%s", opendev_client_status_text(), opendev_client_loading() ? "  (loading)" : "");
        if (strcmp(lv_label_get_text(lbl_status), buf) != 0) lv_label_set_text(lbl_status, buf);
        lv_obj_set_style_text_color(lbl_status,
                                    opendev_client_status() == OPENDEV_UP ? p->accent_secondary : p->text_secondary, 0);
    }

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
        snprintf(buf, sizeof(buf), "%s%s\n%s", s->busy ? LV_SYMBOL_BULLET " " : "", s->title[0] ? s->title : "(untitled)",
                 i == active ? "active" : "");
        if (strcmp(lv_label_get_text(sess_lbls[i]), buf) != 0) lv_label_set_text(sess_lbls[i], buf);
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

    if (lbl_info) {
        const opendev_session_t *s = active >= 0 ? opendev_client_session(active) : NULL;
        if (s) {
            snprintf(buf, sizeof(buf), "%s\nModel: %s\nState: %s%s", s->title[0] ? s->title : s->id,
                     s->model[0] ? s->model : "-", s->busy ? "working " : "idle", s->busy ? LV_SYMBOL_BULLET : "");
        } else {
            snprintf(buf, sizeof(buf), "No session selected");
        }
        if (strcmp(lv_label_get_text(lbl_info), buf) != 0) lv_label_set_text(lbl_info, buf);
    }

    if (opendev_client_needs_login()) {
        if (!s_login_prompted && modal_srv && lv_obj_has_flag(modal_srv, LV_OBJ_FLAG_HIDDEN)) srv_open(true);
        s_login_prompted = true;
    } else {
        s_login_prompted = false;
    }

    opendev_permission_t perm;
    if (opendev_client_permission_pending(&perm)) {
        if (modal_perm) {
            snprintf(buf, sizeof(buf), "The agent wants to run:\n%s", perm.text);
            lv_label_set_text(lbl_perm_desc, buf);
            if (lv_obj_has_flag(modal_perm, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_remove_flag(modal_perm, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(modal_perm);
            }
        }
    } else if (modal_perm) {
        lv_obj_add_flag(modal_perm, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_diff(bool force)
{
    uint32_t g = opendev_client_diff_generation();
    if (!force && g == s_seen_diff_gen) return;
    s_seen_diff_gen = g;
    devos_codeview_set(&s_cv_pane, diff_display_text());
    if (modal_diff && !lv_obj_has_flag(modal_diff, LV_OBJ_FLAG_HIDDEN)) devos_codeview_set(&s_cv_full, diff_display_text());
}

static void refresh_all(void)
{
    if (!screen) return;
    refresh_meta();
    s_seen_blocks_gen = opendev_client_blocks_generation();
    refresh_chat(true);
    refresh_diff(true);
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

static void note_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    const char *fn = app_editor_get_active_filename();
    const char *txt = app_editor_get_active_text();
    if (!fn || !txt || !*txt) {
        if (ta) lv_textarea_set_placeholder_text(ta, "(No active note in Markdown Editor)");
        return;
    }
    if (ta) {
        char prefix[OPENDEV_BLOCK_MAX];
        snprintf(prefix, sizeof(prefix), "[Note: %s]\n%s\n\n", fn, txt);
        lv_textarea_set_text(ta, prefix);
        lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
    }
}

static void save_plan_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int active = opendev_client_active();
    if (active < 0) {
        if (lbl_info) lv_label_set_text(lbl_info, "(No active session to save)");
        return;
    }
    const opendev_session_t *s = opendev_client_session(active);
    if (!s) return;

    static char plan_buf[16384];
    int offset = snprintf(plan_buf, sizeof(plan_buf),
        "# Agent Plan: %s\n\n"
        "- **Session ID:** `%s`\n"
        "- **Model:** %s\n"
        "- **Status:** %s\n\n"
        "## Conversation & Plan Trace\n\n",
        s->title[0] ? s->title : s->id,
        s->id,
        s->model[0] ? s->model : "default",
        s->busy ? "Running" : "Completed");

    int nb = opendev_client_block_count();
    for (int i = 0; i < nb && offset + 128 < (int)sizeof(plan_buf); i++) {
        const opendev_block_t *b = opendev_client_block(i);
        if (!b) continue;
        if (b->role == OPENDEV_ROLE_USER) {
            offset += snprintf(plan_buf + offset, sizeof(plan_buf) - offset,
                               "### User Prompt\n%s\n\n", b->text);
        } else if (b->kind == OPENDEV_KIND_THINK) {
            offset += snprintf(plan_buf + offset, sizeof(plan_buf) - offset,
                               "> **Reasoning:**\n> %s\n\n", b->text);
        } else if (b->kind == OPENDEV_KIND_TOOL) {
            offset += snprintf(plan_buf + offset, sizeof(plan_buf) - offset,
                               "```\n[Tool Exec]: %s\n```\n\n", b->text);
        } else {
            offset += snprintf(plan_buf + offset, sizeof(plan_buf) - offset,
                               "%s\n\n", b->text);
        }
    }

    bool ok = app_editor_save_plan(s->title[0] ? s->title : s->id, plan_buf);
    if (lbl_info) {
        char status_msg[160];
        snprintf(status_msg, sizeof(status_msg), "ID: %s\nModel: %s\n[%s: /plans/%s.md]",
                 s->id, s->model[0] ? s->model : "-",
                 ok ? "Saved" : "Save failed",
                 s->title[0] ? s->title : s->id);
        lv_label_set_text(lbl_info, status_msg);
    }
}

static void save_diff_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int active = opendev_client_active();
    if (active < 0) return;
    const opendev_session_t *s = opendev_client_session(active);
    if (!s) return;
    const char *dt = opendev_client_diff_text();
    if (!dt || !*dt || dt[0] == '(' || strncmp(dt, "Loading", 7) == 0) {
        if (lbl_info) lv_label_set_text(lbl_info, "(No diff to save - fetch diff first)");
        return;
    }
    bool ok = app_editor_save_diff(s->title[0] ? s->title : s->id, dt);
    if (lbl_info) {
        char status_msg[160];
        snprintf(status_msg, sizeof(status_msg), "ID: %s\nModel: %s\n[%s: /diffs/%s.diff]",
                 s->id, s->model[0] ? s->model : "-",
                 ok ? "Saved" : "Save failed",
                 s->title[0] ? s->title : s->id);
        lv_label_set_text(lbl_info, status_msg);
    }
}

static void srv_focus_paint(void)
{
    const devos_palette_t *p = devos_theme_get();
    if (ta_host) {
        lv_obj_set_style_border_color(ta_host,
            (srv_focus == ta_host) ? p->accent_primary : p->surface_border, 0);
        lv_obj_set_style_border_width(ta_host, (srv_focus == ta_host) ? 2 : 1, 0);
    }
    if (ta_port) {
        lv_obj_set_style_border_color(ta_port,
            (srv_focus == ta_port) ? p->accent_primary : p->surface_border, 0);
        lv_obj_set_style_border_width(ta_port, (srv_focus == ta_port) ? 2 : 1, 0);
    }
    if (ta_token) {
        lv_obj_set_style_border_color(ta_token,
            (srv_focus == ta_token) ? p->accent_primary : p->surface_border, 0);
        lv_obj_set_style_border_width(ta_token, (srv_focus == ta_token) ? 2 : 1, 0);
    }
}

static void srv_ta_click_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    if (target == ta_host || target == ta_port || target == ta_token) {
        srv_focus = target;
        srv_focus_paint();
    }
}

static lv_obj_t *lbl_srv_hint = NULL;

static void srv_open(bool login)
{
    if (!modal_srv || !ta_host || !ta_token) return;
    char url[OPENDEV_URL_MAX] = "";
    opendev_client_get_url(url, sizeof(url));
    lv_textarea_set_text(ta_host, url);
    lv_textarea_set_text(ta_token, "");
    lv_label_set_text(lbl_srv_title, login ? LV_SYMBOL_WARNING " Sign in to OpenChamber"
                                           : LV_SYMBOL_SETTINGS " Server (OpenCode / OpenChamber)");
    if (login) {
        char t[200];
        snprintf(t, sizeof(t), "%s", opendev_client_status_text());
        lv_label_set_text(lbl_srv_hint, t);
    } else {
        lv_label_set_text(lbl_srv_hint, "e.g. https://dev-server.tail1234.ts.net (OpenChamber, needs its password)\n"
                                        "or http://10.0.0.5:4096 (opencode serve)");
    }
    srv_focus = login ? ta_token : ta_host;
    srv_focus_paint();
    lv_obj_remove_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(modal_srv);
}

static void srv_row_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    srv_open(opendev_client_needs_login());
}

static void srv_save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    const char *u = ta_host ? lv_textarea_get_text(ta_host) : "";
    const char *pw = ta_token ? lv_textarea_get_text(ta_token) : "";
    bool ok;
    if (u && strncmp(u, "openchamber://", 14) == 0) ok = opendev_client_pair(u) == 0;
    else ok = u && *u && opendev_client_connect_url(u, pw) == 0;
    if (ta_token) lv_textarea_set_text(ta_token, "");       /* never keep the password around */
    s_login_prompted = false;                               /* re-prompt if this sign-in fails */
    if (ok && modal_srv) lv_obj_add_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
    else if (lbl_srv_hint) lv_label_set_text(lbl_srv_hint, "Enter a URL like https://host.tail1234.ts.net or http://10.0.0.5:4096");
    refresh_all();
}

static void srv_cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (modal_srv) lv_obj_add_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
}

static void on_qr_detected(const char *text, void *ud)
{
    LV_UNUSED(ud);
    if (!text || !*text) return;
    snprintf(s_detected_qr, sizeof(s_detected_qr), "%s", text);
    s_detected_qr[sizeof(s_detected_qr) - 1] = '\0';
    s_qr_ready = true;
}

static void cam_cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    bsp_tab5_camera_stop_qr_scanner();
    bsp_tab5_camera_stop();
    if (modal_cam) lv_obj_add_flag(modal_cam, LV_OBJ_FLAG_HIDDEN);
}

static void cam_test_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    bsp_tab5_camera_inject_qr("openchamber://connect?host=100.77.11.92&port=8421&token=sec_camera_scanned");
}

static void scan_qr_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (modal_cam) {
        bsp_tab5_camera_start_qr_scanner(on_qr_detected, NULL);
        lv_obj_remove_flag(modal_cam, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(modal_cam);
    }
}

static void poll_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    /* Keep the event stream and REST results flowing in the background. */
    opendev_client_poll();
    if (!screen || lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN)) return;

    if (s_qr_ready) {
        s_qr_ready = false;
        bsp_tab5_camera_stop_qr_scanner();
        bsp_tab5_camera_stop();
        if (modal_cam) lv_obj_add_flag(modal_cam, LV_OBJ_FLAG_HIDDEN);
        if (modal_srv) lv_obj_add_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
        if (strncmp(s_detected_qr, "openchamber://", 14) == 0) {
            opendev_client_pair(s_detected_qr);
        } else {
            char h[OPENDEV_HOST_MAX] = "";
            int port = 4096;
            if (sscanf(s_detected_qr, "http://%127[^:]:%d", h, &port) == 2 ||
                sscanf(s_detected_qr, "%127[^:]:%d", h, &port) == 2) {
                opendev_client_set_server(h, port);
            } else if (s_detected_qr[0]) {
                opendev_client_set_server(s_detected_qr, 4096);
            }
        }
        refresh_all();
    }

    if (modal_cam && !lv_obj_has_flag(modal_cam, LV_OBJ_FLAG_HIDDEN)) {
        bsp_tab5_camera_qr_poll();
        int cw = 0, ch = 0;
        const uint8_t *frame = bsp_tab5_camera_get_frame(&cw, &ch);
        if (frame && cam_canvas) {
            lv_canvas_set_buffer(cam_canvas, (void *)frame, cw, ch, LV_COLOR_FORMAT_L8);
            lv_obj_invalidate(cam_canvas);
        }
    }

    /* UI refresh (rate-limited: streaming can bump the store many times a
     * second; only the last chat card is re-rendered while it streams). */
    static uint32_t last_ui = 0;
    uint32_t g = opendev_client_generation();
    if (g != s_seen_gen && lv_tick_elaps(last_ui) >= 150) {
        s_seen_gen = g;
        last_ui = lv_tick_get();
        refresh_meta();
        uint32_t bg = opendev_client_blocks_generation();
        if (bg != s_seen_blocks_gen) {
            s_seen_blocks_gen = bg;
            refresh_chat(false);
        } else if (opendev_client_block_count() == 0 && chat_hint() != s_last_hint) {
            refresh_chat(true);                 /* empty-chat hint follows the link state */
        }
        refresh_diff(false);
    }
}

/* ----------------------------------------------------------------- input */
static bool modal_open(void)
{
    return (modal_diff && !lv_obj_has_flag(modal_diff, LV_OBJ_FLAG_HIDDEN)) ||
           (modal_perm && !lv_obj_has_flag(modal_perm, LV_OBJ_FLAG_HIDDEN)) ||
           (modal_srv && !lv_obj_has_flag(modal_srv, LV_OBJ_FLAG_HIDDEN)) ||
           (modal_cam && !lv_obj_has_flag(modal_cam, LV_OBJ_FLAG_HIDDEN));
}

static bool opendev_handle_key(uint32_t key, uint8_t modifiers)
{
    /* Full-screen diff: Esc closes, arrows / PgUp / PgDn scroll */
    if (modal_diff && !lv_obj_has_flag(modal_diff, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_t *sc = s_cv_full.scroll;
        int page = lv_obj_get_height(sc) - 2 * DEVOS_CODEVIEW_LINE_H;
        if (key == LV_KEY_ESC || key == 'q' || key == 'Q') diff_full_close();
        else if (key == LV_KEY_DOWN) lv_obj_scroll_by_bounded(sc, 0, -3 * DEVOS_CODEVIEW_LINE_H, LV_ANIM_OFF);
        else if (key == LV_KEY_UP) lv_obj_scroll_by_bounded(sc, 0, 3 * DEVOS_CODEVIEW_LINE_H, LV_ANIM_OFF);
        else if (key == DEVOS_KEY_PGDN || key == ' ') lv_obj_scroll_by_bounded(sc, 0, -page, LV_ANIM_OFF);
        else if (key == DEVOS_KEY_PGUP) lv_obj_scroll_by_bounded(sc, 0, page, LV_ANIM_OFF);
        else if (key == LV_KEY_RIGHT) lv_obj_scroll_by_bounded(sc, -80, 0, LV_ANIM_OFF);
        else if (key == LV_KEY_LEFT) lv_obj_scroll_by_bounded(sc, 80, 0, LV_ANIM_OFF);
        return true;
    }
    if (modifiers & DEVOS_MOD_FN) {
        if (key == 'd' || key == 'D') {             /* Sym+D: fetch + show the diff */
            opendev_client_fetch_diff();
            diff_full_open();
            return true;
        }
        if (key == 'f' || key == 'F') {
            devos_agent_viewport_toggle_focus(viewport);
            return true;
        } else if (key == 'l' || key == 'L') {      /* Sym+L: left sidebar */
            devos_agent_viewport_toggle_left(viewport);
            return true;
        } else if (key == 'r' || key == 'R') {      /* Sym+R: right inspector */
            devos_agent_viewport_toggle_right(viewport);
            return true;
        } else if (key == 'n' || key == 'N') {
            note_btn_cb(NULL);
            return true;
        }
    }
    if (modifiers & DEVOS_MOD_CTRL) {
        if (key == 'b' || key == 'B') {
            if (modifiers & DEVOS_MOD_SHIFT) {
                devos_agent_viewport_toggle_right(viewport);
            } else {
                devos_agent_viewport_toggle_left(viewport);
            }
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
    }

    /* Camera modal: Esc cancels, Enter or S triggers test injection */
    if (modal_cam && !lv_obj_has_flag(modal_cam, LV_OBJ_FLAG_HIDDEN)) {
        if (key == LV_KEY_ESC) {
            cam_cancel_cb(NULL);
            return true;
        }
#ifndef ESP_PLATFORM
        if (key == '\r' || key == '\n' || key == 's' || key == 'S') {
            cam_test_cb(NULL);
            return true;
        }
#endif
        return true;
    }

    /* Server modal: Tab / Arrows cycle host/port/token, Enter saves */
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
        if (key == '\t' || key == LV_KEY_DOWN || key == LV_KEY_UP) {
            srv_focus = (srv_focus == ta_host) ? ta_token : ta_host;
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
    lv_obj_set_width(lbl_target, DEVOS_PANE_LEFT_WIDTH - 56);
    lv_label_set_long_mode(lbl_target, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl_target, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_target, p->text_primary, 0);

    lbl_status = lv_label_create(status_card);
    lv_label_set_text(lbl_status, "Offline");
    lv_obj_set_pos(lbl_status, 0, 18);
    lv_obj_set_width(lbl_status, DEVOS_PANE_LEFT_WIDTH - 56);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_WRAP);
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
    lv_obj_set_flex_flow(chat_scroll, LV_FLEX_FLOW_COLUMN);     /* cards stack (they overlapped at y=0) */
    lv_obj_set_style_pad_row(chat_scroll, 8, 0);
    lv_obj_set_scroll_dir(chat_scroll, LV_DIR_VER);

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
    lv_obj_set_flex_flow(input_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(input_bar, 6, 0);

    ta = lv_textarea_create(input_bar);
    lv_textarea_set_placeholder_text(ta, "Prompt (Enter sends, Ctrl+C stops)...");
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_size(ta, 0, 38);
    lv_obj_set_flex_grow(ta, 1);
    lv_obj_set_style_bg_color(ta, p->bg_alt, 0);
    lv_obj_set_style_border_color(ta, p->surface_border, 0);
    lv_obj_set_style_text_color(ta, p->text_primary, 0);

    btn_note = lv_button_create(input_bar);
    lv_obj_set_size(btn_note, 88, 38);
    lv_obj_set_style_bg_color(btn_note, p->surface, 0);
    lv_obj_set_style_border_color(btn_note, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_note, 1, 0);
    lv_obj_set_style_radius(btn_note, 4, 0);
    lv_obj_add_event_cb(btn_note, note_btn_cb, LV_EVENT_CLICKED, NULL);

    lbl_note = lv_label_create(btn_note);
    lv_label_set_text(lbl_note, LV_SYMBOL_FILE " Note");
    lv_obj_center(lbl_note);
    lv_obj_set_style_text_font(lbl_note, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_note, p->text_primary, 0);

    btn_send = lv_button_create(input_bar);
    lv_obj_set_size(btn_send, 92, 38);
    lv_obj_set_style_bg_color(btn_send, p->accent_primary, 0);
    lv_obj_set_style_radius(btn_send, 4, 0);
    lv_obj_add_event_cb(btn_send, send_btn_cb, LV_EVENT_CLICKED, NULL);

    lbl_send = lv_label_create(btn_send);
    lv_label_set_text(lbl_send, "Send " LV_SYMBOL_RIGHT);
    lv_obj_center(lbl_send);
    lv_obj_set_style_text_font(lbl_send, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_send,
        devos_theme_is_dark() ? lv_color_black() : lv_color_white(), 0);

    /* Right: session info + diffs */
    lbl_rg = lv_label_create(right_panel);
    lv_label_set_text(lbl_rg, "SESSION (Sym+R)");
    lv_obj_set_pos(lbl_rg, 4, 4);
    lv_obj_set_style_text_font(lbl_rg, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_rg, p->text_secondary, 0);

    lbl_info = lv_label_create(right_panel);
    lv_label_set_text(lbl_info, "No session selected");
    lv_obj_set_pos(lbl_info, 4, 24);
    lv_obj_set_size(lbl_info, DEVOS_PANE_RIGHT_WIDTH - 28, 76);
    lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl_info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_info, p->text_primary, 0);

    /* Action buttons row */
    btn_diff = lv_button_create(right_panel);
    lv_obj_set_size(btn_diff, 86, 30);
    lv_obj_set_pos(btn_diff, 4, 104);
    lv_obj_set_style_bg_color(btn_diff, p->surface_active, 0);
    lv_obj_set_style_border_color(btn_diff, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_diff, 1, 0);
    lv_obj_set_style_radius(btn_diff, 4, 0);
    lv_obj_add_event_cb(btn_diff, diff_btn_cb, LV_EVENT_CLICKED, NULL);

    lbl_diff_btn = lv_label_create(btn_diff);
    lv_label_set_text(lbl_diff_btn, LV_SYMBOL_REFRESH " Diff");
    lv_obj_center(lbl_diff_btn);
    lv_obj_set_style_text_font(lbl_diff_btn, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_diff_btn, p->text_primary, 0);

    btn_save_diff = lv_button_create(right_panel);
    lv_obj_set_size(btn_save_diff, 90, 30);
    lv_obj_set_pos(btn_save_diff, 94, 104);
    lv_obj_set_style_bg_color(btn_save_diff, p->surface_active, 0);
    lv_obj_set_style_border_color(btn_save_diff, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_save_diff, 1, 0);
    lv_obj_set_style_radius(btn_save_diff, 4, 0);
    lv_obj_add_event_cb(btn_save_diff, save_diff_cb, LV_EVENT_CLICKED, NULL);

    lbl_save_diff_btn = lv_label_create(btn_save_diff);
    lv_label_set_text(lbl_save_diff_btn, LV_SYMBOL_DOWNLOAD " Diff");
    lv_obj_center(lbl_save_diff_btn);
    lv_obj_set_style_text_font(lbl_save_diff_btn, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_save_diff_btn, p->text_primary, 0);

    btn_plan = lv_button_create(right_panel);
    lv_obj_set_size(btn_plan, 90, 30);
    lv_obj_set_pos(btn_plan, 188, 104);
    lv_obj_set_style_bg_color(btn_plan, p->surface_active, 0);
    lv_obj_set_style_border_color(btn_plan, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_plan, 1, 0);
    lv_obj_set_style_radius(btn_plan, 4, 0);
    lv_obj_add_event_cb(btn_plan, save_plan_cb, LV_EVENT_CLICKED, NULL);

    lbl_plan_btn = lv_label_create(btn_plan);
    lv_label_set_text(lbl_plan_btn, LV_SYMBOL_SAVE " Plan");
    lv_obj_center(lbl_plan_btn);
    lv_obj_set_style_text_font(lbl_plan_btn, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_plan_btn, p->text_primary, 0);

    diff_scroll = lv_obj_create(right_panel);
    lv_obj_set_pos(diff_scroll, 4, 140);
    lv_obj_set_size(diff_scroll, DEVOS_PANE_RIGHT_WIDTH - 28, DEVOS_CONTENT_HEIGHT - 146);
    lv_obj_set_style_bg_color(diff_scroll, p->code_bg, 0);
    lv_obj_set_style_border_color(diff_scroll, p->surface_border, 0);
    lv_obj_set_style_border_width(diff_scroll, 1, 0);
    lv_obj_set_style_radius(diff_scroll, 4, 0);
    devos_codeview_create(&s_cv_pane, diff_scroll);
    lv_obj_add_event_cb(diff_scroll, diff_pane_click_cb, LV_EVENT_CLICKED, NULL);   /* tap: full screen */

    /* Permission modal */
    modal_perm = lv_obj_create(screen);
    lv_obj_set_size(modal_perm, 460, 190);
    lv_obj_align(modal_perm, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(modal_perm, p->surface, 0);
    lv_obj_set_style_border_color(modal_perm, p->accent_warning, 0);
    lv_obj_set_style_border_width(modal_perm, 2, 0);
    lv_obj_set_style_radius(modal_perm, 8, 0);
    lv_obj_set_style_pad_all(modal_perm, 16, 0);
    lv_obj_remove_flag(modal_perm, LV_OBJ_FLAG_SCROLLABLE);
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
    lv_obj_set_size(modal_srv, 560, 262);
    lv_obj_align(modal_srv, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(modal_srv, p->surface, 0);
    lv_obj_set_style_border_color(modal_srv, p->accent_primary, 0);
    lv_obj_set_style_border_width(modal_srv, 2, 0);
    lv_obj_set_style_radius(modal_srv, 8, 0);
    lv_obj_set_style_pad_all(modal_srv, 16, 0);
    lv_obj_clear_flag(modal_srv, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);

    lbl_srv_title = lv_label_create(modal_srv);
    lv_label_set_text(lbl_srv_title, LV_SYMBOL_SETTINGS " Server (OpenCode / OpenChamber)");
    lv_obj_set_style_text_font(lbl_srv_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_srv_title, p->accent_primary, 0);

    /* Row 1: Host (320px) + Port (120px) */
    ta_host = lv_textarea_create(modal_srv);
    lv_textarea_set_placeholder_text(ta_host, "https://host.tail1234.ts.net  or  http://10.0.0.5:4096");
    lv_textarea_set_one_line(ta_host, true);
    lv_textarea_set_max_length(ta_host, OPENDEV_URL_MAX - 1);
    lv_obj_set_size(ta_host, 528, 36);
    lv_obj_set_pos(ta_host, 0, 32);
    lv_obj_set_style_bg_color(ta_host, p->code_bg, 0);
    lv_obj_set_style_text_color(ta_host, p->text_primary, 0);
    lv_obj_set_style_border_color(ta_host, p->surface_border, 0);
    lv_obj_set_style_border_width(ta_host, 1, 0);
    lv_obj_set_style_radius(ta_host, 4, 0);
    lv_obj_set_style_pad_all(ta_host, 8, 0);
    lv_obj_add_event_cb(ta_host, srv_ta_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ta_host, srv_ta_click_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ta_host, srv_ta_click_cb, LV_EVENT_FOCUSED, NULL);

    ta_port = lv_textarea_create(modal_srv);
    lv_textarea_set_placeholder_text(ta_port, "4096");
    lv_textarea_set_one_line(ta_port, true);
    lv_obj_set_size(ta_port, 120, 36);
    lv_obj_set_pos(ta_port, 328, 32);
    lv_obj_set_style_bg_color(ta_port, p->code_bg, 0);
    lv_obj_set_style_text_color(ta_port, p->text_primary, 0);
    lv_obj_set_style_border_color(ta_port, p->surface_border, 0);
    lv_obj_set_style_border_width(ta_port, 1, 0);
    lv_obj_set_style_radius(ta_port, 4, 0);
    lv_obj_set_style_pad_all(ta_port, 8, 0);
    lv_obj_add_event_cb(ta_port, srv_ta_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ta_port, srv_ta_click_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ta_port, srv_ta_click_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_flag(ta_port, LV_OBJ_FLAG_HIDDEN);              /* the port is part of the URL now */

    /* Row 2: Token (290px) + Scan QR button (150px) */
    ta_token = lv_textarea_create(modal_srv);
    lv_textarea_set_placeholder_text(ta_token, "Password (OpenChamber only, not stored)");
    lv_textarea_set_one_line(ta_token, true);
    lv_textarea_set_password_mode(ta_token, true);
    lv_obj_set_size(ta_token, 370, 36);
    lv_obj_set_pos(ta_token, 0, 76);
    lv_obj_set_style_bg_color(ta_token, p->code_bg, 0);
    lv_obj_set_style_text_color(ta_token, p->text_primary, 0);
    lv_obj_set_style_border_color(ta_token, p->surface_border, 0);
    lv_obj_set_style_border_width(ta_token, 1, 0);
    lv_obj_set_style_radius(ta_token, 4, 0);
    lv_obj_set_style_pad_all(ta_token, 8, 0);
    lv_obj_add_event_cb(ta_token, srv_ta_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ta_token, srv_ta_click_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ta_token, srv_ta_click_cb, LV_EVENT_FOCUSED, NULL);

    btn_scan_qr = lv_button_create(modal_srv);
    lv_obj_set_size(btn_scan_qr, 150, 36);
    lv_obj_set_pos(btn_scan_qr, 378, 76);
    lv_obj_set_style_bg_color(btn_scan_qr, p->surface_active, 0);
    lv_obj_set_style_border_color(btn_scan_qr, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_scan_qr, 1, 0);
    lv_obj_set_style_radius(btn_scan_qr, 4, 0);
    lv_obj_add_event_cb(btn_scan_qr, scan_qr_btn_cb, LV_EVENT_CLICKED, NULL);
    lbl_scan_qr = lv_label_create(btn_scan_qr);
    lv_label_set_text(lbl_scan_qr, LV_SYMBOL_IMAGE " Scan QR");
    lv_obj_center(lbl_scan_qr);
    lv_obj_set_style_text_color(lbl_scan_qr, p->text_primary, 0);

    lbl_srv_hint = lv_label_create(modal_srv);
    lv_obj_set_pos(lbl_srv_hint, 0, 122);
    lv_obj_set_width(lbl_srv_hint, 528);
    lv_label_set_long_mode(lbl_srv_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl_srv_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_srv_hint, p->text_secondary, 0);
    lv_label_set_text(lbl_srv_hint, "");

    /* Row 3: Save & Cancel */
    lv_obj_t *btn_srv_save = lv_button_create(modal_srv);
    lv_obj_set_size(btn_srv_save, 130, 34);
    lv_obj_set_pos(btn_srv_save, 270, 180);
    lv_obj_set_style_bg_color(btn_srv_save, p->accent_primary, 0);
    lv_obj_set_style_radius(btn_srv_save, 4, 0);
    lv_obj_add_event_cb(btn_srv_save, srv_save_cb, LV_EVENT_CLICKED, NULL);
    lbl_srv_save = lv_label_create(btn_srv_save);
    lv_label_set_text(lbl_srv_save, LV_SYMBOL_OK " Connect");
    lv_obj_center(lbl_srv_save);
    lv_obj_set_style_text_color(lbl_srv_save,
        devos_theme_is_dark() ? lv_color_black() : lv_color_white(), 0);

    lv_obj_t *btn_srv_cancel = lv_button_create(modal_srv);
    lv_obj_set_size(btn_srv_cancel, 118, 34);
    lv_obj_set_pos(btn_srv_cancel, 410, 180);
    lv_obj_set_style_bg_color(btn_srv_cancel, p->surface, 0);
    lv_obj_set_style_border_color(btn_srv_cancel, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_srv_cancel, 1, 0);
    lv_obj_set_style_radius(btn_srv_cancel, 4, 0);
    lv_obj_add_event_cb(btn_srv_cancel, srv_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_srv_cancel = lv_label_create(btn_srv_cancel);
    lv_label_set_text(lbl_srv_cancel, "Cancel");
    lv_obj_center(lbl_srv_cancel);
    lv_obj_set_style_text_color(lbl_srv_cancel, p->text_primary, 0);

    /* Camera QR Scanner modal */
    modal_cam = lv_obj_create(screen);
    lv_obj_set_size(modal_cam, 440, 370);
    lv_obj_align(modal_cam, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(modal_cam, p->surface, 0);
    lv_obj_set_style_border_color(modal_cam, p->accent_primary, 0);
    lv_obj_set_style_border_width(modal_cam, 2, 0);
    lv_obj_set_style_radius(modal_cam, 8, 0);
    lv_obj_set_style_pad_all(modal_cam, 12, 0);
    lv_obj_clear_flag(modal_cam, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal_cam, LV_OBJ_FLAG_HIDDEN);

    lbl_cam_title = lv_label_create(modal_cam);
    lv_label_set_text(lbl_cam_title, LV_SYMBOL_IMAGE " Scan OpenChamber QR Code");
    lv_obj_set_style_text_font(lbl_cam_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_cam_title, p->accent_primary, 0);
    lv_obj_align(lbl_cam_title, LV_ALIGN_TOP_LEFT, 4, 0);

    cam_canvas = lv_canvas_create(modal_cam);
    lv_obj_set_size(cam_canvas, 320, 240);
    lv_obj_align(cam_canvas, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_border_color(cam_canvas, p->surface_border, 0);
    lv_obj_set_style_border_width(cam_canvas, 1, 0);
    lv_obj_set_style_radius(cam_canvas, 4, 0);

    lbl_cam_hint = lv_label_create(modal_cam);
    lv_label_set_text(lbl_cam_hint, "Point Tab5 camera at OpenChamber Web UI connect QR");
    lv_obj_set_style_text_font(lbl_cam_hint, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_cam_hint, p->text_secondary, 0);
    lv_obj_align(lbl_cam_hint, LV_ALIGN_TOP_MID, 0, 274);

    btn_cam_test = lv_button_create(modal_cam);
    lv_obj_set_size(btn_cam_test, 150, 34);
    lv_obj_align(btn_cam_test, LV_ALIGN_BOTTOM_LEFT, 20, -4);
    lv_obj_set_style_bg_color(btn_cam_test, p->accent_primary, 0);
    lv_obj_set_style_radius(btn_cam_test, 4, 0);
    lv_obj_add_event_cb(btn_cam_test, cam_test_cb, LV_EVENT_CLICKED, NULL);
    lbl_cam_test = lv_label_create(btn_cam_test);
    lv_label_set_text(lbl_cam_test, LV_SYMBOL_PLAY " Simulate QR");
    lv_obj_center(lbl_cam_test);
    lv_obj_set_style_text_color(lbl_cam_test,
        devos_theme_is_dark() ? lv_color_black() : lv_color_white(), 0);

    btn_cam_cancel = lv_button_create(modal_cam);
    lv_obj_set_size(btn_cam_cancel, 110, 34);
    lv_obj_align(btn_cam_cancel, LV_ALIGN_BOTTOM_RIGHT, -20, -4);
    lv_obj_set_style_bg_color(btn_cam_cancel, p->surface, 0);
    lv_obj_set_style_border_color(btn_cam_cancel, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_cam_cancel, 1, 0);
    lv_obj_set_style_radius(btn_cam_cancel, 4, 0);
    lv_obj_add_event_cb(btn_cam_cancel, cam_cancel_cb, LV_EVENT_CLICKED, NULL);
    lbl_cam_cancel = lv_label_create(btn_cam_cancel);
    lv_label_set_text(lbl_cam_cancel, "Cancel");
    lv_obj_center(lbl_cam_cancel);
    lv_obj_set_style_text_color(lbl_cam_cancel, p->text_primary, 0);

#ifdef ESP_PLATFORM
    /* Test hook for the desktop simulator only: on hardware it would
     * overwrite the real pairing. */
    lv_obj_add_flag(btn_cam_test, LV_OBJ_FLAG_HIDDEN);
#endif

    build_diff_modal(p);
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
    diff_full_close();
    bsp_tab5_camera_stop_qr_scanner();
    bsp_tab5_camera_stop();
    if (modal_perm) lv_obj_add_flag(modal_perm, LV_OBJ_FLAG_HIDDEN);
    if (modal_srv) lv_obj_add_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
    if (modal_cam) lv_obj_add_flag(modal_cam, LV_OBJ_FLAG_HIDDEN);
}

static int opendev_telemetry_lines(char lines[3][64])
{
    snprintf(lines[0], sizeof(lines[0]), "* %s", opendev_client_status_text());
    char host[OPENDEV_HOST_MAX];
    int port = 0;
    opendev_client_get_config(host, sizeof(host), &port, NULL, NULL, 0);
    snprintf(lines[1], sizeof(lines[1]), "* %s:%d", host, port);
    snprintf(lines[2], sizeof(lines[2]), "* REST/SSE agent");
    return 3;
}

devos_app_descriptor_t *app_opendev_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_OPENDEV;
    app_descriptor.uid = "opendev";
    app_descriptor.icon = LV_SYMBOL_PLAY;
    app_descriptor.category = "agents";
    app_descriptor.name = "OpenDev";
    app_descriptor.title = "OpenDev / OpenChamber";
    app_descriptor.subtitle = "Remote AI Coding Agent";
    app_descriptor.screen = screen;
    app_descriptor.init = opendev_init;
    app_descriptor.show = opendev_show;
    app_descriptor.hide = opendev_hide;
    app_descriptor.handle_key = opendev_handle_key;
    app_descriptor.get_telemetry_lines = opendev_telemetry_lines;

    return &app_descriptor;
}
