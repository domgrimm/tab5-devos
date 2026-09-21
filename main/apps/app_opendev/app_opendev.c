#include "app_opendev.h"
#include "devos_config.h"
#include "devos_theme.h"
#include "devos_agent_viewport.h"
#include <stdio.h>

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;
static devos_agent_viewport_t *viewport = NULL;

/* Theme-tracked widgets */
static lv_obj_t *lbl_title = NULL;
static lv_obj_t *btn_sess = NULL;
static lv_obj_t *lbl_s = NULL;
static lv_obj_t *chat_scroll = NULL;
static lv_obj_t *msg_card = NULL;
static lv_obj_t *lbl_msg = NULL;
static lv_obj_t *input_bar = NULL;
static lv_obj_t *ta = NULL;
static lv_obj_t *btn_send = NULL;
static lv_obj_t *lbl_send = NULL;
static lv_obj_t *lbl_rg = NULL;

static void apply_theme(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    if (!screen) return;

    lv_obj_set_style_bg_color(screen, p->bg, 0);

    if (lbl_title) lv_obj_set_style_text_color(lbl_title, p->text_secondary, 0);
    if (btn_sess) {
        lv_obj_set_style_bg_color(btn_sess, p->surface_active, 0);
        lv_obj_set_style_border_color(btn_sess, p->accent_primary, 0);
    }
    if (lbl_s) lv_obj_set_style_text_color(lbl_s, p->accent_primary, 0);

    if (chat_scroll) lv_obj_set_style_bg_color(chat_scroll, p->bg, 0);
    if (msg_card) {
        lv_obj_set_style_bg_color(msg_card, p->surface, 0);
        lv_obj_set_style_border_color(msg_card, p->surface_border, 0);
    }
    if (lbl_msg) lv_obj_set_style_text_color(lbl_msg, p->text_primary, 0);

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
    if (lbl_send) {
        /* ponytail: black on neon cyan (dark) / white on cobalt (light) */
        lv_obj_set_style_text_color(lbl_send,
            devos_theme_is_dark() ? lv_color_black() : lv_color_white(), 0);
    }
    if (lbl_rg) lv_obj_set_style_text_color(lbl_rg, p->text_secondary, 0);
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
    return false;
}

static void opendev_init(void)
{
    const devos_palette_t *p = devos_theme_get();

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

    /* Left Sidebar: Projects & Sessions */
    lbl_title = lv_label_create(left_panel);
    lv_label_set_text(lbl_title, "OPENCODE / OPENCHAMBER");
    lv_obj_set_pos(lbl_title, 4, 4);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_title, p->text_secondary, 0);

    btn_sess = lv_button_create(left_panel);
    lv_obj_set_size(btn_sess, DEVOS_PANE_LEFT_WIDTH - 28, 44);
    lv_obj_set_pos(btn_sess, 4, 28);
    lv_obj_set_style_bg_color(btn_sess, p->surface_active, 0);
    lv_obj_set_style_border_color(btn_sess, p->accent_primary, 0);
    lv_obj_set_style_border_width(btn_sess, 1, 0);
    lv_obj_set_style_radius(btn_sess, 4, 0);

    lbl_s = lv_label_create(btn_sess);
    lv_label_set_text(lbl_s, LV_SYMBOL_BULLET " Session: devos-firmware\nModel: Claude 3.7 Sonnet");
    lv_obj_align(lbl_s, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_text_font(lbl_s, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_s, p->accent_primary, 0);

    /* Center: Chat Stream */
    chat_scroll = lv_obj_create(center_panel);
    lv_obj_set_size(chat_scroll, lv_pct(100), DEVOS_CONTENT_HEIGHT - 56);
    lv_obj_set_pos(chat_scroll, 0, 0);
    lv_obj_set_style_bg_color(chat_scroll, p->bg, 0);
    lv_obj_set_style_border_width(chat_scroll, 0, 0);
    lv_obj_set_style_pad_all(chat_scroll, 8, 0);

    msg_card = lv_obj_create(chat_scroll);
    lv_obj_set_size(msg_card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(msg_card, p->surface, 0);
    lv_obj_set_style_border_color(msg_card, p->surface_border, 0);
    lv_obj_set_style_border_width(msg_card, 1, 0);
    lv_obj_set_style_radius(msg_card, 6, 0);
    lv_obj_set_style_pad_all(msg_card, 10, 0);

    lbl_msg = lv_label_create(msg_card);
    lv_label_set_text(lbl_msg,
        "Connected to OpenCode Server on http://10.2.132.54:4096 (Direct LAN / Optional Mesh)\n\n"
        "Active Stream: SSE /event\n"
        "State: Awaiting instruction from Tab5 keyboard.");
    lv_obj_set_style_text_color(lbl_msg, p->text_primary, 0);
    lv_obj_set_style_text_font(lbl_msg, &lv_font_montserrat_14, 0);

    /* Bottom Input */
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
    lv_textarea_set_placeholder_text(ta, "Send prompt to OpenCode agent...");
    lv_obj_set_size(ta, lv_pct(82), 38);
    lv_obj_align(ta, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(ta, p->bg_alt, 0);
    lv_obj_set_style_border_color(ta, p->surface_border, 0);
    lv_obj_set_style_text_color(ta, p->text_primary, 0);

    btn_send = lv_button_create(input_bar);
    lv_obj_set_size(btn_send, lv_pct(16), 38);
    lv_obj_align(btn_send, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_send, p->accent_primary, 0);

    lbl_send = lv_label_create(btn_send);
    lv_label_set_text(lbl_send, "Send " LV_SYMBOL_RIGHT);
    lv_obj_center(lbl_send);
    lv_obj_set_style_text_color(lbl_send,
        devos_theme_is_dark() ? lv_color_black() : lv_color_white(), 0);

    /* Right: Goals & Diffs */
    lbl_rg = lv_label_create(right_panel);
    lv_label_set_text(lbl_rg, "GOALS & DIFFS (Fn+])");
    lv_obj_set_pos(lbl_rg, 4, 4);
    lv_obj_set_style_text_font(lbl_rg, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_rg, p->text_secondary, 0);

    devos_theme_add_listener(apply_theme, NULL);
}

static void opendev_show(void) {}
static void opendev_hide(void) {}

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
