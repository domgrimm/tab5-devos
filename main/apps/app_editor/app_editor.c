#include "app_editor.h"
#include "devos_config.h"
#include "devos_theme.h"
#include "devos_core.h"
#include "devos_mdview.h"
#include "opendev_client.h"
#include "agy_client.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>

#define EDITOR_MAX_FILES 12
#define EDITOR_NAME_MAX 64
#define EDITOR_BUF_MAX (16 * 1024)

typedef enum {
    EDITOR_VIEW_EDIT = 0,   /* Full editor */
    EDITOR_VIEW_SPLIT,      /* Editor + preview side by side */
    EDITOR_VIEW_PREVIEW     /* Full preview */
} editor_view_t;

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;

/* Theme-tracked widgets */
static lv_obj_t *sidebar = NULL;
static lv_obj_t *lbl_files = NULL;
static lv_obj_t *file_btns[EDITOR_MAX_FILES] = {NULL};
static lv_obj_t *file_lbls[EDITOR_MAX_FILES] = {NULL};
static lv_obj_t *main_area = NULL;
static lv_obj_t *top_bar = NULL;
static lv_obj_t *lbl_fn = NULL;
static lv_obj_t *btn_tree = NULL;
static lv_obj_t *lbl_btn_tree = NULL;
static lv_obj_t *btn_new = NULL;
static lv_obj_t *lbl_btn_new = NULL;
static lv_obj_t *btn_save = NULL;
static lv_obj_t *lbl_btn_save = NULL;
static lv_obj_t *btn_attach = NULL;
static lv_obj_t *lbl_btn_attach = NULL;
static lv_obj_t *btn_mode = NULL;
static lv_obj_t *lbl_btn_mode = NULL;
static lv_obj_t *ta_editor = NULL;
static lv_obj_t *preview_scroll = NULL;

/* State */
static char s_files[EDITOR_MAX_FILES][EDITOR_NAME_MAX];
static int s_file_count = 0;
static int s_active = -1;          /* open file index, -1 = none */
static int s_sel = 0;              /* keyboard cursor in file list */
static bool s_focus_list = false;  /* Tab toggles list <-> editor */
static bool s_sidebar_visible = true;
static bool s_dirty = false;
static bool s_preview_stale = false;
static editor_view_t s_view = EDITOR_VIEW_EDIT;

static void refresh_file_list(void);
static void render_preview(bool preserve_scroll);
static void apply_layout(void);
static void update_title(void);
static void update_telemetry(void);
static void open_file(int idx);
static void save_file(void);
static void flash_msg(const char *msg);
static void apply_theme(const devos_palette_t *p, void *user_data);

/* ponytail: 400ms debounce so typing in split view doesn't rebuild per key */
static void preview_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (s_preview_stale && s_view != EDITOR_VIEW_EDIT && preview_scroll &&
        ta_editor) {
        s_preview_stale = false;
        render_preview(true);
    }
}

/* --------------------------------------------------------------------------
 * File I/O (POSIX: FATFS on target, ./sim_sdcard in simulation)
 * -------------------------------------------------------------------------- */
static void notes_path(const char *name, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/notes/%s", TAB5_SD_MOUNT_POINT, name ? name : "");
}

static void scan_notes(void)
{
    s_file_count = 0;
    char dir[256];
    notes_path(NULL, dir, sizeof(dir));

    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t n = strlen(ent->d_name);
        if (n < 4 || strcmp(ent->d_name + n - 3, ".md") != 0) continue;
        if (s_file_count >= EDITOR_MAX_FILES) break;
        snprintf(s_files[s_file_count], EDITOR_NAME_MAX, "%s", ent->d_name);
        s_files[s_file_count][EDITOR_NAME_MAX - 1] = '\0';
        s_file_count++;
    }
    closedir(d);

    /* Alphabetical, welcome.md first is a nice-to-have; plain sort is fine */
    for (int i = 0; i < s_file_count; i++) {
        for (int j = i + 1; j < s_file_count; j++) {
            if (strcmp(s_files[i], s_files[j]) > 0) {
                char tmp[EDITOR_NAME_MAX];
                memcpy(tmp, s_files[i], sizeof(tmp));
                memcpy(s_files[i], s_files[j], sizeof(s_files[i]));
                memcpy(s_files[j], tmp, sizeof(s_files[j]));
            }
        }
    }
    if (s_sel >= s_file_count) s_sel = s_file_count > 0 ? s_file_count - 1 : 0;
    if (s_active >= s_file_count) s_active = -1;
}

static size_t read_file(const char *name, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return 0;
    buf[0] = '\0';
    char path[256];
    notes_path(name, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, buf_len - 1, f);
    fclose(f);
    /* ponytail: only back off a split codepoint when actually truncated */
    if (n == buf_len - 1) n = devos_md_trunc_ok(buf, n);
    buf[n] = '\0';
    return n;
}

static bool write_file(const char *name, const char *text)
{
    char path[256];
    notes_path(name, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static void open_file(int idx)
{
    if (idx < 0 || idx >= s_file_count || !ta_editor) return;
    if (s_dirty && s_active >= 0 && s_active != idx) {
        save_file();
    }
    static char buf[EDITOR_BUF_MAX];
    read_file(s_files[idx], buf, sizeof(buf));
    lv_textarea_set_text(ta_editor, buf);
    lv_textarea_set_cursor_pos(ta_editor, 0);
    s_active = idx;
    s_sel = idx;
    s_dirty = false;
    s_focus_list = false;
    update_title();
    refresh_file_list();
    update_telemetry();
    if (s_view != EDITOR_VIEW_EDIT) render_preview(false);
}

static void save_file(void)
{
    if (s_active < 0 || s_active >= s_file_count || !ta_editor) return;
    const char *text = lv_textarea_get_text(ta_editor);
    if (write_file(s_files[s_active], text ? text : "")) {
        s_dirty = false;
        update_title();
        update_telemetry();
    } else {
        flash_msg("SAVE FAILED - check SD card");
    }
}

static void new_file(void)
{
    if (s_dirty && s_active >= 0) {
        save_file();
    }
    if (s_file_count >= EDITOR_MAX_FILES) {
        flash_msg("File list full (12 max)");
        return;
    }
    /* ponytail: first free untitled-N.md wins, no dialog */
    for (int n = 1; n < 100; n++) {
        char name[EDITOR_NAME_MAX];
        snprintf(name, sizeof(name), "untitled-%d.md", n);
        bool taken = false;
        for (int i = 0; i < s_file_count; i++) {
            if (strcmp(s_files[i], name) == 0) { taken = true; break; }
        }
        if (!taken) {
            if (!write_file(name, "# Untitled\n\n")) {
                flash_msg("CREATE FAILED - check SD card");
                return;
            }
            scan_notes();
            for (int i = 0; i < s_file_count; i++) {
                if (strcmp(s_files[i], name) == 0) { open_file(i); break; }
            }
            return;
        }
    }
}

/* --------------------------------------------------------------------------
 * UI refresh (all use the live palette, so Fn+T heals everything)
 * -------------------------------------------------------------------------- */
static void update_telemetry(void)
{
    devos_telemetry_t t = *devos_telemetry_get();
    if (s_active >= 0 && s_active < s_file_count) {
        snprintf(t.editor_file, sizeof(t.editor_file), "%s", s_files[s_active]);
        t.editor_file[sizeof(t.editor_file) - 1] = '\0';
        const char *text = ta_editor ? lv_textarea_get_text(ta_editor) : "";
        size_t bytes = strlen(text ? text : "");
        t.editor_file_kb = (uint32_t)((bytes + 1023) / 1024);
        if (bytes > 0 && t.editor_file_kb == 0) t.editor_file_kb = 1;
    } else {
        snprintf(t.editor_file, sizeof(t.editor_file), "%s", "(none)");
        t.editor_file[sizeof(t.editor_file) - 1] = '\0';
        t.editor_file_kb = 0;
    }
    devos_telemetry_update(&t);
}

static void update_title(void)
{
    if (!lbl_fn) return;
    const char *mode = s_view == EDITOR_VIEW_SPLIT ? "Split"
                     : s_view == EDITOR_VIEW_PREVIEW ? "Preview" : "Edit";
    char buf[128];
    if (s_active >= 0 && s_active < s_file_count) {
        const char *text = ta_editor ? lv_textarea_get_text(ta_editor) : "";
        size_t bytes = strlen(text ? text : "");
        char size[16];
        if (bytes < 1024) snprintf(size, sizeof(size), "%zu B", bytes);
        else snprintf(size, sizeof(size), "%zu KB", bytes / 1024);
        snprintf(buf, sizeof(buf), "%s  (%s) - %s%s",
                 s_files[s_active], size, mode, s_dirty ? " [*]" : "");
    } else {
        snprintf(buf, sizeof(buf), "(no file) - %s", mode);
    }
    lv_label_set_text(lbl_fn, buf);
    if (lbl_btn_mode) {
        char mbuf[32];
        snprintf(mbuf, sizeof(mbuf), "Mode: %s", mode);
        lv_label_set_text(lbl_btn_mode, mbuf);
    }
}

/* Transient status line (next update_title overwrites it) */
static void flash_msg(const char *msg)
{
    if (lbl_fn) lv_label_set_text(lbl_fn, msg);
}

static void refresh_file_list(void)
{
    const devos_palette_t *p = devos_theme_get();
    for (int i = 0; i < EDITOR_MAX_FILES; i++) {
        if (!file_btns[i]) continue;
        if (i >= s_file_count) {
            lv_obj_add_flag(file_btns[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(file_btns[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(file_btns[i], 4, 28 + i * 40);
        lv_label_set_text(file_lbls[i], s_files[i]);
        if (i == s_active) {
            lv_obj_set_style_bg_color(file_btns[i], p->surface_active, 0);
            lv_obj_set_style_border_color(file_btns[i], p->accent_primary, 0);
            lv_obj_set_style_text_color(file_lbls[i], p->accent_primary, 0);
        } else if (s_focus_list && i == s_sel) {
            /* ponytail: keyboard cursor = amber border, same language as launcher */
            lv_obj_set_style_bg_color(file_btns[i], p->surface, 0);
            lv_obj_set_style_border_color(file_btns[i], p->accent_warning, 0);
            lv_obj_set_style_text_color(file_lbls[i], p->text_primary, 0);
        } else {
            lv_obj_set_style_bg_color(file_btns[i], p->surface, 0);
            lv_obj_set_style_border_color(file_btns[i], p->surface_border, 0);
            lv_obj_set_style_text_color(file_lbls[i], p->text_primary, 0);
        }
    }
}

static void update_tree_button(void)
{
    if (!btn_tree || !lbl_btn_tree) return;
    const devos_palette_t *p = devos_theme_get();
    if (s_sidebar_visible) {
        lv_obj_set_style_bg_color(btn_tree, p->surface_active, 0);
        lv_obj_set_style_border_color(btn_tree, p->accent_primary, 0);
        lv_obj_set_style_text_color(lbl_btn_tree, p->accent_primary, 0);
    } else {
        lv_obj_set_style_bg_color(btn_tree, p->surface, 0);
        lv_obj_set_style_border_color(btn_tree, p->surface_border, 0);
        lv_obj_set_style_text_color(lbl_btn_tree, p->text_secondary, 0);
    }
}

static void apply_layout(void)
{
    int main_x = 0;
    int main_w = DEVOS_SCREEN_WIDTH;
    if (s_sidebar_visible) {
        if (sidebar) lv_obj_remove_flag(sidebar, LV_OBJ_FLAG_HIDDEN);
        main_x = DEVOS_PANE_LEFT_WIDTH;
        main_w = DEVOS_SCREEN_WIDTH - DEVOS_PANE_LEFT_WIDTH;
    } else if (sidebar) {
        lv_obj_add_flag(sidebar, LV_OBJ_FLAG_HIDDEN);
    }
    if (main_area) {
        lv_obj_set_size(main_area, main_w, DEVOS_CONTENT_HEIGHT);
        lv_obj_set_pos(main_area, main_x, 0);
    }
    if (top_bar && btn_mode && btn_attach && btn_save && btn_new && btn_tree) {
        lv_obj_align(btn_mode, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_align_to(btn_attach, btn_mode, LV_ALIGN_OUT_LEFT_MID, -6, 0);
        lv_obj_align_to(btn_save, btn_attach, LV_ALIGN_OUT_LEFT_MID, -6, 0);
        lv_obj_align_to(btn_new, btn_save, LV_ALIGN_OUT_LEFT_MID, -6, 0);
        lv_obj_align_to(btn_tree, btn_new, LV_ALIGN_OUT_LEFT_MID, -6, 0);
        if (lbl_fn) {
            int lbl_w = main_w - 420;
            if (lbl_w < 120) lbl_w = 120;
            lv_obj_set_width(lbl_fn, lbl_w);
        }
        update_tree_button();
    }
    if (!ta_editor || !preview_scroll) return;
    int edit_h = DEVOS_CONTENT_HEIGHT - 34;
    if (s_view == EDITOR_VIEW_EDIT) {
        lv_obj_remove_flag(ta_editor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_scroll, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(ta_editor, main_w, edit_h);
        lv_obj_set_pos(ta_editor, 0, 34);
        lv_obj_set_style_border_width(preview_scroll, 0, 0);
    } else if (s_view == EDITOR_VIEW_SPLIT) {
        lv_obj_remove_flag(ta_editor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(preview_scroll, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(ta_editor, main_w / 2, edit_h);
        lv_obj_set_pos(ta_editor, 0, 34);
        lv_obj_set_size(preview_scroll, main_w - main_w / 2, edit_h);
        lv_obj_set_pos(preview_scroll, main_w / 2, 34);
        lv_obj_set_style_border_width(preview_scroll, 1, 0);
        lv_obj_set_style_border_side(preview_scroll, LV_BORDER_SIDE_LEFT, 0);
    } else {
        lv_obj_add_flag(ta_editor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(preview_scroll, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(preview_scroll, main_w, edit_h);
        lv_obj_set_pos(preview_scroll, 0, 34);
        lv_obj_set_style_border_width(preview_scroll, 0, 0);
    }
}

static void render_preview(bool preserve_scroll)
{
    if (!preview_scroll || !ta_editor) return;
    int32_t scroll_y = preserve_scroll ? lv_obj_get_scroll_y(preview_scroll) : 0;
    lv_obj_clean(preview_scroll);
    lv_obj_update_layout(preview_scroll);
    const char *text = lv_textarea_get_text(ta_editor);
    devos_md_render(preview_scroll, text ? text : "");
    lv_obj_update_layout(preview_scroll);
    if (preserve_scroll && scroll_y > 0) {
        lv_obj_scroll_to_y(preview_scroll, scroll_y, LV_ANIM_OFF);
    }
}


static void apply_theme(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    if (!screen) return;

    lv_obj_set_style_bg_color(screen, p->bg, 0);
    if (sidebar) {
        lv_obj_set_style_bg_color(sidebar, p->surface, 0);
        lv_obj_set_style_border_color(sidebar, p->surface_border, 0);
    }
    if (lbl_files) lv_obj_set_style_text_color(lbl_files, p->text_secondary, 0);
    if (main_area) lv_obj_set_style_bg_color(main_area, p->bg, 0);
    if (top_bar) {
        lv_obj_set_style_bg_color(top_bar, p->top_bar_bg, 0);
        lv_obj_set_style_border_color(top_bar, p->surface_border, 0);
    }
    if (lbl_fn) lv_obj_set_style_text_color(lbl_fn, p->accent_primary, 0);

    lv_obj_t *action_btns[] = {btn_tree, btn_new, btn_save, btn_attach, btn_mode};
    lv_obj_t *action_lbls[] = {lbl_btn_tree, lbl_btn_new, lbl_btn_save, lbl_btn_attach, lbl_btn_mode};
    for (int i = 0; i < 5; i++) {
        if (action_btns[i]) {
            lv_obj_set_style_bg_color(action_btns[i], p->surface, 0);
            lv_obj_set_style_border_color(action_btns[i], p->surface_border, 0);
        }
        if (action_lbls[i]) {
            lv_obj_set_style_text_color(action_lbls[i], (i == 4) ? p->accent_primary : p->text_primary, 0);
        }
    }
    update_tree_button();

    if (ta_editor) {
        lv_obj_set_style_bg_color(ta_editor, p->code_bg, 0);
        lv_obj_set_style_text_color(ta_editor, p->text_primary, 0);
        lv_obj_set_style_border_color(ta_editor, p->surface_border, 0);
        lv_obj_set_style_bg_color(ta_editor, p->accent_primary, LV_PART_CURSOR);
        lv_obj_set_style_border_color(ta_editor, p->accent_primary, LV_PART_CURSOR);
    }
    if (preview_scroll) {
        lv_obj_set_style_bg_color(preview_scroll, p->code_bg, 0);
        lv_obj_set_style_border_color(preview_scroll, p->surface_border, 0);
    }

    refresh_file_list();
    if (s_view != EDITOR_VIEW_EDIT) render_preview(true);
}

/* --------------------------------------------------------------------------
 * Input & Button callbacks
 * -------------------------------------------------------------------------- */
static void btn_tree_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_sidebar_visible = !s_sidebar_visible;
    if (!s_sidebar_visible) s_focus_list = false;
    apply_layout();
    refresh_file_list();
}

static void btn_new_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    new_file();
}

static void btn_save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    save_file();
}

static void btn_attach_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_active < 0 || s_active >= s_file_count || !ta_editor) {
        flash_msg("No active note to attach");
        return;
    }
    const char *text = lv_textarea_get_text(ta_editor);
    if (!text || !*text) {
        flash_msg("Note is empty");
        return;
    }
    char msg[OPENDEV_BLOCK_MAX];
    snprintf(msg, sizeof(msg), "[Context from %s]:\n%s", s_files[s_active], text);
    if (opendev_client_status() == OPENDEV_UP) {
        int rc = opendev_client_send(msg);
        if (rc == 0) {
            flash_msg("Attached to OpenDev session!");
            return;
        }
    }
    if (agy_client_status() == AGY_UP) {
        int rc = agy_client_send(msg, NULL);
        if (rc == 0) {
            flash_msg("Attached to Antigravity session!");
            return;
        }
    }
    flash_msg("Attach failed (no agent connected)");
}

static void btn_mode_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_view = (s_view == EDITOR_VIEW_EDIT) ? EDITOR_VIEW_SPLIT
           : (s_view == EDITOR_VIEW_SPLIT) ? EDITOR_VIEW_PREVIEW : EDITOR_VIEW_EDIT;
    apply_layout();
    update_title();
    if (s_view != EDITOR_VIEW_EDIT) render_preview(false);
}

static void editor_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_focus_list) {
        s_focus_list = false;
        refresh_file_list();
    }
}

static void file_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    open_file(idx);
}

static bool editor_handle_key(uint32_t key, uint8_t modifiers)
{
    /* Fullscreen editing: Sym + L collapses the file tree; Sym + A attaches note to OpenDev */
    if (modifiers & DEVOS_MOD_FN) {
        if (key == 'l' || key == 'L') {
            s_sidebar_visible = !s_sidebar_visible;
            if (!s_sidebar_visible) s_focus_list = false;
            apply_layout();
            refresh_file_list();
            return true;
        }
        if (key == 'a' || key == 'A') {
            btn_attach_cb(NULL);
            return true;
        }
    }

    /* Tab toggles focus between file list and editor */
    if (key == '\t' && !(modifiers & DEVOS_MOD_ALT)) {
        if (!s_sidebar_visible) {
            s_sidebar_visible = true;
            s_focus_list = true;
            apply_layout();
        } else {
            s_focus_list = !s_focus_list;
        }
        refresh_file_list();
        return true;
    }

    if (s_focus_list) {
        if (key == LV_KEY_ESC) {
            s_focus_list = false;
            refresh_file_list();
            return true;
        }
        if (key == LV_KEY_UP) {
            if (s_sel > 0) {
                s_sel--;
                refresh_file_list();
                if (file_btns[s_sel]) lv_obj_scroll_to_view(file_btns[s_sel], LV_ANIM_OFF);
            }
            return true;
        }
        if (key == LV_KEY_DOWN) {
            if (s_sel < s_file_count - 1) {
                s_sel++;
                refresh_file_list();
                if (file_btns[s_sel]) lv_obj_scroll_to_view(file_btns[s_sel], LV_ANIM_OFF);
            }
            return true;
        }
        if (key == '\r' || key == '\n') {
            open_file(s_sel);
            return true;
        }
        return true; /* absorb the rest so the editor behind never gets them */
    }

    /* Editor shortcuts */
    if (modifiers & DEVOS_MOD_CTRL) {
        if (key == 's' || key == 'S') { save_file(); return true; }
        if (key == 'o' || key == 'O') {
            s_sidebar_visible = true;
            s_focus_list = true;
            apply_layout();
            refresh_file_list();
            return true;
        }
        if (key == 'n' || key == 'N') { new_file(); return true; }
        if (key == 'p' || key == 'P') {
            /* ponytail: Ctrl+P cycles Edit -> Split -> Preview (no palette UI) */
            s_view = (s_view == EDITOR_VIEW_EDIT) ? EDITOR_VIEW_SPLIT
                   : (s_view == EDITOR_VIEW_SPLIT) ? EDITOR_VIEW_PREVIEW : EDITOR_VIEW_EDIT;
            apply_layout();
            update_title();
            if (s_view != EDITOR_VIEW_EDIT) render_preview(false);
            return true;
        }
        return false;
    }

    /* In Full Preview mode: arrows scroll the preview, typing is inhibited */
    if (s_view == EDITOR_VIEW_PREVIEW) {
        if (key == LV_KEY_UP) {
            lv_obj_scroll_by_bounded(preview_scroll, 0, 40, LV_ANIM_ON);
            return true;
        }
        if (key == LV_KEY_DOWN) {
            lv_obj_scroll_by_bounded(preview_scroll, 0, -40, LV_ANIM_ON);
            return true;
        }
        if (key == LV_KEY_PREV) {
            lv_obj_scroll_by_bounded(preview_scroll, 0, 200, LV_ANIM_ON);
            return true;
        }
        if (key == LV_KEY_NEXT || key == ' ') {
            lv_obj_scroll_by_bounded(preview_scroll, 0, -200, LV_ANIM_ON);
            return true;
        }
        return false;
    }

    if (!ta_editor || s_active < 0) return false;

    /* Multiline editing */
    if (key == '\b' || key == 0x7F) {
        lv_textarea_delete_char(ta_editor);
        s_preview_stale = true;
        if (!s_dirty) { s_dirty = true; update_title(); }
        return true;
    }
    if (key == LV_KEY_LEFT) { lv_textarea_cursor_left(ta_editor); return true; }
    if (key == LV_KEY_RIGHT) { lv_textarea_cursor_right(ta_editor); return true; }
    if (key == LV_KEY_UP) { lv_textarea_cursor_up(ta_editor); return true; }
    if (key == LV_KEY_DOWN) { lv_textarea_cursor_down(ta_editor); return true; }
    if (key == '\r' || key == '\n') {
        lv_textarea_add_char(ta_editor, '\n');
        s_preview_stale = true;
        if (s_view != EDITOR_VIEW_EDIT) render_preview(true);
        if (!s_dirty) { s_dirty = true; update_title(); }
        return true;
    }
    if (key >= 32 && key <= 126) {
        lv_textarea_add_char(ta_editor, (char)key);
        s_preview_stale = true;
        if (!s_dirty) { s_dirty = true; update_title(); }
        return true;
    }
    return false;
}

/* --------------------------------------------------------------------------
 * Init
 * -------------------------------------------------------------------------- */
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

    /* Left File Tree Sidebar (260px, scrollable for long listings) */
    sidebar = lv_obj_create(screen);
    lv_obj_set_size(sidebar, DEVOS_PANE_LEFT_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(sidebar, 0, 0);
    lv_obj_set_style_bg_color(sidebar, p->surface, 0);
    lv_obj_set_style_border_color(sidebar, p->surface_border, 0);
    lv_obj_set_style_border_width(sidebar, 1, 0);
    lv_obj_set_style_border_side(sidebar, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_radius(sidebar, 0, 0);
    lv_obj_set_style_pad_all(sidebar, 10, 0);

    lbl_files = lv_label_create(sidebar);
    lv_label_set_text(lbl_files, "STORAGE: /sdcard/notes/");
    lv_obj_set_pos(lbl_files, 4, 4);
    lv_obj_set_style_text_font(lbl_files, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_files, p->text_secondary, 0);

    for (int i = 0; i < EDITOR_MAX_FILES; i++) {
        file_btns[i] = lv_button_create(sidebar);
        lv_obj_set_size(file_btns[i], DEVOS_PANE_LEFT_WIDTH - 28, 34);
        lv_obj_set_pos(file_btns[i], 4, 28 + i * 40);
        lv_obj_set_style_bg_color(file_btns[i], p->surface, 0);
        lv_obj_set_style_border_color(file_btns[i], p->surface_border, 0);
        lv_obj_set_style_border_width(file_btns[i], 1, 0);
        lv_obj_set_style_radius(file_btns[i], 4, 0);
        lv_obj_add_flag(file_btns[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(file_btns[i], file_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        file_lbls[i] = lv_label_create(file_btns[i]);
        lv_obj_align(file_lbls[i], LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_size(file_lbls[i], DEVOS_PANE_LEFT_WIDTH - 44, 30);
        lv_label_set_long_mode(file_lbls[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(file_lbls[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(file_lbls[i], p->text_primary, 0);
    }

    /* Main Area */
    main_area = lv_obj_create(screen);
    lv_obj_set_size(main_area, DEVOS_SCREEN_WIDTH - DEVOS_PANE_LEFT_WIDTH,
                    DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(main_area, DEVOS_PANE_LEFT_WIDTH, 0);
    lv_obj_set_style_bg_color(main_area, p->bg, 0);
    lv_obj_set_style_radius(main_area, 0, 0);
    lv_obj_set_style_border_width(main_area, 0, 0);
    lv_obj_set_style_pad_all(main_area, 0, 0);
    lv_obj_clear_flag(main_area, LV_OBJ_FLAG_SCROLLABLE);

    /* Action bar */
    top_bar = lv_obj_create(main_area);
    lv_obj_set_size(top_bar, lv_pct(100), 34);
    lv_obj_set_pos(top_bar, 0, 0);
    lv_obj_set_style_bg_color(top_bar, p->top_bar_bg, 0);
    lv_obj_set_style_border_color(top_bar, p->surface_border, 0);
    lv_obj_set_style_border_width(top_bar, 1, 0);
    lv_obj_set_style_border_side(top_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_set_style_pad_left(top_bar, 10, 0);
    lv_obj_set_style_pad_right(top_bar, 8, 0);
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);

    lbl_fn = lv_label_create(top_bar);
    lv_label_set_text(lbl_fn, "(no file)");
    lv_obj_align(lbl_fn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(lbl_fn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_fn, p->accent_primary, 0);

    /* Action bar buttons: Tree, New, Save, Mode */
    btn_tree = lv_button_create(top_bar);
    lv_obj_set_size(btn_tree, 72, 26);
    lv_obj_set_style_bg_color(btn_tree, p->surface, 0);
    lv_obj_set_style_border_color(btn_tree, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_tree, 1, 0);
    lv_obj_set_style_radius(btn_tree, 4, 0);
    lv_obj_set_style_pad_all(btn_tree, 0, 0);
    lv_obj_add_event_cb(btn_tree, btn_tree_cb, LV_EVENT_CLICKED, NULL);
    lbl_btn_tree = lv_label_create(btn_tree);
    lv_label_set_text(lbl_btn_tree, LV_SYMBOL_DIRECTORY " Tree");
    lv_obj_center(lbl_btn_tree);
    lv_obj_set_style_text_font(lbl_btn_tree, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_btn_tree, p->text_primary, 0);

    btn_new = lv_button_create(top_bar);
    lv_obj_set_size(btn_new, 66, 26);
    lv_obj_set_style_bg_color(btn_new, p->surface, 0);
    lv_obj_set_style_border_color(btn_new, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_new, 1, 0);
    lv_obj_set_style_radius(btn_new, 4, 0);
    lv_obj_set_style_pad_all(btn_new, 0, 0);
    lv_obj_add_event_cb(btn_new, btn_new_cb, LV_EVENT_CLICKED, NULL);
    lbl_btn_new = lv_label_create(btn_new);
    lv_label_set_text(lbl_btn_new, LV_SYMBOL_PLUS " New");
    lv_obj_center(lbl_btn_new);
    lv_obj_set_style_text_font(lbl_btn_new, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_btn_new, p->text_primary, 0);

    btn_save = lv_button_create(top_bar);
    lv_obj_set_size(btn_save, 68, 26);
    lv_obj_set_style_bg_color(btn_save, p->surface, 0);
    lv_obj_set_style_border_color(btn_save, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_save, 1, 0);
    lv_obj_set_style_radius(btn_save, 4, 0);
    lv_obj_set_style_pad_all(btn_save, 0, 0);
    lv_obj_add_event_cb(btn_save, btn_save_cb, LV_EVENT_CLICKED, NULL);
    lbl_btn_save = lv_label_create(btn_save);
    lv_label_set_text(lbl_btn_save, LV_SYMBOL_SAVE " Save");
    lv_obj_center(lbl_btn_save);
    lv_obj_set_style_text_font(lbl_btn_save, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_btn_save, p->text_primary, 0);

    btn_attach = lv_button_create(top_bar);
    lv_obj_set_size(btn_attach, 76, 26);
    lv_obj_set_style_bg_color(btn_attach, p->surface, 0);
    lv_obj_set_style_border_color(btn_attach, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_attach, 1, 0);
    lv_obj_set_style_radius(btn_attach, 4, 0);
    lv_obj_set_style_pad_all(btn_attach, 0, 0);
    lv_obj_add_event_cb(btn_attach, btn_attach_cb, LV_EVENT_CLICKED, NULL);
    lbl_btn_attach = lv_label_create(btn_attach);
    lv_label_set_text(lbl_btn_attach, LV_SYMBOL_UPLOAD " Attach");
    lv_obj_center(lbl_btn_attach);
    lv_obj_set_style_text_font(lbl_btn_attach, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_btn_attach, p->text_primary, 0);

    btn_mode = lv_button_create(top_bar);
    lv_obj_set_size(btn_mode, 92, 26);
    lv_obj_set_style_bg_color(btn_mode, p->surface, 0);
    lv_obj_set_style_border_color(btn_mode, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_mode, 1, 0);
    lv_obj_set_style_radius(btn_mode, 4, 0);
    lv_obj_set_style_pad_all(btn_mode, 0, 0);
    lv_obj_add_event_cb(btn_mode, btn_mode_cb, LV_EVENT_CLICKED, NULL);
    lbl_btn_mode = lv_label_create(btn_mode);
    lv_label_set_text(lbl_btn_mode, "Mode: Edit");
    lv_obj_center(lbl_btn_mode);
    lv_obj_set_style_text_font(lbl_btn_mode, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_btn_mode, p->accent_primary, 0);

    /* Multiline editor */
    ta_editor = lv_textarea_create(main_area);
    lv_textarea_set_text(ta_editor, "");
    lv_textarea_set_max_length(ta_editor, EDITOR_BUF_MAX - 1);
    lv_obj_add_event_cb(ta_editor, editor_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(ta_editor, p->code_bg, 0);
    lv_obj_set_style_text_color(ta_editor, p->text_primary, 0);
    lv_obj_set_style_border_color(ta_editor, p->surface_border, 0);
    lv_obj_set_style_border_width(ta_editor, 0, 0);
    lv_obj_set_style_radius(ta_editor, 0, 0);
    lv_obj_set_style_pad_all(ta_editor, 16, 0);
    lv_obj_set_style_text_font(ta_editor, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(ta_editor, p->accent_primary, LV_PART_CURSOR);
    lv_obj_set_style_border_color(ta_editor, p->accent_primary, LV_PART_CURSOR);

    /* Preview pane (hidden in Edit view) */
    preview_scroll = lv_obj_create(main_area);
    lv_obj_set_style_bg_color(preview_scroll, p->code_bg, 0);
    lv_obj_set_style_radius(preview_scroll, 0, 0);
    lv_obj_set_style_border_width(preview_scroll, 0, 0);
    lv_obj_set_style_pad_all(preview_scroll, 16, 0);
    lv_obj_add_flag(preview_scroll, LV_OBJ_FLAG_HIDDEN);

    devos_theme_add_listener(apply_theme, NULL);
    lv_timer_create(preview_timer_cb, 400, NULL);

    scan_notes();
    apply_layout();
    if (s_file_count > 0) {
        /* ponytail: open welcome.md first when present, else alphabetical first */
        int welcome = -1;
        for (int i = 0; i < s_file_count; i++) {
            if (strcmp(s_files[i], "welcome.md") == 0) { welcome = i; break; }
        }
        open_file(welcome >= 0 ? welcome : 0);
    } else {
        update_title();
        refresh_file_list();
        update_telemetry();
    }
}

static void editor_show(void)
{
    /* Rescan so files created elsewhere (agent export, PC) appear */
    int keep = s_active;
    char keep_name[EDITOR_NAME_MAX] = {0};
    if (keep >= 0 && keep < s_file_count) {
        snprintf(keep_name, sizeof(keep_name), "%s", s_files[keep]);
    }
    scan_notes();
    int found = -1;
    for (int i = 0; i < s_file_count; i++) {
        if (strcmp(s_files[i], keep_name) == 0) { found = i; break; }
    }
    if (found >= 0) {
        /* Still there: reload if not dirty to pick up external changes */
        s_active = found;
        s_sel = found;
        if (!s_dirty) {
            static char buf[EDITOR_BUF_MAX];
            read_file(s_files[found], buf, sizeof(buf));
            lv_textarea_set_text(ta_editor, buf);
        }
        update_title();
        refresh_file_list();
    } else if (s_file_count > 0) {
        open_file(0);
    } else {
        s_active = -1;
        if (ta_editor) lv_textarea_set_text(ta_editor, "");
        update_title();
        refresh_file_list();
    }
    update_telemetry();
    if (s_view != EDITOR_VIEW_EDIT) render_preview(false);
}

static void editor_hide(void)
{
    if (s_dirty) save_file();
}

static int editor_telemetry_lines(char lines[3][64])
{
    const char *fn = app_editor_get_active_filename();
    snprintf(lines[0], sizeof(lines[0]), "* %s", fn && *fn ? fn : "(no file)");
    snprintf(lines[1], sizeof(lines[1]), "* Markdown notes");
    snprintf(lines[2], sizeof(lines[2]), "* Split preview");
    return 3;
}

devos_app_descriptor_t *app_editor_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_EDITOR;
    app_descriptor.uid = "editor";
    app_descriptor.icon = LV_SYMBOL_DIRECTORY;
    app_descriptor.category = "notes";
    app_descriptor.name = "Editor";
    app_descriptor.title = "Markdown Editor";
    app_descriptor.subtitle = "Distraction-Free Notes & Docs";
    app_descriptor.screen = screen;
    app_descriptor.init = editor_init;
    app_descriptor.show = editor_show;
    app_descriptor.hide = editor_hide;
    app_descriptor.handle_key = editor_handle_key;
    app_descriptor.get_telemetry_lines = editor_telemetry_lines;

    return &app_descriptor;
}

const char *app_editor_get_active_filename(void)
{
    if (s_active >= 0 && s_active < s_file_count) {
        return s_files[s_active];
    }
    return NULL;
}

const char *app_editor_get_active_text(void)
{
    if (ta_editor) {
        return lv_textarea_get_text(ta_editor);
    }
    return NULL;
}

bool app_editor_save_plan(const char *title, const char *content)
{
    if (!title || !content) return false;
    char clean[64];
    size_t ci = 0;
    for (const char *p = title; *p && ci < sizeof(clean) - 1; p++) {
        char c = *p;
        if (isalnum((unsigned char)c) || c == '-' || c == '_') {
            clean[ci++] = c;
        } else if (c == ' ' || c == ':' || c == '/') {
            if (ci > 0 && clean[ci - 1] != '_') clean[ci++] = '_';
        }
    }
    clean[ci] = '\0';
    if (ci == 0) snprintf(clean, sizeof(clean), "%s", "plan");

    char path[256];
    snprintf(path, sizeof(path), "%s/plans/%s.md", TAB5_SD_MOUNT_POINT, clean);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs(content, f);
    fclose(f);
    return true;
}

bool app_editor_save_diff(const char *title, const char *diff_content)
{
    if (!title || !diff_content) return false;
    char clean[64];
    size_t ci = 0;
    for (const char *p = title; *p && ci < sizeof(clean) - 1; p++) {
        char c = *p;
        if (isalnum((unsigned char)c) || c == '-' || c == '_') {
            clean[ci++] = c;
        } else if (c == ' ' || c == ':' || c == '/') {
            if (ci > 0 && clean[ci - 1] != '_') clean[ci++] = '_';
        }
    }
    clean[ci] = '\0';
    if (ci == 0) snprintf(clean, sizeof(clean), "%s", "diff");

    char path[256];
    snprintf(path, sizeof(path), "%s/diffs/%s.diff", TAB5_SD_MOUNT_POINT, clean);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs(diff_content, f);
    fclose(f);
    return true;
}
