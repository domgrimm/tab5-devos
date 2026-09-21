#include "app_antigravity.h"
#include "devos_config.h"
#include "devos_theme.h"
#include "devos_agent_viewport.h"
#include "devos_mdview.h"
#include "apps/app_editor/app_editor.h"
#include "agy_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_nimbus_mono_14);

#define AGY_SUB_BTNS AGY_MAX_AGENTS
#define AGY_ART_BTNS AGY_MAX_ARTIFACTS
#define AGY_Q_BTNS 4

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;
static devos_agent_viewport_t *viewport = NULL;

/* Thinking accordion + focus */
static bool thinking_expanded = true;

/* Left: session + subagents + slash */
static lv_obj_t *lbl_agy_title = NULL;
static lv_obj_t *sess_box = NULL;
static lv_obj_t *lbl_conv = NULL;
static lv_obj_t *lbl_mod = NULL;
static lv_obj_t *lbl_link = NULL;
static lv_obj_t *btn_new = NULL;
static lv_obj_t *lbl_new = NULL;
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
static lv_obj_t *art_btns[AGY_ART_BTNS] = {NULL};
static lv_obj_t *art_lbls[AGY_ART_BTNS] = {NULL};
static lv_obj_t *lbl_diff_h = NULL;
static lv_obj_t *btn_save_diff = NULL;
static lv_obj_t *lbl_save_diff = NULL;
static lv_obj_t *diff_scroll = NULL;

/* Permission modal */
static lv_obj_t *modal_permission = NULL;
static lv_obj_t *lbl_m_title = NULL;
static lv_obj_t *lbl_m_desc = NULL;
static lv_obj_t *perm_btns[3] = {NULL};
static lv_obj_t *perm_lbls[3] = {NULL};

/* Question modal */
static lv_obj_t *modal_question = NULL;
static lv_obj_t *lbl_q_title = NULL;
static lv_obj_t *lbl_q_desc = NULL;
static lv_obj_t *q_btns[AGY_Q_BTNS] = {NULL};
static lv_obj_t *q_lbls[AGY_Q_BTNS] = {NULL};

/* Artifact viewer modal */
static lv_obj_t *modal_artifact = NULL;
static lv_obj_t *lbl_art_title = NULL;
static lv_obj_t *art_scroll = NULL;
static lv_obj_t *btn_art_save = NULL;
static lv_obj_t *lbl_art_save = NULL;
static lv_obj_t *lbl_art_close = NULL;
static int s_open_artifact = -1;

/* Server modal */
static lv_obj_t *modal_srv = NULL;
static lv_obj_t *lbl_srv_title = NULL;
static lv_obj_t *ta_srv_host = NULL;
static lv_obj_t *ta_srv_port = NULL;
static lv_obj_t *ta_srv_token = NULL;
static lv_obj_t *srv_focus = NULL;
static lv_obj_t *lbl_srv_save = NULL;
static lv_obj_t *lbl_srv_cancel = NULL;

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

    if (lbl_agy_title) {
        lv_obj_set_style_text_color(lbl_agy_title, p->text_secondary, 0);
    }
    if (sess_box) {
        lv_obj_set_style_bg_color(sess_box, p->surface_active, 0);
        lv_obj_set_style_border_color(sess_box, p->accent_primary, 0);
    }
    if (lbl_conv) lv_obj_set_style_text_color(lbl_conv, p->accent_primary, 0);
    if (lbl_mod) lv_obj_set_style_text_color(lbl_mod, p->text_primary, 0);
    if (lbl_link) lv_obj_set_style_text_color(lbl_link, p->text_secondary, 0);
    if (btn_new) {
        lv_obj_set_style_bg_color(btn_new, p->surface, 0);
        lv_obj_set_style_border_color(btn_new, p->surface_border, 0);
    }
    if (lbl_new) lv_obj_set_style_text_color(lbl_new, p->text_primary, 0);
    if (lbl_sub) lv_obj_set_style_text_color(lbl_sub, p->accent_secondary, 0);
    for (int i = 0; i < AGY_SUB_BTNS; i++) {
        if (sub_btns[i]) {
            lv_obj_set_style_bg_color(sub_btns[i], p->surface, 0);
            lv_obj_set_style_border_color(sub_btns[i], p->surface_border, 0);
        }
        if (sub_lbls[i]) {
            lv_obj_set_style_text_color(sub_lbls[i], p->text_secondary, 0);
        }
    }
    if (lbl_slash) {
        lv_obj_set_style_text_color(lbl_slash, p->text_secondary, 0);
    }
    for (int i = 0; i < 4; i++) {
        if (cmd_btns[i]) {
            lv_obj_set_style_bg_color(cmd_btns[i], p->surface, 0);
            lv_obj_set_style_border_color(cmd_btns[i], p->surface_border, 0);
        }
        if (cmd_lbls[i]) {
            lv_obj_set_style_text_color(cmd_lbls[i], p->accent_primary, 0);
        }
    }

    if (action_strip) {
        lv_obj_set_style_bg_color(action_strip, p->bg_alt, 0);
        lv_obj_set_style_border_color(action_strip, p->surface_border, 0);
    }
    if (lbl_strip) lv_obj_set_style_text_color(lbl_strip, p->text_secondary, 0);
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

    if (lbl_insp_title) {
        lv_obj_set_style_text_color(lbl_insp_title, p->text_secondary, 0);
    }
    if (lbl_art_h) lv_obj_set_style_text_color(lbl_art_h, p->accent_primary, 0);
    for (int i = 0; i < AGY_ART_BTNS; i++) {
        if (art_btns[i]) {
            lv_obj_set_style_bg_color(art_btns[i], p->surface, 0);
            lv_obj_set_style_border_color(art_btns[i], p->surface_border, 0);
        }
        if (art_lbls[i]) {
            lv_obj_set_style_text_color(art_lbls[i], p->text_primary, 0);
        }
    }
    if (lbl_diff_h) {
        lv_obj_set_style_text_color(lbl_diff_h, p->accent_primary, 0);
    }
    if (btn_save_diff) {
        lv_obj_set_style_bg_color(btn_save_diff, p->surface_active, 0);
        lv_obj_set_style_border_color(btn_save_diff, p->surface_border, 0);
    }
    if (lbl_save_diff) {
        lv_obj_set_style_text_color(lbl_save_diff, p->text_primary, 0);
    }
    if (diff_scroll) {
        lv_obj_set_style_bg_color(diff_scroll, p->code_bg, 0);
        lv_obj_set_style_border_color(diff_scroll, p->surface_border, 0);
    }

    if (modal_permission) {
        lv_obj_set_style_bg_color(modal_permission, p->surface, 0);
        lv_obj_set_style_border_color(modal_permission, p->accent_warning, 0);
    }
    if (lbl_m_title) {
        lv_obj_set_style_text_color(lbl_m_title, p->accent_warning, 0);
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
                       : i == 2 ? p->text_primary : on_accent,
                0);
        }
    }
    if (modal_question) {
        lv_obj_set_style_bg_color(modal_question, p->surface, 0);
        lv_obj_set_style_border_color(modal_question, p->accent_primary, 0);
    }
    if (lbl_q_title) {
        lv_obj_set_style_text_color(lbl_q_title, p->accent_primary, 0);
    }
    for (int i = 0; i < AGY_Q_BTNS; i++) {
        if (q_btns[i]) {
            lv_obj_set_style_bg_color(q_btns[i], p->surface_active, 0);
            lv_obj_set_style_border_color(q_btns[i], p->surface_border, 0);
        }
        if (q_lbls[i]) {
            lv_obj_set_style_text_color(q_lbls[i], p->text_primary, 0);
        }
    }
    if (modal_artifact) {
        lv_obj_set_style_bg_color(modal_artifact, p->surface, 0);
        lv_obj_set_style_border_color(modal_artifact, p->accent_primary, 0);
    }
    if (lbl_art_title) {
        lv_obj_set_style_text_color(lbl_art_title, p->accent_primary, 0);
    }
    if (art_scroll) lv_obj_set_style_bg_color(art_scroll, p->code_bg, 0);
    /* ponytail: re-render an open artifact so its colors follow the theme */
    if (modal_artifact && !lv_obj_has_flag(modal_artifact, LV_OBJ_FLAG_HIDDEN) &&
        s_open_artifact >= 0 && art_scroll) {
        const agy_artifact_t *a = agy_client_artifact(s_open_artifact);
        if (a) {
            lv_obj_clean(art_scroll);
            devos_md_render(art_scroll, a->text);
        }
    }
    if (btn_art_save) {
        lv_obj_set_style_bg_color(btn_art_save, p->surface_active, 0);
        lv_obj_set_style_border_color(btn_art_save, p->surface_border, 0);
    }
    if (lbl_art_save) {
        lv_obj_set_style_text_color(lbl_art_save, p->text_primary, 0);
    }
    if (lbl_art_close) {
        lv_obj_set_style_text_color(lbl_art_close, p->text_primary, 0);
    }
    if (modal_srv) {
        lv_obj_set_style_bg_color(modal_srv, p->surface, 0);
        lv_obj_set_style_border_color(modal_srv, p->accent_primary, 0);
    }
    if (lbl_srv_title) {
        lv_obj_set_style_text_color(lbl_srv_title, p->accent_primary, 0);
    }
    if (ta_srv_host) {
        lv_obj_set_style_bg_color(ta_srv_host, p->code_bg, 0);
        lv_obj_set_style_text_color(ta_srv_host, p->text_primary, 0);
    }
    if (ta_srv_port) {
        lv_obj_set_style_bg_color(ta_srv_port, p->code_bg, 0);
        lv_obj_set_style_text_color(ta_srv_port, p->text_primary, 0);
    }
    if (ta_srv_token) {
        lv_obj_set_style_bg_color(ta_srv_token, p->code_bg, 0);
        lv_obj_set_style_text_color(ta_srv_token, p->text_primary, 0);
    }
    if (lbl_srv_save) lv_obj_set_style_text_color(lbl_srv_save, on_accent, 0);
    if (lbl_srv_cancel) {
        lv_obj_set_style_text_color(lbl_srv_cancel, p->text_primary, 0);
    }
    srv_focus_paint();

    refresh_all();
}

/* ---------------------------------------------------------------- refresh */
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

static void chat_text(lv_obj_t *card, const char *text, lv_color_t color,
                      const lv_font_t *font)
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
    refresh_all();
}

static void render_diff_view(const char *raw_diff, const devos_palette_t *p)
{
    if (!diff_scroll) return;
    lv_obj_clean(diff_scroll);
    if (!raw_diff || !*raw_diff || strcmp(raw_diff, "(no changes)") == 0 ||
        strcmp(raw_diff, "(diff unavailable)") == 0) {
        lv_obj_t *lbl = lv_label_create(diff_scroll);
        lv_label_set_text(lbl, (raw_diff && *raw_diff) ? raw_diff : "(no changes)");
        lv_obj_set_style_text_color(lbl, p->text_secondary, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        return;
    }

    const char *line = raw_diff;
    while (*line) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        if (len > 0) {
            char line_buf[256];
            size_t take = len < sizeof(line_buf) - 1 ? len : sizeof(line_buf) - 1;
            memcpy(line_buf, line, take);
            line_buf[take] = '\0';

            lv_obj_t *lbl = lv_label_create(diff_scroll);
            lv_label_set_text(lbl, line_buf);
            lv_obj_set_width(lbl, lv_pct(100));
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(lbl, &lv_font_nimbus_mono_14, 0);

            if (line_buf[0] == '+' && line_buf[1] != '+') {
                lv_obj_set_style_text_color(lbl, p->accent_secondary, 0);
            } else if (line_buf[0] == '-' && line_buf[1] != '-') {
                lv_obj_set_style_text_color(lbl, p->accent_danger, 0);
            } else if (line_buf[0] == '@' && line_buf[1] == '@') {
                lv_obj_set_style_text_color(lbl, p->accent_primary, 0);
            } else if (strncmp(line_buf, "diff ", 5) == 0 ||
                       strncmp(line_buf, "index ", 6) == 0 ||
                       strncmp(line_buf, "---", 3) == 0 ||
                       strncmp(line_buf, "+++", 3) == 0) {
                lv_obj_set_style_text_color(lbl, p->text_secondary, 0);
            } else {
                lv_obj_set_style_text_color(lbl, p->text_primary, 0);
            }
        }
        line = next ? next + 1 : line + len;
    }
}

static void refresh_all(void)
{
    if (!screen) return;
    const devos_palette_t *p = devos_theme_get();
    char buf[256];

    /* Left: conversation card */
    const char *conv = agy_client_conversation_id();
    const char *model = agy_client_model();
    if (lbl_conv) {
        snprintf(buf, sizeof(buf), "%.16s",
                 conv[0] ? conv : "(no session)");
        lv_label_set_text(lbl_conv, buf);
    }
    if (lbl_mod) {
        lv_label_set_text(lbl_mod, model[0] ? model : "agy-bridge");
    }
    if (lbl_link) {
        const char *st = agy_client_status_text();
        lv_label_set_text(lbl_link, agy_client_busy() ? "busy" : st);
        lv_obj_set_style_text_color(lbl_link,
            agy_client_status() == AGY_UP ? p->accent_secondary
                                          : p->text_secondary,
            0);
    }

    /* Left: subagents (live states) */
    int na = agy_client_agent_count();
    for (int i = 0; i < AGY_SUB_BTNS; i++) {
        if (!sub_btns[i]) continue;
        if (i >= na) {
            lv_obj_add_flag(sub_btns[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(sub_btns[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(sub_btns[i], 4, 208 + i * 40);
        const agy_agent_t *a = agy_client_agent(i);
        snprintf(buf, sizeof(buf), "%s %s (%s)",
                 strcmp(a->state, "running") == 0 ? LV_SYMBOL_BULLET : "-",
                 a->name, a->state);
        lv_label_set_text(sub_lbls[i], buf);
        lv_obj_set_style_text_color(sub_lbls[i],
            strcmp(a->state, "running") == 0 ? p->accent_secondary
                                             : p->text_secondary,
            0);
    }

    /* Center: status strip */
    if (lbl_strip) {
        snprintf(buf, sizeof(buf),
                 "Bridge: %s%s  |  Fn+F Focus  |  Fn+[ Left  |  Fn+] Right",
                 agy_client_status_text(),
                 agy_client_busy() ? " (working)" : "");
        lv_label_set_text(lbl_strip, buf);
    }

    /* Center: chat blocks in chronological order */
    if (chat_scroll) {
        lv_obj_clean(chat_scroll);
        int nb = agy_client_block_count();
        for (int i = 0; i < nb;) {
            const agy_block_t *b = agy_client_block(i);
            if (!b) { i++; continue; }
            if (b->kind == AGY_KIND_THINK) {
                lv_obj_t *box = chat_card(p, p->thinking_border);
                lv_obj_set_style_bg_color(box, p->thinking_bg, 0);
                lv_obj_add_event_cb(box, toggle_thinking_cb, LV_EVENT_CLICKED,
                                    NULL);
                lv_obj_t *arrow = lv_label_create(box);
                lv_label_set_text(arrow, thinking_expanded
                                  ? LV_SYMBOL_DOWN " Thinking"
                                  : LV_SYMBOL_RIGHT " Thinking (collapsed)");
                lv_obj_set_style_text_color(arrow, p->accent_primary, 0);
                lv_obj_set_style_text_font(arrow, &lv_font_montserrat_14, 0);
                if (thinking_expanded) {
                    lv_obj_t *body = lv_obj_create(box);
                    lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
                    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
                    lv_obj_set_style_border_width(body, 0, 0);
                    lv_obj_set_style_pad_all(body, 0, 0);
                    while (i < nb && agy_client_block(i) &&
                           agy_client_block(i)->kind == AGY_KIND_THINK) {
                        chat_text(body, agy_client_block(i)->text,
                                  p->text_secondary,
                                  &lv_font_montserrat_12);
                        i++;
                    }
                } else {
                    while (i < nb && agy_client_block(i) &&
                           agy_client_block(i)->kind == AGY_KIND_THINK) {
                        i++;
                    }
                }
                continue;
            }
            if (b->kind == AGY_KIND_TOOL) {
                lv_obj_t *card = chat_card(p, p->tool_card_border);
                lv_obj_set_style_bg_color(card, p->tool_card_bg, 0);
                chat_text(card, b->text, p->accent_secondary,
                          &lv_font_montserrat_12);
            } else {
                bool user = b->role == AGY_ROLE_USER;
                lv_obj_t *card = chat_card(p, user ? p->accent_primary
                                                  : p->surface_border);
                if (user) {
                    lv_obj_set_style_bg_color(card, p->surface_active, 0);
                    chat_text(card, b->text, p->text_primary,
                              &lv_font_montserrat_14);
                } else {
                    /* ponytail: agent output renders through the shared
                     * markdown engine; measured inner container + explicit height */
                    lv_obj_update_layout(card);
                    lv_obj_t *inner = lv_obj_create(card);
                    lv_obj_set_size(inner, lv_pct(100), LV_SIZE_CONTENT);
                    lv_obj_set_style_bg_opa(inner, LV_OPA_TRANSP, 0);
                    lv_obj_set_style_border_width(inner, 0, 0);
                    lv_obj_set_style_pad_all(inner, 0, 0);
                    lv_obj_update_layout(inner);
                    int endy = devos_md_render(inner, b->text);
                    if (endy <= 0) {
                        lv_obj_delete(inner);
                        chat_text(card, b->text, p->text_primary,
                                  &lv_font_montserrat_14);
                    } else {
                        lv_obj_set_height(inner, endy);
                        lv_obj_set_height(card, endy + 20);
                    }
                }
            }
            i++;
        }
        if (nb == 0) {
            lv_obj_t *card = chat_card(p, p->surface_border);
            const char *hint =
                agy_client_status() == AGY_UP
                ? "Bridge live. Type below or tap a /command."
                : "No bridge link. Tap ANTGRAVITY (AGY) card or fix the "
                  "server row, or start tools/agy_bridge/bridge_server.py "
                  "--demo on your workstation.";
            chat_text(card, hint, p->text_secondary, &lv_font_montserrat_14);
        }
        lv_obj_scroll_to_y(chat_scroll, LV_COORD_MAX, LV_ANIM_OFF);
    }

    /* Right: artifacts */
    int nart = agy_client_artifact_count();
    for (int i = 0; i < AGY_ART_BTNS; i++) {
        if (!art_btns[i]) continue;
        if (i >= nart) {
            lv_obj_add_flag(art_btns[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(art_btns[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(art_btns[i], 4, 56 + i * 40);
        const agy_artifact_t *a = agy_client_artifact(i);
        snprintf(buf, sizeof(buf), "%s %s", LV_SYMBOL_FILE, a->name);
        lv_label_set_text(art_lbls[i], buf);
    }
    render_diff_view(agy_client_diff_text(), p);

    /* Permission modal */
    agy_permission_t perm;
    if (agy_client_permission_pending(&perm)) {
        if (modal_permission) {
            snprintf(buf, sizeof(buf), "Approve tool?\n%s", perm.text);
            lv_label_set_text(lbl_m_desc, buf);
            lv_obj_remove_flag(modal_permission, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(modal_permission);
        }
    } else if (modal_permission) {
        lv_obj_add_flag(modal_permission, LV_OBJ_FLAG_HIDDEN);
    }

    /* Question modal */
    agy_question_t q;
    if (agy_client_question_pending(&q)) {
        if (modal_question) {
            lv_label_set_text(lbl_q_desc, q.prompt);
            for (int i = 0; i < AGY_Q_BTNS; i++) {
                if (!q_btns[i]) continue;
                if (i >= q.choice_count) {
                    lv_obj_add_flag(q_btns[i], LV_OBJ_FLAG_HIDDEN);
                    continue;
                }
                lv_obj_remove_flag(q_btns[i], LV_OBJ_FLAG_HIDDEN);
                snprintf(buf, sizeof(buf), "[%d] %s", i + 1,
                         q.choices[i]);
                lv_label_set_text(q_lbls[i], buf);
            }
            lv_obj_remove_flag(modal_question, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(modal_question);
        }
    } else if (modal_question) {
        lv_obj_add_flag(modal_question, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------------------------------------------------------- events */
static void send_current(void)
{
    if (!ta) return;
    const char *t = lv_textarea_get_text(ta);
    if (!t || !*t) return;
    /* ponytail: leading /word becomes the slash command */
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
            /* bare "/goal" sends literally (keep command intact) */
            if (!*body) {
                body = t;
            }
        }
    }
    agy_client_send(body, cmd[0] ? cmd : NULL);
    lv_textarea_set_text(ta, "");
    refresh_all();
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
        char prefix[AGY_BLOCK_MAX];
        snprintf(prefix, sizeof(prefix), "[Note: %s]\n%s\n\n", fn, txt);
        lv_textarea_set_text(ta, prefix);
        lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
    }
}

static void save_diff_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    const char *dt = agy_client_diff_text();
    if (!dt || !*dt || strcmp(dt, "(diff unavailable)") == 0 ||
        strcmp(dt, "(no changes)") == 0) {
        return;
    }
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
    }
}

static void new_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    agy_client_new();
    refresh_all();
}

static void sub_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const agy_agent_t *a = agy_client_agent(idx);
    /* ponytail: dead buttons confuse; a tap addresses the agent instead */
    if (a && ta) {
        lv_textarea_add_text(ta, "@");
        lv_textarea_add_text(ta, a->name);
        lv_textarea_add_char(ta, ' ');
    }
}

static void sess_box_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    /* ponytail: the card doubles as the server/PSk editor entry point */
    if (modal_srv && ta_srv_host && ta_srv_port && ta_srv_token) {
        char host[AGY_HOST_MAX];
        int port = 0;
        char token[AGY_TOKEN_MAX];
        agy_client_get_config(host, sizeof(host), &port, token,
                              sizeof(token));
        lv_textarea_set_text(ta_srv_host, host);
        char pb[16];
        snprintf(pb, sizeof(pb), "%d", port);
        lv_textarea_set_text(ta_srv_port, pb);
        lv_textarea_set_text(ta_srv_token, token);
        srv_focus = ta_srv_host;
        srv_focus_paint();
        lv_obj_remove_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(modal_srv);
    }
}

static void perm_btn_cb(lv_event_t *e)
{
    int ans = (int)(intptr_t)lv_event_get_user_data(e);
    /* ponytail: 1 = once (Y), 2 = always (A), 0 = reject (N) */
    agy_client_answer_permission(ans > 0, ans == 2);
    refresh_all();
}

static void question_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    agy_client_answer_question(idx);
    refresh_all();
}

static void art_open(int idx)
{
    const agy_artifact_t *a = agy_client_artifact(idx);
    if (!a || !modal_artifact || !art_scroll) return;
    s_open_artifact = idx;
    if (lbl_art_title) lv_label_set_text(lbl_art_title, a->name);
    /* ponytail: artifacts render through the shared markdown engine */
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
        if (h && *h && port > 0) {
            ok = agy_client_set_server(h, port) == 0;
        }
        if (tok) agy_client_set_token(tok);
    }
    /* ponytail: bad host/port keeps the modal open instead of vanishing */
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
    agy_client_poll();
    uint32_t g = agy_client_generation();
    if (g != s_seen_gen) {
        s_seen_gen = g;
        devos_telemetry_t telem = *devos_telemetry_get();
        telem.agy_bridge_online = (agy_client_status() == AGY_UP);
        telem.agy_subagents_count = (uint8_t)agy_client_agent_count();
        devos_telemetry_update(&telem);
        if (screen && !lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN)) {
            refresh_all();
        }
    }
}

static void srv_focus_paint(void)
{
    const devos_palette_t *p = devos_theme_get();
    lv_obj_t *tas[3] = {ta_srv_host, ta_srv_port, ta_srv_token};
    for (int i = 0; i < 3; i++) {
        if (tas[i]) {
            lv_obj_set_style_border_color(tas[i],
                (tas[i] == srv_focus) ? p->accent_primary
                                      : p->surface_border,
                0);
        }
    }
}

/* ----------------------------------------------------------------- input */
static bool any_modal_open(void)
{
    return (modal_permission &&
            !lv_obj_has_flag(modal_permission, LV_OBJ_FLAG_HIDDEN)) ||
           (modal_question &&
            !lv_obj_has_flag(modal_question, LV_OBJ_FLAG_HIDDEN)) ||
           (modal_artifact &&
            !lv_obj_has_flag(modal_artifact, LV_OBJ_FLAG_HIDDEN)) ||
           (modal_srv && !lv_obj_has_flag(modal_srv, LV_OBJ_FLAG_HIDDEN));
}

void app_antigravity_toggle_focus(void)
{
    if (viewport) {
        devos_agent_viewport_toggle_focus(viewport);
    }
}

void app_antigravity_toggle_left(void)
{
    if (viewport) {
        devos_agent_viewport_toggle_left(viewport);
    }
}

void app_antigravity_toggle_right(void)
{
    if (viewport) {
        devos_agent_viewport_toggle_right(viewport);
    }
}

static bool antigravity_handle_key(uint32_t key, uint8_t modifiers)
{
    if (modifiers & DEVOS_MOD_FN) {
        if (key == 'f' || key == 'F') {
            app_antigravity_toggle_focus();
            return true;
        } else if (key == '[') {
            app_antigravity_toggle_left();
            return true;
        } else if (key == ']') {
            app_antigravity_toggle_right();
            return true;
        } else if (key == 'n' || key == 'N') {
            note_btn_cb(NULL);
            return true;
        }
    }

    /* Permission modal: Y once, A always, N/Esc reject */
    if (modal_permission &&
        !lv_obj_has_flag(modal_permission, LV_OBJ_FLAG_HIDDEN)) {
        if (key == 'y' || key == 'Y') {
            agy_client_answer_permission(true, false);
            refresh_all();
            return true;
        }
        if (key == 'a' || key == 'A') {
            agy_client_answer_permission(true, true);
            refresh_all();
            return true;
        }
        if (key == 'n' || key == 'N' || key == LV_KEY_ESC) {
            agy_client_answer_permission(false, false);
            refresh_all();
            return true;
        }
        return true;
    }

    /* Question modal: 1-4 pick, Esc picks the first */
    if (modal_question &&
        !lv_obj_has_flag(modal_question, LV_OBJ_FLAG_HIDDEN)) {
        if (key >= '1' && key <= '4') {
            agy_client_answer_question((int)(key - '1'));
            refresh_all();
            return true;
        }
        if (key == LV_KEY_ESC) {
            agy_client_answer_question(0);
            refresh_all();
            return true;
        }
        return true;
    }

    /* Artifact viewer: Esc closes */
    if (modal_artifact &&
        !lv_obj_has_flag(modal_artifact, LV_OBJ_FLAG_HIDDEN)) {
        if (key == LV_KEY_ESC) {
            lv_obj_add_flag(modal_artifact, LV_OBJ_FLAG_HIDDEN);
            return true;
        }
        return true;
    }

    /* Server modal: Tab cycles the three fields, Enter saves */
    if (modal_srv && !lv_obj_has_flag(modal_srv, LV_OBJ_FLAG_HIDDEN)) {
        if (!srv_focus) srv_focus = ta_srv_host;
        if (key == LV_KEY_ESC) {
            lv_obj_add_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
            return true;
        }
        if (key == '\r' || key == '\n') {
            srv_save_cb(NULL);
            return true;
        }
        if (key == '\t') {
            if (srv_focus == ta_srv_host) srv_focus = ta_srv_port;
            else if (srv_focus == ta_srv_port) srv_focus = ta_srv_token;
            else srv_focus = ta_srv_host;
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

    /* Main prompt input */
    if (!ta || any_modal_open()) return false;
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
    return b;
}

static lv_obj_t *mk_label_btn(lv_obj_t *btn, const lv_font_t *font,
                              lv_color_t color)
{
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
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
    lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
    return m;
}

static lv_obj_t *mk_field(lv_obj_t *parent, const char *ph, int w, int x,
                          int y)
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

static void antigravity_init(void)
{
    const devos_palette_t *p = devos_theme_get();
    agy_client_init();

    /* Screen root container */
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

    /* Create shared responsive Tri-Pane Viewport */
    viewport = devos_agent_viewport_create(screen);

    lv_obj_t *left_panel = devos_agent_viewport_get_left(viewport);
    lv_obj_t *center_panel = devos_agent_viewport_get_center(viewport);
    lv_obj_t *right_panel = devos_agent_viewport_get_right(viewport);

    /* 1. Left: session card (tap = server/PSK editor) */
    lbl_agy_title = lv_label_create(left_panel);
    lv_label_set_text(lbl_agy_title, "ANTIGRAVITY (AGY)");
    lv_obj_set_pos(lbl_agy_title, 4, 4);
    lv_obj_set_style_text_font(lbl_agy_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_agy_title, p->text_secondary, 0);

    sess_box = lv_obj_create(left_panel);
    lv_obj_set_size(sess_box, DEVOS_PANE_LEFT_WIDTH - 28, 96);
    lv_obj_set_pos(sess_box, 4, 26);
    lv_obj_set_style_bg_color(sess_box, p->surface_active, 0);
    lv_obj_set_style_border_color(sess_box, p->accent_primary, 0);
    lv_obj_set_style_border_width(sess_box, 1, 0);
    lv_obj_set_style_radius(sess_box, 6, 0);
    lv_obj_set_style_pad_all(sess_box, 6, 0);
    lv_obj_clear_flag(sess_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sess_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sess_box, sess_box_cb, LV_EVENT_CLICKED, NULL);

    lbl_conv = lv_label_create(sess_box);
    lv_label_set_text(lbl_conv, "(no session)");
    lv_obj_set_pos(lbl_conv, 4, 2);
    lv_obj_set_style_text_font(lbl_conv, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_conv, p->accent_primary, 0);

    lbl_mod = lv_label_create(sess_box);
    lv_label_set_text(lbl_mod, "agy-bridge");
    lv_obj_set_pos(lbl_mod, 4, 26);
    lv_obj_set_style_text_font(lbl_mod, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_mod, p->text_primary, 0);

    lbl_link = lv_label_create(sess_box);
    lv_label_set_text(lbl_link, "Offline");
    lv_obj_set_pos(lbl_link, 4, 48);
    lv_obj_set_style_text_font(lbl_link, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_link, p->text_secondary, 0);

    btn_new = mk_button(left_panel, 120, 28, 4, 130);
    lv_obj_add_event_cb(btn_new, new_btn_cb, LV_EVENT_CLICKED, NULL);
    lbl_new = lv_label_create(btn_new);
    lv_label_set_text(lbl_new, LV_SYMBOL_PLUS " New");
    lv_obj_center(lbl_new);
    lv_obj_set_style_text_font(lbl_new, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_new, p->text_primary, 0);

    /* Subagents (live pool) */
    lbl_sub = lv_label_create(left_panel);
    lv_label_set_text(lbl_sub, "SUBAGENTS");
    lv_obj_set_pos(lbl_sub, 4, 168);
    lv_obj_set_style_text_font(lbl_sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_sub, p->accent_secondary, 0);

    for (int i = 0; i < AGY_SUB_BTNS; i++) {
        sub_btns[i] = mk_button(left_panel, DEVOS_PANE_LEFT_WIDTH - 28, 34,
                                4, 190 + i * 40);
        lv_obj_add_flag(sub_btns[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(sub_btns[i], sub_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        sub_lbls[i] = mk_label_btn(sub_btns[i], &lv_font_montserrat_12,
                                   p->text_secondary);
        lv_obj_set_size(sub_lbls[i], DEVOS_PANE_LEFT_WIDTH - 44, 30);
        lv_label_set_long_mode(sub_lbls[i], LV_LABEL_LONG_DOT);
    }

    /* Slash commands (tap inserts into the prompt box) */
    lbl_slash = lv_label_create(left_panel);
    lv_label_set_text(lbl_slash, "SLASH COMMANDS");
    lv_obj_set_pos(lbl_slash, 4, 190 + AGY_SUB_BTNS * 40);
    lv_obj_set_style_text_font(lbl_slash, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_slash, p->text_secondary, 0);

    static const char *cmds[4] = {"/goal", "/plan", "/boost", "/learn"};
    static const char *hints[4] = {"(autonomous)", "(architecture)",
                                   "(deep think)", "(save skill)"};
    for (int i = 0; i < 4; i++) {
        cmd_btns[i] = mk_button(left_panel, DEVOS_PANE_LEFT_WIDTH - 28, 30,
                                4, 214 + AGY_SUB_BTNS * 40 + i * 36);
        lv_obj_add_event_cb(cmd_btns[i], cmd_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        cmd_lbls[i] = mk_label_btn(cmd_btns[i], &lv_font_montserrat_12,
                                   p->accent_primary);
        char buf[48];
        snprintf(buf, sizeof(buf), "%s %s", cmds[i], hints[i]);
        lv_label_set_text(cmd_lbls[i], buf);
    }

    /* 2. Center: action strip + chat + input */
    action_strip = lv_obj_create(center_panel);
    lv_obj_set_size(action_strip, lv_pct(100), 28);
    lv_obj_set_pos(action_strip, 0, 0);
    lv_obj_set_style_bg_color(action_strip, p->bg_alt, 0);
    lv_obj_set_style_border_color(action_strip, p->surface_border, 0);
    lv_obj_set_style_border_width(action_strip, 1, 0);
    lv_obj_set_style_border_side(action_strip, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(action_strip, 0, 0);
    lv_obj_set_style_pad_all(action_strip, 2, 0);
    lv_obj_clear_flag(action_strip, LV_OBJ_FLAG_SCROLLABLE);

    lbl_strip = lv_label_create(action_strip);
    lv_label_set_text(lbl_strip, "Bridge: ...");
    lv_obj_align(lbl_strip, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_text_font(lbl_strip, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_strip, p->text_secondary, 0);

    chat_scroll = lv_obj_create(center_panel);
    lv_obj_set_size(chat_scroll, lv_pct(100), DEVOS_CONTENT_HEIGHT - 28 - 56);
    lv_obj_set_pos(chat_scroll, 0, 28);
    lv_obj_set_style_bg_color(chat_scroll, p->bg, 0);
    lv_obj_set_style_border_width(chat_scroll, 0, 0);
    lv_obj_set_style_pad_all(chat_scroll, 8, 0);

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
    lv_textarea_set_placeholder_text(ta, "Prompt or /command (Fn+N adds note)...");
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

    /* 3. Right: artifacts + diffs */
    lbl_insp_title = lv_label_create(right_panel);
    lv_label_set_text(lbl_insp_title, "INSPECTOR (Fn+])");
    lv_obj_set_pos(lbl_insp_title, 4, 4);
    lv_obj_set_style_text_font(lbl_insp_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_insp_title, p->text_secondary, 0);

    lbl_art_h = lv_label_create(right_panel);
    lv_label_set_text(lbl_art_h, "ARTIFACTS");
    lv_obj_set_pos(lbl_art_h, 4, 28);
    lv_obj_set_style_text_font(lbl_art_h, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_art_h, p->accent_primary, 0);

    for (int i = 0; i < AGY_ART_BTNS; i++) {
        art_btns[i] = mk_button(right_panel, DEVOS_PANE_RIGHT_WIDTH - 28, 34,
                                4, 52 + i * 40);
        lv_obj_add_flag(art_btns[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(art_btns[i], art_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        art_lbls[i] = mk_label_btn(art_btns[i], &lv_font_montserrat_12,
                                   p->text_primary);
        lv_obj_set_size(art_lbls[i], DEVOS_PANE_RIGHT_WIDTH - 44, 30);
        lv_label_set_long_mode(art_lbls[i], LV_LABEL_LONG_DOT);
    }

    lbl_diff_h = lv_label_create(right_panel);
    lv_label_set_text(lbl_diff_h, "DIFFS");
    lv_obj_set_pos(lbl_diff_h, 4, 52 + AGY_ART_BTNS * 40 + 4);
    lv_obj_set_style_text_font(lbl_diff_h, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_diff_h, p->accent_primary, 0);

    btn_save_diff = lv_button_create(right_panel);
    lv_obj_set_size(btn_save_diff, 84, 26);
    lv_obj_set_pos(btn_save_diff, DEVOS_PANE_RIGHT_WIDTH - 84 - 24, 52 + AGY_ART_BTNS * 40);
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

    diff_scroll = lv_obj_create(right_panel);
    lv_obj_set_pos(diff_scroll, 4, 82 + AGY_ART_BTNS * 40);
    lv_obj_set_size(diff_scroll, DEVOS_PANE_RIGHT_WIDTH - 28,
                    DEVOS_CONTENT_HEIGHT - (82 + AGY_ART_BTNS * 40) - 10);
    lv_obj_set_style_bg_color(diff_scroll, p->code_bg, 0);
    lv_obj_set_style_border_color(diff_scroll, p->surface_border, 0);
    lv_obj_set_style_border_width(diff_scroll, 1, 0);
    lv_obj_set_style_radius(diff_scroll, 4, 0);
    lv_obj_set_style_pad_all(diff_scroll, 6, 0);

    /* 4. Permission modal */
    modal_permission = mk_modal(460, 190, p->accent_warning);
    lbl_m_title = lv_label_create(modal_permission);
    lv_label_set_text(lbl_m_title, LV_SYMBOL_WARNING " Tool Permission");
    lv_obj_set_style_text_font(lbl_m_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_m_title, p->accent_warning, 0);

    lbl_m_desc = lv_label_create(modal_permission);
    lv_label_set_text(lbl_m_desc, "");
    lv_obj_set_pos(lbl_m_desc, 0, 36);
    lv_obj_set_size(lbl_m_desc, 428, 60);
    lv_label_set_long_mode(lbl_m_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl_m_desc, p->text_primary, 0);

    const char *ynames[3] = {"[Y] Once", "[N] Deny", "[A] Always"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = perm_btns[i] = lv_button_create(modal_permission);
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
        lv_obj_set_style_text_color(l,
            i == 1 ? lv_color_white()
                   : i == 2 ? p->text_primary
                            : devos_theme_is_dark() ? lv_color_black()
                                                    : lv_color_white(),
            0);
    }

    /* 5. Question modal (1-4 choices) */
    modal_question = mk_modal(460, 280, p->accent_primary);
    lbl_q_title = lv_label_create(modal_question);
    lv_label_set_text(lbl_q_title, LV_SYMBOL_SHUFFLE " Agent Question");
    lv_obj_set_style_text_font(lbl_q_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_q_title, p->accent_primary, 0);

    lbl_q_desc = lv_label_create(modal_question);
    lv_label_set_text(lbl_q_desc, "");
    lv_obj_set_pos(lbl_q_desc, 0, 36);
    lv_obj_set_size(lbl_q_desc, 428, 48);
    lv_label_set_long_mode(lbl_q_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl_q_desc, p->text_primary, 0);

    for (int i = 0; i < AGY_Q_BTNS; i++) {
        lv_obj_t *b = q_btns[i] = lv_button_create(modal_question);
        lv_obj_set_size(b, 428, 34);
        lv_obj_set_pos(b, 0, 92 + i * 38);
        lv_obj_set_style_bg_color(b, p->surface_active, 0);
        lv_obj_set_style_border_color(b, p->surface_border, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_add_event_cb(b, question_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *l = q_lbls[i] = lv_label_create(b);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, p->text_primary, 0);
    }

    /* 6. Artifact viewer modal */
    modal_artifact = mk_modal(640, 420, p->accent_primary);
    lbl_art_title = lv_label_create(modal_artifact);
    lv_label_set_text(lbl_art_title, "Artifact");
    lv_obj_set_style_text_font(lbl_art_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_art_title, p->accent_primary, 0);

    art_scroll = lv_obj_create(modal_artifact);
    lv_obj_set_size(art_scroll, 608, 300);
    lv_obj_set_pos(art_scroll, 0, 36);
    lv_obj_set_style_bg_color(art_scroll, p->code_bg, 0);
    lv_obj_set_style_border_width(art_scroll, 0, 0);
    lv_obj_set_style_pad_all(art_scroll, 10, 0);

    btn_art_save = lv_button_create(modal_artifact);
    lv_obj_set_size(btn_art_save, 110, 32);
    lv_obj_set_pos(btn_art_save, 360, 344);
    lv_obj_set_style_bg_color(btn_art_save, p->surface_active, 0);
    lv_obj_set_style_border_color(btn_art_save, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_art_save, 1, 0);
    lv_obj_set_style_radius(btn_art_save, 4, 0);
    lv_obj_add_event_cb(btn_art_save, art_save_cb, LV_EVENT_CLICKED, NULL);
    lbl_art_save = lv_label_create(btn_art_save);
    lv_label_set_text(lbl_art_save, LV_SYMBOL_SAVE " Save");
    lv_obj_center(lbl_art_save);
    lv_obj_set_style_text_color(lbl_art_save, p->text_primary, 0);

    lv_obj_t *btn_art_close = lv_button_create(modal_artifact);
    lv_obj_set_size(btn_art_close, 110, 32);
    lv_obj_set_pos(btn_art_close, 488, 344);
    lv_obj_set_style_bg_color(btn_art_close, p->surface_active, 0);
    lv_obj_set_style_border_color(btn_art_close, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_art_close, 1, 0);
    lv_obj_set_style_radius(btn_art_close, 4, 0);
    lv_obj_add_event_cb(btn_art_close, art_close_cb, LV_EVENT_CLICKED, NULL);
    lbl_art_close = lv_label_create(btn_art_close);
    lv_label_set_text(lbl_art_close, "Close");
    lv_obj_center(lbl_art_close);
    lv_obj_set_style_text_color(lbl_art_close, p->text_primary, 0);

    /* 7. Server modal (host / port / token) */
    modal_srv = mk_modal(480, 280, p->accent_primary);
    lv_obj_clear_flag(modal_srv, LV_OBJ_FLAG_SCROLLABLE);
    lbl_srv_title = lv_label_create(modal_srv);
    lv_label_set_text(lbl_srv_title, LV_SYMBOL_SETTINGS " Bridge Server");
    lv_obj_set_style_text_font(lbl_srv_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_srv_title, p->accent_primary, 0);

    ta_srv_host = mk_field(modal_srv, "host (100.x.y.z)", 448, 0, 40);
    ta_srv_port = mk_field(modal_srv, "port (8420)", 140, 0, 84);
    ta_srv_token = mk_field(modal_srv, "token / PSK (optional)", 300, 148, 84);

    lv_obj_t *btn_srv_save = lv_button_create(modal_srv);
    lv_obj_set_size(btn_srv_save, 130, 34);
    lv_obj_set_pos(btn_srv_save, 190, 200);
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
    lv_obj_set_pos(btn_srv_cancel, 330, 200);
    lv_obj_set_style_bg_color(btn_srv_cancel, p->surface, 0);
    lv_obj_set_style_border_color(btn_srv_cancel, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_srv_cancel, 1, 0);
    lv_obj_set_style_radius(btn_srv_cancel, 4, 0);
    lv_obj_add_event_cb(btn_srv_cancel, srv_cancel_cb, LV_EVENT_CLICKED, NULL);
    lbl_srv_cancel = lv_label_create(btn_srv_cancel);
    lv_label_set_text(lbl_srv_cancel, "Cancel");
    lv_obj_center(lbl_srv_cancel);
    lv_obj_set_style_text_color(lbl_srv_cancel, p->text_primary, 0);

    devos_theme_add_listener(apply_theme, NULL);
    poll_timer = lv_timer_create(poll_cb, 100, NULL);
    refresh_all();
}

static void antigravity_show(void)
{
    refresh_all();
}

static void antigravity_hide(void)
{
    if (modal_permission) {
        lv_obj_add_flag(modal_permission, LV_OBJ_FLAG_HIDDEN);
    }
    if (modal_question) {
        lv_obj_add_flag(modal_question, LV_OBJ_FLAG_HIDDEN);
    }
    if (modal_artifact) {
        lv_obj_add_flag(modal_artifact, LV_OBJ_FLAG_HIDDEN);
    }
    if (modal_srv) {
        lv_obj_add_flag(modal_srv, LV_OBJ_FLAG_HIDDEN);
    }
}

devos_app_descriptor_t *app_antigravity_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_ANTIGRAVITY;
    app_descriptor.name = "Antigravity";
    app_descriptor.title = "Google Antigravity";
    app_descriptor.subtitle = "Native AGY Client (Path B)";
    app_descriptor.screen = screen;
    app_descriptor.init = antigravity_init;
    app_descriptor.show = antigravity_show;
    app_descriptor.hide = antigravity_hide;
    app_descriptor.handle_key = antigravity_handle_key;

    return &app_descriptor;
}
