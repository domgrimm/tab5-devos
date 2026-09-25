#include "app_antigravity.h"
#include "devos_config.h"
#include "devos_theme.h"
#include "devos_agent_viewport.h"
#include "devos_mdview.h"
#include "devos_codeview.h"
#include "apps/app_editor/app_editor.h"
#include "agy_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_nimbus_mono_14);

#define AGY_SUB_BTNS AGY_MAX_AGENTS
#define AGY_ART_BTNS AGY_MAX_ARTIFACTS
#define AGY_Q_BTNS 4
#define REFRESH_MS 250            /* redraw at most 4x/s while the agent streams */

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;
static devos_agent_viewport_t *viewport = NULL;

static bool thinking_expanded = false;

/* Left: session + subagents + slash */
static lv_obj_t *lbl_agy_title = NULL;
static lv_obj_t *sess_box = NULL;
static lv_obj_t *lbl_conv = NULL;
static lv_obj_t *lbl_mod = NULL;
static lv_obj_t *lbl_ws = NULL;
static lv_obj_t *lbl_link = NULL;
static lv_obj_t *btn_new = NULL;
static lv_obj_t *lbl_new = NULL;
static lv_obj_t *btn_hist = NULL;
static lv_obj_t *lbl_hist = NULL;
static lv_obj_t *lbl_sub = NULL;
static lv_obj_t *sub_btns[AGY_SUB_BTNS] = {NULL};
static lv_obj_t *sub_lbls[AGY_SUB_BTNS] = {NULL};
static lv_obj_t *lbl_slash = NULL;
static lv_obj_t *cmd_btns[4] = {NULL};
static lv_obj_t *cmd_lbls[4] = {NULL};

/* Center: strip + chat + input */
static lv_obj_t *action_strip = NULL;
static lv_obj_t *lbl_strip = NULL;
static lv_obj_t *chat_scroll = NULL;
static lv_obj_t *input_bar = NULL;
static lv_obj_t *ta = NULL;
static lv_obj_t *btn_note = NULL;
static lv_obj_t *lbl_note = NULL;
static lv_obj_t *btn_send = NULL;
static lv_obj_t *lbl_send = NULL;

/* Right: artifacts + diffs */
static lv_obj_t *lbl_insp_title = NULL;
static lv_obj_t *lbl_art_h = NULL;
static lv_obj_t *lbl_art_none = NULL;
static lv_obj_t *art_btns[AGY_ART_BTNS] = {NULL};
static lv_obj_t *art_lbls[AGY_ART_BTNS] = {NULL};
static lv_obj_t *lbl_diff_h = NULL;
static lv_obj_t *btn_save_diff = NULL;
static lv_obj_t *lbl_save_diff = NULL;
static lv_obj_t *diff_scroll = NULL;
static devos_codeview_t s_cv_pane, s_cv_full;
static lv_obj_t *modal_diff = NULL;
static lv_obj_t *lbl_diff_title = NULL;
static EXT_RAM_BSS_ATTR char s_diff_display[AGY_DIFF_MAX + 256];
static uint32_t s_seen_diff_rev = UINT32_MAX;

/* Permission modal */
static lv_obj_t *modal_permission = NULL;
static lv_obj_t *lbl_m_title = NULL;
static lv_obj_t *lbl_m_desc = NULL;
static lv_obj_t *perm_btns[3] = {NULL};
static lv_obj_t *perm_lbls[3] = {NULL};
static lv_obj_t *perm_prev = NULL;          /* diff of a pending edit */
static devos_codeview_t s_cv_perm;
static char s_shown_perm[AGY_NAME_MAX] = "";

/* Question modal */
static lv_obj_t *modal_question = NULL;
static lv_obj_t *lbl_q_title = NULL;
static lv_obj_t *lbl_q_desc = NULL;
static lv_obj_t *q_btns[AGY_Q_BTNS] = {NULL};
static lv_obj_t *q_lbls[AGY_Q_BTNS] = {NULL};
static lv_obj_t *ta_q = NULL;
static char s_shown_q[AGY_NAME_MAX] = "";
static int s_q_choices = 0;
/* keys are ignored briefly after an ask pops up, so a word being typed
 * into the prompt ("yes", "1.") can't answer it by accident */
#define ASK_GUARD_MS 700
static uint32_t s_ask_since = 0;

/* Artifact viewer modal */
static lv_obj_t *modal_artifact = NULL;
static lv_obj_t *lbl_art_title = NULL;
static lv_obj_t *art_scroll = NULL;
static lv_obj_t *btn_art_save = NULL;
static lv_obj_t *lbl_art_save = NULL;
static lv_obj_t *lbl_art_close = NULL;
static int s_open_artifact = -1;

/* History modal: agy's conversations (antigravity.google.com) */
static lv_obj_t *modal_hist = NULL;
static lv_obj_t *lbl_hist_title = NULL;
static lv_obj_t *hist_list = NULL;
static lv_obj_t *hist_rows[AGY_MAX_CONVS];
static int s_hist_sel = 0;
static int s_hist_n = -1;                   /* rows built; -1 = rebuild */
static uint32_t s_hist_gen = 0;

/* Up/Down in the prompt walks the prompt history */
static int s_recall = -1;

/* Server modal */
static lv_obj_t *modal_srv = NULL;
static lv_obj_t *lbl_srv_title = NULL;
static lv_obj_t *lbl_srv_hint = NULL;
static lv_obj_t *ta_srv_host = NULL;
static lv_obj_t *ta_srv_port = NULL;
static lv_obj_t *ta_srv_token = NULL;
static lv_obj_t *srv_focus = NULL;
static lv_obj_t *lbl_srv_save = NULL;
static lv_obj_t *lbl_srv_cancel = NULL;

/* Chat: one card per block, re-filled when the block's rev changes */
static lv_obj_t *s_cards[AGY_MAX_BLOCKS];
static uint32_t s_card_rev[AGY_MAX_BLOCKS];
static uint8_t s_card_kind[AGY_MAX_BLOCKS];
static int s_rendered = -1;
static char s_hint[1200] = "";

static uint32_t s_seen_gen = 0;
static bool s_dirty = true;
static uint32_t s_last_refresh = 0;
static lv_timer_t *poll_timer = NULL;

static void refresh_all(bool force);
static void hist_build(void);
static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color, int x, int y);
static void apply_theme(const devos_palette_t *p, void *user_data);
static void srv_focus_paint(void);

/* ------------------------------------------------------------------ theme */
static void paint_button(lv_obj_t *b, lv_color_t bg, lv_color_t border)
{
    if (!b) return;
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_border_color(b, border, 0);
}

static void paint_text(lv_obj_t *l, lv_color_t c)
{
    if (l) lv_obj_set_style_text_color(l, c, 0);
}

static void apply_theme(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    if (!screen) return;
    lv_color_t on_accent = devos_theme_is_dark() ? lv_color_black() : lv_color_white();

    lv_obj_set_style_bg_color(screen, p->bg, 0);
    paint_text(lbl_agy_title, p->text_secondary);
    paint_button(sess_box, p->surface_active, p->accent_primary);
    paint_text(lbl_conv, p->accent_primary);
    paint_text(lbl_mod, p->text_primary);
    paint_text(lbl_ws, p->text_secondary);
    paint_button(btn_new, p->surface, p->surface_border);
    paint_text(lbl_new, p->text_primary);
    paint_button(btn_hist, p->surface, p->surface_border);
    paint_text(lbl_hist, p->text_primary);
    paint_button(modal_hist, p->surface, p->accent_primary);
    paint_text(lbl_hist_title, p->accent_primary);
    if (modal_hist && !lv_obj_has_flag(modal_hist, LV_OBJ_FLAG_HIDDEN)) hist_build();
    paint_text(lbl_sub, p->accent_secondary);
    for (int i = 0; i < AGY_SUB_BTNS; i++) paint_button(sub_btns[i], p->surface, p->surface_border);
    paint_text(lbl_slash, p->text_secondary);
    for (int i = 0; i < 4; i++) {
        paint_button(cmd_btns[i], p->surface, p->surface_border);
        paint_text(cmd_lbls[i], p->accent_primary);
    }

    paint_button(action_strip, p->bg_alt, p->surface_border);
    paint_text(lbl_strip, p->text_secondary);
    if (chat_scroll) lv_obj_set_style_bg_color(chat_scroll, p->bg, 0);
    paint_button(input_bar, p->surface, p->surface_border);
    if (ta) {
        paint_button(ta, p->bg_alt, p->surface_border);
        paint_text(ta, p->text_primary);
    }
    paint_button(btn_note, p->surface, p->surface_border);
    paint_text(lbl_note, p->text_primary);
    if (btn_send) lv_obj_set_style_bg_color(btn_send, p->accent_primary, 0);
    paint_text(lbl_send, on_accent);

    paint_text(lbl_insp_title, p->text_secondary);
    paint_text(lbl_art_h, p->accent_primary);
    paint_text(lbl_art_none, p->text_secondary);
    for (int i = 0; i < AGY_ART_BTNS; i++) {
        paint_button(art_btns[i], p->surface, p->surface_border);
        paint_text(art_lbls[i], p->text_primary);
    }
    paint_text(lbl_diff_h, p->accent_primary);
    paint_button(btn_save_diff, p->surface_active, p->surface_border);
    paint_text(lbl_save_diff, p->text_primary);
    paint_button(diff_scroll, p->code_bg, p->surface_border);
    if (modal_diff) lv_obj_set_style_bg_color(modal_diff, p->bg, 0);
    paint_text(lbl_diff_title, p->text_primary);
    if (s_cv_full.scroll) paint_button(s_cv_full.scroll, p->code_bg, p->surface_border);

    paint_button(modal_permission, p->surface, p->accent_warning);
    paint_text(lbl_m_title, p->accent_warning);
    paint_text(lbl_m_desc, p->text_primary);
    paint_button(perm_prev, p->code_bg, p->surface_border);
    for (int i = 0; i < 3; i++) {
        if (perm_btns[i]) {
            lv_obj_set_style_bg_color(perm_btns[i],
                i == 1 ? p->accent_danger : i == 2 ? p->surface_active : p->accent_secondary, 0);
        }
        paint_text(perm_lbls[i], i == 1 ? lv_color_white() : i == 2 ? p->text_primary : on_accent);
    }
    paint_button(modal_question, p->surface, p->accent_primary);
    paint_text(lbl_q_title, p->accent_primary);
    paint_text(lbl_q_desc, p->text_primary);
    for (int i = 0; i < AGY_Q_BTNS; i++) {
        paint_button(q_btns[i], p->surface_active, p->surface_border);
        paint_text(q_lbls[i], p->text_primary);
    }
    if (ta_q) {
        paint_button(ta_q, p->code_bg, p->accent_primary);
        paint_text(ta_q, p->text_primary);
    }
    paint_button(modal_artifact, p->surface, p->accent_primary);
    paint_text(lbl_art_title, p->accent_primary);
    if (art_scroll) lv_obj_set_style_bg_color(art_scroll, p->code_bg, 0);
    /* re-render an open artifact so its colors follow the theme */
    if (modal_artifact && !lv_obj_has_flag(modal_artifact, LV_OBJ_FLAG_HIDDEN) && s_open_artifact >= 0 &&
        art_scroll) {
        const agy_artifact_t *a = agy_client_artifact(s_open_artifact);
        if (a) {
            lv_obj_clean(art_scroll);
            devos_md_render(art_scroll, a->text);
        }
    }
    paint_button(btn_art_save, p->surface_active, p->surface_border);
    paint_text(lbl_art_save, p->text_primary);
    paint_text(lbl_art_close, p->text_primary);
    paint_button(modal_srv, p->surface, p->accent_primary);
    paint_text(lbl_srv_title, p->accent_primary);
    paint_text(lbl_srv_hint, p->text_secondary);
    lv_obj_t *fields[3] = {ta_srv_host, ta_srv_port, ta_srv_token};
    for (int i = 0; i < 3; i++) {
        if (!fields[i]) continue;
        lv_obj_set_style_bg_color(fields[i], p->code_bg, 0);
        paint_text(fields[i], p->text_primary);
    }
    paint_text(lbl_srv_save, on_accent);
    paint_text(lbl_srv_cancel, p->text_primary);
    srv_focus_paint();

    s_seen_diff_rev = UINT32_MAX;
    refresh_all(true);
}

/* ------------------------------------------------------------------- chat */
static lv_obj_t *chat_card(void)
{
    lv_obj_t *card = lv_obj_create(chat_scroll);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static void chat_text(lv_obj_t *card, const char *text, lv_color_t color, const lv_font_t *font)
{
    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
}

static void toggle_thinking_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    thinking_expanded = !thinking_expanded;
    refresh_all(true);
}

static void fill_card(lv_obj_t *card, const agy_block_t *b, const devos_palette_t *p)
{
    lv_obj_clean(card);
    char head[96];
    switch (b->kind) {
    case AGY_KIND_TOOL: {
        lv_color_t c = b->state == AGY_TOOL_ERROR ? p->accent_danger
                     : b->state == AGY_TOOL_RUNNING ? p->accent_primary : p->accent_secondary;
        lv_obj_set_style_bg_color(card, p->tool_card_bg, 0);
        lv_obj_set_style_border_color(card, b->state == AGY_TOOL_DONE ? p->tool_card_border : c, 0);
        snprintf(head, sizeof(head), "%s  %s%s",
                 b->state == AGY_TOOL_ERROR ? LV_SYMBOL_CLOSE : b->state == AGY_TOOL_RUNNING ? LV_SYMBOL_REFRESH
                                                                                            : LV_SYMBOL_OK,
                 b->name, b->state == AGY_TOOL_RUNNING ? "  (running)" : "");
        chat_text(card, head, c, &lv_font_montserrat_14);
        if (b->text[0]) chat_text(card, b->text, p->text_secondary, &lv_font_nimbus_mono_14);
        break;
    }
    case AGY_KIND_THINK:
        lv_obj_set_style_bg_color(card, p->thinking_bg, 0);
        lv_obj_set_style_border_color(card, p->thinking_border, 0);
        chat_text(card, thinking_expanded ? LV_SYMBOL_DOWN " Thinking" : LV_SYMBOL_RIGHT " Thinking (tap to expand)",
                  p->accent_primary, &lv_font_montserrat_14);
        if (thinking_expanded) chat_text(card, b->text, p->text_secondary, &lv_font_montserrat_12);
        break;
    case AGY_KIND_ERROR:
        lv_obj_set_style_bg_color(card, p->surface, 0);
        lv_obj_set_style_border_color(card, p->accent_danger, 0);
        chat_text(card, LV_SYMBOL_WARNING " Problem", p->accent_danger, &lv_font_montserrat_14);
        chat_text(card, b->text, p->text_primary, &lv_font_montserrat_14);
        break;
    default:
        if (b->role == AGY_ROLE_USER) {
            lv_obj_set_style_bg_color(card, p->surface_active, 0);
            lv_obj_set_style_border_color(card, p->accent_primary, 0);
            chat_text(card, b->text, p->text_primary, &lv_font_montserrat_14);
            break;
        }
        lv_obj_set_style_bg_color(card, p->surface, 0);
        lv_obj_set_style_border_color(card, p->surface_border, 0);
        /* agent output renders through the shared markdown engine, which
         * places blocks absolutely: measured inner box with explicit height */
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
        }
        break;
    }
}

static lv_obj_t *new_card(const agy_block_t *b, const devos_palette_t *p)
{
    lv_obj_t *card = chat_card();
    if (b->kind == AGY_KIND_THINK) {
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, toggle_thinking_cb, LV_EVENT_CLICKED, NULL);
    }
    fill_card(card, b, p);
    return card;
}

#define SETUP_STEPS \
    "1. On the computer, install the Antigravity CLI (agy) and sign in once by running  agy  " \
    "(docs: antigravity.google/docs/cli).\n" \
    "2. Copy the tools/agy_bridge folder from devOS to it and run  pip install websockets\n" \
    "3. In your project folder run:\n     python3 <path>/agy_bridge/bridge_server.py\n" \
    "   It prints the address, port and token to use.\n" \
    "4. Enter them here. The computer must be reachable from the Tab5 (same Wi-Fi or Tailscale)."

static void sess_box_cb(lv_event_t *e);

static void setup_btn_cb(lv_event_t *e)
{
    sess_box_cb(e);
}

static void chat_hint(char *out, size_t cap)
{
    char host[AGY_HOST_MAX];
    int port = 0;
    agy_client_get_config(host, sizeof(host), &port, NULL, 0);
    if (!agy_client_configured()) {
        snprintf(out, cap,
                 "Antigravity runs on your computer; the Tab5 drives it through a small bridge.\n\n" SETUP_STEPS);
        return;
    }
    switch (agy_client_status()) {
    case AGY_UP: {
        const char *ws = agy_client_workspace();
        snprintf(out, cap,
                 "Connected to the Antigravity bridge%s%s.\n\nType a prompt below and press Enter. "
                 "Tap a /command on the left to start with it. Tool calls that change things wait for "
                 "your Allow / Deny here.",
                 ws[0] ? " in " : "", ws);
        break;
    }
    case AGY_CONNECTING:
        snprintf(out, cap, "%s", agy_client_status_text());
        break;
    default:
        if (strstr(agy_client_status_text(), "token")) {
            snprintf(out, cap,
                     "The bridge at %s:%d turned down the token.\n\n"
                     "Tap Connection and enter the token bridge_server.py printed when it started "
                     "(it changes every run unless you start it with --psk).",
                     host, port);
            break;
        }
        snprintf(out, cap,
                 "Can't reach the bridge at %s:%d (%s).\n\n"
                 "Is bridge_server.py running on that computer? Tap Connection to change the address.\n\n"
                 SETUP_STEPS,
                 host, port, agy_client_status_text());
        break;
    }
}

static void refresh_chat(bool force)
{
    if (!chat_scroll) return;
    const devos_palette_t *p = devos_theme_get();
    int nb = agy_client_block_count();
    bool near_bottom = lv_obj_get_scroll_bottom(chat_scroll) < 48;
    bool rebuild = force || s_rendered < 0 || nb < s_rendered || (nb == 0) != (s_rendered == 0);
    for (int i = 0; !rebuild && i < s_rendered && i < nb; i++) {
        if (agy_client_block(i)->kind != s_card_kind[i]) rebuild = true;
    }
    if (nb == 0) {
        char hint[sizeof(s_hint)];
        chat_hint(hint, sizeof(hint));
        if (!rebuild && strcmp(hint, s_hint) == 0) return;
        lv_obj_clean(chat_scroll);
        snprintf(s_hint, sizeof(s_hint), "%s", hint);
        lv_obj_t *card = chat_card();
        lv_obj_set_style_bg_color(card, p->surface, 0);
        lv_obj_set_style_border_color(card, p->surface_border, 0);
        chat_text(card, s_hint, p->text_secondary, &lv_font_montserrat_14);
        if (agy_client_status() != AGY_UP) {
            lv_obj_t *b = lv_button_create(card);
            lv_obj_set_size(b, 220, 36);
            lv_obj_set_style_bg_color(b, p->accent_primary, 0);
            lv_obj_set_style_radius(b, 4, 0);
            lv_obj_add_event_cb(b, setup_btn_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_t *l = lv_label_create(b);
            lv_label_set_text(l, agy_client_configured() ? LV_SYMBOL_SETTINGS " Connection"
                                                         : LV_SYMBOL_PLAY " Set up connection");
            lv_obj_center(l);
            lv_obj_set_style_text_color(l, devos_theme_is_dark() ? lv_color_black() : lv_color_white(), 0);
        }
        s_rendered = 0;
        return;
    }
    bool grew = nb > s_rendered;
    if (rebuild) {
        lv_obj_clean(chat_scroll);
        s_rendered = 0;
        near_bottom = true;
    }
    for (int i = 0; i < s_rendered; i++) {
        const agy_block_t *b = agy_client_block(i);
        if (b->rev != s_card_rev[i]) {
            fill_card(s_cards[i], b, p);
            s_card_rev[i] = b->rev;
        }
    }
    for (int i = s_rendered; i < nb; i++) {
        const agy_block_t *b = agy_client_block(i);
        s_cards[i] = new_card(b, p);
        s_card_rev[i] = b->rev;
        s_card_kind[i] = b->kind;
    }
    s_rendered = nb;
    if (near_bottom || (grew && agy_client_block(nb - 1)->role == AGY_ROLE_USER)) {
        lv_obj_update_layout(chat_scroll);
        lv_obj_scroll_to_y(chat_scroll, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

/* ------------------------------------------------------------------- diff */
static const char *diff_display_text(void)
{
    const char *dt = agy_client_diff_text();
    if (!dt[0]) return "No edits yet.\n\nFile changes appear\nhere as a diff.";
    int nf = agy_client_diff_file_count();
    int add = 0, del = 0;
    for (const char *l = dt; l && *l;) {
        if (l[0] == '+' && strncmp(l, "+++", 3) != 0) add++;
        else if (l[0] == '-' && strncmp(l, "---", 3) != 0) del++;
        l = strchr(l, '\n');
        if (l) l++;
    }
    snprintf(s_diff_display, sizeof(s_diff_display), "%d file%s changed, +%d -%d\n\n%s", nf, nf == 1 ? "" : "s", add,
             del, dt);
    return s_diff_display;
}

static void refresh_diff(void)
{
    uint32_t rev = agy_client_diff_rev();
    if (rev == s_seen_diff_rev) return;
    s_seen_diff_rev = rev;
    const char *t = diff_display_text();
    devos_codeview_set(&s_cv_pane, t);
    if (modal_diff && !lv_obj_has_flag(modal_diff, LV_OBJ_FLAG_HIDDEN)) devos_codeview_set(&s_cv_full, t);
}

static void diff_full_open(void)
{
    if (!modal_diff) return;
    int nf = agy_client_diff_file_count();
    char t[96];
    snprintf(t, sizeof(t), "Diff  -  %d file%s  (Esc closes, arrows scroll)", nf, nf == 1 ? "" : "s");
    lv_label_set_text(lbl_diff_title, t);
    lv_obj_remove_flag(modal_diff, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(modal_diff);
    lv_obj_update_layout(modal_diff);
    devos_codeview_set(&s_cv_full, diff_display_text());
}

static void diff_full_close(void)
{
    if (modal_diff) lv_obj_add_flag(modal_diff, LV_OBJ_FLAG_HIDDEN);
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

/* ------------------------------------------------------------ meta/modals */
static void set_text_if(lv_obj_t *l, const char *t)
{
    if (l && strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

static void update_send_label(void)
{
    if (!lbl_send || !ta) return;
    const char *t = lv_textarea_get_text(ta);
    bool stop = agy_client_busy() && (!t || !*t);
    set_text_if(lbl_send, stop ? LV_SYMBOL_STOP " Stop" : "Send " LV_SYMBOL_RIGHT);
}

static void fmt_tokens(char *out, size_t cap, int n)
{
    if (n >= 1000000) snprintf(out, cap, "%d.%dM", n / 1000000, (n / 100000) % 10);
    else if (n >= 1000) snprintf(out, cap, "%d.%dk", n / 1000, (n / 100) % 10);
    else snprintf(out, cap, "%d", n);
}

static void refresh_meta(void)
{
    const devos_palette_t *p = devos_theme_get();
    char buf[300];

    /* Left: session card */
    const char *conv = agy_client_conversation_id();
    if (conv[0]) snprintf(buf, sizeof(buf), "conv %.8s", conv);
    else if (!agy_client_configured()) snprintf(buf, sizeof(buf), "Tap to connect");
    else snprintf(buf, sizeof(buf), "(new conversation)");
    set_text_if(lbl_conv, buf);
    set_text_if(lbl_mod, agy_client_model()[0] ? agy_client_model()
                         : agy_client_status() == AGY_UP ? "default model" : "");
    const char *ws = agy_client_workspace();
    const char *base = strrchr(ws, '/');
    base = base && base[1] ? base + 1 : ws;
    int in = 0, out = 0, total = 0;
    agy_client_usage(&in, &out, &total);
    char tok[16] = "";
    if (total > 0) fmt_tokens(tok, sizeof(tok), total);
    snprintf(buf, sizeof(buf), "%s%s%s%s", base[0] ? LV_SYMBOL_DIRECTORY " " : "", base,
             tok[0] ? (base[0] ? "  -  " : "") : "", tok[0] ? tok : "");
    if (tok[0]) strncat(buf, " tokens", sizeof(buf) - strlen(buf) - 1);
    set_text_if(lbl_ws, buf);
    if (lbl_link) {
        set_text_if(lbl_link, agy_client_busy() ? LV_SYMBOL_BULLET " working..." : agy_client_status_text());
        lv_obj_set_style_text_color(lbl_link, agy_client_status() == AGY_UP ? p->accent_secondary : p->text_secondary,
                                    0);
    }

    /* Left: subagents, then slash commands right below them */
    int na = agy_client_agent_count();
    int y = 212;
    for (int i = 0; i < AGY_SUB_BTNS; i++) {
        if (!sub_btns[i]) continue;
        if (i >= na) {
            lv_obj_add_flag(sub_btns[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(sub_btns[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(sub_btns[i], 4, y);
        y += 38;
        const agy_agent_t *a = agy_client_agent(i);
        bool run = strcmp(a->state, "running") == 0;
        snprintf(buf, sizeof(buf), "%s %s  (%s)", run ? LV_SYMBOL_BULLET : "-", a->name, a->state);
        set_text_if(sub_lbls[i], buf);
        lv_obj_set_style_text_color(sub_lbls[i], run ? p->accent_secondary : p->text_secondary, 0);
    }
    if (na == 0) y += 4;
    if (lbl_slash) lv_obj_set_pos(lbl_slash, 4, y + 10);
    for (int i = 0; i < 4; i++) {
        if (cmd_btns[i]) lv_obj_set_pos(cmd_btns[i], 4, y + 34 + i * 36);
    }

    /* Center: status strip */
    snprintf(buf, sizeof(buf), "%s%s  |  Esc stop  |  Up/Down earlier prompts  |  Sym+O history  |  Sym+D diff",
             agy_client_status_text(), agy_client_busy() ? "  (working)" : "");
    set_text_if(lbl_strip, buf);
    update_send_label();

    /* Right: artifacts, then the diff pane below them */
    int nart = agy_client_artifact_count();
    for (int i = 0; i < AGY_ART_BTNS; i++) {
        if (!art_btns[i]) continue;
        if (i >= nart) {
            lv_obj_add_flag(art_btns[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(art_btns[i], LV_OBJ_FLAG_HIDDEN);
        const agy_artifact_t *a = agy_client_artifact(i);
        snprintf(buf, sizeof(buf), "%s %s", LV_SYMBOL_FILE, a->name);
        set_text_if(art_lbls[i], buf);
    }
    if (lbl_art_none) {
        if (nart) lv_obj_add_flag(lbl_art_none, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(lbl_art_none, LV_OBJ_FLAG_HIDDEN);
    }
    int dy = nart ? 52 + nart * 40 + 6 : 80;
    if (lbl_diff_h) lv_obj_set_pos(lbl_diff_h, 4, dy + 4);
    if (btn_save_diff) lv_obj_set_pos(btn_save_diff, DEVOS_PANE_RIGHT_WIDTH - 84 - 24, dy);
    if (diff_scroll) {
        int avail = lv_obj_get_content_height(lv_obj_get_parent(diff_scroll));
        if (avail <= 0) avail = DEVOS_CONTENT_HEIGHT - 20;
        int h = avail - (dy + 32) - 4;
        if (lv_obj_get_y(diff_scroll) != dy + 32 || lv_obj_get_height(diff_scroll) != h) {
            lv_obj_set_pos(diff_scroll, 4, dy + 32);
            lv_obj_set_height(diff_scroll, h);
        }
    }
}

static void refresh_modals(void)
{
    char buf[700];
    agy_permission_t perm;
    if (agy_client_permission_pending(&perm)) {
        if (strcmp(perm.id, s_shown_perm) != 0) {
            snprintf(s_shown_perm, sizeof(s_shown_perm), "%s", perm.id);
            s_ask_since = lv_tick_get();
            /* first line is the tool name, the rest describes the call */
            const char *nl = strchr(perm.text, '\n');
            snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " Allow %.*s?", nl ? (int)(nl - perm.text) : 60, perm.text);
            lv_label_set_text(lbl_m_title, buf);
            lv_label_set_text(lbl_m_desc, nl ? nl + 1 : "");
            const char *pv = agy_client_permission_preview();
            if (pv[0]) {
                lv_obj_set_height(lbl_m_desc, 40);
                lv_obj_remove_flag(perm_prev, LV_OBJ_FLAG_HIDDEN);
                lv_obj_update_layout(modal_permission);
                devos_codeview_set(&s_cv_perm, pv);
            } else {
                lv_obj_set_height(lbl_m_desc, 300);
                lv_obj_add_flag(perm_prev, LV_OBJ_FLAG_HIDDEN);
                devos_codeview_set(&s_cv_perm, "");
            }
        }
        lv_obj_remove_flag(modal_permission, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(modal_permission);
    } else {
        s_shown_perm[0] = '\0';
        lv_obj_add_flag(modal_permission, LV_OBJ_FLAG_HIDDEN);
    }

    agy_question_t q;
    if (agy_client_question_pending(&q)) {
        if (strcmp(q.id, s_shown_q) != 0) {
            snprintf(s_shown_q, sizeof(s_shown_q), "%s", q.id);
            s_ask_since = lv_tick_get();
            s_q_choices = q.choice_count;
            lv_label_set_text(lbl_q_desc, q.prompt);
            for (int i = 0; i < AGY_Q_BTNS; i++) {
                if (i >= q.choice_count) {
                    lv_obj_add_flag(q_btns[i], LV_OBJ_FLAG_HIDDEN);
                    continue;
                }
                lv_obj_remove_flag(q_btns[i], LV_OBJ_FLAG_HIDDEN);
                snprintf(buf, sizeof(buf), "[%d] %s", i + 1, q.choices[i]);
                lv_label_set_text(q_lbls[i], buf);
            }
            lv_textarea_set_text(ta_q, "");
            lv_textarea_set_placeholder_text(ta_q, q.choice_count ? "...or type your own answer, Enter sends"
                                                                  : "Type your answer, Enter sends");
        }
        lv_obj_remove_flag(modal_question, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(modal_question);
    } else {
        s_shown_q[0] = '\0';
        lv_obj_add_flag(modal_question, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_all(bool force)
{
    if (!screen) return;
    refresh_meta();
    refresh_chat(force);
    refresh_diff();
    refresh_modals();
    if (modal_hist && !lv_obj_has_flag(modal_hist, LV_OBJ_FLAG_HIDDEN) && s_hist_gen != agy_client_generation()) {
        s_hist_gen = agy_client_generation();
        hist_build();
    }
    s_dirty = false;
    s_last_refresh = lv_tick_get();
}

/* ---------------------------------------------------------------- history */
static void hist_paint(void)
{
    const devos_palette_t *p = devos_theme_get();
    const char *cur = agy_client_conversation_id();
    for (int i = 0; i < s_hist_n; i++) {
        const agy_conv_t *c = agy_client_conv(i);
        bool sel = i == s_hist_sel;
        bool open = c && cur[0] && strcmp(c->id, cur) == 0;
        lv_obj_set_style_bg_color(hist_rows[i], sel ? p->surface_active : p->surface, 0);
        lv_obj_set_style_border_color(hist_rows[i], sel || open ? p->accent_primary : p->surface_border, 0);
        lv_obj_set_style_border_width(hist_rows[i], sel ? 2 : 1, 0);
    }
    if (s_hist_sel >= 0 && s_hist_sel < s_hist_n) lv_obj_scroll_to_view(hist_rows[s_hist_sel], LV_ANIM_OFF);
}

static void hist_row_cb(lv_event_t *e);

static void hist_build(void)
{
    const devos_palette_t *p = devos_theme_get();
    lv_obj_clean(hist_list);
    int n = agy_client_conv_count();
    const char *cur = agy_client_conversation_id();
    char buf[200];
    for (int i = 0; i < n; i++) {
        const agy_conv_t *c = agy_client_conv(i);
        lv_obj_t *row = lv_obj_create(hist_list);
        hist_rows[i] = row;
        lv_obj_set_size(row, lv_pct(100), 56);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_pad_ver(row, 6, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, hist_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        bool open = cur[0] && strcmp(c->id, cur) == 0;
        snprintf(buf, sizeof(buf), "%s%s", open ? LV_SYMBOL_RIGHT " " : "", c->title[0] ? c->title : "(untitled)");
        lv_obj_t *t = mk_label(row, buf, &lv_font_montserrat_14, open ? p->accent_primary : p->text_primary, 0, 0);
        lv_obj_set_width(t, lv_pct(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        snprintf(buf, sizeof(buf), "%s%s%s%s  -  %d steps%s", c->age[0] ? c->age : "", c->age[0] && strcmp(c->age, "now") ? " ago" : "",
                 c->ws[0] ? "  -  " : "", c->ws, c->steps, c->busy ? "  -  " LV_SYMBOL_BULLET " running" : "");
        lv_obj_t *m = mk_label(row, buf, &lv_font_montserrat_12, c->busy ? p->accent_secondary : p->text_secondary, 0, 22);
        lv_obj_set_width(m, lv_pct(100));
        lv_label_set_long_mode(m, LV_LABEL_LONG_DOT);
    }
    if (n == 0) {
        mk_label(hist_list, agy_client_status() == AGY_UP ? "No conversations yet." : "Not connected to the bridge.",
                 &lv_font_montserrat_14, p->text_secondary, 8, 8);
    }
    s_hist_n = n;
    if (s_hist_sel >= n) s_hist_sel = n ? n - 1 : 0;
    const char *inst = agy_client_instance();
    snprintf(buf, sizeof(buf), LV_SYMBOL_LIST " Conversations%s%s%s", inst[0] ? "  -  " : "", inst,
             inst[0] ? " on antigravity.google.com" : "");
    lv_label_set_text(lbl_hist_title, buf);
    hist_paint();
}

static void hist_open(void)
{
    if (!modal_hist) return;
    agy_client_list();
    s_hist_sel = 0;
    const char *cur = agy_client_conversation_id();
    for (int i = 0; cur[0] && i < agy_client_conv_count(); i++) {
        if (strcmp(agy_client_conv(i)->id, cur) == 0) s_hist_sel = i;
    }
    lv_obj_remove_flag(modal_hist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(modal_hist);
    hist_build();
    s_hist_gen = agy_client_generation();
}

static void hist_close(void)
{
    if (modal_hist) lv_obj_add_flag(modal_hist, LV_OBJ_FLAG_HIDDEN);
}

static void hist_choose(int idx)
{
    const agy_conv_t *c = agy_client_conv(idx);
    if (!c) return;
    agy_client_open(c->id);
    hist_close();
}

static void hist_row_cb(lv_event_t *e)
{
    hist_choose((int)(intptr_t)lv_event_get_user_data(e));
}

static void hist_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    hist_open();
}

static void hist_close_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    hist_close();
}

/* ---------------------------------------------------------------- events */
static void send_current(void)
{
    if (!ta) return;
    const char *t = lv_textarea_get_text(ta);
    if (!t || !*t) {
        if (agy_client_busy()) agy_client_abort();
        return;
    }
    /* leading /word becomes the slash command */
    char cmd[32] = "";
    const char *body = t;
    if (t[0] == '/') {
        int k = 0;
        while (t[k] && t[k] != ' ' && t[k] != '\t' && k < 30) k++;
        if (k > 1 && k < 30) {
            memcpy(cmd, t, (size_t)k);
            cmd[k] = '\0';
            body = t + k;
            while (*body == ' ' || *body == '\t') body++;
            if (!*body) body = t;   /* bare "/goal" is sent as-is */
            if (body == t) cmd[0] = '\0';
        }
    }
    if (agy_client_send(body, cmd[0] ? cmd : NULL) == 0) lv_textarea_set_text(ta, "");
    s_recall = -1;
    refresh_all(false);
}

static void send_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    send_current();
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
        static EXT_RAM_BSS_ATTR char prefix[AGY_PROMPT_MAX];
        snprintf(prefix, sizeof(prefix), "[Note: %s]\n%s\n\n", fn, txt);
        lv_textarea_set_text(ta, prefix);
        lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
    }
}

static void save_diff_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    const char *dt = agy_client_diff_text();
    if (!dt || !*dt) return;
    const char *conv = agy_client_conversation_id();
    char title[64];
    snprintf(title, sizeof(title), "agy-%.16s", (conv && conv[0]) ? conv : "session");
    app_editor_save_diff(title, dt);
}

static void cmd_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    static const char *cmds[4] = {"/goal", "/plan", "/boost", "/learn"};
    if (ta && idx >= 0 && idx < 4) {
        lv_textarea_set_text(ta, cmds[idx]);
        lv_textarea_add_char(ta, ' ');
        update_send_label();
    }
}

static void new_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    agy_client_new();
    refresh_all(true);
}

static void sub_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const agy_agent_t *a = agy_client_agent(idx);
    /* a tap addresses the agent in the prompt */
    if (a && ta) {
        lv_textarea_add_text(ta, "@");
        lv_textarea_add_text(ta, a->name);
        lv_textarea_add_char(ta, ' ');
        update_send_label();
    }
}

static void sess_box_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    /* the card doubles as the server/PSK editor entry point */
    if (modal_srv && ta_srv_host && ta_srv_port && ta_srv_token) {
        char host[AGY_HOST_MAX];
        int port = 0;
        char token[AGY_TOKEN_MAX];
        agy_client_get_config(host, sizeof(host), &port, token, sizeof(token));
        lv_textarea_set_text(ta_srv_host, host);
        char pb[16];
        snprintf(pb, sizeof(pb), "%d", port > 0 ? port : 8420);
        lv_textarea_set_text(ta_srv_port, pb);
        lv_textarea_set_text(ta_srv_token, token);
        srv_focus = ta_srv_host;
        srv_focus_paint();
        lv_obj_remove_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(modal_srv);
    }
}

static void answer_permission(bool allow, bool always)
{
    agy_client_answer_permission(allow, always);
    refresh_all(false);
}

static void perm_btn_cb(lv_event_t *e)
{
    int ans = (int)(intptr_t)lv_event_get_user_data(e);
    /* 1 = once (Y), 2 = always (A), 0 = deny (N) */
    answer_permission(ans > 0, ans == 2);
}

static void answer_question(int choice)
{
    agy_client_answer_question(choice);
    refresh_all(false);
}

static void question_btn_cb(lv_event_t *e)
{
    answer_question((int)(intptr_t)lv_event_get_user_data(e));
}

static void art_open(int idx)
{
    const agy_artifact_t *a = agy_client_artifact(idx);
    if (!a || !modal_artifact || !art_scroll) return;
    s_open_artifact = idx;
    if (lbl_art_title) lv_label_set_text(lbl_art_title, a->name);
    lv_obj_clean(art_scroll);
    devos_md_render(art_scroll, a->text);
    lv_obj_scroll_to_y(art_scroll, 0, LV_ANIM_OFF);
    lv_obj_remove_flag(modal_artifact, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(modal_artifact);
}

static void art_btn_cb(lv_event_t *e)
{
    art_open((int)(intptr_t)lv_event_get_user_data(e));
}

static void art_save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_open_artifact < 0) return;
    const agy_artifact_t *a = agy_client_artifact(s_open_artifact);
    if (!a || !a->name[0] || !a->text[0]) return;
    app_editor_save_plan(a->name, a->text);
}

static void art_close_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_open_artifact = -1;
    if (modal_artifact) lv_obj_add_flag(modal_artifact, LV_OBJ_FLAG_HIDDEN);
}

static void srv_save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    bool ok = false;
    if (ta_srv_host && ta_srv_port && ta_srv_token) {
        const char *h = lv_textarea_get_text(ta_srv_host);
        int port = atoi(lv_textarea_get_text(ta_srv_port));
        const char *tok = lv_textarea_get_text(ta_srv_token);
        if (tok) agy_client_set_token(tok);
        if (h && *h && port > 0) ok = agy_client_set_server(h, port) == 0;
    }
    /* bad host/port keeps the modal open instead of vanishing */
    if (ok && modal_srv) lv_obj_add_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
    refresh_all(false);
}

static void srv_cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (modal_srv) lv_obj_add_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
}

static void poll_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    agy_client_poll();
    uint32_t g = agy_client_generation();
    if (g != s_seen_gen) {
        s_seen_gen = g;
        s_dirty = true;
        devos_telemetry_t telem = *devos_telemetry_get();
        telem.agy_bridge_online = (agy_client_status() == AGY_UP);
        telem.agy_subagents_count = (uint8_t)agy_client_agent_count();
        devos_telemetry_update(&telem);
    }
    if (s_dirty && screen && !lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN) &&
        (!agy_client_busy() || lv_tick_elaps(s_last_refresh) >= REFRESH_MS ||
         agy_client_permission_pending(NULL) || agy_client_question_pending(NULL))) {
        refresh_all(false);
    }
}

static void srv_focus_paint(void)
{
    const devos_palette_t *p = devos_theme_get();
    lv_obj_t *tas[3] = {ta_srv_host, ta_srv_port, ta_srv_token};
    for (int i = 0; i < 3; i++) {
        if (tas[i]) {
            lv_obj_set_style_border_color(tas[i], (tas[i] == srv_focus) ? p->accent_primary : p->surface_border, 0);
            lv_obj_set_style_border_width(tas[i], (tas[i] == srv_focus) ? 2 : 1, 0);
        }
    }
}

static bool shown(lv_obj_t *o)
{
    return o && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static bool any_modal_open(void)
{
    return shown(modal_permission) || shown(modal_question) || shown(modal_artifact) || shown(modal_srv) ||
           shown(modal_diff) || shown(modal_hist);
}

void app_antigravity_toggle_focus(void)
{
    if (viewport) devos_agent_viewport_toggle_focus(viewport);
}

void app_antigravity_toggle_left(void)
{
    if (viewport) devos_agent_viewport_toggle_left(viewport);
}

void app_antigravity_toggle_right(void)
{
    if (viewport) devos_agent_viewport_toggle_right(viewport);
}

/* Shared line editing for the one-line text fields. */
static bool edit_key(lv_obj_t *field, uint32_t key)
{
    if (!field) return false;
    if (key == '\b' || key == 0x7F) lv_textarea_delete_char(field);
    else if (key == LV_KEY_LEFT) lv_textarea_cursor_left(field);
    else if (key == LV_KEY_RIGHT) lv_textarea_cursor_right(field);
    else if (key >= 32 && key <= 126) lv_textarea_add_char(field, (char)key);
    else return false;
    return true;
}

static bool scroll_key(lv_obj_t *sc, uint32_t key, int step)
{
    int page = lv_obj_get_height(sc) - 2 * step;
    if (key == LV_KEY_DOWN) lv_obj_scroll_by_bounded(sc, 0, -3 * step, LV_ANIM_OFF);
    else if (key == LV_KEY_UP) lv_obj_scroll_by_bounded(sc, 0, 3 * step, LV_ANIM_OFF);
    else if (key == DEVOS_KEY_PGDN || key == ' ') lv_obj_scroll_by_bounded(sc, 0, -page, LV_ANIM_OFF);
    else if (key == DEVOS_KEY_PGUP) lv_obj_scroll_by_bounded(sc, 0, page, LV_ANIM_OFF);
    else if (key == LV_KEY_RIGHT) lv_obj_scroll_by_bounded(sc, -80, 0, LV_ANIM_OFF);
    else if (key == LV_KEY_LEFT) lv_obj_scroll_by_bounded(sc, 80, 0, LV_ANIM_OFF);
    else return false;
    return true;
}

static bool antigravity_handle_key(uint32_t key, uint8_t modifiers)
{
    if ((shown(modal_permission) || shown(modal_question)) && lv_tick_elaps(s_ask_since) < ASK_GUARD_MS) {
        return true;
    }

    /* Permission modal: Y once, A always, N/Esc deny */
    if (shown(modal_permission)) {
        if (key == 'y' || key == 'Y') answer_permission(true, false);
        else if (key == 'a' || key == 'A') answer_permission(true, true);
        else if (key == 'n' || key == 'N' || key == LV_KEY_ESC) answer_permission(false, false);
        else if (shown(perm_prev)) scroll_key(perm_prev, key, DEVOS_CODEVIEW_LINE_H);
        return true;
    }

    /* Question modal: 1-4 pick, typing answers freely, Esc skips */
    if (shown(modal_question)) {
        const char *typed = lv_textarea_get_text(ta_q);
        bool empty = !typed || !*typed;
        if (key == LV_KEY_ESC) {
            answer_question(-1);
        } else if (empty && key >= '1' && key < '1' + (uint32_t)s_q_choices) {
            answer_question((int)(key - '1'));
        } else if (key == '\r' || key == '\n') {
            if (!empty) {
                agy_client_answer_question_text(typed);
                refresh_all(false);
            } else if (s_q_choices == 1) {
                answer_question(0);
            }
        } else {
            edit_key(ta_q, key);
        }
        return true;
    }

    /* Full-screen diff: Esc closes, arrows / PgUp / PgDn scroll */
    if (shown(modal_diff)) {
        if (key == LV_KEY_ESC || key == 'q' || key == 'Q') diff_full_close();
        else scroll_key(s_cv_full.scroll, key, DEVOS_CODEVIEW_LINE_H);
        return true;
    }

    /* History: arrows pick, Enter opens, R refreshes, Esc closes */
    if (shown(modal_hist)) {
        if (key == LV_KEY_ESC) hist_close();
        else if (key == LV_KEY_DOWN && s_hist_sel + 1 < s_hist_n) s_hist_sel++, hist_paint();
        else if (key == LV_KEY_UP && s_hist_sel > 0) s_hist_sel--, hist_paint();
        else if (key == '\r' || key == '\n') hist_choose(s_hist_sel);
        else if (key == 'r' || key == 'R') agy_client_list();
        else if (key >= '1' && key <= '9' && key - '1' < (uint32_t)s_hist_n) hist_choose((int)(key - '1'));
        return true;
    }

    /* Artifact viewer: Esc closes, arrows scroll */
    if (shown(modal_artifact)) {
        if (key == LV_KEY_ESC) art_close_cb(NULL);
        else scroll_key(art_scroll, key, 18);
        return true;
    }

    /* Server modal: Tab cycles the three fields, Enter saves */
    if (shown(modal_srv)) {
        if (!srv_focus) srv_focus = ta_srv_host;
        if (key == LV_KEY_ESC) {
            lv_obj_add_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
        } else if (key == '\r' || key == '\n') {
            srv_save_cb(NULL);
        } else if (key == '\t' || key == LV_KEY_DOWN || key == LV_KEY_UP) {
            if (srv_focus == ta_srv_host) srv_focus = ta_srv_port;
            else if (srv_focus == ta_srv_port) srv_focus = ta_srv_token;
            else srv_focus = ta_srv_host;
            srv_focus_paint();
        } else {
            edit_key(srv_focus, key);
        }
        return true;
    }

    if (modifiers & DEVOS_MOD_FN) {
        if (key == 'f' || key == 'F') app_antigravity_toggle_focus();
        else if (key == 'l' || key == 'L') app_antigravity_toggle_left();     /* Sym+L: left sidebar */
        else if (key == 'r' || key == 'R') app_antigravity_toggle_right();    /* Sym+R: right inspector */
        else if (key == 'n' || key == 'N') note_btn_cb(NULL);
        else if (key == 'd' || key == 'D') diff_full_open();
        else if (key == 'o' || key == 'O') hist_open();
        else return false;
        return true;
    }

    /* Main prompt input */
    if (!ta || any_modal_open()) return false;
    bool handled = true;
    if (key == '\r' || key == '\n') {
        send_current();
    } else if (key == LV_KEY_ESC) {
        const char *t = lv_textarea_get_text(ta);
        if (agy_client_busy()) agy_client_abort();
        else if (t && *t) lv_textarea_set_text(ta, "");
        else handled = false;
    } else if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        /* like a shell: walk back through earlier prompts */
        int n = agy_client_prompt_count();
        if (key == LV_KEY_UP && n > 0) {
            s_recall = s_recall < 0 ? n - 1 : (s_recall > 0 ? s_recall - 1 : 0);
        } else if (key == LV_KEY_DOWN && s_recall >= 0) {
            s_recall = s_recall + 1 < n ? s_recall + 1 : -1;
        }
        const char *t = s_recall >= 0 ? agy_client_prompt(s_recall) : "";
        char line[AGY_RECALL_MAX];
        snprintf(line, sizeof(line), "%s", t ? t : "");
        for (char *c = line; *c; c++) {
            if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';   /* one-line box */
        }
        lv_textarea_set_text(ta, line);
    } else if (key == DEVOS_KEY_PGUP || key == DEVOS_KEY_PGDN) {
        handled = scroll_key(chat_scroll, key, 20);
    } else {
        s_recall = -1;
        handled = edit_key(ta, key);
    }
    update_send_label();
    return handled;
}

/* ------------------------------------------------------------------ init */
static lv_obj_t *mk_button(lv_obj_t *parent, int w, int h, int x, int y)
{
    const devos_palette_t *p = devos_theme_get();
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, p->surface, 0);
    lv_obj_set_style_border_color(b, p->surface_border, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_pad_hor(b, 10, 0);
    lv_obj_set_style_pad_ver(b, 0, 0);
    return b;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static lv_obj_t *mk_label_btn(lv_obj_t *btn, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

static lv_obj_t *mk_modal(int w, int h, lv_color_t border)
{
    const devos_palette_t *p = devos_theme_get();
    lv_obj_t *m = lv_obj_create(screen);
    lv_obj_set_size(m, w, h);
    lv_obj_align(m, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(m, p->surface, 0);
    lv_obj_set_style_border_color(m, border, 0);
    lv_obj_set_style_border_width(m, 2, 0);
    lv_obj_set_style_radius(m, 8, 0);
    lv_obj_set_style_pad_all(m, 16, 0);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
    return m;
}

static lv_obj_t *mk_field(lv_obj_t *parent, const char *ph, int w, int x, int y)
{
    const devos_palette_t *p = devos_theme_get();
    lv_obj_t *t = lv_textarea_create(parent);
    lv_textarea_set_placeholder_text(t, ph);
    lv_textarea_set_one_line(t, true);
    lv_obj_set_size(t, w, 36);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_style_bg_color(t, p->code_bg, 0);
    lv_obj_set_style_text_color(t, p->text_primary, 0);
    lv_obj_set_style_border_color(t, p->surface_border, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_radius(t, 4, 0);
    lv_obj_set_style_pad_all(t, 8, 0);
    return t;
}

static lv_obj_t *mk_modal_button(lv_obj_t *parent, const char *text, int w, int x, int y, lv_color_t bg,
                                 lv_color_t fg, lv_event_cb_t cb, intptr_t ud, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, 36);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_set_style_text_color(l, fg, 0);
    if (lbl_out) *lbl_out = l;
    return b;
}

static void build_left(lv_obj_t *left, const devos_palette_t *p)
{
    lbl_agy_title = mk_label(left, "ANTIGRAVITY (AGY)", &lv_font_montserrat_12, p->text_secondary, 4, 4);

    sess_box = lv_obj_create(left);
    lv_obj_set_size(sess_box, DEVOS_PANE_LEFT_WIDTH - 28, 112);
    lv_obj_set_pos(sess_box, 4, 26);
    lv_obj_set_style_bg_color(sess_box, p->surface_active, 0);
    lv_obj_set_style_border_color(sess_box, p->accent_primary, 0);
    lv_obj_set_style_border_width(sess_box, 1, 0);
    lv_obj_set_style_radius(sess_box, 6, 0);
    lv_obj_set_style_pad_all(sess_box, 6, 0);
    lv_obj_remove_flag(sess_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sess_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sess_box, sess_box_cb, LV_EVENT_CLICKED, NULL);

    int w = DEVOS_PANE_LEFT_WIDTH - 50;
    lbl_conv = mk_label(sess_box, "(new conversation)", &lv_font_montserrat_14, p->accent_primary, 4, 2);
    lbl_mod = mk_label(sess_box, "", &lv_font_montserrat_12, p->text_primary, 4, 26);
    lbl_ws = mk_label(sess_box, "", &lv_font_montserrat_12, p->text_secondary, 4, 46);
    lbl_link = mk_label(sess_box, "Offline", &lv_font_montserrat_12, p->text_secondary, 4, 66);
    lv_obj_t *ls[4] = {lbl_conv, lbl_mod, lbl_ws, lbl_link};
    for (int i = 0; i < 4; i++) {
        lv_obj_set_width(ls[i], w);
        lv_label_set_long_mode(ls[i], LV_LABEL_LONG_DOT);
    }

    int bw = (DEVOS_PANE_LEFT_WIDTH - 36) / 2;
    btn_new = mk_button(left, bw, 30, 4, 146);
    lv_obj_add_event_cb(btn_new, new_btn_cb, LV_EVENT_CLICKED, NULL);
    lbl_new = lv_label_create(btn_new);
    lv_label_set_text(lbl_new, LV_SYMBOL_PLUS " New");
    lv_obj_center(lbl_new);
    lv_obj_set_style_text_font(lbl_new, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_new, p->text_primary, 0);

    btn_hist = mk_button(left, bw, 30, 12 + bw, 146);
    lv_obj_add_event_cb(btn_hist, hist_btn_cb, LV_EVENT_CLICKED, NULL);
    lbl_hist = lv_label_create(btn_hist);
    lv_label_set_text(lbl_hist, LV_SYMBOL_LIST " History");
    lv_obj_center(lbl_hist);
    lv_obj_set_style_text_font(lbl_hist, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_hist, p->text_primary, 0);

    lbl_sub = mk_label(left, "AGENTS", &lv_font_montserrat_12, p->accent_secondary, 4, 190);
    for (int i = 0; i < AGY_SUB_BTNS; i++) {
        sub_btns[i] = mk_button(left, DEVOS_PANE_LEFT_WIDTH - 28, 34, 4, 212 + i * 38);
        lv_obj_add_flag(sub_btns[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(sub_btns[i], sub_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        sub_lbls[i] = mk_label_btn(sub_btns[i], &lv_font_montserrat_12, p->text_secondary);
    }

    lbl_slash = mk_label(left, "SLASH COMMANDS", &lv_font_montserrat_12, p->text_secondary, 4, 260);
    static const char *cmds[4] = {"/goal", "/plan", "/boost", "/learn"};
    static const char *hints[4] = {"(autonomous)", "(plan first)", "(deep think)", "(save skill)"};
    for (int i = 0; i < 4; i++) {
        cmd_btns[i] = mk_button(left, DEVOS_PANE_LEFT_WIDTH - 28, 30, 4, 284 + i * 36);
        lv_obj_add_event_cb(cmd_btns[i], cmd_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        cmd_lbls[i] = mk_label_btn(cmd_btns[i], &lv_font_montserrat_12, p->accent_primary);
        char buf[48];
        snprintf(buf, sizeof(buf), "%s %s", cmds[i], hints[i]);
        lv_label_set_text(cmd_lbls[i], buf);
    }
}

static void build_center(lv_obj_t *center, const devos_palette_t *p)
{
    action_strip = lv_obj_create(center);
    lv_obj_set_size(action_strip, lv_pct(100), 28);
    lv_obj_set_pos(action_strip, 0, 0);
    lv_obj_set_style_bg_color(action_strip, p->bg_alt, 0);
    lv_obj_set_style_border_color(action_strip, p->surface_border, 0);
    lv_obj_set_style_border_width(action_strip, 1, 0);
    lv_obj_set_style_border_side(action_strip, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(action_strip, 0, 0);
    lv_obj_set_style_pad_all(action_strip, 2, 0);
    lv_obj_remove_flag(action_strip, LV_OBJ_FLAG_SCROLLABLE);

    lbl_strip = lv_label_create(action_strip);
    lv_label_set_text(lbl_strip, "");
    lv_obj_set_width(lbl_strip, lv_pct(98));
    lv_label_set_long_mode(lbl_strip, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_strip, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_text_font(lbl_strip, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_strip, p->text_secondary, 0);

    chat_scroll = lv_obj_create(center);
    lv_obj_set_size(chat_scroll, lv_pct(100), DEVOS_CONTENT_HEIGHT - 28 - 50);
    lv_obj_set_pos(chat_scroll, 0, 28);
    lv_obj_set_style_bg_color(chat_scroll, p->bg, 0);
    lv_obj_set_style_border_width(chat_scroll, 0, 0);
    lv_obj_set_style_radius(chat_scroll, 0, 0);
    lv_obj_set_style_pad_all(chat_scroll, 8, 0);
    lv_obj_set_flex_flow(chat_scroll, LV_FLEX_FLOW_COLUMN);   /* cards stack */
    lv_obj_set_style_pad_row(chat_scroll, 8, 0);
    lv_obj_set_scroll_dir(chat_scroll, LV_DIR_VER);

    input_bar = lv_obj_create(center);
    lv_obj_set_size(input_bar, lv_pct(100), 50);
    lv_obj_set_pos(input_bar, 0, DEVOS_CONTENT_HEIGHT - 50);
    lv_obj_set_style_bg_color(input_bar, p->surface, 0);
    lv_obj_set_style_border_color(input_bar, p->surface_border, 0);
    lv_obj_set_style_border_width(input_bar, 1, 0);
    lv_obj_set_style_border_side(input_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(input_bar, 0, 0);
    lv_obj_set_style_pad_all(input_bar, 6, 0);
    lv_obj_remove_flag(input_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(input_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(input_bar, 6, 0);

    ta = lv_textarea_create(input_bar);
    lv_textarea_set_placeholder_text(ta, "Prompt or /command, Enter sends (Sym+N adds the open note)");
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, AGY_PROMPT_MAX - 1);
    lv_obj_set_size(ta, 0, 38);
    lv_obj_set_flex_grow(ta, 1);
    lv_obj_set_style_pad_ver(ta, 9, 0);          /* the text line fits the 38 px box */
    lv_obj_set_style_pad_hor(ta, 10, 0);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
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
    lv_obj_set_style_text_color(lbl_send, devos_theme_is_dark() ? lv_color_black() : lv_color_white(), 0);
}

static void build_right(lv_obj_t *right, const devos_palette_t *p)
{
    lbl_insp_title = mk_label(right, "INSPECTOR (Sym+R)", &lv_font_montserrat_12, p->text_secondary, 4, 4);
    lbl_art_h = mk_label(right, "ARTIFACTS", &lv_font_montserrat_12, p->accent_primary, 4, 28);
    lbl_art_none = mk_label(right, "Plans, task lists and walkthroughs appear here.", &lv_font_montserrat_12,
                            p->text_secondary, 4, 50);
    lv_obj_set_width(lbl_art_none, DEVOS_PANE_RIGHT_WIDTH - 28);
    lv_label_set_long_mode(lbl_art_none, LV_LABEL_LONG_DOT);

    for (int i = 0; i < AGY_ART_BTNS; i++) {
        art_btns[i] = mk_button(right, DEVOS_PANE_RIGHT_WIDTH - 28, 34, 4, 52 + i * 40);
        lv_obj_add_flag(art_btns[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(art_btns[i], art_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        art_lbls[i] = mk_label_btn(art_btns[i], &lv_font_montserrat_12, p->text_primary);
    }

    lbl_diff_h = mk_label(right, "DIFF (tap to enlarge)", &lv_font_montserrat_12, p->accent_primary, 4, 84);

    btn_save_diff = lv_button_create(right);
    lv_obj_set_size(btn_save_diff, 84, 26);
    lv_obj_set_pos(btn_save_diff, DEVOS_PANE_RIGHT_WIDTH - 84 - 24, 80);
    lv_obj_set_style_bg_color(btn_save_diff, p->surface_active, 0);
    lv_obj_set_style_border_color(btn_save_diff, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_save_diff, 1, 0);
    lv_obj_set_style_radius(btn_save_diff, 4, 0);
    lv_obj_add_event_cb(btn_save_diff, save_diff_cb, LV_EVENT_CLICKED, NULL);
    lbl_save_diff = lv_label_create(btn_save_diff);
    lv_label_set_text(lbl_save_diff, LV_SYMBOL_SAVE " Save");
    lv_obj_center(lbl_save_diff);
    lv_obj_set_style_text_font(lbl_save_diff, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_save_diff, p->text_primary, 0);

    diff_scroll = lv_obj_create(right);
    lv_obj_set_pos(diff_scroll, 4, 112);
    lv_obj_set_size(diff_scroll, DEVOS_PANE_RIGHT_WIDTH - 28, DEVOS_CONTENT_HEIGHT - 112 - 10);
    lv_obj_set_style_bg_color(diff_scroll, p->code_bg, 0);
    lv_obj_set_style_border_color(diff_scroll, p->surface_border, 0);
    lv_obj_set_style_border_width(diff_scroll, 1, 0);
    lv_obj_set_style_radius(diff_scroll, 4, 0);
    lv_obj_add_event_cb(diff_scroll, diff_pane_click_cb, LV_EVENT_CLICKED, NULL);
    devos_codeview_create(&s_cv_pane, diff_scroll);
}

static void build_modals(const devos_palette_t *p)
{
    lv_color_t on_accent = devos_theme_is_dark() ? lv_color_black() : lv_color_white();

    /* Full-screen diff */
    modal_diff = lv_obj_create(screen);
    lv_obj_set_size(modal_diff, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(modal_diff, 0, 0);
    lv_obj_set_style_bg_color(modal_diff, p->bg, 0);
    lv_obj_set_style_radius(modal_diff, 0, 0);
    lv_obj_set_style_border_width(modal_diff, 0, 0);
    lv_obj_set_style_pad_all(modal_diff, 8, 0);
    lv_obj_remove_flag(modal_diff, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal_diff, LV_OBJ_FLAG_HIDDEN);
    lbl_diff_title = mk_label(modal_diff, "Diff", &lv_font_montserrat_16, p->text_primary, 4, 6);
    lv_obj_t *close = mk_modal_button(modal_diff, LV_SYMBOL_CLOSE " Close", 110, 0, 0, p->surface_active,
                                      p->text_primary, diff_close_cb, 0, NULL);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *sc = lv_obj_create(modal_diff);
    lv_obj_set_pos(sc, 0, 40);
    lv_obj_set_size(sc, DEVOS_SCREEN_WIDTH - 16, DEVOS_CONTENT_HEIGHT - 56);
    lv_obj_set_style_bg_color(sc, p->code_bg, 0);
    lv_obj_set_style_border_color(sc, p->surface_border, 0);
    lv_obj_set_style_border_width(sc, 1, 0);
    lv_obj_set_style_radius(sc, 4, 0);
    devos_codeview_create(&s_cv_full, sc);

    /* Permission: title, what the call does, a diff when it's an edit */
    modal_permission = mk_modal(780, 470, p->accent_warning);
    lbl_m_title = mk_label(modal_permission, "", &lv_font_montserrat_16, p->accent_warning, 0, 0);
    lv_obj_set_width(lbl_m_title, 744);
    lv_label_set_long_mode(lbl_m_title, LV_LABEL_LONG_DOT);
    lbl_m_desc = mk_label(modal_permission, "", &lv_font_nimbus_mono_14, p->text_primary, 0, 32);
    lv_obj_set_size(lbl_m_desc, 744, 300);
    lv_label_set_long_mode(lbl_m_desc, LV_LABEL_LONG_DOT);
    perm_prev = lv_obj_create(modal_permission);
    lv_obj_set_pos(perm_prev, 0, 78);
    lv_obj_set_size(perm_prev, 744, 300);
    lv_obj_set_style_bg_color(perm_prev, p->code_bg, 0);
    lv_obj_set_style_border_color(perm_prev, p->surface_border, 0);
    lv_obj_set_style_border_width(perm_prev, 1, 0);
    lv_obj_set_style_radius(perm_prev, 4, 0);
    lv_obj_add_flag(perm_prev, LV_OBJ_FLAG_HIDDEN);
    devos_codeview_create(&s_cv_perm, perm_prev);
    const char *pnames[3] = {"[Y] Allow once", "[N] Deny", "[A] Always allow"};
    lv_color_t pbg[3] = {p->accent_secondary, p->accent_danger, p->surface_active};
    lv_color_t pfg[3] = {on_accent, lv_color_white(), p->text_primary};
    intptr_t pans[3] = {1, 0, 2};
    for (int i = 0; i < 3; i++) {
        perm_btns[i] = mk_modal_button(modal_permission, pnames[i], 200, i * 272, 392, pbg[i], pfg[i], perm_btn_cb,
                                       pans[i], &perm_lbls[i]);
    }

    /* Question */
    modal_question = mk_modal(520, 340, p->accent_primary);
    lbl_q_title = mk_label(modal_question, LV_SYMBOL_BELL " The agent asks  (1-4 picks, Esc skips)",
                           &lv_font_montserrat_16, p->accent_primary, 0, 0);
    lbl_q_desc = mk_label(modal_question, "", &lv_font_montserrat_14, p->text_primary, 0, 30);
    lv_obj_set_size(lbl_q_desc, 484, 56);
    lv_label_set_long_mode(lbl_q_desc, LV_LABEL_LONG_DOT);
    for (int i = 0; i < AGY_Q_BTNS; i++) {
        q_btns[i] = mk_button(modal_question, 484, 34, 0, 92 + i * 38);
        lv_obj_set_style_bg_color(q_btns[i], p->surface_active, 0);
        lv_obj_add_event_cb(q_btns[i], question_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        q_lbls[i] = mk_label_btn(q_btns[i], &lv_font_montserrat_12, p->text_primary);
    }
    ta_q = mk_field(modal_question, "Type your answer, Enter sends", 484, 0, 250);
    lv_obj_set_style_border_color(ta_q, p->accent_primary, 0);

    /* Artifact viewer */
    modal_artifact = mk_modal(720, 520, p->accent_primary);
    lbl_art_title = mk_label(modal_artifact, "Artifact", &lv_font_montserrat_16, p->accent_primary, 0, 0);
    art_scroll = lv_obj_create(modal_artifact);
    lv_obj_set_size(art_scroll, 688, 400);
    lv_obj_set_pos(art_scroll, 0, 32);
    lv_obj_set_style_bg_color(art_scroll, p->code_bg, 0);
    lv_obj_set_style_border_width(art_scroll, 0, 0);
    lv_obj_set_style_pad_all(art_scroll, 10, 0);
    btn_art_save = mk_modal_button(modal_artifact, LV_SYMBOL_SAVE " Save to notes", 170, 390, 444,
                                   p->surface_active, p->text_primary, art_save_cb, 0, &lbl_art_save);
    mk_modal_button(modal_artifact, "Close (Esc)", 120, 568, 444, p->surface_active, p->text_primary, art_close_cb,
                    0, &lbl_art_close);

    /* History */
    modal_hist = mk_modal(900, 600, p->accent_primary);
    lbl_hist_title = mk_label(modal_hist, LV_SYMBOL_LIST " Conversations", &lv_font_montserrat_16, p->accent_primary,
                              0, 4);
    lv_obj_set_width(lbl_hist_title, 700);
    lv_label_set_long_mode(lbl_hist_title, LV_LABEL_LONG_DOT);
    lv_obj_t *hc = mk_modal_button(modal_hist, "Close (Esc)", 130, 738, 0, p->surface_active, p->text_primary,
                                   hist_close_cb, 0, NULL);
    LV_UNUSED(hc);
    mk_label(modal_hist, "Up/Down + Enter or tap to open  -  1-9 quick pick  -  R refresh", &lv_font_montserrat_12,
             p->text_secondary, 0, 30);
    hist_list = lv_obj_create(modal_hist);
    lv_obj_set_pos(hist_list, 0, 54);
    lv_obj_set_size(hist_list, 868, 600 - 32 - 54);
    lv_obj_set_style_bg_opa(hist_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hist_list, 0, 0);
    lv_obj_set_style_pad_all(hist_list, 2, 0);
    lv_obj_set_style_pad_row(hist_list, 6, 0);
    lv_obj_set_flex_flow(hist_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(hist_list, LV_DIR_VER);

    /* Server (host / port / token) */
    modal_srv = mk_modal(500, 280, p->accent_primary);
    lbl_srv_title = mk_label(modal_srv, LV_SYMBOL_SETTINGS " Connect to your Antigravity bridge",
                             &lv_font_montserrat_16, p->accent_primary, 0, 0);
    lbl_srv_hint = mk_label(modal_srv,
                            "The computer running bridge_server.py: its address (Tailscale name, LAN name or IP), "
                            "the port (8420 unless you chose --port) and the token it printed. "
                            "Tab moves between fields, Enter connects.",
                            &lv_font_montserrat_12, p->text_secondary, 0, 26);
    lv_obj_set_width(lbl_srv_hint, 468);
    lv_label_set_long_mode(lbl_srv_hint, LV_LABEL_LONG_WRAP);
    ta_srv_host = mk_field(modal_srv, "address, e.g. my-pc.tail1234.ts.net or 192.168.1.20", 468, 0, 70);
    ta_srv_port = mk_field(modal_srv, "port (8420)", 140, 0, 114);
    ta_srv_token = mk_field(modal_srv, "bridge token", 320, 148, 114);
    mk_modal_button(modal_srv, LV_SYMBOL_OK " Connect", 130, 208, 196, p->accent_primary, on_accent, srv_save_cb,
                    0, &lbl_srv_save);
    lv_obj_t *cancel = mk_modal_button(modal_srv, "Cancel", 110, 350, 196, p->surface, p->text_primary,
                                       srv_cancel_cb, 0, &lbl_srv_cancel);
    lv_obj_set_style_border_color(cancel, p->surface_border, 0);
    lv_obj_set_style_border_width(cancel, 1, 0);
}

static void antigravity_init(void)
{
    const devos_palette_t *p = devos_theme_get();
    agy_client_init();

    screen = lv_obj_create(lv_screen_active());
    app_descriptor.screen = screen;
    lv_obj_set_size(screen, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(screen, 0, DEVOS_TOP_BAR_HEIGHT);
    lv_obj_set_style_bg_color(screen, p->bg, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);

    viewport = devos_agent_viewport_create(screen);
    build_left(devos_agent_viewport_get_left(viewport), p);
    build_center(devos_agent_viewport_get_center(viewport), p);
    build_right(devos_agent_viewport_get_right(viewport), p);
    build_modals(p);

    devos_theme_add_listener(apply_theme, NULL);
    poll_timer = lv_timer_create(poll_cb, 100, NULL);
    refresh_all(true);
}

static bool s_setup_prompted = false;

static void antigravity_show(void)
{
    refresh_all(false);
    /* first visit without a bridge: go straight to the connection dialog */
    if (!agy_client_configured() && !s_setup_prompted) {
        s_setup_prompted = true;
        sess_box_cb(NULL);
    }
}

static void antigravity_hide(void)
{
    /* pending asks stay pending: they reappear when the app is opened */
    lv_obj_t *ms[6] = {modal_permission, modal_question, modal_artifact, modal_srv, modal_diff, modal_hist};
    for (int i = 0; i < 6; i++) {
        if (ms[i]) lv_obj_add_flag(ms[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_shown_perm[0] = '\0';
    s_shown_q[0] = '\0';
    s_open_artifact = -1;
}

static int antigravity_telemetry_lines(char lines[3][64])
{
    if (!agy_client_configured()) {
        snprintf(lines[0], sizeof(lines[0]), "* Not set up yet");
        snprintf(lines[1], sizeof(lines[1]), "* Open to connect a computer");
        snprintf(lines[2], sizeof(lines[2]), "* running agy + the bridge");
        return 3;
    }
    snprintf(lines[0], sizeof(lines[0]), "* Bridge: %s%s", agy_client_status() == AGY_UP ? "online" : "offline",
             agy_client_busy() ? " (working)" : "");
    if (agy_client_permission_pending(NULL) || agy_client_question_pending(NULL)) {
        snprintf(lines[1], sizeof(lines[1]), "* Waiting for you");
    } else {
        snprintf(lines[1], sizeof(lines[1]), "* Agents: %d", agy_client_agent_count());
    }
    snprintf(lines[2], sizeof(lines[2]), "* %s", agy_client_model()[0] ? agy_client_model() : "native client");
    return 3;
}

devos_app_descriptor_t *app_antigravity_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_ANTIGRAVITY;
    app_descriptor.uid = "antigravity";
    app_descriptor.icon = LV_SYMBOL_SHUFFLE;
    app_descriptor.category = "agents";
    app_descriptor.name = "Antigravity";
    app_descriptor.title = "Google Antigravity";
    app_descriptor.subtitle = "agy on your computer, driven from here";
    app_descriptor.screen = screen;
    app_descriptor.init = antigravity_init;
    app_descriptor.show = antigravity_show;
    app_descriptor.hide = antigravity_hide;
    app_descriptor.handle_key = antigravity_handle_key;
    app_descriptor.get_telemetry_lines = antigravity_telemetry_lines;

    return &app_descriptor;
}
