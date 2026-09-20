#include "app_editor.h"
#include "devos_config.h"
#include "devos_theme.h"
#include <stdio.h>

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;

static void editor_init(void)
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

    /* Left File Tree Sidebar (260px) */
    lv_obj_t *sidebar = lv_obj_create(screen);
    lv_obj_set_size(sidebar, DEVOS_PANE_LEFT_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(sidebar, 0, 0);
    lv_obj_set_style_bg_color(sidebar, p->surface, 0);
    lv_obj_set_style_border_color(sidebar, p->surface_border, 0);
    lv_obj_set_style_border_width(sidebar, 1, 0);
    lv_obj_set_style_border_side(sidebar, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_radius(sidebar, 0, 0);
    lv_obj_set_style_pad_all(sidebar, 10, 0);

    lv_obj_t *lbl_files = lv_label_create(sidebar);
    lv_label_set_text(lbl_files, "STORAGE: /sdcard/notes/");
    lv_obj_set_pos(lbl_files, 4, 4);
    lv_obj_set_style_text_font(lbl_files, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_files, p->text_secondary, 0);

    const char *files[4] = {"📄 welcome.md (Active)", "📄 todo.md", "📄 devos-spec.md", "📄 meeting-notes.md"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn_f = lv_button_create(sidebar);
        lv_obj_set_size(btn_f, DEVOS_PANE_LEFT_WIDTH - 28, 34);
        lv_obj_set_pos(btn_f, 4, 28 + i * 40);
        lv_obj_set_style_bg_color(btn_f, (i == 0) ? p->surface_active : p->surface, 0);
        lv_obj_set_style_border_color(btn_f, (i == 0) ? p->accent_primary : p->surface_border, 0);
        lv_obj_set_style_border_width(btn_f, 1, 0);
        lv_obj_set_style_radius(btn_f, 4, 0);

        lv_obj_t *lf = lv_label_create(btn_f);
        lv_label_set_text(lf, files[i]);
        lv_obj_align(lf, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_style_text_font(lf, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lf, (i == 0) ? p->accent_primary : p->text_primary, 0);
    }

    /* Main Editor Area */
    lv_obj_t *main_area = lv_obj_create(screen);
    lv_obj_set_size(main_area, DEVOS_SCREEN_WIDTH - DEVOS_PANE_LEFT_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(main_area, DEVOS_PANE_LEFT_WIDTH, 0);
    lv_obj_set_style_bg_color(main_area, p->bg, 0);
    lv_obj_set_style_radius(main_area, 0, 0);
    lv_obj_set_style_border_width(main_area, 0, 0);
    lv_obj_set_style_pad_all(main_area, 0, 0);
    lv_obj_clear_flag(main_area, LV_OBJ_FLAG_SCROLLABLE);

    /* Action bar */
    lv_obj_t *top_bar = lv_obj_create(main_area);
    lv_obj_set_size(top_bar, lv_pct(100), 34);
    lv_obj_set_pos(top_bar, 0, 0);
    lv_obj_set_style_bg_color(top_bar, p->top_bar_bg, 0);
    lv_obj_set_style_border_color(top_bar, p->surface_border, 0);
    lv_obj_set_style_border_width(top_bar, 1, 0);
    lv_obj_set_style_border_side(top_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_pad_left(top_bar, 10, 0);

    lv_obj_t *lbl_fn = lv_label_create(top_bar);
    lv_label_set_text(lbl_fn, "welcome.md  (14.2 KB) - Markdown Preview");
    lv_obj_align(lbl_fn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(lbl_fn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_fn, p->accent_primary, 0);

    /* Editor Text Container */
    lv_obj_t *text_container = lv_obj_create(main_area);
    lv_obj_set_size(text_container, lv_pct(100), DEVOS_CONTENT_HEIGHT - 34);
    lv_obj_set_pos(text_container, 0, 34);
    lv_obj_set_style_bg_color(text_container, p->code_bg, 0);
    lv_obj_set_style_radius(text_container, 0, 0);
    lv_obj_set_style_border_width(text_container, 0, 0);
    lv_obj_set_style_pad_all(text_container, 16, 0);

    lv_obj_t *lbl_content = lv_label_create(text_container);
    lv_label_set_text(lbl_content,
        "# Welcome to devOS on M5Stack Tab5!\n\n"
        "devOS is an open-source, developer-focused mobile cyberdeck firmware.\n\n"
        "## Key Global Shortcuts\n"
        "- [1] .. [6]: Quick Launch App from Home\n"
        "- [Fn + H]: Return Home from anywhere\n"
        "- [Fn + T]: Toggle Dark / Light Theme\n"
        "- [Fn + F]: Toggle Focus Mode in Agent / AGY Viewports\n"
        "- [Fn + []: Toggle Left Sidebar (Connections, Subagents, Files)\n"
        "- [Fn + ]]: Toggle Right Inspector (Artifacts, Diffs)\n\n"
        "## Offline Storage & Synchronization\n"
        "All notes are stored directly on the MicroSD card at `/sdcard/notes/`.\n"
        "No cloud sync required—fully offline capable.");
    lv_obj_set_style_text_color(lbl_content, p->text_primary, 0);
    lv_obj_set_style_text_font(lbl_content, &lv_font_montserrat_14, 0);
}

static void editor_show(void) {}
static void editor_hide(void) {}

devos_app_descriptor_t *app_editor_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_EDITOR;
    app_descriptor.name = "Editor";
    app_descriptor.title = "Markdown Editor";
    app_descriptor.subtitle = "Distraction-Free Notes & Docs";
    app_descriptor.screen = screen;
    app_descriptor.init = editor_init;
    app_descriptor.show = editor_show;
    app_descriptor.hide = editor_hide;
    app_descriptor.handle_key = NULL;

    return &app_descriptor;
}
