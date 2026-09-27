/* Markdown editor with an SD-card file browser.
 *
 * Left: browse the whole card (folders, sizes; new file / folder, rename,
 * delete). Right: the editor (Markdown gets a live preview; code files a
 * monospace font), a find bar, and a status line. Files up to ED_EDIT_MAX
 * are edited in place; larger text files open read-only in the fast code
 * viewer; binary files are refused.
 *
 * Keys (editor): Ctrl+S save, Ctrl+N new, Ctrl+F find, Ctrl+G next match,
 * Ctrl+Z undo, Ctrl+X/C/V cut/copy/paste (selection or whole line),
 * Ctrl+K delete line, Ctrl+D duplicate line, Ctrl+A select all, Ctrl+P
 * Edit/Split/Preview, Ctrl+B bold, Ctrl+Enter tick a task, Sym+Left/Right
 * line start/end, Sym+Up/Down page, Alt+Left/Right word, Tab indent,
 * Sym+L / Sym+F hide the file list, Esc -> file list.
 * Scratchpad / voice memos (any note): Ctrl+T appends a timestamped log line
 * (a date heading when the day changes), Ctrl+R records a voice memo into
 * memos/ next to the note and links it, Ctrl+L plays the memo on the cursor's
 * line, Ctrl+M lists the memos (play, transcribe, delete, settings), Ctrl+J
 * opens /notes/scratchpad.md. Sym+F is the distraction-free view.
 * Keys (file list): arrows, Enter open, Backspace up a folder, N new file,
 * F new folder, R rename, D/Del delete, H hidden files, Esc -> editor.
 */
#include "app_editor.h"
#include "devos_config.h"
#include "devos_icons.h"
#include "devos_theme.h"
#include "devos_core.h"
#include "devos_mdview.h"
#include "devos_codeview.h"
#include "ed_voice.h"
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <strings.h>

LV_FONT_DECLARE(lv_font_nimbus_mono_14);

#define ED_MAX_ENTRIES 300
#define ED_PATH_MAX 256
#define ED_NAME_MAX 96
#define ED_EDIT_MAX (48 * 1024)          /* edited in the text area */
#define ED_VIEW_MAX (512 * 1024)         /* larger text files: read-only viewer */
#define ED_UNDO_MAX 24
#define ED_CLIP_MAX (32 * 1024)
#define ED_AUTOSAVE_MS 30000
#define ED_STATUS_H 40                   /* status line + key hints below it */
#define ED_FIND_H 40
#define ED_CONFIG_FILE TAB5_SD_MOUNT_POINT "/.devos/editor.json"
#define ED_SCRATCHPAD  "notes/scratchpad.md"

typedef enum { VIEW_EDIT = 0, VIEW_SPLIT, VIEW_PREVIEW } ed_view_t;
typedef enum { MODAL_NONE = 0, MODAL_NEW_FILE, MODAL_NEW_DIR, MODAL_RENAME, MODAL_DELETE } ed_modal_t;

typedef struct {
    char name[ED_NAME_MAX];
    bool dir;
    uint32_t size;
} ed_entry_t;

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;

/* widgets */
static lv_obj_t *sidebar, *lbl_side_title, *lbl_path, *file_list, *lbl_side_hint;
static lv_obj_t *btn_add_file, *btn_add_dir;
static lv_obj_t *main_area, *top_bar, *lbl_fn;
static lv_obj_t *btn_tree, *btn_save, *btn_find, *btn_mode;
static lv_obj_t *lbl_btn_tree, *lbl_btn_save, *lbl_btn_find, *lbl_btn_mode;
static lv_obj_t *ta_editor, *preview_scroll, *viewer_scroll, *lbl_empty;
static lv_obj_t *find_bar, *ta_find, *lbl_find_info;
static lv_obj_t *status_bar, *lbl_status, *lbl_keys;
static lv_obj_t *modal, *lbl_modal_title, *lbl_modal_desc, *ta_modal, *btn_modal_ok, *lbl_modal_ok;
static devos_codeview_t s_viewer;

/* file browser */
static EXT_RAM_BSS_ATTR ed_entry_t s_ents[ED_MAX_ENTRIES];
static int s_ent_count = 0;
static lv_obj_t *s_rows[ED_MAX_ENTRIES + 1];
static int s_row_ent[ED_MAX_ENTRIES + 1];     /* entry index, -1 = ".." */
static int s_row_count = 0;
static int s_sel = 0;
static char s_dir[ED_PATH_MAX] = "";          /* relative to the card root */
static bool s_show_hidden = false;
static bool s_focus_list = false;

/* open file */
static char s_file[ED_PATH_MAX] = "";         /* relative path, "" = none */
static bool s_readonly = false;               /* shown in the viewer */
static bool s_markdown = false;
static bool s_dirty = false;
static bool s_preview_stale = false;
static uint32_t s_last_edit = 0;
static ed_view_t s_view = VIEW_EDIT;
static ed_view_t s_md_view = VIEW_EDIT;       /* remembered for Markdown files */
static EXT_RAM_BSS_ATTR char s_buf[ED_VIEW_MAX + 1];

/* editing helpers */
static struct { char *text; uint32_t cursor; } s_undo[ED_UNDO_MAX];
static int s_undo_n = 0;
static EXT_RAM_BSS_ATTR char s_clip[ED_CLIP_MAX];
static char s_find[128] = "";
static bool s_find_open = false;
static ed_modal_t s_modal = MODAL_NONE;
static char s_modal_target[ED_PATH_MAX] = "";
static char s_flash[96] = "";
static uint32_t s_flash_until = 0;

static void apply_layout(void);
static void refresh_status(void);
static void list_rebuild(void);
static void list_paint(void);
static void refresh_keys(void);
static void render_preview(bool keep_scroll);
static void open_path(const char *rel);
static bool save_file(void);
static void apply_theme(const devos_palette_t *p, void *user_data);

/* ------------------------------------------------------------------ paths */
static void abs_path(const char *rel, char *out, size_t n)
{
    if (rel && *rel) snprintf(out, n, "%s/%s", TAB5_SD_MOUNT_POINT, rel);
    else snprintf(out, n, "%s", TAB5_SD_MOUNT_POINT);
}

static void join(const char *dir, const char *name, char *out, size_t n)
{
    if (dir && *dir) snprintf(out, n, "%s/%s", dir, name);
    else snprintf(out, n, "%s", name);
}

static const char *base_name(const char *rel)
{
    const char *b = strrchr(rel, '/');
    return b ? b + 1 : rel;
}

static void parent_of(char *rel)
{
    char *b = strrchr(rel, '/');
    if (b) *b = '\0';
    else rel[0] = '\0';
}

static bool has_ext(const char *name, const char *const *exts)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    for (int i = 0; exts[i]; i++) {
        if (strcasecmp(dot + 1, exts[i]) == 0) return true;
    }
    return false;
}

static bool is_markdown_name(const char *name)
{
    static const char *const md[] = {"md", "markdown", "mdown", "txt", NULL};
    return has_ext(name, md);
}

static bool valid_name(const char *n)
{
    if (!n || !*n || strcmp(n, ".") == 0 || strcmp(n, "..") == 0) return false;
    for (const char *p = n; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' ||
            *p == '>' || *p == '|') {
            return false;
        }
    }
    return true;
}

static void size_str(uint32_t b, char *out, size_t n)
{
    if (b < 1024) snprintf(out, n, "%u B", (unsigned)b);
    else if (b < 1024 * 1024) snprintf(out, n, "%.1f KB", b / 1024.0);
    else snprintf(out, n, "%.1f MB", b / (1024.0 * 1024.0));
}

/* ------------------------------------------------------------------ UTF-8 */
/* The text area counts characters; the text is UTF-8. */
static uint32_t char_to_byte(const char *t, uint32_t ci)
{
    uint32_t b = 0, c = 0;
    while (t[b] && c < ci) {
        b++;
        while (t[b] && ((unsigned char)t[b] & 0xC0) == 0x80) b++;
        c++;
    }
    return b;
}

static uint32_t byte_to_char(const char *t, uint32_t bi)
{
    uint32_t c = 0;
    for (uint32_t b = 0; b < bi && t[b]; b++) {
        if (((unsigned char)t[b] & 0xC0) != 0x80) c++;
    }
    return c;
}

/* ----------------------------------------------------------------- config */
static void config_save(void)
{
    FILE *f = fopen(ED_CONFIG_FILE, "w");
    if (!f) return;
    fprintf(f, "{\n  \"dir\": \"%s\",\n  \"file\": \"%s\",\n  \"view\": %d,\n  \"hidden\": %d\n}\n", s_dir,
            s_readonly ? "" : s_file, (int)s_md_view, s_show_hidden ? 1 : 0);
    fclose(f);
}

static void config_load(char *file_out, size_t n)
{
    file_out[0] = '\0';
    FILE *f = fopen(ED_CONFIG_FILE, "r");
    if (!f) return;
    char line[ED_PATH_MAX + 32], val[ED_PATH_MAX];
    int iv;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, " \"dir\": \"%255[^\"]\"", val) == 1) snprintf(s_dir, sizeof(s_dir), "%s", val);
        else if (sscanf(line, " \"file\": \"%255[^\"]\"", val) == 1) snprintf(file_out, n, "%s", val);
        else if (sscanf(line, " \"view\": %d", &iv) == 1 && iv >= 0 && iv <= 2) s_md_view = (ed_view_t)iv;
        else if (sscanf(line, " \"hidden\": %d", &iv) == 1) s_show_hidden = iv != 0;
    }
    fclose(f);
}

/* ------------------------------------------------------------------ flash */
static void flash(const char *msg)
{
    snprintf(s_flash, sizeof(s_flash), "%s", msg);
    s_flash_until = lv_tick_get() + 3000;
    refresh_status();
}

/* -------------------------------------------------------------- directory */
static int ent_cmp(const void *a, const void *b)
{
    const ed_entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static bool load_dir(void)
{
    char path[ED_PATH_MAX + 32];
    abs_path(s_dir, path, sizeof(path));
    DIR *d = opendir(path);
    if (!d) {
        if (s_dir[0]) {                 /* folder vanished: go to the root */
            s_dir[0] = '\0';
            return load_dir();
        }
        s_ent_count = 0;
        return false;
    }
    s_ent_count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_ent_count < ED_MAX_ENTRIES) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (e->d_name[0] == '.' && !s_show_hidden) continue;
        ed_entry_t *en = &s_ents[s_ent_count];
        snprintf(en->name, sizeof(en->name), "%s", e->d_name);
        char full[ED_PATH_MAX * 2];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            en->dir = S_ISDIR(st.st_mode);
            en->size = (uint32_t)st.st_size;
        } else {
            en->dir = false;
            en->size = 0;
        }
        s_ent_count++;
    }
    closedir(d);
    qsort(s_ents, (size_t)s_ent_count, sizeof(s_ents[0]), ent_cmp);
    return true;
}

static void enter_dir(const char *rel, const char *select_name)
{
    if (rel != s_dir) snprintf(s_dir, sizeof(s_dir), "%s", rel);   /* callers may pass s_dir */
    load_dir();
    s_sel = 0;
    list_rebuild();
    if (select_name) {
        for (int r = 0; r < s_row_count; r++) {
            int ei = s_row_ent[r];
            if (ei >= 0 && strcmp(s_ents[ei].name, select_name) == 0) s_sel = r;
        }
        list_paint();
    }
    config_save();
}

static void row_cb(lv_event_t *e);

static void list_rebuild(void)
{
    const devos_palette_t *p = devos_theme_get();
    lv_obj_clean(file_list);
    s_row_count = 0;
    char path[ED_PATH_MAX + 2];
    snprintf(path, sizeof(path), "/%s", s_dir);
    lv_label_set_text(lbl_path, path);

    int total = s_ent_count + (s_dir[0] ? 1 : 0);
    for (int r = 0; r < total && s_row_count < ED_MAX_ENTRIES + 1; r++) {
        int ei = s_dir[0] ? r - 1 : r;
        lv_obj_t *row = lv_button_create(file_list);
        lv_obj_set_size(row, lv_pct(100), 32);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_pad_ver(row, 0, 0);
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)s_row_count);
        lv_obj_t *l = lv_label_create(row);
        char t[ED_NAME_MAX + 16];
        if (ei < 0) snprintf(t, sizeof(t), LV_SYMBOL_UP "  ..");
        else snprintf(t, sizeof(t), "%s  %s", s_ents[ei].dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE, s_ents[ei].name);
        lv_label_set_text(l, t);
        lv_obj_set_width(l, lv_pct(ei >= 0 && !s_ents[ei].dir ? 72 : 100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(l, ei >= 0 && s_ents[ei].dir ? p->accent_secondary : p->text_primary, 0);
        if (ei >= 0 && !s_ents[ei].dir) {
            lv_obj_t *sz = lv_label_create(row);
            char b[16];
            size_str(s_ents[ei].size, b, sizeof(b));
            lv_label_set_text(sz, b);
            lv_obj_align(sz, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_text_font(sz, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(sz, p->text_secondary, 0);
        }
        s_rows[s_row_count] = row;
        s_row_ent[s_row_count] = ei;
        s_row_count++;
    }
    if (total == 0) {
        lv_obj_t *l = lv_label_create(file_list);
        lv_label_set_text(l, "(empty folder)\nN: new file   F: new folder");
        lv_obj_set_style_text_color(l, p->text_secondary, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    }
    if (s_sel >= s_row_count) s_sel = s_row_count ? s_row_count - 1 : 0;
    list_paint();
}

static void list_paint(void)
{
    refresh_keys();
    const devos_palette_t *p = devos_theme_get();
    for (int r = 0; r < s_row_count; r++) {
        int ei = s_row_ent[r];
        char rel[ED_PATH_MAX];
        rel[0] = '\0';
        if (ei >= 0) join(s_dir, s_ents[ei].name, rel, sizeof(rel));
        bool open = ei >= 0 && s_file[0] && strcmp(rel, s_file) == 0;
        bool sel = r == s_sel;
        lv_obj_set_style_bg_color(s_rows[r], open ? p->surface_active : p->surface, 0);
        lv_obj_set_style_border_color(s_rows[r],
                                      sel && s_focus_list ? p->accent_warning
                                      : open              ? p->accent_primary
                                                          : p->surface_border,
                                      0);
    }
    if (s_focus_list && s_sel >= 0 && s_sel < s_row_count) lv_obj_scroll_to_view(s_rows[s_sel], LV_ANIM_OFF);
    lv_obj_set_style_border_color(sidebar, s_focus_list ? p->accent_primary : p->surface_border, 0);
}

static void activate_row(int r)
{
    if (r < 0 || r >= s_row_count) return;
    int ei = s_row_ent[r];
    if (ei < 0) {
        char up[ED_PATH_MAX], was[ED_NAME_MAX];
        snprintf(was, sizeof(was), "%s", base_name(s_dir));
        snprintf(up, sizeof(up), "%s", s_dir);
        parent_of(up);
        enter_dir(up, was);
        return;
    }
    char rel[ED_PATH_MAX];
    join(s_dir, s_ents[ei].name, rel, sizeof(rel));
    static const char *const wav[] = {"wav", NULL};
    if (s_ents[ei].dir) {
        enter_dir(rel, NULL);
    } else if (has_ext(rel, wav)) {                 /* voice memos play */
        s_sel = r;
        char path[ED_PATH_MAX + 32], msg[160];
        abs_path(rel, path, sizeof(path));
        if (ed_voice_playing()) {
            ed_voice_stop();
            flash("Stopped");
        } else {
            ed_voice_play(path, msg, sizeof(msg));
            flash(msg);
        }
    } else {
        s_sel = r;
        open_path(rel);
    }
}

static void row_cb(lv_event_t *e)
{
    int r = (int)(intptr_t)lv_event_get_user_data(e);
    s_focus_list = true;
    s_sel = r;
    activate_row(r);
}

/* ------------------------------------------------------------------ files */
static void set_editor_text(const char *t)
{
    lv_textarea_set_text(ta_editor, t ? t : "");
    lv_textarea_set_cursor_pos(ta_editor, 0);
}

static void undo_clear(void)
{
    for (int i = 0; i < s_undo_n; i++) free(s_undo[i].text);
    s_undo_n = 0;
}

static void close_file(void)
{
    s_file[0] = '\0';
    s_readonly = false;
    s_dirty = false;
    undo_clear();
    set_editor_text("");
    apply_layout();
    refresh_status();
    list_paint();
}

static void open_path(const char *rel)
{
    char path[ED_PATH_MAX + 32];
    abs_path(rel, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        flash("Can't open that file");
        return;
    }
    if (st.st_size > ED_VIEW_MAX) {
        char m[96], b[16];
        size_str((uint32_t)st.st_size, b, sizeof(b));
        snprintf(m, sizeof(m), "Too large to open here (%s)", b);
        flash(m);
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        flash("Can't read that file");
        return;
    }
    /* sniff first: s_buf may still be on screen in the viewer */
    static char probe[4096];
    size_t pn = fread(probe, 1, sizeof(probe), f);
    if (memchr(probe, '\0', pn)) {
        fclose(f);
        flash("Binary file - not a text file");
        return;
    }
    if (s_dirty && !save_file()) {
        fclose(f);
        return;
    }
    rewind(f);
    size_t n = fread(s_buf, 1, ED_VIEW_MAX, f);
    fclose(f);
    s_buf[n] = '\0';
    snprintf(s_file, sizeof(s_file), "%s", rel);
    s_markdown = is_markdown_name(rel);
    s_readonly = n > ED_EDIT_MAX;
    s_dirty = false;
    undo_clear();
    if (s_readonly) {
        set_editor_text("");
        s_viewer.plain = true;
        lv_obj_remove_flag(viewer_scroll, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(main_area);
        devos_codeview_set(&s_viewer, s_buf);
        s_view = VIEW_EDIT;
    } else {
        lv_obj_set_style_text_font(ta_editor, s_markdown ? &lv_font_montserrat_14 : &lv_font_nimbus_mono_14, 0);
        set_editor_text(s_buf);
        s_view = s_markdown ? s_md_view : VIEW_EDIT;
    }
    s_focus_list = false;
    apply_layout();
    if (!s_readonly) {                      /* start at the top once sized */
        lv_obj_update_layout(ta_editor);
        lv_textarea_set_cursor_pos(ta_editor, 0);
        lv_obj_scroll_to_y(ta_editor, 0, LV_ANIM_OFF);
    }
    if (s_view != VIEW_EDIT) render_preview(false);
    refresh_status();
    list_paint();
    config_save();
}

static bool write_file(const char *rel, const char *text)
{
    char path[ED_PATH_MAX + 32], tmp[ED_PATH_MAX + 40];
    abs_path(rel, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp~", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    size_t len = strlen(text), w = fwrite(text, 1, len, f);
    bool ok = w == len && fflush(f) == 0;
    fclose(f);
    if (!ok) {
        unlink(tmp);
        return false;
    }
    unlink(path);                           /* FAT can't rename over a file */
    if (rename(tmp, path) != 0) {
        /* fall back to writing in place */
        f = fopen(path, "wb");
        if (!f) return false;
        ok = fwrite(text, 1, len, f) == len;
        fclose(f);
        unlink(tmp);
        return ok;
    }
    return true;
}

static bool save_file(void)
{
    if (!s_file[0] || s_readonly) return true;
    const char *t = lv_textarea_get_text(ta_editor);
    if (!write_file(s_file, t ? t : "")) {
        flash("SAVE FAILED - check the SD card");
        return false;
    }
    s_dirty = false;
    /* the listing shows sizes: refresh the entry */
    load_dir();
    list_rebuild();
    refresh_status();
    return true;
}

/* ------------------------------------------------------------------ undo */
static void undo_push(void)
{
    const char *t = lv_textarea_get_text(ta_editor);
    if (s_undo_n == ED_UNDO_MAX) {
        free(s_undo[0].text);
        memmove(s_undo, s_undo + 1, sizeof(s_undo[0]) * (ED_UNDO_MAX - 1));
        s_undo_n--;
    }
    size_t n = strlen(t ? t : "");
    char *c = malloc(n + 1);
    if (!c) return;
    memcpy(c, t ? t : "", n + 1);
    s_undo[s_undo_n].text = c;
    s_undo[s_undo_n].cursor = lv_textarea_get_cursor_pos(ta_editor);
    s_undo_n++;
}

static void undo(void)
{
    if (!s_undo_n) {
        flash("Nothing to undo");
        return;
    }
    s_undo_n--;
    lv_textarea_set_text(ta_editor, s_undo[s_undo_n].text);
    lv_textarea_set_cursor_pos(ta_editor, (int32_t)s_undo[s_undo_n].cursor);
    free(s_undo[s_undo_n].text);
    s_dirty = true;
    s_preview_stale = true;
    refresh_status();
}

/* Call before changing the text: snapshots at the start of each burst. */
static void before_edit(bool force)
{
    uint32_t now = lv_tick_get();
    if (force || now - s_last_edit > 800 || s_undo_n == 0) undo_push();
    s_last_edit = now;
}

static void after_edit(void)
{
    s_dirty = true;
    s_preview_stale = true;
    s_last_edit = lv_tick_get();
    refresh_status();
}

/* ------------------------------------------------------------- selection */
static bool get_selection(uint32_t *bs, uint32_t *be)
{
    lv_obj_t *lbl = lv_textarea_get_label(ta_editor);
    uint32_t s = lv_label_get_text_selection_start(lbl), e = lv_label_get_text_selection_end(lbl);
    if (s == LV_LABEL_TEXT_SELECTION_OFF || e == LV_LABEL_TEXT_SELECTION_OFF || s == e) return false;
    if (s > e) {
        uint32_t t = s;
        s = e;
        e = t;
    }
    const char *t = lv_textarea_get_text(ta_editor);
    *bs = char_to_byte(t, s);
    *be = char_to_byte(t, e);
    return true;
}

static void clear_selection(void)
{
    lv_textarea_clear_selection(ta_editor);
}

/* Replace bytes [bs, be) with `ins` and put the cursor after it. */
static void replace_range(uint32_t bs, uint32_t be, const char *ins)
{
    const char *t = lv_textarea_get_text(ta_editor);
    size_t len = strlen(t), il = strlen(ins);
    if (be > len) be = (uint32_t)len;
    if (len - (be - bs) + il > ED_EDIT_MAX) {
        flash("File would get too large to edit here");
        return;
    }
    char *n = malloc(len - (be - bs) + il + 1);
    if (!n) return;
    memcpy(n, t, bs);
    memcpy(n + bs, ins, il);
    memcpy(n + bs + il, t + be, len - be + 1);
    uint32_t cur = byte_to_char(n, (uint32_t)(bs + il));
    clear_selection();
    lv_textarea_set_text(ta_editor, n);
    lv_textarea_set_cursor_pos(ta_editor, (int32_t)cur);
    free(n);
}

/* Byte range of the cursor's line (end includes the newline if any). */
static void line_range(uint32_t *ls, uint32_t *le)
{
    const char *t = lv_textarea_get_text(ta_editor);
    uint32_t b = char_to_byte(t, lv_textarea_get_cursor_pos(ta_editor));
    uint32_t s = b, e = b;
    while (s > 0 && t[s - 1] != '\n') s--;
    while (t[e] && t[e] != '\n') e++;
    if (t[e] == '\n') e++;
    *ls = s;
    *le = e;
}

/* Ctrl+Enter: tick/untick the line's task box; a plain line or bullet
 * becomes a task. The cursor stays on the same text. */
static void toggle_task(void)
{
    uint32_t s, e;
    line_range(&s, &e);
    const char *t = lv_textarea_get_text(ta_editor);
    uint32_t i = s;
    while (t[i] == ' ' || t[i] == '\t') i++;
    int32_t cur = (int32_t)lv_textarea_get_cursor_pos(ta_editor);
    bool bullet = (t[i] == '-' || t[i] == '*' || t[i] == '+') && t[i + 1] == ' ';
    uint32_t d = i;
    while (t[d] >= '0' && t[d] <= '9') d++;
    if (d > i && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ') {
        i = d;                  /* "1. item" is treated like "- item" */
        bullet = true;
    }
    before_edit(true);
    if (bullet && t[i + 2] == '[' && t[i + 3] && strchr(" xX", t[i + 3]) && t[i + 4] == ']') {
        replace_range(i + 3, i + 4, t[i + 3] == ' ' ? "x" : " ");
    } else if (bullet) {
        replace_range(i + 2, i + 2, "[ ] ");
        cur += 4;
    } else {
        replace_range(i, i, "- [ ] ");
        cur += 6;
    }
    lv_textarea_set_cursor_pos(ta_editor, cur);
    after_edit();
}

static void copy_or_cut(bool cut)
{
    uint32_t s, e;
    bool sel = get_selection(&s, &e);
    if (!sel) line_range(&s, &e);
    const char *t = lv_textarea_get_text(ta_editor);
    size_t n = e - s < ED_CLIP_MAX - 1 ? e - s : ED_CLIP_MAX - 1;
    memcpy(s_clip, t + s, n);
    s_clip[n] = '\0';
    if (cut && !s_readonly) {
        before_edit(true);
        replace_range(s, e, "");
        after_edit();
    }
    flash(cut ? (sel ? "Cut" : "Cut line") : (sel ? "Copied" : "Copied line"));
}

static void paste(void)
{
    if (!s_clip[0]) {
        flash("Clipboard is empty");
        return;
    }
    before_edit(true);
    uint32_t s, e;
    if (!get_selection(&s, &e)) {
        const char *t = lv_textarea_get_text(ta_editor);
        s = e = char_to_byte(t, lv_textarea_get_cursor_pos(ta_editor));
    }
    replace_range(s, e, s_clip);
    after_edit();
}

/* ---------------------------------------------------------------- cursor */
static void cursor_to_byte(uint32_t b)
{
    const char *t = lv_textarea_get_text(ta_editor);
    lv_textarea_set_cursor_pos(ta_editor, (int32_t)byte_to_char(t, b));
}

static void cursor_line_edge(bool end)
{
    uint32_t s, e;
    line_range(&s, &e);
    const char *t = lv_textarea_get_text(ta_editor);
    if (end && e > s && t[e - 1] == '\n') e--;
    cursor_to_byte(end ? e : s);
}

static void cursor_word(bool right)
{
    const char *t = lv_textarea_get_text(ta_editor);
    uint32_t b = char_to_byte(t, lv_textarea_get_cursor_pos(ta_editor));
    if (right) {
        while (t[b] && !isalnum((unsigned char)t[b])) b++;
        while (t[b] && (isalnum((unsigned char)t[b]) || (unsigned char)t[b] >= 0x80)) b++;
    } else {
        while (b > 0 && !isalnum((unsigned char)t[b - 1])) b--;
        while (b > 0 && (isalnum((unsigned char)t[b - 1]) || (unsigned char)t[b - 1] >= 0x80)) b--;
    }
    cursor_to_byte(b);
}

/* Enter: keep the indent; continue Markdown lists ("- ", "* ", "1. ",
 * "- [ ] "); an empty list item ends the list instead. */
static void newline_indent(void)
{
    uint32_t ls, le;
    line_range(&ls, &le);
    const char *t = lv_textarea_get_text(ta_editor);
    uint32_t cur = char_to_byte(t, lv_textarea_get_cursor_pos(ta_editor));
    char prefix[64];
    size_t pn = 0;
    uint32_t i = ls;
    while (i < cur && (t[i] == ' ' || t[i] == '\t') && pn < sizeof(prefix) - 8) prefix[pn++] = t[i++];
    uint32_t marker_start = i;
    if (s_markdown && i < cur) {
        if ((t[i] == '-' || t[i] == '*' || t[i] == '+') && t[i + 1] == ' ') {
            prefix[pn++] = t[i];
            prefix[pn++] = ' ';
            i += 2;
            if (t[i] == '[' && (t[i + 1] == ' ' || t[i + 1] == 'x' || t[i + 1] == 'X') && t[i + 2] == ']' &&
                t[i + 3] == ' ') {
                memcpy(prefix + pn, "[ ] ", 4);
                pn += 4;
                i += 4;
            }
        } else if (isdigit((unsigned char)t[i])) {
            int num = 0;
            uint32_t j = i;
            while (isdigit((unsigned char)t[j])) num = num * 10 + (t[j++] - '0');
            if ((t[j] == '.' || t[j] == ')') && t[j + 1] == ' ') {
                pn += (size_t)snprintf(prefix + pn, sizeof(prefix) - pn, "%d%c ", num + 1, t[j]);
                i = j + 2;
            }
        }
    }
    prefix[pn] = '\0';
    before_edit(true);                          /* undo steps back line by line */
    /* marker with nothing after it: end the list */
    uint32_t line_end = le;
    if (line_end > ls && t[line_end - 1] == '\n') line_end--;
    if (i > marker_start && i >= line_end && cur >= line_end) {
        replace_range(marker_start, line_end, "");
    } else {
        char ins[72];
        snprintf(ins, sizeof(ins), "\n%s", prefix);
        replace_range(cur, cur, ins);
    }
    after_edit();
}

/* ------------------------------------------------------------------ find */
/* ------------------------------------------------------- scratchpad + voice */
/* the note's own folder, absolute, + "/memos" */
static void memo_dir(char *out, size_t n)
{
    char dir[ED_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", s_file[0] ? s_file : ED_SCRATCHPAD);
    parent_of(dir);
    char a[ED_PATH_MAX + 32];
    abs_path(dir, a, sizeof(a));
    snprintf(out, n, "%s/memos", a);
}

/* ed_voice's callback: a memo link at the cursor, or a transcript on its own
 * line under the line that mentions `after` */
static void voice_insert(const char *after, const char *line)
{
    if (!s_file[0] || s_readonly) {
        flash("Open a note to put the memo link in");
        return;
    }
    const char *t = lv_textarea_get_text(ta_editor);
    size_t ll = strlen(line);
    char *ins = malloc(ll + 4);
    if (!ins) return;
    uint32_t at;
    const char *hit = after ? strstr(t, after) : NULL;
    if (hit) {
        at = (uint32_t)(hit - t);
        while (t[at] && t[at] != '\n') at++;
        snprintf(ins, ll + 4, "\n%s", line);
    } else {
        at = char_to_byte(t, lv_textarea_get_cursor_pos(ta_editor));
        bool gap = at > 0 && t[at - 1] != '\n' && t[at - 1] != ' ';
        snprintf(ins, ll + 4, "%s%s", gap ? " " : "", line);
    }
    uint32_t keep = lv_textarea_get_cursor_pos(ta_editor);
    before_edit(true);
    replace_range(at, at, ins);
    if (hit) lv_textarea_set_cursor_pos(ta_editor, (int32_t)keep);     /* transcripts don't move you */
    free(ins);
    after_edit();
}

/* Ctrl+T: a log line at the end, under today's heading */
static void insert_timestamp(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char day[64], hm[16];
    const char *t = lv_textarea_get_text(ta_editor);
    before_edit(true);
    if (!s_markdown) {
        strftime(hm, sizeof(hm), "[%Y-%m-%d %H:%M] ", &tm);
        lv_textarea_add_text(ta_editor, hm);
        after_edit();
        return;
    }
    strftime(day, sizeof(day), "## %A %d %B %Y", &tm);
    strftime(hm, sizeof(hm), "- %H:%M ", &tm);
    const char *last = NULL;
    for (const char *p = t; (p = strstr(p, "## ")) != NULL; p += 3) {
        if (p == t || p[-1] == '\n') last = p;
    }
    bool today = last && !strncmp(last, day, strlen(day)) && (last[strlen(day)] == '\n' || !last[strlen(day)]);
    size_t len = strlen(t);
    char ins[128];
    snprintf(ins, sizeof(ins), "%s%s%s%s%s", len && t[len - 1] != '\n' ? "\n" : "", today || !len ? "" : "\n",
             today ? "" : day, today ? "" : "\n\n", hm);
    replace_range((uint32_t)len, (uint32_t)len, ins);
    after_edit();
}

/* Ctrl+J: the scratchpad, cursor at the end */
static void open_scratchpad(void)
{
    char path[ED_PATH_MAX + 32];
    abs_path(ED_SCRATCHPAD, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0) {
        char d[ED_PATH_MAX + 32];
        abs_path("notes", d, sizeof(d));
        mkdir(d, 0755);
        if (!write_file(ED_SCRATCHPAD, "# Scratchpad\n\n"
                                       "Ctrl+T adds a timestamped line, Ctrl+R records a voice memo, "
                                       "Ctrl+L plays the one on the line, Ctrl+M lists them.\n")) {
            flash("Couldn't create notes/scratchpad.md (SD card?)");
            return;
        }
    }
    if (strcmp(s_file, ED_SCRATCHPAD) != 0) {
        enter_dir("notes", "scratchpad.md");
        open_path(ED_SCRATCHPAD);
    }
    if (!s_file[0] || s_readonly) return;
    s_focus_list = false;
    if (s_view == VIEW_PREVIEW) {
        s_view = VIEW_EDIT;
        apply_layout();
    }
    lv_textarea_set_cursor_pos(ta_editor, LV_TEXTAREA_CURSOR_LAST);
    list_paint();
    refresh_status();
}

/* Ctrl+R */
static void toggle_record(void)
{
    if (ed_voice_recording()) {
        ed_voice_record_stop();
        refresh_status();
        return;
    }
    if (!s_file[0] || s_readonly || !s_markdown) open_scratchpad();
    if (!s_file[0] || s_readonly) return;
    char dir[ED_PATH_MAX + 48], msg[160];
    memo_dir(dir, sizeof(dir));
    ed_voice_record_start(dir, msg, sizeof(msg));
    flash(msg);
}

/* Ctrl+L: the memo linked on the cursor's line */
static void play_line_memo(void)
{
    if (ed_voice_playing()) {
        ed_voice_stop();
        flash("Stopped");
        return;
    }
    if (!s_file[0] || s_readonly) return;
    uint32_t ls, le;
    line_range(&ls, &le);
    const char *t = lv_textarea_get_text(ta_editor);
    char line[512];
    size_t n = le - ls < sizeof(line) - 1 ? le - ls : sizeof(line) - 1;
    memcpy(line, t + ls, n);
    line[n] = '\0';
    char *w = strstr(line, ".wav)");
    char *o = w ? w : NULL;
    while (o && o > line && o[-1] != '(') o--;
    if (!w || o == line) {
        flash("No voice memo on this line");
        return;
    }
    w[4] = '\0';                                        /* keep ".wav" */
    char dir[ED_PATH_MAX], rel[ED_PATH_MAX * 2], path[ED_PATH_MAX * 2 + 32], msg[160];
    snprintf(dir, sizeof(dir), "%s", s_file);
    parent_of(dir);
    if (o[0] == '/') snprintf(rel, sizeof(rel), "%s", o + 1);
    else join(dir, o, rel, sizeof(rel));
    abs_path(rel, path, sizeof(path));
    ed_voice_play(path, msg, sizeof(msg));
    flash(msg);
}

static int count_matches(void)
{
    if (!s_find[0]) return 0;
    const char *t = s_readonly ? s_buf : lv_textarea_get_text(ta_editor);
    int n = 0;
    size_t fl = strlen(s_find);
    for (const char *p = t; *p; p++) {
        if (strncasecmp(p, s_find, fl) == 0) n++;
    }
    return n;
}

static void find_next(void)
{
    if (!s_find[0] || s_readonly) return;
    const char *t = lv_textarea_get_text(ta_editor);
    size_t fl = strlen(s_find), len = strlen(t);
    uint32_t start = char_to_byte(t, lv_textarea_get_cursor_pos(ta_editor));
    for (size_t k = 0; k < len; k++) {
        size_t i = (start + k) % len;
        /* the cursor sits after the previous match, so this finds the next */
        if (i + fl <= len && strncasecmp(t + i, s_find, fl) == 0) {
            uint32_t cs = byte_to_char(t, (uint32_t)i), ce = byte_to_char(t, (uint32_t)(i + fl));
            lv_textarea_set_cursor_pos(ta_editor, (int32_t)ce);
            lv_obj_t *lbl = lv_textarea_get_label(ta_editor);
            lv_label_set_text_selection_start(lbl, cs);
            lv_label_set_text_selection_end(lbl, ce);
            char info[48];
            snprintf(info, sizeof(info), "%d match%s", count_matches(), count_matches() == 1 ? "" : "es");
            lv_label_set_text(lbl_find_info, info);
            return;
        }
    }
    lv_label_set_text(lbl_find_info, "No match");
}

static bool s_find_fresh;   /* the last query is shown: typing replaces it */

static void find_open(bool open)
{
    s_find_open = open;
    s_find_fresh = open && s_find[0];
    if (open) {
        s_focus_list = false;
        list_paint();
        lv_textarea_set_text(ta_find, s_find);
        lv_label_set_text(lbl_find_info, "Enter: next   Esc: close");
    }
    apply_layout();
    refresh_keys();
}

/* ------------------------------------------------------------------ modal */
static void modal_close(void)
{
    s_modal = MODAL_NONE;
    lv_obj_add_flag(modal, LV_OBJ_FLAG_HIDDEN);
    refresh_keys();
}

static bool s_modal_fresh;   /* the suggested name is untouched: typing replaces it */

static void modal_open(ed_modal_t kind, const char *target_rel)
{
    s_modal = kind;
    s_modal_fresh = kind == MODAL_NEW_FILE;
    snprintf(s_modal_target, sizeof(s_modal_target), "%s", target_rel ? target_rel : "");
    char title[ED_PATH_MAX + 32];
    const char *ok = "Create";
    bool input = true;
    switch (kind) {
    case MODAL_NEW_FILE: {
        snprintf(title, sizeof(title), LV_SYMBOL_FILE "  New file in /%s", s_dir);
        char name[ED_NAME_MAX] = "untitled.md";
        for (int k = 2; k < 100; k++) {
            char rel[ED_PATH_MAX], path[ED_PATH_MAX + 32];
            join(s_dir, name, rel, sizeof(rel));
            abs_path(rel, path, sizeof(path));
            struct stat st;
            if (stat(path, &st) != 0) break;
            snprintf(name, sizeof(name), "untitled-%d.md", k);
        }
        lv_textarea_set_text(ta_modal, name);
        break;
    }
    case MODAL_NEW_DIR:
        snprintf(title, sizeof(title), LV_SYMBOL_DIRECTORY "  New folder in /%s", s_dir);
        lv_textarea_set_text(ta_modal, "");
        break;
    case MODAL_RENAME:
        snprintf(title, sizeof(title), LV_SYMBOL_EDIT "  Rename %s", base_name(s_modal_target));
        lv_textarea_set_text(ta_modal, base_name(s_modal_target));
        ok = "Rename";
        break;
    case MODAL_DELETE:
        snprintf(title, sizeof(title), LV_SYMBOL_TRASH "  Delete /%s?", s_modal_target);
        ok = "Delete";
        input = false;
        break;
    default:
        return;
    }
    lv_label_set_text(lbl_modal_title, title);
    lv_label_set_text(lbl_modal_desc, kind == MODAL_NEW_FILE ? "No extension = Markdown (.md).  Enter creates, Esc cancels"
                                      : input ? "Enter confirms, Esc cancels"
                                              : "This can't be undone. Folders must be empty.  Y / Enter deletes, Esc cancels");
    if (input) lv_obj_remove_flag(ta_modal, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ta_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(lbl_modal_desc, input ? 82 : 40);
    lv_label_set_text(lbl_modal_ok, ok);
    const devos_palette_t *p = devos_theme_get();
    lv_obj_set_style_bg_color(btn_modal_ok, kind == MODAL_DELETE ? p->accent_danger : p->accent_primary, 0);
    lv_obj_set_style_text_color(lbl_modal_ok, kind == MODAL_DELETE ? lv_color_white() : lv_color_black(), 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(modal);
    refresh_keys();
}

static void modal_confirm(void)
{
    char name[ED_NAME_MAX];
    snprintf(name, sizeof(name), "%s", lv_textarea_get_text(ta_modal));
    /* trim spaces */
    char *nm = name;
    while (*nm == ' ') nm++;
    for (size_t l = strlen(nm); l && nm[l - 1] == ' '; l--) nm[l - 1] = '\0';
    char rel[ED_PATH_MAX], path[ED_PATH_MAX + 32], path2[ED_PATH_MAX + 32];
    ed_modal_t kind = s_modal;
    if (kind != MODAL_DELETE && !valid_name(nm)) {
        lv_label_set_text(lbl_modal_desc, "Not a valid name (no / \\ : * ? \" < > |)");
        return;
    }
    switch (kind) {
    case MODAL_NEW_FILE: {
        char named[ED_NAME_MAX + 4];
        snprintf(named, sizeof(named), "%s%s", nm, strchr(nm, '.') ? "" : ".md");   /* bare name = Markdown */
        if (strlen(named) >= ED_NAME_MAX) {
            lv_label_set_text(lbl_modal_desc, "That name is too long");
            return;
        }
        nm = strcpy(name, named);
        join(s_dir, nm, rel, sizeof(rel));
        abs_path(rel, path, sizeof(path));
        struct stat st;
        if (stat(path, &st) == 0) {
            lv_label_set_text(lbl_modal_desc, "That name is taken");
            return;
        }
        const char *seed = is_markdown_name(nm) ? "# " : "";
        char first[ED_NAME_MAX + 8];
        snprintf(first, sizeof(first), "%s", seed);
        if (seed[0]) {
            /* title from the file name */
            char title[ED_NAME_MAX];
            snprintf(title, sizeof(title), "%s", nm);
            char *dot = strrchr(title, '.');
            if (dot) *dot = '\0';
            snprintf(first, sizeof(first), "# %s\n\n", title);
        }
        if (!write_file(rel, first)) {
            lv_label_set_text(lbl_modal_desc, "Could not create it (SD card?)");
            return;
        }
        modal_close();
        load_dir();
        list_rebuild();
        open_path(rel);
        lv_textarea_set_cursor_pos(ta_editor, LV_TEXTAREA_CURSOR_LAST);
        return;
    }
    case MODAL_NEW_DIR:
        join(s_dir, nm, rel, sizeof(rel));
        abs_path(rel, path, sizeof(path));
        if (mkdir(path, 0755) != 0) {
            lv_label_set_text(lbl_modal_desc, errno == EEXIST ? "That name is taken" : "Could not create the folder");
            return;
        }
        modal_close();
        enter_dir(s_dir, nm);
        flash("Folder created");
        return;
    case MODAL_RENAME: {
        char dir[ED_PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", s_modal_target);
        parent_of(dir);
        join(dir, nm, rel, sizeof(rel));
        abs_path(s_modal_target, path, sizeof(path));
        abs_path(rel, path2, sizeof(path2));
        struct stat st;
        if (strcmp(path, path2) != 0 && stat(path2, &st) == 0) {
            lv_label_set_text(lbl_modal_desc, "That name is taken");
            return;
        }
        bool was_open = strcmp(s_file, s_modal_target) == 0;
        if (was_open && s_dirty) save_file();
        if (rename(path, path2) != 0) {
            lv_label_set_text(lbl_modal_desc, "Rename failed");
            return;
        }
        if (was_open) {
            snprintf(s_file, sizeof(s_file), "%s", rel);
            s_markdown = is_markdown_name(rel);
        }
        modal_close();
        enter_dir(s_dir, nm);
        refresh_status();
        flash("Renamed");
        return;
    }
    case MODAL_DELETE: {
        abs_path(s_modal_target, path, sizeof(path));
        struct stat st;
        bool dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
        int rc = dir ? rmdir(path) : unlink(path);
        if (rc != 0) {
            lv_label_set_text(lbl_modal_desc, dir ? "Folder isn't empty (delete its files first)" : "Delete failed");
            return;
        }
        if (strcmp(s_file, s_modal_target) == 0 ||
            (dir && strncmp(s_file, s_modal_target, strlen(s_modal_target)) == 0)) {
            close_file();
        }
        modal_close();
        load_dir();
        list_rebuild();
        flash("Deleted");
        return;
    }
    default:
        modal_close();
    }
}

static void modal_ok_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    modal_confirm();
}

static void modal_cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    modal_close();
}

/* Selected browser entry as a relative path ("" if none / ".."). */
static bool selected_rel(char *out, size_t n)
{
    if (s_sel < 0 || s_sel >= s_row_count || s_row_ent[s_sel] < 0) return false;
    join(s_dir, s_ents[s_row_ent[s_sel]].name, out, n);
    return true;
}

/* ----------------------------------------------------------------- layout */
static void set_btn_active(lv_obj_t *b, lv_obj_t *l, bool on)
{
    const devos_palette_t *p = devos_theme_get();
    if (!b) return;
    lv_obj_set_style_bg_color(b, on ? p->surface_active : p->surface, 0);
    lv_obj_set_style_border_color(b, on ? p->accent_primary : p->surface_border, 0);
    if (l) lv_obj_set_style_text_color(l, on ? p->accent_primary : p->text_primary, 0);
}

static bool s_sidebar_visible = true;

static void apply_layout(void)
{
    int main_x = s_sidebar_visible ? DEVOS_PANE_LEFT_WIDTH : 0;
    int main_w = DEVOS_SCREEN_WIDTH - main_x;
    if (s_sidebar_visible) lv_obj_remove_flag(sidebar, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(sidebar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(main_area, main_w, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(main_area, main_x, 0);

    lv_obj_align(btn_mode, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_align_to(btn_find, btn_mode, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    lv_obj_align_to(btn_save, btn_find, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    lv_obj_align_to(btn_tree, btn_save, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    int lbl_w = main_w - 380;
    lv_obj_set_width(lbl_fn, lbl_w < 120 ? 120 : lbl_w);
    set_btn_active(btn_tree, lbl_btn_tree, s_sidebar_visible);
    set_btn_active(btn_find, lbl_btn_find, s_find_open);

    int top = 34 + (s_find_open ? ED_FIND_H : 0);
    int h = DEVOS_CONTENT_HEIGHT - top - ED_STATUS_H;
    if (s_find_open) {
        lv_obj_remove_flag(find_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(find_bar, main_w, ED_FIND_H);
    } else {
        lv_obj_add_flag(find_bar, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_size(status_bar, main_w, ED_STATUS_H);
    lv_obj_set_pos(status_bar, 0, DEVOS_CONTENT_HEIGHT - ED_STATUS_H);
    lv_obj_set_width(lbl_status, main_w - 20);
    lv_obj_set_width(lbl_keys, main_w - 20);

    bool none = !s_file[0];
    if (none) {
        lv_obj_add_flag(ta_editor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_scroll, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(viewer_scroll, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(lbl_empty, main_w - 80);
        lv_obj_align(lbl_empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    lv_obj_add_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    if (s_readonly) {
        lv_obj_add_flag(ta_editor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_scroll, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(viewer_scroll, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(viewer_scroll, main_w, h);
        lv_obj_set_pos(viewer_scroll, 0, top);
        return;
    }
    lv_obj_add_flag(viewer_scroll, LV_OBJ_FLAG_HIDDEN);
    if (s_view == VIEW_EDIT) {
        lv_obj_remove_flag(ta_editor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_scroll, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(ta_editor, main_w, h);
        lv_obj_set_pos(ta_editor, 0, top);
    } else if (s_view == VIEW_SPLIT) {
        lv_obj_remove_flag(ta_editor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(preview_scroll, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(ta_editor, main_w / 2, h);
        lv_obj_set_pos(ta_editor, 0, top);
        lv_obj_set_size(preview_scroll, main_w - main_w / 2, h);
        lv_obj_set_pos(preview_scroll, main_w / 2, top);
        lv_obj_set_style_border_width(preview_scroll, 1, 0);
        lv_obj_set_style_border_side(preview_scroll, LV_BORDER_SIDE_LEFT, 0);
    } else {
        lv_obj_add_flag(ta_editor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(preview_scroll, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(preview_scroll, main_w, h);
        lv_obj_set_pos(preview_scroll, 0, top);
        lv_obj_set_style_border_width(preview_scroll, 0, 0);
    }
}

static void render_preview(bool keep_scroll)
{
    if (!s_markdown || s_readonly) return;
    int32_t y = keep_scroll ? lv_obj_get_scroll_y(preview_scroll) : 0;
    lv_obj_clean(preview_scroll);
    lv_obj_update_layout(preview_scroll);
    const char *t = lv_textarea_get_text(ta_editor);
    devos_md_render(preview_scroll, t ? t : "");
    lv_obj_update_layout(preview_scroll);
    if (keep_scroll && y > 0) lv_obj_scroll_to_y(preview_scroll, y, LV_ANIM_OFF);
}

static void refresh_keys(void)
{
    /* what the keys do right now (the file list has its own hint) */
    const char *keys;
    if (ed_voice_list_open()) keys = ed_voice_keys();
    else if (s_modal != MODAL_NONE) keys = "Enter confirms   Esc cancels";
    else if (s_focus_list) keys = s_file[0] ? "Esc  back to the editor" : "";
    else if (s_find_open) keys = "Type to find   Enter next   Esc close";
    else if (!s_file[0]) keys = "Esc  file list   Ctrl+N  new file";
    else if (s_readonly) keys = "Arrows  scroll   Esc  file list";
    else if (s_view == VIEW_PREVIEW) keys = "Up / Down  scroll   Ctrl+P  edit   Esc  file list";
    else keys = "Esc  files   Ctrl+S  save   Ctrl+F  find   Ctrl+P  preview   Ctrl+Z  undo   Ctrl+T  time   "
                "Ctrl+R  record   Ctrl+L  play   Ctrl+M  memos   Ctrl+J  scratchpad";
    if (lbl_keys && strcmp(lv_label_get_text(lbl_keys), keys) != 0) lv_label_set_text(lbl_keys, keys);
}

static void refresh_status(void)
{
    if (!lbl_status) return;
    char buf[200];
    if (!s_file[0]) {
        snprintf(buf, sizeof(buf), "No file open");
    } else {
        const char *t = s_readonly ? s_buf : lv_textarea_get_text(ta_editor);
        size_t len = strlen(t ? t : "");
        char sz[16];
        size_str((uint32_t)len, sz, sizeof(sz));
        if (s_readonly) {
            snprintf(buf, sizeof(buf), "Read-only (larger than %d KB)  -  %s  -  arrows / Sym+Up/Down scroll",
                     ED_EDIT_MAX / 1024, sz);
        } else {
            uint32_t b = char_to_byte(t, lv_textarea_get_cursor_pos(ta_editor));
            int ln = 1, col = 1;
            for (uint32_t i = 0; i < b && t[i]; i++) {
                if (t[i] == '\n') {
                    ln++;
                    col = 1;
                } else if (((unsigned char)t[i] & 0xC0) != 0x80) {
                    col++;
                }
            }
            int words = 0;
            bool in = false;
            if (s_markdown) {
                for (const char *q = t; *q; q++) {
                    bool w = !isspace((unsigned char)*q);
                    if (w && !in) words++;
                    in = w;
                }
            }
            char wbuf[24] = "";
            if (s_markdown) snprintf(wbuf, sizeof(wbuf), "  -  %d words", words);
            snprintf(buf, sizeof(buf), "Ln %d, Col %d  -  %s%s  -  %s  -  %s", ln, col, sz, wbuf,
                     s_markdown ? "Markdown" : "Text", s_dirty ? "modified" : "saved");
        }
    }
    char vs[64];
    ed_voice_status(vs, sizeof(vs));
    if (vs[0]) {
        char tmp[200];
        snprintf(tmp, sizeof(tmp), "%s     %s", vs, buf);
        snprintf(buf, sizeof(buf), "%s", tmp);
    }
    if (s_flash[0] && lv_tick_get() < s_flash_until) {
        size_t l = strlen(buf);
        snprintf(buf + l, sizeof(buf) - l, "     %s", s_flash);
    }
    if (strcmp(lv_label_get_text(lbl_status), buf) != 0) lv_label_set_text(lbl_status, buf);

    refresh_keys();

    char title[ED_PATH_MAX + 24];
    if (s_file[0]) snprintf(title, sizeof(title), "%s%s", s_file, s_dirty ? "  [*]" : "");
    else snprintf(title, sizeof(title), "Markdown Editor");
    if (strcmp(lv_label_get_text(lbl_fn), title) != 0) lv_label_set_text(lbl_fn, title);
    const char *mode = s_view == VIEW_SPLIT ? "Split" : s_view == VIEW_PREVIEW ? "Preview" : "Edit";
    char m[24];
    snprintf(m, sizeof(m), "%s", s_markdown && !s_readonly ? mode : "Edit");
    if (strcmp(lv_label_get_text(lbl_btn_mode), m) != 0) lv_label_set_text(lbl_btn_mode, m);

    devos_telemetry_t tel = *devos_telemetry_get();
    snprintf(tel.editor_file, sizeof(tel.editor_file), "%s", s_file[0] ? base_name(s_file) : "(none)");
    tel.editor_file_kb = s_file[0] ? (uint32_t)((strlen(s_readonly ? s_buf : lv_textarea_get_text(ta_editor)) + 1023) / 1024) : 0;
    devos_telemetry_update(&tel);
}

static void cycle_view(void)
{
    if (!s_file[0] || !s_markdown || s_readonly) {
        flash("Preview is for Markdown files");
        return;
    }
    s_view = s_view == VIEW_EDIT ? VIEW_SPLIT : s_view == VIEW_SPLIT ? VIEW_PREVIEW : VIEW_EDIT;
    s_md_view = s_view;
    apply_layout();
    if (s_view != VIEW_EDIT) render_preview(false);
    refresh_status();
    config_save();
}

static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    ed_voice_tick();                    /* even hidden: a stopped recording still gets its link */
    const char *vm = ed_voice_take_message();
    if (vm[0]) flash(vm);
    if (!screen || lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN)) return;
    static bool was_busy;
    char vs[64];
    ed_voice_status(vs, sizeof(vs));
    if (vs[0] || was_busy) refresh_status();          /* the recording clock / level meter */
    was_busy = vs[0] != '\0';
    if (s_preview_stale && s_view != VIEW_EDIT && lv_tick_elaps(s_last_edit) > 350) {
        s_preview_stale = false;
        render_preview(true);
    }
    if (s_dirty && lv_tick_elaps(s_last_edit) > ED_AUTOSAVE_MS) {
        if (save_file()) flash("Autosaved");
    }
    if (s_flash[0] && lv_tick_get() >= s_flash_until) {
        s_flash[0] = '\0';
        refresh_status();
    }
}

/* ------------------------------------------------------------------ theme */
static void apply_theme(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    if (!screen) return;
    lv_obj_set_style_bg_color(screen, p->bg, 0);
    lv_obj_set_style_bg_color(sidebar, p->surface, 0);
    lv_obj_set_style_text_color(lbl_side_title, p->text_secondary, 0);
    lv_obj_set_style_text_color(lbl_path, p->accent_primary, 0);
    lv_obj_set_style_text_color(lbl_side_hint, p->text_secondary, 0);
    lv_obj_set_style_bg_color(main_area, p->bg, 0);
    lv_obj_set_style_bg_color(top_bar, p->top_bar_bg, 0);
    lv_obj_set_style_border_color(top_bar, p->surface_border, 0);
    lv_obj_set_style_text_color(lbl_fn, p->accent_primary, 0);
    lv_obj_t *bs[] = {btn_save, btn_mode, btn_add_file, btn_add_dir};
    for (unsigned i = 0; i < sizeof(bs) / sizeof(bs[0]); i++) {
        lv_obj_set_style_bg_color(bs[i], p->surface, 0);
        lv_obj_set_style_border_color(bs[i], p->surface_border, 0);
    }
    lv_obj_t *ls[] = {lbl_btn_save, lbl_btn_mode};
    for (unsigned i = 0; i < sizeof(ls) / sizeof(ls[0]); i++) lv_obj_set_style_text_color(ls[i], p->text_primary, 0);
    lv_obj_set_style_bg_color(ta_editor, p->code_bg, 0);
    lv_obj_set_style_text_color(ta_editor, p->text_primary, 0);
    lv_obj_set_style_bg_color(ta_editor, p->accent_primary, LV_PART_CURSOR);
    lv_obj_set_style_border_color(ta_editor, p->accent_primary, LV_PART_CURSOR);
    lv_obj_set_style_bg_color(lv_textarea_get_label(ta_editor), p->accent_primary, LV_PART_SELECTED);
    lv_obj_set_style_text_color(lv_textarea_get_label(ta_editor), p->bg, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(preview_scroll, p->code_bg, 0);
    lv_obj_set_style_border_color(preview_scroll, p->surface_border, 0);
    lv_obj_set_style_bg_color(viewer_scroll, p->code_bg, 0);
    lv_obj_set_style_text_color(lbl_empty, p->text_secondary, 0);
    lv_obj_set_style_bg_color(find_bar, p->bg_alt, 0);
    lv_obj_set_style_bg_color(ta_find, p->code_bg, 0);
    lv_obj_set_style_text_color(ta_find, p->text_primary, 0);
    lv_obj_set_style_text_color(lbl_find_info, p->text_secondary, 0);
    lv_obj_set_style_bg_color(status_bar, p->bg_alt, 0);
    lv_obj_set_style_text_color(lbl_status, p->text_secondary, 0);
    lv_obj_set_style_text_color(lbl_keys, p->text_secondary, 0);
    lv_obj_set_style_bg_color(modal, p->surface, 0);
    lv_obj_set_style_border_color(modal, p->accent_primary, 0);
    lv_obj_set_style_text_color(lbl_modal_title, p->accent_primary, 0);
    lv_obj_set_style_text_color(lbl_modal_desc, p->text_secondary, 0);
    lv_obj_set_style_bg_color(ta_modal, p->code_bg, 0);
    lv_obj_set_style_text_color(ta_modal, p->text_primary, 0);
    list_rebuild();
    apply_layout();
    if (s_view != VIEW_EDIT) render_preview(true);
}

/* ---------------------------------------------------------------- buttons */
static void tree_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_sidebar_visible = !s_sidebar_visible;
    if (!s_sidebar_visible) s_focus_list = false;
    apply_layout();
    list_paint();
}

static void save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_file[0] && save_file()) flash("Saved");
}

static void find_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_file[0]) find_open(!s_find_open);
}

static void mode_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    cycle_view();
}

static void add_file_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    modal_open(MODAL_NEW_FILE, NULL);
}

static void add_dir_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    modal_open(MODAL_NEW_DIR, NULL);
}

static void editor_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_focus_list) {
        s_focus_list = false;
        list_paint();
    }
    refresh_status();
}

/* -------------------------------------------------------------------- keys */
static bool edit_field_key(lv_obj_t *ta, uint32_t key)
{
    if (key == '\b' || key == 0x7F) lv_textarea_delete_char(ta);
    else if (key == LV_KEY_DEL) lv_textarea_delete_char_forward(ta);
    else if (key == LV_KEY_LEFT) lv_textarea_cursor_left(ta);
    else if (key == LV_KEY_RIGHT) lv_textarea_cursor_right(ta);
    else if (key >= 32 && key <= 126) lv_textarea_add_char(ta, (char)key);
    else return false;
    return true;
}

static bool list_key(uint32_t key, uint8_t mods)
{
    char rel[ED_PATH_MAX];
    if (key == LV_KEY_ESC || key == '\t') {
        if (s_file[0]) {
            s_focus_list = false;
            list_paint();
        }
        return true;
    }
    if (key == LV_KEY_UP && s_sel > 0) s_sel--, list_paint();
    else if (key == LV_KEY_DOWN && s_sel + 1 < s_row_count) s_sel++, list_paint();
    else if (key == DEVOS_KEY_PGUP) s_sel = s_sel > 10 ? s_sel - 10 : 0, list_paint();
    else if (key == DEVOS_KEY_PGDN) s_sel = s_sel + 10 < s_row_count ? s_sel + 10 : (s_row_count ? s_row_count - 1 : 0), list_paint();
    else if (key == '\r' || key == '\n' || key == LV_KEY_RIGHT) activate_row(s_sel);
    else if ((key == '\b' || key == LV_KEY_LEFT) && s_dir[0]) {
        char up[ED_PATH_MAX], was[ED_NAME_MAX];
        snprintf(was, sizeof(was), "%s", base_name(s_dir));
        snprintf(up, sizeof(up), "%s", s_dir);
        parent_of(up);
        enter_dir(up, was);
    } else if (key == 'n' || key == 'N') modal_open(MODAL_NEW_FILE, NULL);
    else if (key == 'f' || key == 'F') modal_open(MODAL_NEW_DIR, NULL);
    else if ((key == 'r' || key == 'R') && selected_rel(rel, sizeof(rel))) modal_open(MODAL_RENAME, rel);
    else if ((key == 'd' || key == 'D' || key == LV_KEY_DEL) && selected_rel(rel, sizeof(rel))) modal_open(MODAL_DELETE, rel);
    else if (key == 'h' || key == 'H') {
        char keep[ED_NAME_MAX] = "";
        if (s_sel >= 0 && s_sel < s_row_count && s_row_ent[s_sel] >= 0)
            snprintf(keep, sizeof(keep), "%s", s_ents[s_row_ent[s_sel]].name);
        s_show_hidden = !s_show_hidden;
        enter_dir(s_dir, keep[0] ? keep : NULL);
        flash(s_show_hidden ? "Showing hidden files" : "Hiding hidden files");
    }
    LV_UNUSED(mods);
    return true;
}

static bool editor_handle_key(uint32_t key, uint8_t mods)
{
    /* Ctrl+letter arrives as a control code in the simulator */
    if ((mods & DEVOS_MOD_CTRL) && key >= 1 && key <= 26 &&
        key != '\b' && key != '\t' && key != '\n' && key != '\r') key += 'a' - 1;
    if ((mods & DEVOS_MOD_CTRL) && key >= 'A' && key <= 'Z') key += 32;

    if (s_modal != MODAL_NONE) {
        if (key == LV_KEY_ESC || (s_modal == MODAL_DELETE && (key == 'n' || key == 'N'))) modal_close();
        else if (key == '\r' || key == '\n' || (s_modal == MODAL_DELETE && (key == 'y' || key == 'Y'))) modal_confirm();
        else if (s_modal != MODAL_DELETE) {
            if (s_modal_fresh && key >= 32 && key <= 126) lv_textarea_set_text(ta_modal, "");
            s_modal_fresh = false;
            edit_field_key(ta_modal, key);
        }
        return true;
    }

    if (ed_voice_list_open()) return ed_voice_key(key, mods);

    if (mods & DEVOS_MOD_FN) {
        if (key == 'l' || key == 'L') {                 /* Sym+L: file list */
            tree_cb(NULL);
            return true;
        }
        if (key == 'f' || key == 'F') {                 /* Sym+F: focus (hide the file list) */
            s_sidebar_visible = !s_sidebar_visible;
            if (!s_sidebar_visible) s_focus_list = false;
            apply_layout();
            list_paint();
            return true;
        }
    }

    if (mods & DEVOS_MOD_CTRL) {
        switch (key) {
        case 's': save_cb(NULL); return true;
        case 'n': modal_open(MODAL_NEW_FILE, NULL); return true;
        case 'o':
            s_sidebar_visible = true;
            s_focus_list = true;
            apply_layout();
            list_paint();
            return true;
        case 'f': find_cb(NULL); return true;
        case 'g':
            if (s_find[0]) find_next();
            return true;
        case 'p':
            s_focus_list = false;
            list_paint();
            cycle_view();
            return true;
        case 'j': open_scratchpad(); return true;
        case 'r': toggle_record(); return true;
        case 'l': play_line_memo(); return true;
        case 'm': {
            char dir[ED_PATH_MAX + 48];
            memo_dir(dir, sizeof(dir));
            ed_voice_open_list(dir);
            refresh_keys();
            return true;
        }
        default: break;
        }
    }

    if (s_find_open && !s_focus_list) {
        if (key == LV_KEY_ESC) {
            find_open(false);
            return true;
        }
        if (key == '\r' || key == '\n' || ((mods & DEVOS_MOD_CTRL) && key == 'g')) {
            snprintf(s_find, sizeof(s_find), "%s", lv_textarea_get_text(ta_find));
            find_next();
            return true;
        }
        if (!(mods & DEVOS_MOD_CTRL)) {
            if (s_find_fresh && key >= 32 && key <= 126) lv_textarea_set_text(ta_find, "");
            s_find_fresh = false;
            if (edit_field_key(ta_find, key)) return true;
        }
    }

    if (s_focus_list) return list_key(key, mods);
    if (!s_file[0]) {
        if (key == LV_KEY_ESC || key == '\t') {
            s_focus_list = true;
            list_paint();
            return true;
        }
        return false;
    }

    /* read-only viewer: scrolling only */
    if (s_readonly) {
        lv_obj_t *sc = viewer_scroll;
        int page = lv_obj_get_height(sc) - 2 * DEVOS_CODEVIEW_LINE_H;
        if (key == LV_KEY_DOWN) lv_obj_scroll_by_bounded(sc, 0, -3 * DEVOS_CODEVIEW_LINE_H, LV_ANIM_OFF);
        else if (key == LV_KEY_UP) lv_obj_scroll_by_bounded(sc, 0, 3 * DEVOS_CODEVIEW_LINE_H, LV_ANIM_OFF);
        else if (key == DEVOS_KEY_PGDN || key == ' ') lv_obj_scroll_by_bounded(sc, 0, -page, LV_ANIM_OFF);
        else if (key == DEVOS_KEY_PGUP) lv_obj_scroll_by_bounded(sc, 0, page, LV_ANIM_OFF);
        else if (key == LV_KEY_RIGHT) lv_obj_scroll_by_bounded(sc, -80, 0, LV_ANIM_OFF);
        else if (key == LV_KEY_LEFT) lv_obj_scroll_by_bounded(sc, 80, 0, LV_ANIM_OFF);
        else if (key == LV_KEY_ESC || key == '\t') s_focus_list = true, list_paint();
        else if ((mods & DEVOS_MOD_CTRL) && key == 'c') copy_or_cut(false);
        return true;
    }

    /* full preview: arrows scroll, no typing */
    if (s_view == VIEW_PREVIEW) {
        if (key == LV_KEY_UP) lv_obj_scroll_by_bounded(preview_scroll, 0, 40, LV_ANIM_OFF);
        else if (key == LV_KEY_DOWN) lv_obj_scroll_by_bounded(preview_scroll, 0, -40, LV_ANIM_OFF);
        else if (key == DEVOS_KEY_PGUP) lv_obj_scroll_by_bounded(preview_scroll, 0, 300, LV_ANIM_OFF);
        else if (key == DEVOS_KEY_PGDN || key == ' ') lv_obj_scroll_by_bounded(preview_scroll, 0, -300, LV_ANIM_OFF);
        else if (key == LV_KEY_ESC) s_focus_list = true, list_paint();
        return true;
    }

    if (mods & DEVOS_MOD_CTRL) {
        switch (key) {
        case 't': insert_timestamp(); return true;
        case 'z': undo(); return true;
        case '\r':
        case '\n':
            if (s_markdown) toggle_task();
            return true;
        case 'x': copy_or_cut(true); return true;
        case 'c': copy_or_cut(false); return true;
        case 'v': paste(); return true;
        case 'k': {
            uint32_t s, e;
            line_range(&s, &e);
            before_edit(true);
            replace_range(s, e, "");
            after_edit();
            return true;
        }
        case 'd': {
            uint32_t s, e;
            line_range(&s, &e);
            const char *t = lv_textarea_get_text(ta_editor);
            size_t n = e - s;
            char *line = malloc(n + 2);
            if (!line) return true;
            memcpy(line, t + s, n);
            if (!n || line[n - 1] != '\n') line[n++] = '\n';
            line[n] = '\0';
            before_edit(true);
            replace_range(s, s, line);
            free(line);
            after_edit();
            return true;
        }
        case 'a': {
            lv_obj_t *lbl = lv_textarea_get_label(ta_editor);
            uint32_t n = byte_to_char(lv_textarea_get_text(ta_editor), 0xFFFFFFFFu);
            lv_label_set_text_selection_start(lbl, 0);
            lv_label_set_text_selection_end(lbl, n);
            lv_textarea_set_cursor_pos(ta_editor, LV_TEXTAREA_CURSOR_LAST);
            return true;
        }
        case 'b':
            if (s_markdown) {
                uint32_t s, e;
                before_edit(true);
                if (get_selection(&s, &e)) {
                    const char *t = lv_textarea_get_text(ta_editor);
                    size_t n = e - s;
                    char *w = malloc(n + 5);
                    if (w) {
                        snprintf(w, n + 5, "**%.*s**", (int)n, t + s);
                        replace_range(s, e, w);
                        free(w);
                    }
                } else {
                    const char *t = lv_textarea_get_text(ta_editor);
                    uint32_t c = char_to_byte(t, lv_textarea_get_cursor_pos(ta_editor));
                    replace_range(c, c, "****");
                    lv_textarea_set_cursor_pos(ta_editor, (int32_t)lv_textarea_get_cursor_pos(ta_editor) - 2);
                }
                after_edit();
            }
            return true;
        default: return false;
        }
    }

    bool moved = true;
    if ((mods & DEVOS_MOD_FN) && key == LV_KEY_LEFT) cursor_line_edge(false);
    else if ((mods & DEVOS_MOD_FN) && key == LV_KEY_RIGHT) cursor_line_edge(true);
    else if ((mods & DEVOS_MOD_ALT) && key == LV_KEY_LEFT) cursor_word(false);
    else if ((mods & DEVOS_MOD_ALT) && key == LV_KEY_RIGHT) cursor_word(true);
    else if (key == LV_KEY_LEFT) lv_textarea_cursor_left(ta_editor);
    else if (key == LV_KEY_RIGHT) lv_textarea_cursor_right(ta_editor);
    else if (key == LV_KEY_UP) lv_textarea_cursor_up(ta_editor);
    else if (key == LV_KEY_DOWN) lv_textarea_cursor_down(ta_editor);
    else if (key == DEVOS_KEY_PGUP || key == DEVOS_KEY_PGDN) {
        for (int i = 0; i < 18; i++) {
            if (key == DEVOS_KEY_PGUP) lv_textarea_cursor_up(ta_editor);
            else lv_textarea_cursor_down(ta_editor);
        }
    } else moved = false;
    if (moved) {
        clear_selection();
        refresh_status();
        return true;
    }

    if (key == LV_KEY_ESC) {
        s_focus_list = true;
        s_sidebar_visible = true;
        apply_layout();
        list_paint();
        return true;
    }

    uint32_t s, e;
    bool sel = get_selection(&s, &e);
    if (key == '\b' || key == 0x7F || key == LV_KEY_DEL) {
        before_edit(sel);
        if (sel) replace_range(s, e, "");
        else if (key == LV_KEY_DEL) lv_textarea_delete_char_forward(ta_editor);
        else lv_textarea_delete_char(ta_editor);
        after_edit();
        return true;
    }
    if (key == '\r' || key == '\n') {
        if (sel) replace_range(s, e, "");
        newline_indent();
        return true;
    }
    if (key == '\t') {
        before_edit(false);
        if (sel) replace_range(s, e, "");
        lv_textarea_add_text(ta_editor, s_markdown ? "  " : "    ");
        after_edit();
        return true;
    }
    if (key >= 32 && key <= 126) {
        if (mods & (DEVOS_MOD_FN | DEVOS_MOD_ALT)) return true;   /* unbound shortcut */
        if (strlen(lv_textarea_get_text(ta_editor)) >= ED_EDIT_MAX - 1) {
            flash("File is at the editing limit");
            return true;
        }
        before_edit(sel);
        if (sel) {
            char c[2] = {(char)key, '\0'};
            replace_range(s, e, c);
        } else {
            lv_textarea_add_char(ta_editor, (char)key);
        }
        after_edit();
        return true;
    }
    return false;
}

/* -------------------------------------------------------------------- init */
static lv_obj_t *mk_btn(lv_obj_t *parent, const char *text, int w, lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    const devos_palette_t *p = devos_theme_get();
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, 26);
    lv_obj_set_style_bg_color(b, p->surface, 0);
    lv_obj_set_style_border_color(b, p->surface_border, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, p->text_primary, 0);
    if (lbl_out) *lbl_out = l;
    return b;
}

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
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);

    /* ---- file browser ---- */
    sidebar = lv_obj_create(screen);
    lv_obj_set_size(sidebar, DEVOS_PANE_LEFT_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(sidebar, 0, 0);
    lv_obj_set_style_bg_color(sidebar, p->surface, 0);
    lv_obj_set_style_border_width(sidebar, 1, 0);
    lv_obj_set_style_radius(sidebar, 0, 0);
    lv_obj_set_style_pad_all(sidebar, 8, 0);
    lv_obj_remove_flag(sidebar, LV_OBJ_FLAG_SCROLLABLE);

    lbl_side_title = lv_label_create(sidebar);
    lv_label_set_text(lbl_side_title, "SD CARD");
    lv_obj_set_pos(lbl_side_title, 2, 4);
    lv_obj_set_style_text_font(lbl_side_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_side_title, p->text_secondary, 0);
    btn_add_dir = mk_btn(sidebar, LV_SYMBOL_DIRECTORY "+", 44, add_dir_cb, NULL);
    lv_obj_align(btn_add_dir, LV_ALIGN_TOP_RIGHT, 0, 0);
    btn_add_file = mk_btn(sidebar, LV_SYMBOL_FILE "+", 44, add_file_cb, NULL);
    lv_obj_align_to(btn_add_file, btn_add_dir, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    lbl_path = lv_label_create(sidebar);
    lv_label_set_text(lbl_path, "/");
    lv_obj_set_pos(lbl_path, 2, 32);
    lv_obj_set_width(lbl_path, DEVOS_PANE_LEFT_WIDTH - 20);
    lv_label_set_long_mode(lbl_path, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl_path, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_path, p->accent_primary, 0);

    file_list = lv_obj_create(sidebar);
    lv_obj_set_pos(file_list, -4, 56);
    lv_obj_set_size(file_list, DEVOS_PANE_LEFT_WIDTH - 8, DEVOS_CONTENT_HEIGHT - 56 - 16 - 50);
    lv_obj_set_style_bg_opa(file_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(file_list, 0, 0);
    lv_obj_set_style_pad_all(file_list, 4, 0);
    lv_obj_set_style_pad_row(file_list, 4, 0);
    lv_obj_set_flex_flow(file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(file_list, LV_DIR_VER);

    lbl_side_hint = lv_label_create(sidebar);
    lv_label_set_text(lbl_side_hint, "Enter open  Bksp up  Esc editor\nN file  F folder  R rename  D delete  H hidden");
    lv_obj_align(lbl_side_hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_text_font(lbl_side_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_side_hint, p->text_secondary, 0);

    /* ---- main area ---- */
    main_area = lv_obj_create(screen);
    lv_obj_set_style_bg_color(main_area, p->bg, 0);
    lv_obj_set_style_radius(main_area, 0, 0);
    lv_obj_set_style_border_width(main_area, 0, 0);
    lv_obj_set_style_pad_all(main_area, 0, 0);
    lv_obj_remove_flag(main_area, LV_OBJ_FLAG_SCROLLABLE);

    top_bar = lv_obj_create(main_area);
    lv_obj_set_size(top_bar, lv_pct(100), 34);
    lv_obj_set_pos(top_bar, 0, 0);
    lv_obj_set_style_bg_color(top_bar, p->top_bar_bg, 0);
    lv_obj_set_style_border_width(top_bar, 1, 0);
    lv_obj_set_style_border_side(top_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_set_style_pad_left(top_bar, 10, 0);
    lv_obj_remove_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);

    lbl_fn = lv_label_create(top_bar);
    lv_label_set_text(lbl_fn, "Markdown Editor");
    lv_obj_align(lbl_fn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_long_mode(lbl_fn, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl_fn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_fn, p->accent_primary, 0);

    btn_tree = mk_btn(top_bar, LV_SYMBOL_DIRECTORY " Files", 76, tree_cb, &lbl_btn_tree);
    btn_save = mk_btn(top_bar, LV_SYMBOL_SAVE " Save", 70, save_cb, &lbl_btn_save);
    btn_find = mk_btn(top_bar, LV_SYMBOL_EYE_OPEN " Find", 70, find_cb, &lbl_btn_find);
    btn_mode = mk_btn(top_bar, "Edit", 76, mode_cb, &lbl_btn_mode);
    lv_obj_set_style_text_color(lbl_btn_mode, p->accent_primary, 0);

    find_bar = lv_obj_create(main_area);
    lv_obj_set_pos(find_bar, 0, 34);
    lv_obj_set_style_radius(find_bar, 0, 0);
    lv_obj_set_style_border_width(find_bar, 0, 0);
    lv_obj_set_style_pad_all(find_bar, 4, 0);
    lv_obj_remove_flag(find_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(find_bar, LV_OBJ_FLAG_HIDDEN);
    ta_find = lv_textarea_create(find_bar);
    lv_textarea_set_one_line(ta_find, true);
    lv_textarea_set_placeholder_text(ta_find, "Find...");
    lv_obj_set_size(ta_find, 360, 32);
    lv_obj_align(ta_find, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_pad_ver(ta_find, 7, 0);
    lv_obj_set_style_pad_hor(ta_find, 10, 0);
    lv_obj_set_style_radius(ta_find, 6, 0);
    lv_obj_set_style_text_font(ta_find, &lv_font_montserrat_14, 0);
    lv_obj_set_scrollbar_mode(ta_find, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_state(ta_find, LV_STATE_FOCUSED);
    lbl_find_info = lv_label_create(find_bar);
    lv_label_set_text(lbl_find_info, "");
    lv_obj_align(lbl_find_info, LV_ALIGN_LEFT_MID, 380, 0);
    lv_obj_set_style_text_font(lbl_find_info, &lv_font_montserrat_12, 0);

    ta_editor = lv_textarea_create(main_area);
    lv_textarea_set_text(ta_editor, "");
    lv_textarea_set_max_length(ta_editor, ED_EDIT_MAX);
    lv_textarea_set_text_selection(ta_editor, true);
    lv_obj_add_event_cb(ta_editor, editor_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(ta_editor, p->code_bg, 0);
    lv_obj_set_style_text_color(ta_editor, p->text_primary, 0);
    lv_obj_set_style_border_width(ta_editor, 0, 0);
    lv_obj_set_style_radius(ta_editor, 0, 0);
    lv_obj_set_style_pad_all(ta_editor, 14, 0);
    lv_obj_set_style_text_font(ta_editor, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(ta_editor, p->accent_primary, LV_PART_CURSOR);
    lv_obj_set_style_border_color(ta_editor, p->accent_primary, LV_PART_CURSOR);
    lv_obj_set_style_bg_color(lv_textarea_get_label(ta_editor), p->accent_primary, LV_PART_SELECTED);
    lv_obj_set_style_text_color(lv_textarea_get_label(ta_editor), p->bg, LV_PART_SELECTED);

    preview_scroll = lv_obj_create(main_area);
    lv_obj_set_style_bg_color(preview_scroll, p->code_bg, 0);
    lv_obj_set_style_radius(preview_scroll, 0, 0);
    lv_obj_set_style_border_width(preview_scroll, 0, 0);
    lv_obj_set_style_pad_all(preview_scroll, 16, 0);
    lv_obj_add_flag(preview_scroll, LV_OBJ_FLAG_HIDDEN);

    viewer_scroll = lv_obj_create(main_area);
    lv_obj_set_style_bg_color(viewer_scroll, p->code_bg, 0);
    lv_obj_set_style_radius(viewer_scroll, 0, 0);
    lv_obj_set_style_border_width(viewer_scroll, 0, 0);
    lv_obj_add_flag(viewer_scroll, LV_OBJ_FLAG_HIDDEN);
    devos_codeview_create(&s_viewer, viewer_scroll);
    s_viewer.plain = true;

    lbl_empty = lv_label_create(main_area);
    lv_label_set_text(lbl_empty,
                      "Pick a file on the left to open it, or press Ctrl+N for a new one.\n\n"
                      "Esc switches between the file list and the editor. Markdown files get a live "
                      "preview (Ctrl+P); Ctrl+F finds text.");
    lv_label_set_long_mode(lbl_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lbl_empty, p->text_secondary, 0);

    status_bar = lv_obj_create(main_area);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    lv_obj_set_style_pad_left(status_bar, 10, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lbl_status = lv_label_create(status_bar);
    lv_label_set_text(lbl_status, "");
    lv_obj_set_pos(lbl_status, 0, 3);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_12, 0);
    lbl_keys = lv_label_create(status_bar);
    lv_label_set_text(lbl_keys, "");
    lv_obj_set_pos(lbl_keys, 0, 21);
    lv_label_set_long_mode(lbl_keys, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl_keys, &lv_font_montserrat_12, 0);

    /* ---- name / confirm dialog ---- */
    modal = lv_obj_create(screen);
    lv_obj_set_size(modal, 520, 200);
    lv_obj_align(modal, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_border_width(modal, 2, 0);
    lv_obj_set_style_radius(modal, 8, 0);
    lv_obj_set_style_pad_all(modal, 16, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_HIDDEN);
    lbl_modal_title = lv_label_create(modal);
    lv_obj_set_width(lbl_modal_title, 480);
    lv_label_set_long_mode(lbl_modal_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl_modal_title, &lv_font_montserrat_16, 0);
    ta_modal = lv_textarea_create(modal);
    lv_textarea_set_one_line(ta_modal, true);
    lv_obj_set_size(ta_modal, 480, 38);
    lv_obj_set_pos(ta_modal, 0, 36);
    lv_obj_set_style_pad_ver(ta_modal, 9, 0);
    lv_obj_set_style_pad_hor(ta_modal, 10, 0);
    lv_obj_set_style_radius(ta_modal, 6, 0);
    lv_obj_set_style_text_font(ta_modal, &lv_font_montserrat_14, 0);
    lv_obj_set_scrollbar_mode(ta_modal, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_state(ta_modal, LV_STATE_FOCUSED);   /* show the cursor */
    lbl_modal_desc = lv_label_create(modal);
    lv_obj_set_width(lbl_modal_desc, 480);
    lv_label_set_long_mode(lbl_modal_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(lbl_modal_desc, 0, 82);
    lv_obj_set_style_text_font(lbl_modal_desc, &lv_font_montserrat_12, 0);
    btn_modal_ok = mk_btn(modal, "OK", 110, modal_ok_cb, &lbl_modal_ok);
    lv_obj_set_size(btn_modal_ok, 110, 34);
    lv_obj_align(btn_modal_ok, LV_ALIGN_BOTTOM_RIGHT, -120, 0);
    lv_obj_set_style_text_color(lbl_modal_ok, lv_color_black(), 0);
    lv_obj_t *cancel = mk_btn(modal, "Cancel", 110, modal_cancel_cb, NULL);
    lv_obj_set_size(cancel, 110, 34);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    devos_theme_add_listener(apply_theme, NULL);
    ed_voice_init(screen, voice_insert);
    lv_timer_create(tick_cb, 250, NULL);

    /* last folder + file, else /notes when it exists */
    char last[ED_PATH_MAX];
    config_load(last, sizeof(last));
    if (!s_dir[0] && !last[0]) {
        char np[ED_PATH_MAX + 16];
        abs_path("notes", np, sizeof(np));
        struct stat st;
        if (stat(np, &st) == 0 && S_ISDIR(st.st_mode)) snprintf(s_dir, sizeof(s_dir), "notes");
    }
    load_dir();
    list_rebuild();
    apply_theme(p, NULL);
    if (last[0]) {
        char lp[ED_PATH_MAX + 32];
        abs_path(last, lp, sizeof(lp));
        struct stat st;
        if (stat(lp, &st) == 0) open_path(last);
    } else {
        char wp[ED_PATH_MAX + 32];
        abs_path("notes/welcome.md", wp, sizeof(wp));
        struct stat st;
        if (stat(wp, &st) == 0) open_path("notes/welcome.md");
    }
    s_focus_list = !s_file[0];
    apply_layout();
    list_paint();
    refresh_status();
}

static void editor_show(void)
{
    /* pick up files changed elsewhere (e.g. edited on a PC) */
    load_dir();
    list_rebuild();
    if (s_file[0] && !s_dirty && !s_readonly) {
        char path[ED_PATH_MAX + 32];
        abs_path(s_file, path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (f) {
            size_t n = fread(s_buf, 1, ED_EDIT_MAX, f);
            fclose(f);
            s_buf[n] = '\0';
            const char *cur = lv_textarea_get_text(ta_editor);
            if (strcmp(cur, s_buf) != 0) {
                uint32_t pos = lv_textarea_get_cursor_pos(ta_editor);
                lv_textarea_set_text(ta_editor, s_buf);
                lv_textarea_set_cursor_pos(ta_editor, (int32_t)pos);
                if (s_view != VIEW_EDIT) render_preview(true);
            }
        } else {
            close_file();                   /* deleted elsewhere */
        }
    }
    apply_layout();
    refresh_status();
}

static void editor_hide(void)
{
    if (ed_voice_recording()) ed_voice_record_stop();   /* its link goes in when the file closes */
    if (ed_voice_playing()) ed_voice_stop();
    if (s_dirty) save_file();
}

static int editor_telemetry_lines(char lines[3][64])
{
    char vs[64];
    ed_voice_status(vs, sizeof(vs));
    if (vs[0]) {
        snprintf(lines[0], sizeof(lines[0]), "* %s", vs);
        snprintf(lines[1], sizeof(lines[1]), "* %s", s_file[0] ? base_name(s_file) : "(no file open)");
        snprintf(lines[2], sizeof(lines[2]), "* Ctrl+R stops");
        return 3;
    }
    snprintf(lines[0], sizeof(lines[0]), "* %s", s_file[0] ? base_name(s_file) : "(no file open)");
    snprintf(lines[1], sizeof(lines[1]), "* /%.58s", s_dir);
    snprintf(lines[2], sizeof(lines[2]), "* %s", s_dirty ? "unsaved changes" : "SD card files");
    return 3;
}

devos_app_descriptor_t *app_editor_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_EDITOR;
    app_descriptor.uid = "editor";
    app_descriptor.icon = LV_SYMBOL_DIRECTORY;
    app_descriptor.draw_icon = devos_icon_editor;
    app_descriptor.category = "notes";
    app_descriptor.name = "Editor";
    app_descriptor.title = "Markdown Editor";
    app_descriptor.subtitle = "Notes, docs and files on the SD card";
    app_descriptor.screen = screen;
    app_descriptor.init = editor_init;
    app_descriptor.show = editor_show;
    app_descriptor.hide = editor_hide;
    app_descriptor.handle_key = editor_handle_key;
    app_descriptor.get_telemetry_lines = editor_telemetry_lines;
    return &app_descriptor;
}
