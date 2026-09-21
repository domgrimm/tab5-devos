#include "app_editor.h"
#include "devos_config.h"
#include "devos_theme.h"
#include "devos_core.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>

/* Nimbus Mono 14: regular-weight mono matching Montserrat 14 body size.
 * Generated via lv_font_conv from NimbusMonoPS-Regular.otf (see file header
 * in components/devos_ui/lv_font_nimbus_mono_14.c to regenerate). */
LV_FONT_DECLARE(lv_font_nimbus_mono_14);

#define EDITOR_MAX_FILES 12
#define EDITOR_NAME_MAX 64
#define EDITOR_BUF_MAX (16 * 1024)
#define MD_LINE_MAX 512
#define MD_TABLE_ROWS 18
#define MD_TABLE_COLS 6
#define MD_CELL_MAX 28
#define MD_CELL_PAD 6

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
static size_t md_trunc_ok(const char *s, size_t n);

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
        strncpy(s_files[s_file_count], ent->d_name, EDITOR_NAME_MAX - 1);
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
    n = md_trunc_ok(buf, n);
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
        strncpy(t.editor_file, s_files[s_active], sizeof(t.editor_file) - 1);
        t.editor_file[sizeof(t.editor_file) - 1] = '\0';
        const char *text = ta_editor ? lv_textarea_get_text(ta_editor) : "";
        size_t bytes = strlen(text ? text : "");
        t.editor_file_kb = (uint32_t)((bytes + 1023) / 1024);
        if (bytes > 0 && t.editor_file_kb == 0) t.editor_file_kb = 1;
    } else {
        strncpy(t.editor_file, "(none)", sizeof(t.editor_file) - 1);
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
    if (top_bar && btn_mode && btn_save && btn_new && btn_tree) {
        lv_obj_align(btn_mode, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_align_to(btn_save, btn_mode, LV_ALIGN_OUT_LEFT_MID, -6, 0);
        lv_obj_align_to(btn_new, btn_save, LV_ALIGN_OUT_LEFT_MID, -6, 0);
        lv_obj_align_to(btn_tree, btn_new, LV_ALIGN_OUT_LEFT_MID, -6, 0);
        if (lbl_fn) {
            int lbl_w = main_w - 320;
            if (lbl_w < 120) lbl_w = 120;
            lv_obj_set_width(lbl_fn, lbl_w);
        }
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

/* Lightweight Markdown renderer: one label per block, styled by prefix.
 * (Inline bold/italic spans are out of scope; raw markers stay visible.) */
/* --------------------------------------------------------------------------
 * Markdown renderer (CommonMark subset)
 *
 * Blocks: ATX headings, fenced code, tables, hr, quotes, ul/ol/task lists,
 * paragraphs. Inline: **bold**, *italic*, ~~strike~~, `code`, links, images,
 * autolinks, backslash escapes. Code and tables use Nimbus Mono 14, a
 * regular-weight mono sized to match Montserrat 14 body text.
 *
 * Single regular font in the build, so emphasis falls back honestly:
 *   bold   -> underline (typewriter emphasis; links use color-only, no clash)
 *   italic -> secondary color (no oblique face exists)
 * Out of scope: setext headings, reference links, bare-URL linking,
 * nested-bracket links, indented code blocks, CJK column widths,
 * `\|` escapes in tables.
 * -------------------------------------------------------------------------- */

/* Emit one styled run (lv_span_set_text copies, so a shared temp is safe) */
static void md_add_span(lv_obj_t *sg, const char *s, size_t n,
                        const lv_font_t *font, lv_color_t color,
                        lv_text_decor_t decor)
{
    static char seg[501];
    while (n > 0) {
        size_t chunk = n > 500 ? 500 : n;
        memcpy(seg, s, chunk);
        seg[chunk] = '\0';
        lv_span_t *sp = lv_spangroup_new_span(sg);
        lv_span_set_text(sp, seg);
        lv_style_t *st = lv_span_get_style(sp);
        lv_style_set_text_font(st, font);
        lv_style_set_text_color(st, color);
        lv_style_set_text_decor(st, decor);
        s += chunk;
        n -= chunk;
    }
}

static const char *md_find_marker(const char *s, size_t n, const char *m,
                                  size_t from)
{
    size_t mlen = strlen(m);
    for (size_t i = from; i + mlen <= n; i++) {
        if (memcmp(s + i, m, mlen) == 0) return s + i;
    }
    return NULL;
}

/* Link destination after "](": sets *url/*ulen, returns closing ')', or NULL.
 * Handles <dest> (spaces allowed), balanced parens, and "title" tails. */
static const char *md_link_end(const char *s, size_t n, const char **url,
                               size_t *ulen)
{
    const char *end = s + n;
    const char *p = s;
    const char *u;
    size_t ul;
    if (p < end && *p == '<') {
        const char *gt = memchr(p + 1, '>', (size_t)(end - (p + 1)));
        if (!gt) return NULL;
        u = p + 1;
        ul = (size_t)(gt - u);
        p = gt + 1;
    } else {
        int depth = 0;
        const char *q = p;
        while (q < end && (depth > 0 ||
               (*q != ' ' && *q != '\t' && *q != ')'))) {
            if (*q == '(') depth++;
            else if (*q == ')') depth--;
            q++;
        }
        u = p;
        ul = (size_t)(q - p);
        p = q;
    }
    while (p < end && *p != ')') p++; /* skip "title" to the closer */
    if (p >= end) return NULL;
    *url = u;
    *ulen = ul;
    return p;
}

/* Closer `*` that is not half of a `**` run nor intraword (2*3*4) */
static const char *md_find_star(const char *s, size_t n, size_t from)
{
    for (size_t i = from; i < n; i++) {
        if (s[i] != '*') continue;
        if ((i > 0 && s[i - 1] == '*') || (i + 1 < n && s[i + 1] == '*')) continue;
        bool prev_a = i > 0 && isalnum((unsigned char)s[i - 1]);
        bool next_a = i + 1 < n && isalnum((unsigned char)s[i + 1]);
        if (prev_a && next_a) continue; /* intraword, not emphasis */
        return s + i;
    }
    return NULL;
}

static bool md_star_opener(const char *s, size_t n, size_t i)
{
    bool prev_a = i > 0 && isalnum((unsigned char)s[i - 1]);
    bool next_a = i + 1 < n && isalnum((unsigned char)s[i + 1]);
    return !(prev_a && next_a);
}

static bool md_uscore_edge(const char *s, size_t n, size_t i, bool opener)
{
    bool prev_alnum = i > 0 && isalnum((unsigned char)s[i - 1]);
    bool next_alnum = i + 1 < n && isalnum((unsigned char)s[i + 1]);
    bool next_space = i + 1 >= n || isspace((unsigned char)s[i + 1]);
    return opener ? (!prev_alnum && !next_space) : (!next_alnum);
}

static void md_render_inline(lv_obj_t *sg, const char *s, size_t n,
                             const lv_font_t *font, lv_color_t color,
                             lv_text_decor_t decor, const devos_palette_t *p)
{
    size_t i = 0, start = 0;
    while (i < n) {
        char c = s[i];

        /* Image: ![alt](src) */
        if (c == '!' && i + 1 < n && s[i + 1] == '[') {
            const char *k = md_find_marker(s, n, "](", i + 2);
            const char *e = NULL;
            const char *u = NULL;
            size_t ul = 0;
            if (k) {
                const char *inner_bracket = memchr(s + i + 2, ']', (size_t)(k - (s + i + 2)));
                if (!inner_bracket) e = md_link_end(k + 2, (s + n) - (k + 2), &u, &ul);
            }
            if (e) {
                md_add_span(sg, s + start, i - start, font, color, decor);
                md_add_span(sg, "[Image: ", 8, font, p->text_muted, decor);
                md_add_span(sg, s + i + 2, k - (s + i + 2), font,
                            p->text_muted, decor);
                md_add_span(sg, "]", 1, font, p->text_muted, decor);
                i = (size_t)(e - s) + 1;
                start = i;
                continue;
            }
        }

        /* Link: [text](url) */
        if (c == '[') {
            const char *k = md_find_marker(s, n, "](", i + 1);
            const char *e = NULL;
            const char *u = NULL;
            size_t ulen = 0;
            if (k) {
                const char *inner_bracket = memchr(s + i + 1, ']', (size_t)(k - (s + i + 1)));
                if (!inner_bracket) e = md_link_end(k + 2, (s + n) - (k + 2), &u, &ulen);
            }
            if (e) {
                md_add_span(sg, s + start, i - start, font, color, decor);
                md_render_inline(sg, s + i + 1, k - (s + i + 1), font,
                                 p->accent_primary, decor, p);
                /* ponytail: device can't tap links, so the URL stays visible */
                md_add_span(sg, " (", 2, font, p->text_muted, decor);
                md_add_span(sg, u, ulen, font, p->text_muted, decor);
                md_add_span(sg, ")", 1, font, p->text_muted, decor);
                i = (size_t)(e - s) + 1;
                start = i;
                continue;
            }
        }

        /* Autolink: <https://..> / <mail@..> */
        if (c == '<') {
            const char *e = memchr(s + i + 1, '>', n - i - 1);
            if (e && e - (s + i + 1) > 3) {
                bool spaces = false;
                for (const char *t = s + i + 1; t < e; t++) {
                    if (isspace((unsigned char)*t)) { spaces = true; break; }
                }
                if (!spaces) {
                    md_add_span(sg, s + start, i - start, font, color, decor);
                    md_add_span(sg, s + i + 1, (size_t)(e - (s + i + 1)), font,
                                p->accent_primary, decor);
                    i = (size_t)(e - s) + 1;
                    start = i;
                    continue;
                }
            }
        }

        /* Backslash escape */
        if (c == '\\' && i + 1 < n && ispunct((unsigned char)s[i + 1])) {
            md_add_span(sg, s + start, i - start, font, color, decor);
            md_add_span(sg, s + i + 1, 1, font, color, decor);
            i += 2;
            start = i;
            continue;
        }

        /* Inline code (literal, no nesting) */
        if (c == '`') {
            const char *e = md_find_marker(s, n, "`", i + 1);
            if (e && e > s + i + 1) {
                md_add_span(sg, s + start, i - start, font, color, decor);
                md_add_span(sg, s + i + 1, (size_t)(e - (s + i + 1)), font,
                            p->accent_secondary, decor);
                i = (size_t)(e - s) + 1;
                start = i;
                continue;
            }
        }

        /* Bold ** / __ */
        if ((c == '*' && i + 1 < n && s[i + 1] == '*') ||
            (c == '_' && i + 1 < n && s[i + 1] == '_' &&
             md_uscore_edge(s, n, i, true))) {
            char m[3] = {c, c, '\0'};
            const char *e = md_find_marker(s, n, m, i + 2);
            if (e && e > s + i + 2) {
                md_add_span(sg, s + start, i - start, font, color, decor);
                md_render_inline(sg, s + i + 2, (size_t)(e - (s + i + 2)),
                                 font, color,
                                 (lv_text_decor_t)(decor | LV_TEXT_DECOR_UNDERLINE),
                                 p);
                i = (size_t)(e - s) + 2;
                start = i;
                continue;
            }
        }

        /* Strikethrough ~~ */
        if (c == '~' && i + 1 < n && s[i + 1] == '~') {
            const char *e = md_find_marker(s, n, "~~", i + 2);
            if (e && e > s + i + 2) {
                md_add_span(sg, s + start, i - start, font, color, decor);
                md_render_inline(sg, s + i + 2, (size_t)(e - (s + i + 2)),
                                 font, color,
                                 (lv_text_decor_t)(decor | LV_TEXT_DECOR_STRIKETHROUGH),
                                 p);
                i = (size_t)(e - s) + 2;
                start = i;
                continue;
            }
        }

        /* Italic * / _ */
        if (c == '*' && (i + 1 >= n || s[i + 1] != '*') &&
            md_star_opener(s, n, i)) {
            const char *e = md_find_star(s, n, i + 1);
            if (e && e > s + i + 1) {
                md_add_span(sg, s + start, i - start, font, color, decor);
                md_render_inline(sg, s + i + 1, (size_t)(e - (s + i + 1)),
                                 font, p->text_secondary, decor, p);
                i = (size_t)(e - s) + 1;
                start = i;
                continue;
            }
        }
        if (c == '_' && md_uscore_edge(s, n, i, true)) {
            size_t j = i + 1;
            const char *e = NULL;
            for (; j < n; j++) {
                if (s[j] == '_' && (j + 1 >= n || s[j + 1] != '_') &&
                    (j == 0 || s[j - 1] != '_') && md_uscore_edge(s, n, j, false)) {
                    e = s + j;
                    break;
                }
            }
            if (e && e > s + i + 1) {
                md_add_span(sg, s + start, i - start, font, color, decor);
                md_render_inline(sg, s + i + 1, (size_t)(e - (s + i + 1)),
                                 font, p->text_secondary, decor, p);
                i = (size_t)(e - s) + 1;
                start = i;
                continue;
            }
        }

        i++;
    }
    md_add_span(sg, s + start, n - start, font, color, decor);
}

/* Strip inline markers for contexts that can't style (table cells) */
static size_t md_strip_into(const char *s, size_t n, char *out, size_t cap,
                            size_t o)
{
    size_t i = 0;
    while (i < n && o + 1 < cap) {
        char c = s[i];
        /* Doubled markers: drop openers and closers alike */
        if ((c == '*' || c == '_' || c == '~' || c == '`') && i + 1 < n &&
            s[i + 1] == c) {
            i += 2;
            continue;
        }
        /* Singles: strip the pair, recurse into the middle */
        if (c == '*' || c == '_' || c == '`') {
            bool ok = false;
            if (c == '`') ok = true;
            else if (c == '*') ok = md_star_opener(s, n, i);
            else ok = md_uscore_edge(s, n, i, true);
            const char *e = NULL;
            if (ok) {
                for (size_t j = i + 1; j < n; j++) {
                    if (s[j] != c) continue;
                    if (j + 1 < n && s[j + 1] == c) { j++; continue; }
                    if (j > 0 && s[j - 1] == c) continue;
                    e = s + j;
                    break;
                }
            }
            if (e) {
                o = md_strip_into(s + i + 1, (size_t)(e - (s + i + 1)), out,
                                  cap, o);
                i = (size_t)(e - s) + 1;
                continue;
            }
        }
        if (c == '[') {
            const char *k = md_find_marker(s, n, "](", i + 1);
            const char *e = NULL;
            const char *u = NULL;
            size_t ulen = 0;
            if (k) {
                const char *inner_bracket = memchr(s + i + 1, ']', (size_t)(k - (s + i + 1)));
                if (!inner_bracket) e = md_link_end(k + 2, (s + n) - (k + 2), &u, &ulen);
            }
            if (e) {
                o = md_strip_into(s + i + 1, (size_t)(k - (s + i + 1)), out,
                                  cap, o);
                if (o + 3 >= cap) break;
                out[o++] = ' ';
                out[o++] = '(';
                if (o + ulen + 1 >= cap) break;
                memcpy(out + o, u, ulen);
                o += ulen;
                out[o++] = ')';
                i = (size_t)(e - s) + 1;
                continue;
            }
        }
        out[o++] = c;
        i++;
    }
    out[o] = '\0';
    return o;
}

static size_t md_strip_inline(const char *s, size_t n, char *out, size_t cap)
{
    size_t o = md_strip_into(s, n, out, cap, 0);
    out[o] = '\0';
    return o;
}

static lv_obj_t *md_new_block(int y)
{
    lv_obj_t *sg = lv_spangroup_create(preview_scroll);
    lv_obj_set_width(sg, lv_pct(100));
    lv_obj_set_pos(sg, 0, y);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
    lv_obj_set_style_pad_all(sg, 0, 0);
    lv_obj_set_style_border_width(sg, 0, 0);
    return sg;
}

/* Fence accumulation: one contiguous block, not striped per-line boxes */
static char s_codebuf[3072];
static size_t s_codelen = 0;
static bool s_codedrop = false;

static void md_finish_block(lv_obj_t *sg, int *y, int gap);

static void md_emit_code_block(int *y, const devos_palette_t *p)
{
    if (s_codelen == 0) return;
    if (s_codebuf[s_codelen - 1] == '\n') s_codebuf[--s_codelen] = '\0';
    lv_obj_t *sg = md_new_block(*y);
    lv_obj_set_style_bg_color(sg, p->surface_active, 0);
    lv_obj_set_style_bg_opa(sg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sg, 4, 0);
    lv_obj_set_style_pad_all(sg, 8, 0);
    md_add_span(sg, s_codebuf, s_codelen, &lv_font_nimbus_mono_14,
                p->text_primary, LV_TEXT_DECOR_NONE);
    md_finish_block(sg, y, 8);
    s_codelen = 0;
}

static void md_finish_block(lv_obj_t *sg, int *y, int gap)
{
    lv_spangroup_refr_mode(sg);
    lv_obj_update_layout(sg);
    *y += lv_obj_get_height(sg) + gap;
}

/* Back a byte count off a split UTF-8 character so truncation never
 * produces tofu (editor targets notes, not CJK tables, but no tofu). */
static size_t md_trunc_ok(const char *s, size_t n)
{
    if (n == 0) return 0;
    if ((s[n - 1] & 0xC0) == 0xC0) {
        n--; /* lead byte with its tail cut off */
    } else {
        while (n > 0 && (s[n] & 0xC0) == 0x80) n--;
    }
    return n;
}

/* Split a |table| row into trimmed cells. Returns cell count. */
static int md_split_row(char *row, char cells[MD_TABLE_COLS][MD_CELL_MAX + 1])
{
    while (*row == ' ' || *row == '\t') row++;
    size_t len = strlen(row);
    while (len > 0 && (row[len - 1] == ' ' || row[len - 1] == '\t')) row[--len] = '\0';
    if (len > 0 && row[0] == '|') { row++; len--; }
    while (len > 0 && (row[len - 1] == ' ' || row[len - 1] == '\t')) row[--len] = '\0';
    if (len > 0 && row[len - 1] == '|') row[--len] = '\0';

    int count = 0;
    char *p = row;
    while (*p && count < MD_TABLE_COLS) {
        char *sep = strchr(p, '|');
        if (sep) *sep = '\0';

        char *tok = p;
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t tlen = strlen(tok);
        while (tlen > 0 && (tok[tlen - 1] == ' ' || tok[tlen - 1] == '\t')) tok[--tlen] = '\0';
        if (tlen > MD_CELL_MAX) tlen = md_trunc_ok(tok, MD_CELL_MAX);
        memcpy(cells[count], tok, tlen);
        cells[count][tlen] = '\0';
        count++;

        if (!sep) break;
        p = sep + 1;
    }
    return count;
}

static bool md_is_delim_cell(const char *cell, int *align)
{
    size_t n = strlen(cell);
    if (n == 0) return false;
    bool left = cell[0] == ':';
    bool right = n > 1 && cell[n - 1] == ':';
    size_t a = left ? 1 : 0;
    size_t b = right ? n - 1 : n;
    if (b <= a) return false;
    for (size_t i = a; i < b; i++) {
        if (cell[i] != '-') return false;
    }
    *align = left && right ? 1 : (right ? 2 : 0); /* 0L 1C 2R */
    return true;
}

/* Whole-line delimiter check (for pipe-less GFM table lookahead) */
static bool md_is_delim_line(const char *s)
{
    static char tmp[MD_LINE_MAX];
    static char cells[MD_TABLE_COLS][MD_CELL_MAX + 1];
    size_t n = strlen(s);
    if (n > sizeof(tmp) - 1) n = sizeof(tmp) - 1;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    /* ponytail: md_split_row takes char* (strtok); tmp is expendable */
    int count = md_split_row(tmp, cells);
    if (count == 0) return false;
    for (int c = 0; c < count; c++) {
        int dummy;
        if (!md_is_delim_cell(cells[c], &dummy)) return false;
    }
    return true;
}

/* Padded string for table cells (lv_table_set_cell_value copies) */
static size_t md_pad_string(const char *cell, int width, int align,
                            char *out, size_t cap)
{
    size_t n = strlen(cell);
    if ((int)n > width) n = md_trunc_ok(cell, (size_t)width);
    int fill = width - (int)n;
    int left = align == 2 ? fill : (align == 1 ? fill / 2 : 0);
    int right = fill - left;
    size_t o = 0;
    while (left-- > 0 && o + 1 < cap) out[o++] = ' ';
    if (o + n + 1 > cap) n = cap - o - 1;
    memcpy(out + o, cell, n);
    o += n;
    while (right-- > 0 && o + 1 < cap) out[o++] = ' ';
    out[o] = '\0';
    return o;
}

static void render_preview(bool preserve_scroll)
{
    if (!preview_scroll || !ta_editor) return;
    const devos_palette_t *p = devos_theme_get();

    int32_t scroll_y = preserve_scroll ? lv_obj_get_scroll_y(preview_scroll) : 0;
    lv_obj_clean(preview_scroll);
    lv_obj_update_layout(preview_scroll);

    const char *text = lv_textarea_get_text(ta_editor);
    if (!text) text = "";

    static char line[MD_LINE_MAX];
    static char tline[MD_LINE_MAX];
    static char rowbuf[MD_LINE_MAX];
    static char cells[MD_TABLE_ROWS][MD_TABLE_COLS][MD_CELL_MAX + 1];

    bool in_code = false;
    bool in_ol = false;
    int ol_num = 1;
    int y = 0;
    const char *cur = text;
    for (;;) {
        const char *nl = strchr(cur, '\n');
        size_t len = nl ? (size_t)(nl - cur) : strlen(cur);
        if (len > sizeof(line) - 1) len = md_trunc_ok(cur, sizeof(line) - 1);
        memcpy(line, cur, len);
        line[len] = '\0';
        while (len > 0 && line[len - 1] == '\r') line[--len] = '\0';

        const char *t = line;
        while (*t == ' ' || *t == '\t') t++; /* block indent tolerance */

        /* Fence toggle (info string ignored) */
        if (strncmp(t, "```", 3) == 0) {
            if (in_code) {
                md_emit_code_block(&y, p);
            } else {
                s_codelen = 0;
                s_codedrop = false;
            }
            in_code = !in_code;
            in_ol = false;
            if (!nl) break;
            cur = nl + 1;
            continue;
        }

        if (in_code) {
            if (!s_codedrop) {
                /* ponytail: truncate absurd fences instead of overflowing */
                if (s_codelen + len + 1 < sizeof(s_codebuf)) {
                    memcpy(s_codebuf + s_codelen, line, len);
                    s_codelen += len;
                    s_codebuf[s_codelen++] = '\n';
                } else {
                    s_codedrop = true;
                }
            }
            if (!nl) break;
            cur = nl + 1;
            continue;
        }

        /* Blank */
        if (len == 0) {
            y += 10;
            in_ol = false;
            if (!nl) break;
            cur = nl + 1;
            continue;
        }

        /* HR: --- *** ___ (3+ of one marker, spaces tolerated) */
        {
            char m = 0;
            int cnt = 0;
            bool hr = true;
            for (const char *c = t; *c; c++) {
                if (*c == ' ' || *c == '\t') continue;
                if (*c != '-' && *c != '*' && *c != '_') { hr = false; break; }
                if (!m) m = *c;
                if (*c != m) { hr = false; break; }
                cnt++;
            }
            if (hr && cnt >= 3) {
                lv_obj_t *bar = lv_obj_create(preview_scroll);
                lv_obj_set_size(bar, lv_pct(100), 2);
                lv_obj_set_pos(bar, 0, y + 7);
                lv_obj_set_style_bg_color(bar, p->surface_border, 0);
                lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(bar, 0, 0);
                lv_obj_set_style_radius(bar, 1, 0);
                lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
                y += 18;
                in_ol = false;
                if (!nl) break;
                cur = nl + 1;
                continue;
            }
        }

        /* ATX headings (closing #'s stripped) */
        if (*t == '#') {
            int level = 0;
            while (t[level] == '#') level++;
            if (level <= 6 && (t[level] == ' ' || t[level] == '\t' || t[level] == '\0')) {
                const char *c = t + level;
                while (*c == ' ' || *c == '\t') c++;
                static char head[MD_LINE_MAX];
                size_t hlen = strlen(c);
                if (hlen > sizeof(head) - 1) hlen = md_trunc_ok(c, sizeof(head) - 1);
                memcpy(head, c, hlen);
                head[hlen] = '\0';
                while (hlen > 0 && (head[hlen - 1] == ' ' || head[hlen - 1] == '\t')) {
                    head[--hlen] = '\0';
                }
                while (hlen > 0 && head[hlen - 1] == '#') head[--hlen] = '\0';
                while (hlen > 0 && (head[hlen - 1] == ' ' || head[hlen - 1] == '\t')) {
                    head[--hlen] = '\0';
                }
                /* ponytail: H1 16/accent, H2 16/primary, H3 14/accent, H4+ 12/muted */
                const lv_font_t *f = level == 1 ? &lv_font_montserrat_16
                                    : level == 2 ? &lv_font_montserrat_16
                                    : level == 3 ? &lv_font_montserrat_14
                                                 : &lv_font_montserrat_12;
                lv_color_t col = level == 1 ? p->accent_primary
                               : level == 2 ? p->text_primary
                               : level == 3 ? p->accent_primary
                                            : p->text_secondary;
                lv_obj_t *sg = md_new_block(y);
                md_render_inline(sg, head, hlen, f, col, LV_TEXT_DECOR_NONE, p);
                md_finish_block(sg, &y, 8);
                in_ol = false;
                if (!nl) break;
                cur = nl + 1;
                continue;
            }
        }

        /* Table: header line with | + delimiter row (outer pipes optional) */
        if (strchr(t, '|') != NULL && nl != NULL) {
            /* ponytail: peek the next line; only a delimiter commits to table */
            const char *nstart = nl + 1;
            const char *nnl = strchr(nstart, '\n');
            size_t nlen = nnl ? (size_t)(nnl - nstart) : strlen(nstart);
            bool delim_next = false;
            if (nlen < sizeof(tline)) {
                memcpy(tline, nstart, nlen);
                tline[nlen] = '\0';
                while (nlen > 0 && tline[nlen - 1] == '\r') tline[--nlen] = '\0';
                delim_next = md_is_delim_line(tline);
            }
            if (delim_next) {
                const char *tt = cur;
                int rows = 0;
                int cols = 0;
                int rowcounts[MD_TABLE_ROWS] = {0};
                bool fits = true;
            while (tt && rows < MD_TABLE_ROWS) {
                const char *tnl = strchr(tt, '\n');
                size_t tlen = tnl ? (size_t)(tnl - tt) : strlen(tt);
                if (tlen > sizeof(tline) - 1) tlen = md_trunc_ok(tt, sizeof(tline) - 1);
                    memcpy(tline, tt, tlen);
                    tline[tlen] = '\0';
                    while (tlen > 0 && tline[tlen - 1] == '\r') tline[--tlen] = '\0';
                    const char *tl = tline;
                    while (*tl == ' ' || *tl == '\t') tl++;
                    /* body rows need |; the delim row (rows==1) was pre-validated */
                    if (!strchr(tl, '|') &&
                        !(rows == 1 && md_is_delim_line(tline))) {
                        break;
                    }
                rowcounts[rows] = md_split_row(tline, cells[rows]);
                if (rows == 0) cols = rowcounts[0];
                if (rowcounts[rows] == 0) { fits = false; break; }
                rows++;
                tt = tnl ? tnl + 1 : NULL;
            }
            /* ponytail: ragged rows pad empty; delimiter must cover all cols */
            bool valid = fits && rows >= 2 && cols > 0 && cols <= MD_TABLE_COLS &&
                         rowcounts[1] >= cols;
            int aligns[MD_TABLE_COLS] = {0};
            if (valid) {
                for (int c = 0; c < cols; c++) {
                    if (!md_is_delim_cell(cells[1][c], &aligns[c])) {
                        valid = false;
                        break;
                    }
                }
            }
            if (valid) {
                /* ponytail: LVGL 9.2 tables style every cell from ITEMS with
                 * no per-cell override, so the accent header is its own
                 * 1-row table stacked on the body table. Shared column
                 * widths; alignment is space padding (mono font aligns it). */
                int glyph_adv = lv_text_get_width("0000000000", 10,
                                                  &lv_font_nimbus_mono_14, 0) / 10;
                if (glyph_adv < 1) glyph_adv = 8;
                int widths[MD_TABLE_COLS] = {0};
                for (int r = 0; r < rows; r++) {
                    if (r == 1) continue; /* delimiter */
                    for (int c = 0; c < cols; c++) {
                        const char *cell = c < rowcounts[r] ? cells[r][c] : "";
                        int w = (int)strlen(cell);
                        if (w > widths[c]) widths[c] = w;
                    }
                }
                /* Shrink widest cols (in px, incl. cell padding) to fit.
                 * ponytail: columns share the content width minus the card
                 * chrome (MAIN pad 16 + MAIN border 2). */
                int max_px = (int)lv_obj_get_content_width(preview_scroll) - 18;
                if (max_px < 160) max_px = 160;
                for (;;) {
                    int total = 0;
                    for (int c = 0; c < cols; c++) {
                        total += widths[c] * glyph_adv + 2 * MD_CELL_PAD;
                    }
                    int widest = -1;
                    for (int c = 0; c < cols; c++) {
                        if (widths[c] > 4 &&
                            (widest < 0 || widths[c] > widths[widest])) {
                            widest = c;
                        }
                    }
                    if (total <= max_px || widest < 0) break;
                    widths[widest]--;
                }
                int body_rows = 0;
                for (int r = 2; r < rows; r++) body_rows++;
                for (int pass = 0; pass < 2; pass++) {
                    bool is_head = pass == 0;
                    int nrows = is_head ? 1 : body_rows;
                    if (nrows == 0) continue;
                    lv_obj_t *tbl = lv_table_create(preview_scroll);
                    int tw = 18; /* MAIN pad 16 + MAIN border 2 */
                    for (int c = 0; c < cols; c++) {
                        tw += widths[c] * glyph_adv + 2 * MD_CELL_PAD;
                    }
                    lv_obj_set_size(tbl, tw, LV_SIZE_CONTENT);
                    lv_obj_set_pos(tbl, 0, y);
                    lv_obj_set_style_bg_color(tbl, p->surface, 0);
                    lv_obj_set_style_bg_opa(tbl, LV_OPA_COVER, 0);
                    lv_obj_set_style_border_color(tbl, p->surface_border, 0);
                    lv_obj_set_style_border_width(tbl, 1, 0);
                    lv_obj_set_style_radius(tbl, 4, 0);
                    lv_obj_set_style_pad_all(tbl, 8, 0);
                    lv_obj_set_style_bg_color(tbl, is_head ? p->surface_active : p->surface,
                                               LV_PART_ITEMS);
                    lv_obj_set_style_bg_opa(tbl, LV_OPA_COVER, LV_PART_ITEMS);
                    lv_obj_set_style_text_font(tbl, &lv_font_nimbus_mono_14,
                                               LV_PART_ITEMS);
                    lv_obj_set_style_text_color(tbl,
                        is_head ? p->accent_primary : p->text_primary,
                        LV_PART_ITEMS);
                    lv_obj_set_style_border_color(tbl, p->surface_border,
                                                  LV_PART_ITEMS);
                    lv_obj_set_style_border_width(tbl, 1, LV_PART_ITEMS);
                    lv_obj_set_style_border_side(tbl, LV_BORDER_SIDE_FULL,
                                                 LV_PART_ITEMS);
                    lv_obj_set_style_pad_left(tbl, MD_CELL_PAD, LV_PART_ITEMS);
                    lv_obj_set_style_pad_right(tbl, MD_CELL_PAD, LV_PART_ITEMS);
                    lv_obj_set_style_pad_top(tbl, 4, LV_PART_ITEMS);
                    lv_obj_set_style_pad_bottom(tbl, 4, LV_PART_ITEMS);
                    lv_obj_clear_flag(tbl, LV_OBJ_FLAG_SCROLLABLE);
                    lv_table_set_column_count(tbl, (uint32_t)cols);
                    lv_table_set_row_count(tbl, (uint32_t)nrows);
                    for (int c = 0; c < cols; c++) {
                        lv_table_set_column_width(tbl, (uint32_t)c,
                            (int32_t)(widths[c] * glyph_adv + 2 * MD_CELL_PAD));
                    }
                    for (int r = 0; r < nrows; r++) {
                        int src = is_head ? 0 : r + 2;
                        for (int c = 0; c < cols; c++) {
                            /* ponytail: LVGL table cells are plain text,
                             * so strip markers (no spans to break) */
                            const char *cell = c < rowcounts[src] ? cells[src][c] : "";
                            md_strip_inline(cell, strlen(cell),
                                            rowbuf, sizeof(rowbuf));
                            char padded[MD_CELL_MAX + 1];
                            md_pad_string(rowbuf, widths[c], aligns[c],
                                          padded, sizeof(padded));
                            lv_table_set_cell_value(tbl, (uint32_t)r,
                                                    (uint32_t)c, padded);
                        }
                    }
                    lv_obj_update_layout(tbl);
                    y += lv_obj_get_height(tbl) + (is_head ? 6 : 8);
                }
                in_ol = false;
                if (!tt) break; /* group ran to end of buffer */
                cur = tt;
                continue;
            }
            /* else fall through: |line| without a table renders as paragraph */
            }
        }

        /* Quote (one level stripped, rest inline) */
        if (*t == '>') {
            const char *c = t + 1;
            if (*c == ' ' || *c == '\t') c++;
            lv_obj_t *sg = md_new_block(y);
            md_add_span(sg, "> ", 2, &lv_font_montserrat_14,
                        p->accent_secondary, LV_TEXT_DECOR_NONE);
            md_render_inline(sg, c, strlen(c), &lv_font_montserrat_14,
                             p->accent_secondary, LV_TEXT_DECOR_NONE, p);
            md_finish_block(sg, &y, 6);
            in_ol = false;
            if (!nl) break;
            cur = nl + 1;
            continue;
        }

        /* Task list */
        if ((strncmp(t, "- [ ]", 5) == 0 || strncmp(t, "* [ ]", 5) == 0 ||
             strncmp(t, "+ [ ]", 5) == 0) ||
            (strncmp(t, "- [x]", 5) == 0 || strncmp(t, "- [X]", 5) == 0 ||
             strncmp(t, "* [x]", 5) == 0 || strncmp(t, "* [X]", 5) == 0 ||
             strncmp(t, "+ [x]", 5) == 0 || strncmp(t, "+ [X]", 5) == 0)) {
            bool done = t[3] == 'x' || t[3] == 'X';
            lv_obj_t *sg = md_new_block(y);
            lv_color_t col = done ? p->accent_secondary : p->text_secondary;
            md_render_inline(sg, t, strlen(t), &lv_font_montserrat_14, col,
                             LV_TEXT_DECOR_NONE, p);
            md_finish_block(sg, &y, 6);
            in_ol = false;
            if (!nl) break;
            cur = nl + 1;
            continue;
        }

        /* Unordered list (- * +) */
        if (((*t == '-' || *t == '*' || *t == '+')) &&
            (t[1] == ' ' || t[1] == '\t')) {
            lv_obj_t *sg = md_new_block(y);
            md_add_span(sg, "- ", 2, &lv_font_montserrat_14, p->text_primary,
                        LV_TEXT_DECOR_NONE);
            md_render_inline(sg, t + 2, strlen(t + 2), &lv_font_montserrat_14,
                             p->text_primary, LV_TEXT_DECOR_NONE, p);
            md_finish_block(sg, &y, 6);
            in_ol = false;
            if (!nl) break;
            cur = nl + 1;
            continue;
        }

        /* Ordered list (renumbered) */
        {
            const char *c = t;
            while (isdigit((unsigned char)*c)) c++;
            if (c > t && (*c == '.' || *c == ')') &&
                (c[1] == ' ' || c[1] == '\t')) {
                if (!in_ol) { in_ol = true; ol_num = 1; }
                const char *body = c + 1;
                while (*body == ' ' || *body == '\t') body++;
                char num[16];
                int numlen = snprintf(num, sizeof(num), "%d. ", ol_num++);
                lv_obj_t *sg = md_new_block(y);
                md_add_span(sg, num, (size_t)numlen, &lv_font_montserrat_14,
                            p->text_primary, LV_TEXT_DECOR_NONE);
                md_render_inline(sg, body, strlen(body), &lv_font_montserrat_14,
                                 p->text_primary, LV_TEXT_DECOR_NONE, p);
                md_finish_block(sg, &y, 6);
                if (!nl) break;
                cur = nl + 1;
                continue;
            }
            in_ol = false;
        }

        /* Paragraph */
        {
            lv_obj_t *sg = md_new_block(y);
            md_render_inline(sg, line, len, &lv_font_montserrat_14,
                             p->text_primary, LV_TEXT_DECOR_NONE, p);
            md_finish_block(sg, &y, 6);
            in_ol = false; /* a paragraph breaks list continuation */
            if (!nl) break;
            cur = nl + 1;
            continue;
        }
    }
    if (in_code) md_emit_code_block(&y, p); /* unclosed fence still renders */

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

    lv_obj_t *action_btns[] = {btn_tree, btn_new, btn_save, btn_mode};
    lv_obj_t *action_lbls[] = {lbl_btn_tree, lbl_btn_new, lbl_btn_save, lbl_btn_mode};
    for (int i = 0; i < 4; i++) {
        if (action_btns[i]) {
            lv_obj_set_style_bg_color(action_btns[i], p->surface, 0);
            lv_obj_set_style_border_color(action_btns[i], p->surface_border, 0);
        }
        if (action_lbls[i]) {
            lv_obj_set_style_text_color(action_lbls[i], (i == 3) ? p->accent_primary : p->text_primary, 0);
        }
    }

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
    /* Fullscreen editing: Fn + [ collapses the file tree */
    if ((modifiers & DEVOS_MOD_FN) && key == '[') {
        s_sidebar_visible = !s_sidebar_visible;
        if (!s_sidebar_visible) s_focus_list = false;
        apply_layout();
        refresh_file_list();
        return true;
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
    lv_obj_set_size(btn_tree, 68, 26);
    lv_obj_set_style_bg_color(btn_tree, p->surface, 0);
    lv_obj_set_style_border_color(btn_tree, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_tree, 1, 0);
    lv_obj_set_style_radius(btn_tree, 4, 0);
    lv_obj_set_style_pad_all(btn_tree, 0, 0);
    lv_obj_add_event_cb(btn_tree, btn_tree_cb, LV_EVENT_CLICKED, NULL);
    lbl_btn_tree = lv_label_create(btn_tree);
    lv_label_set_text(lbl_btn_tree, "[⇋ Tree]");
    lv_obj_center(lbl_btn_tree);
    lv_obj_set_style_text_font(lbl_btn_tree, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_btn_tree, p->text_primary, 0);

    btn_new = lv_button_create(top_bar);
    lv_obj_set_size(btn_new, 60, 26);
    lv_obj_set_style_bg_color(btn_new, p->surface, 0);
    lv_obj_set_style_border_color(btn_new, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_new, 1, 0);
    lv_obj_set_style_radius(btn_new, 4, 0);
    lv_obj_set_style_pad_all(btn_new, 0, 0);
    lv_obj_add_event_cb(btn_new, btn_new_cb, LV_EVENT_CLICKED, NULL);
    lbl_btn_new = lv_label_create(btn_new);
    lv_label_set_text(lbl_btn_new, "[+ New]");
    lv_obj_center(lbl_btn_new);
    lv_obj_set_style_text_font(lbl_btn_new, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_btn_new, p->text_primary, 0);

    btn_save = lv_button_create(top_bar);
    lv_obj_set_size(btn_save, 62, 26);
    lv_obj_set_style_bg_color(btn_save, p->surface, 0);
    lv_obj_set_style_border_color(btn_save, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_save, 1, 0);
    lv_obj_set_style_radius(btn_save, 4, 0);
    lv_obj_set_style_pad_all(btn_save, 0, 0);
    lv_obj_add_event_cb(btn_save, btn_save_cb, LV_EVENT_CLICKED, NULL);
    lbl_btn_save = lv_label_create(btn_save);
    lv_label_set_text(lbl_btn_save, "[Save]");
    lv_obj_center(lbl_btn_save);
    lv_obj_set_style_text_font(lbl_btn_save, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_btn_save, p->text_primary, 0);

    btn_mode = lv_button_create(top_bar);
    lv_obj_set_size(btn_mode, 88, 26);
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
        strncpy(keep_name, s_files[keep], sizeof(keep_name) - 1);
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
    app_descriptor.handle_key = editor_handle_key;

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
    char path[256];
    snprintf(path, sizeof(path), "%s/plans/%s.md", TAB5_SD_MOUNT_POINT, title);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs(content, f);
    fclose(f);
    return true;
}
