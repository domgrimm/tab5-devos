#include "app_antigravity.h"
#include "devos_config.h"
#include "devos_theme.h"
#include "devos_agent_viewport.h"
#include <stdio.h>

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;
static devos_agent_viewport_t *viewport = NULL;

/* Thinking accordion toggle state */
static bool thinking_expanded = true;
static lv_obj_t *thinking_body = NULL;
static lv_obj_t *lbl_thinking_arrow = NULL;

/* Interactive Permission Modal */
static lv_obj_t *modal_permission = NULL;

static void toggle_thinking_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    thinking_expanded = !thinking_expanded;
    if (thinking_expanded) {
        lv_obj_remove_flag(thinking_body, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_thinking_arrow, LV_SYMBOL_DOWN " Thinking Trace (1.8s)");
    } else {
        lv_obj_add_flag(thinking_body, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_thinking_arrow, LV_SYMBOL_RIGHT " Thinking Trace (1.8s - Collapsed)");
    }
}

static void permission_modal_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (modal_permission) {
        lv_obj_add_flag(modal_permission, LV_OBJ_FLAG_HIDDEN);
    }
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
        }
    }

    /* Modal keys: Y, N, A */
    if (modal_permission && !lv_obj_has_flag(modal_permission, LV_OBJ_FLAG_HIDDEN)) {
        if (key == 'y' || key == 'Y' || key == 'n' || key == 'N' || key == 'a' || key == 'A') {
            lv_obj_add_flag(modal_permission, LV_OBJ_FLAG_HIDDEN);
            return true;
        }
    }

    return false;
}

static void antigravity_init(void)
{
    const devos_palette_t *p = devos_theme_get();

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

    /* ----------------------------------------------------------------------
     * 1. Left Sidebar: Sessions, Subagents & Models (260px)
     * ---------------------------------------------------------------------- */
    lv_obj_t *lbl_agy_title = lv_label_create(left_panel);
    lv_label_set_text(lbl_agy_title, "ANTIGRAVITY (AGY)");
    lv_obj_set_pos(lbl_agy_title, 4, 4);
    lv_obj_set_style_text_font(lbl_agy_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_agy_title, p->text_secondary, 0);

    /* Session card */
    lv_obj_t *sess_box = lv_obj_create(left_panel);
    lv_obj_set_size(sess_box, DEVOS_PANE_LEFT_WIDTH - 28, 64);
    lv_obj_set_pos(sess_box, 4, 26);
    lv_obj_set_style_bg_color(sess_box, p->surface_active, 0);
    lv_obj_set_style_border_color(sess_box, p->accent_primary, 0);
    lv_obj_set_style_border_width(sess_box, 1, 0);
    lv_obj_set_style_radius(sess_box, 6, 0);
    lv_obj_set_style_pad_all(sess_box, 6, 0);

    lv_obj_t *lbl_conv = lv_label_create(sess_box);
    lv_label_set_text(lbl_conv, "#41b1d485 (devOS)");
    lv_obj_set_pos(lbl_conv, 4, 2);
    lv_obj_set_style_text_font(lbl_conv, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_conv, p->accent_primary, 0);

    lv_obj_t *lbl_mod = lv_label_create(sess_box);
    lv_label_set_text(lbl_mod, "Gemini 3.8 Flash (High)");
    lv_obj_set_pos(lbl_mod, 4, 26);
    lv_obj_set_style_text_font(lbl_mod, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_mod, p->text_primary, 0);

    /* Subagent Hierarchy */
    lv_obj_t *lbl_sub = lv_label_create(left_panel);
    lv_label_set_text(lbl_sub, "SUBAGENTS TREE");
    lv_obj_set_pos(lbl_sub, 4, 102);
    lv_obj_set_style_text_font(lbl_sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_sub, p->accent_secondary, 0);

    const char *subagents[2] = {LV_SYMBOL_BULLET " research (idle)", LV_SYMBOL_BULLET " self (running)"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *btn_sa = lv_button_create(left_panel);
        lv_obj_set_size(btn_sa, DEVOS_PANE_LEFT_WIDTH - 28, 34);
        lv_obj_set_pos(btn_sa, 4, 124 + i * 40);
        lv_obj_set_style_bg_color(btn_sa, p->surface, 0);
        lv_obj_set_style_border_color(btn_sa, p->surface_border, 0);
        lv_obj_set_style_border_width(btn_sa, 1, 0);
        lv_obj_set_style_radius(btn_sa, 4, 0);

        lv_obj_t *lsa = lv_label_create(btn_sa);
        lv_label_set_text(lsa, subagents[i]);
        lv_obj_align(lsa, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_style_text_font(lsa, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lsa, (i == 1) ? p->accent_secondary : p->text_secondary, 0);
    }

    /* Slash commands list */
    lv_obj_t *lbl_slash = lv_label_create(left_panel);
    lv_label_set_text(lbl_slash, "SLASH COMMANDS");
    lv_obj_set_pos(lbl_slash, 4, 220);
    lv_obj_set_style_text_font(lbl_slash, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_slash, p->text_secondary, 0);

    const char *cmds[4] = {"/goal (autonomous)", "/plan (architecture)", "/boost (deep think)", "/learn (save skill)"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *lbl_cmd = lv_label_create(left_panel);
        lv_label_set_text(lbl_cmd, cmds[i]);
        lv_obj_set_pos(lbl_cmd, 8, 244 + i * 26);
        lv_obj_set_style_text_font(lbl_cmd, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl_cmd, p->accent_primary, 0);
    }

    /* ----------------------------------------------------------------------
     * 2. Center Panel: Chat Stream & Thought Accordion
     * ---------------------------------------------------------------------- */
    /* Viewport layout action strip */
    lv_obj_t *action_strip = lv_obj_create(center_panel);
    lv_obj_set_size(action_strip, lv_pct(100), 28);
    lv_obj_set_pos(action_strip, 0, 0);
    lv_obj_set_style_bg_color(action_strip, p->bg_alt, 0);
    lv_obj_set_style_border_color(action_strip, p->surface_border, 0);
    lv_obj_set_style_border_width(action_strip, 1, 0);
    lv_obj_set_style_border_side(action_strip, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(action_strip, 0, 0);
    lv_obj_set_style_pad_all(action_strip, 2, 0);
    lv_obj_clear_flag(action_strip, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_strip = lv_label_create(action_strip);
    lv_label_set_text(lbl_strip, "Hotkeys: [Fn+F] Focus Mode  |  [Fn+[] Toggle Left  |  [Fn+]] Toggle Right");
    lv_obj_align(lbl_strip, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_text_font(lbl_strip, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_strip, p->text_secondary, 0);

    /* Chat Scroll Area */
    lv_obj_t *chat_scroll = lv_obj_create(center_panel);
    lv_obj_set_size(chat_scroll, lv_pct(100), DEVOS_CONTENT_HEIGHT - 28 - 56);
    lv_obj_set_pos(chat_scroll, 0, 28);
    lv_obj_set_style_bg_color(chat_scroll, p->bg, 0);
    lv_obj_set_style_border_width(chat_scroll, 0, 0);
    lv_obj_set_style_pad_all(chat_scroll, 8, 0);

    /* User Bubble */
    lv_obj_t *user_bubble = lv_obj_create(chat_scroll);
    lv_obj_set_size(user_bubble, lv_pct(100), 44);
    lv_obj_set_style_bg_color(user_bubble, p->surface_active, 0);
    lv_obj_set_style_border_color(user_bubble, p->surface_border, 0);
    lv_obj_set_style_border_width(user_bubble, 1, 0);
    lv_obj_set_style_radius(user_bubble, 6, 0);
    lv_obj_set_style_pad_all(user_bubble, 8, 0);

    lv_obj_t *lbl_user = lv_label_create(user_bubble);
    lv_label_set_text(lbl_user, "> User: Review PLAN.md and AGENTS.md and begin building devOS.");
    lv_obj_set_style_text_color(lbl_user, p->text_primary, 0);
    lv_obj_set_style_text_font(lbl_user, &lv_font_montserrat_14, 0);

    /* Collapsible Thinking Accordion */
    lv_obj_t *thinking_box = lv_obj_create(chat_scroll);
    lv_obj_set_size(thinking_box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(thinking_box, p->thinking_bg, 0);
    lv_obj_set_style_border_color(thinking_box, p->thinking_border, 0);
    lv_obj_set_style_border_width(thinking_box, 1, 0);
    lv_obj_set_style_radius(thinking_box, 6, 0);
    lv_obj_set_style_pad_all(thinking_box, 8, 0);
    lv_obj_add_event_cb(thinking_box, toggle_thinking_cb, LV_EVENT_CLICKED, NULL);

    lbl_thinking_arrow = lv_label_create(thinking_box);
    lv_label_set_text(lbl_thinking_arrow, LV_SYMBOL_DOWN " Thinking Trace (1.8s)");
    lv_obj_set_style_text_color(lbl_thinking_arrow, p->accent_primary, 0);
    lv_obj_set_style_text_font(lbl_thinking_arrow, &lv_font_montserrat_14, 0);

    thinking_body = lv_label_create(thinking_box);
    lv_label_set_text(thinking_body,
        "1. Verified environment prerequisites on Fedora 43 (Xvfb, x11vnc, noVNC, SDL2).\n"
        "2. Cloned LVGL v9.2 into components/lvgl and configured lv_conf.h.\n"
        "3. Segregated dual-core FreeRTOS tasks (Core 0: network/crypto, Core 1: UI/input).\n"
        "4. Built Theme Engine supporting instant Dark Cyberdeck and Light mode switching.\n"
        "5. Built Home Screen dashboard with live telemetry and 6 interactive app cards.");
    lv_obj_set_style_text_color(thinking_body, p->text_secondary, 0);
    lv_obj_set_style_text_font(thinking_body, &lv_font_montserrat_12, 0);

    /* Tool Execution Card */
    lv_obj_t *tool_card = lv_obj_create(chat_scroll);
    lv_obj_set_size(tool_card, lv_pct(100), 52);
    lv_obj_set_style_bg_color(tool_card, p->tool_card_bg, 0);
    lv_obj_set_style_border_color(tool_card, p->tool_card_border, 0);
    lv_obj_set_style_border_width(tool_card, 1, 0);
    lv_obj_set_style_radius(tool_card, 6, 0);
    lv_obj_set_style_pad_all(tool_card, 8, 0);

    lv_obj_t *lbl_tool_title = lv_label_create(tool_card);
    lv_label_set_text(lbl_tool_title, LV_SYMBOL_SETTINGS " Tool: run_command - cmake -B build_sim -S . -DDEVOS_SIMULATOR=ON");
    lv_obj_set_style_text_color(lbl_tool_title, p->accent_secondary, 0);
    lv_obj_set_style_text_font(lbl_tool_title, &lv_font_montserrat_12, 0);

    lv_obj_t *lbl_tool_status = lv_label_create(tool_card);
    lv_label_set_text(lbl_tool_status, "Status: SUCCESS (exit code 0)");
    lv_obj_align(lbl_tool_status, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_text_color(lbl_tool_status, p->text_muted, 0);
    lv_obj_set_style_text_font(lbl_tool_status, &lv_font_montserrat_12, 0);

    /* Assistant Markdown Response */
    lv_obj_t *resp_card = lv_obj_create(chat_scroll);
    lv_obj_set_size(resp_card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(resp_card, p->surface, 0);
    lv_obj_set_style_border_color(resp_card, p->surface_border, 0);
    lv_obj_set_style_border_width(resp_card, 1, 0);
    lv_obj_set_style_radius(resp_card, 6, 0);
    lv_obj_set_style_pad_all(resp_card, 10, 0);

    lv_obj_t *lbl_resp = lv_label_create(resp_card);
    lv_label_set_text(lbl_resp,
        "I have initialized the devOS core architecture and components for the M5Stack Tab5!\n\n"
        "Key Systems Implemented:\n"
        "- Global Theme Engine with Dark Cyberdeck & High-Contrast Light palettes\n"
        "- Dual-core FreeRTOS affinity model (Core 0: Network, Core 1: UI)\n"
        "- Auto-scaffolding MicroSD storage bootstrap (welcome.md, bookmarks.json)\n"
        "- Dynamic PTY resizing (128 <-> 160 columns) on sidebar toggle\n"
        "- Full 1280x720 Remote Web Simulator on Tailscale port 6080");
    lv_obj_set_style_text_color(lbl_resp, p->text_primary, 0);
    lv_obj_set_style_text_font(lbl_resp, &lv_font_montserrat_14, 0);

    /* Bottom Prompt Input Bar */
    lv_obj_t *input_bar = lv_obj_create(center_panel);
    lv_obj_set_size(input_bar, lv_pct(100), 50);
    lv_obj_set_pos(input_bar, 0, DEVOS_CONTENT_HEIGHT - 50);
    lv_obj_set_style_bg_color(input_bar, p->surface, 0);
    lv_obj_set_style_border_color(input_bar, p->surface_border, 0);
    lv_obj_set_style_border_width(input_bar, 1, 0);
    lv_obj_set_style_border_side(input_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(input_bar, 0, 0);
    lv_obj_set_style_pad_all(input_bar, 6, 0);
    lv_obj_clear_flag(input_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ta = lv_textarea_create(input_bar);
    lv_textarea_set_placeholder_text(ta, "Type message or slash command (/goal, /plan)...");
    lv_obj_set_size(ta, lv_pct(82), 38);
    lv_obj_align(ta, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(ta, p->bg_alt, 0);
    lv_obj_set_style_border_color(ta, p->surface_border, 0);
    lv_obj_set_style_text_color(ta, p->text_primary, 0);

    lv_obj_t *btn_send = lv_button_create(input_bar);
    lv_obj_set_size(btn_send, lv_pct(16), 38);
    lv_obj_align(btn_send, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_send, p->accent_primary, 0);
    lv_obj_set_style_radius(btn_send, 4, 0);

    lv_obj_t *lbl_send = lv_label_create(btn_send);
    lv_label_set_text(lbl_send, "Send " LV_SYMBOL_RIGHT);
    lv_obj_center(lbl_send);
    lv_obj_set_style_text_color(lbl_send, lv_color_black(), 0);

    /* ----------------------------------------------------------------------
     * 3. Right Inspector: Artifacts & Changes (300px)
     * ---------------------------------------------------------------------- */
    lv_obj_t *lbl_insp_title = lv_label_create(right_panel);
    lv_label_set_text(lbl_insp_title, "INSPECTOR (Fn+])");
    lv_obj_set_pos(lbl_insp_title, 4, 4);
    lv_obj_set_style_text_font(lbl_insp_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_insp_title, p->text_secondary, 0);

    /* Inspector Tabs: Artifacts | Diffs | Tasks */
    lv_obj_t *tab_artifacts = lv_button_create(right_panel);
    lv_obj_set_size(tab_artifacts, DEVOS_PANE_RIGHT_WIDTH - 28, 32);
    lv_obj_set_pos(tab_artifacts, 4, 26);
    lv_obj_set_style_bg_color(tab_artifacts, p->surface_active, 0);
    lv_obj_set_style_border_color(tab_artifacts, p->accent_primary, 0);
    lv_obj_set_style_border_width(tab_artifacts, 1, 0);
    lv_obj_set_style_radius(tab_artifacts, 4, 0);

    lv_obj_t *lbl_tab_art = lv_label_create(tab_artifacts);
    lv_label_set_text(lbl_tab_art, LV_SYMBOL_DIRECTORY " Artifacts & Diffs");
    lv_obj_center(lbl_tab_art);
    lv_obj_set_style_text_font(lbl_tab_art, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_tab_art, p->accent_primary, 0);

    /* Artifact list */
    const char *artifacts[3] = {LV_SYMBOL_FILE " welcome.md", LV_SYMBOL_FILE " devos_config.h", LV_SYMBOL_FILE " PLAN.md"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn_art = lv_button_create(right_panel);
        lv_obj_set_size(btn_art, DEVOS_PANE_RIGHT_WIDTH - 28, 34);
        lv_obj_set_pos(btn_art, 4, 68 + i * 40);
        lv_obj_set_style_bg_color(btn_art, p->surface, 0);
        lv_obj_set_style_border_color(btn_art, p->surface_border, 0);
        lv_obj_set_style_border_width(btn_art, 1, 0);
        lv_obj_set_style_radius(btn_art, 4, 0);

        lv_obj_t *la = lv_label_create(btn_art);
        lv_label_set_text(la, artifacts[i]);
        lv_obj_align(la, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_style_text_font(la, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(la, p->text_primary, 0);
    }

    /* 4. Interactive Permission Modal (Hidden by default) */
    modal_permission = lv_obj_create(screen);
    lv_obj_set_size(modal_permission, 460, 180);
    lv_obj_align(modal_permission, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(modal_permission, p->surface, 0);
    lv_obj_set_style_border_color(modal_permission, p->accent_warning, 0);
    lv_obj_set_style_border_width(modal_permission, 2, 0);
    lv_obj_set_style_radius(modal_permission, 8, 0);
    lv_obj_set_style_pad_all(modal_permission, 16, 0);
    lv_obj_add_flag(modal_permission, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *lbl_m_title = lv_label_create(modal_permission);
    lv_label_set_text(lbl_m_title, LV_SYMBOL_WARNING " Agent Permission Request");
    lv_obj_set_style_text_font(lbl_m_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_m_title, p->accent_warning, 0);

    lv_obj_t *lbl_m_desc = lv_label_create(modal_permission);
    lv_label_set_text(lbl_m_desc, "Execute tool: run_command\nTarget: 'ninja -C build_sim'");
    lv_obj_set_pos(lbl_m_desc, 0, 36);
    lv_obj_set_style_text_color(lbl_m_desc, p->text_primary, 0);

    /* Action Buttons: [Y] Approve, [N] Deny, [A] Always */
    lv_obj_t *btn_y = lv_button_create(modal_permission);
    lv_obj_set_size(btn_y, 120, 36);
    lv_obj_set_pos(btn_y, 0, 100);
    lv_obj_set_style_bg_color(btn_y, p->accent_secondary, 0);
    lv_obj_add_event_cb(btn_y, permission_modal_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_by = lv_label_create(btn_y);
    lv_label_set_text(lbl_by, "[Y] Approve");
    lv_obj_center(lbl_by);
    lv_obj_set_style_text_color(lbl_by, lv_color_black(), 0);

    lv_obj_t *btn_n = lv_button_create(modal_permission);
    lv_obj_set_size(btn_n, 120, 36);
    lv_obj_set_pos(btn_n, 135, 100);
    lv_obj_set_style_bg_color(btn_n, p->accent_danger, 0);
    lv_obj_add_event_cb(btn_n, permission_modal_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_bn = lv_label_create(btn_n);
    lv_label_set_text(lbl_bn, "[N] Deny");
    lv_obj_center(lbl_bn);
    lv_obj_set_style_text_color(lbl_bn, lv_color_white(), 0);

    lv_obj_t *btn_a = lv_button_create(modal_permission);
    lv_obj_set_size(btn_a, 145, 36);
    lv_obj_set_pos(btn_a, 270, 100);
    lv_obj_set_style_bg_color(btn_a, p->surface_active, 0);
    lv_obj_add_event_cb(btn_a, permission_modal_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_ba = lv_label_create(btn_a);
    lv_label_set_text(lbl_ba, "[A] Always Allow");
    lv_obj_center(lbl_ba);
    lv_obj_set_style_text_color(lbl_ba, p->text_primary, 0);
}

static void antigravity_show(void)
{
}

static void antigravity_hide(void)
{
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
