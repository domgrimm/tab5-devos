/* devos_cmdpal: see devos_cmdpal.h. */
#include "devos_cmdpal.h"
#include "devos_match.h"
#include "devos_hud.h"
#include "devos_widgets.h"
#include "devos_focus.h"
#include "devos_theme.h"
#include "devos_core.h"
#include "devos_power.h"
#include "devos_config.h"

#include <stdio.h>
#include <string.h>

#define PAL_W        640
#define PAL_PAD      16
#define PAL_TA_H     40
#define PAL_ROW_H    36
#define PAL_ROWS     7
#define PAL_LIST_Y   (PAL_TA_H + 10)
#define PAL_LIST_H   (PAL_ROWS * PAL_ROW_H)
#define PAL_INNER_W  (PAL_W - 2 * PAL_PAD - 4)       /* 2 px border */
#define PAL_H        (PAL_LIST_Y + PAL_LIST_H + 8 + 16 + 2 * PAL_PAD + 4)
#define PAL_MAX_CMDS 48
#define PAL_MAX      (DEVOS_MAX_APPS + PAL_MAX_CMDS)

#define HINT_KEYS "Type to search   Up / Down  choose   Enter  run   Esc  close"

typedef struct {
    devos_app_descriptor_t *app;        /* an app ... */
    const devos_command_t *cmd;     /* ... or a command */
    char title[64];
    char keywords[192];
    char hint[16];
    int score;
} entry_t;

static const devos_command_t *s_cmds[PAL_MAX_CMDS];
static int s_cmd_n;

/* What the palette offers, gathered when it opens, and the matches shown. */
static EXT_RAM_BSS_ATTR entry_t s_all[PAL_MAX];
static int s_all_n;
static int s_shown[PAL_MAX];
static int s_shown_n;

static lv_obj_t *s_overlay, *s_box, *s_ta, *s_empty, *s_hint;
static devos_vlist_t s_list;
static devos_focus_t s_focus;
static char s_query[48];               /* the text last searched for */
static int s_confirm = -1;             /* entry waiting for a second Enter */

static const char *str(const char *s) { return s ? s : ""; }

void devos_cmdpal_add(const devos_command_t *cmd)
{
    if (!cmd || s_cmd_n >= PAL_MAX_CMDS) return;
    for (int i = 0; i < s_cmd_n; i++) {
        if (s_cmds[i] == cmd) return;
    }
    s_cmds[s_cmd_n++] = cmd;
}

bool devos_cmdpal_is_open(void)
{
    return s_overlay && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ entries */
static void add_app(devos_app_descriptor_t *app)
{
    if (s_all_n >= PAL_MAX) return;
    entry_t *e = &s_all[s_all_n++];
    memset(e, 0, sizeof(*e));
    e->app = app;
    snprintf(e->title, sizeof(e->title), "%s", app->title ? app->title : app->name ? app->name : str(app->uid));
    snprintf(e->keywords, sizeof(e->keywords), "%s %s %s %s", str(app->uid), str(app->name), str(app->category),
             str(app->subtitle));
    if (app->id == DEVOS_APP_LAUNCHER) snprintf(e->hint, sizeof(e->hint), "Sym+H");
    else if (app->id >= 1 && app->id <= 8) snprintf(e->hint, sizeof(e->hint), "Sym+%d", (int)app->id);
    else snprintf(e->hint, sizeof(e->hint), "App");
}

static void add_cmd(const devos_command_t *cmd)
{
    if (s_all_n >= PAL_MAX) return;
    entry_t *e = &s_all[s_all_n++];
    memset(e, 0, sizeof(*e));
    e->cmd = cmd;
    const char *label = cmd->label ? cmd->label(cmd->ud) : NULL;
    snprintf(e->title, sizeof(e->title), "%s", label ? label : str(cmd->title));
    /* the fixed title counts as keywords, so a changing label is still found by it */
    snprintf(e->keywords, sizeof(e->keywords), "%s %s", str(cmd->title), str(cmd->keywords));
    snprintf(e->hint, sizeof(e->hint), "%s", str(cmd->hint));
}

static void gather(void)
{
    s_all_n = 0;
    for (int i = 0; i < devos_core_app_count(); i++) {
        devos_app_descriptor_t *app = devos_core_get_app_at(i);
        if (app) add_app(app);
    }
    for (int i = 0; i < s_cmd_n; i++) add_cmd(s_cmds[i]);
}

static void set_hint(const char *text, bool warn)
{
    devos_w_set_text(s_hint, text ? text : HINT_KEYS);
    devos_w_track(s_hint, warn ? DEVOS_W_TEXT_WARN : DEVOS_W_TEXT_DIM);
}

static void cancel_confirm(void)
{
    if (s_confirm < 0) return;
    s_confirm = -1;
    set_hint(NULL, false);
}

/* Best match first; equal scores keep their order (apps, then commands). */
static void filter(void)
{
    const char *q = lv_textarea_get_text(s_ta);
    snprintf(s_query, sizeof(s_query), "%s", q);
    s_shown_n = 0;
    for (int i = 0; i < s_all_n; i++) {
        int sc = devos_match_score(q, s_all[i].title, s_all[i].keywords);
        if (sc < 0) continue;
        s_all[i].score = sc;
        int j = s_shown_n++;
        while (j > 0 && s_all[s_shown[j - 1]].score < sc) {
            s_shown[j] = s_shown[j - 1];
            j--;
        }
        s_shown[j] = i;
    }
    cancel_confirm();
    devos_vlist_set_count(&s_list, s_shown_n);
    lv_obj_scroll_to_y(s_list.scroll, 0, LV_ANIM_OFF);
    devos_vlist_select(&s_list, 0);
    if (s_shown_n) lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
}

static void run_shown(int idx)
{
    if (idx < 0 || idx >= s_shown_n) return;
    int i = s_shown[idx];
    entry_t *e = &s_all[i];
    if (e->cmd && e->cmd->confirm && s_confirm != i) {
        const char *ask = e->cmd->confirm(e->cmd->ud);
        if (ask) {
            s_confirm = i;
            set_hint(ask, true);
            return;
        }
    }
    devos_cmdpal_close();
    if (e->app) devos_core_switch_app(e->app->id);
    else if (e->cmd->run) e->cmd->run(e->cmd->ud);
}

/* ------------------------------------------------------------------ drawing */
static void row_draw(devos_vlist_t *v, lv_layer_t *layer, const lv_area_t *row, int idx)
{
    const devos_palette_t *p = devos_theme_get();
    const entry_t *e = &s_all[s_shown[idx]];
    bool sel = idx == v->sel;
    int cy = (row->y1 + row->y2) / 2;
    lv_color_t ic = sel ? p->accent_primary : p->text_secondary;

    lv_area_t ia = { row->x1 + 14, cy - 10, row->x1 + 33, cy + 9 };
    if (e->app && e->app->draw_icon) {
        e->app->draw_icon(layer, &ia, ic);
    } else {
        const char *sym = e->app ? e->app->icon : e->cmd->icon;
        if (sym && *sym) {
            const lv_font_t *f = &lv_font_montserrat_16;
            int w = lv_text_get_width(sym, strlen(sym), f, 0);
            devos_w_draw_text(layer, f, (ia.x1 + ia.x2 + 1) / 2 - w / 2, cy - lv_font_get_line_height(f) / 2, 0,
                              sym, ic);
        }
    }

    const lv_font_t *hf = &lv_font_montserrat_12;
    int hint_w = e->hint[0] ? lv_text_get_width(e->hint, strlen(e->hint), hf, 0) : 0;
    int hint_x = row->x2 - 22 - hint_w;                  /* clear of the scrollbar */
    const lv_font_t *tf = &lv_font_montserrat_14;
    int tx = row->x1 + 46;
    devos_w_draw_text(layer, tf, tx, cy - lv_font_get_line_height(tf) / 2, hint_x - 16 - tx, e->title,
                      sel ? p->text_primary : p->text_secondary);
    if (hint_w) {
        devos_w_draw_text(layer, hf, hint_x, cy - lv_font_get_line_height(hf) / 2, 0, e->hint, p->text_muted);
    }
}

/* A tap runs the row (the list's own handler has already selected it). */
static void row_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    lv_area_t a;
    lv_obj_get_coords(s_list.view, &a);
    int idx = (pt.y - a.y1) / PAL_ROW_H;
    if (idx >= 0 && idx < s_shown_n) run_shown(idx);
}

static void overlay_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_cmdpal_close();
}

static void build(void)
{
    if (s_overlay) return;
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, DEVOS_SCREEN_WIDTH, DEVOS_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_50, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, overlay_click_cb, LV_EVENT_CLICKED, NULL);

    s_box = lv_obj_create(s_overlay);
    lv_obj_set_size(s_box, PAL_W, PAL_H);
    lv_obj_align(s_box, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_border_width(s_box, 2, 0);
    lv_obj_set_style_radius(s_box, 8, 0);
    lv_obj_set_style_pad_all(s_box, PAL_PAD, 0);
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_SCROLLABLE);
    devos_w_track(s_box, DEVOS_W_MODAL);

    s_ta = devos_w_ta(s_box, true, PAL_INNER_W, PAL_TA_H);
    lv_obj_set_style_text_font(s_ta, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_ver(s_ta, 9, 0);
    lv_textarea_set_placeholder_text(s_ta, "Search apps and commands");
    lv_textarea_set_max_length(s_ta, sizeof(s_query) - 1);
    lv_obj_set_pos(s_ta, 0, 0);
    devos_focus_init(&s_focus);
    devos_focus_add(&s_focus, s_ta);

    devos_vlist_create(&s_list, s_box, PAL_ROW_H, row_draw);
    lv_obj_set_pos(s_list.scroll, 0, PAL_LIST_Y);
    lv_obj_set_size(s_list.scroll, PAL_INNER_W, PAL_LIST_H);
    s_list.active = true;
    lv_obj_add_event_cb(s_list.view, row_click_cb, LV_EVENT_CLICKED, NULL);

    s_empty = devos_w_label(s_box, &lv_font_montserrat_14, DEVOS_W_TEXT_MUTED, "Nothing matches");
    lv_obj_align(s_empty, LV_ALIGN_TOP_MID, 0, PAL_LIST_Y + PAL_ROW_H);
    lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);

    s_hint = devos_w_label(s_box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, HINT_KEYS);
    lv_obj_set_width(s_hint, PAL_INNER_W);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_hint, 0, PAL_LIST_Y + PAL_LIST_H + 8);
    lv_obj_update_layout(s_overlay);    /* real coordinates before the first show redraws them */
}

void devos_cmdpal_open(void)
{
    if (devos_cmdpal_is_open()) return;
    devos_hud_close();
    build();
    gather();
    lv_textarea_set_text(s_ta, "");
    s_confirm = -1;
    set_hint(NULL, false);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    devos_focus_set(&s_focus, s_ta);
    filter();
}

void devos_cmdpal_close(void)
{
    if (!devos_cmdpal_is_open()) return;
    devos_focus_clear(&s_focus);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_confirm = -1;
}

/* ------------------------------------------------------------------ keys */
static bool palette_key(uint32_t key, uint8_t mods)
{
    bool toggle = (mods & DEVOS_MOD_FN) && key == ' ';
    if (!devos_cmdpal_is_open()) {
        if (!toggle) return false;
        devos_cmdpal_open();
        return true;
    }
    /* open: the palette has the keyboard */
    if (toggle) {
        devos_cmdpal_close();
        return true;
    }
    if (key == LV_KEY_ESC) {
        if (s_confirm >= 0) cancel_confirm();
        else devos_cmdpal_close();
        return true;
    }
    if (key == '\r' || key == '\n') {
        run_shown(s_list.sel);
        return true;
    }
    cancel_confirm();
    if ((mods & DEVOS_MOD_FN) && (key == LV_KEY_UP || key == LV_KEY_DOWN)) {
        key = key == LV_KEY_UP ? DEVOS_KEY_PGUP : DEVOS_KEY_PGDN;
    } else if (mods & (DEVOS_MOD_CTRL | DEVOS_MOD_ALT | DEVOS_MOD_FN)) {
        return true;
    }
    if (key == '\t') key = (mods & DEVOS_MOD_SHIFT) ? LV_KEY_UP : LV_KEY_DOWN;
    if (devos_vlist_key(&s_list, key)) return true;
    if (devos_focus_key(&s_focus, key, mods) && strcmp(lv_textarea_get_text(s_ta), s_query) != 0) filter();
    return true;
}

/* ------------------------------------------------------------------ system commands */
static const char *theme_label(void *ud)
{
    LV_UNUSED(ud);
    return devos_theme_is_dark() ? "Switch to the light theme" : "Switch to the dark theme";
}

static void theme_run(void *ud)
{
    LV_UNUSED(ud);
    devos_theme_toggle();
}

static void info_run(void *ud)
{
    LV_UNUSED(ud);
    devos_hud_open();
}

static void sleep_run(void *ud)
{
    LV_UNUSED(ud);
    devos_power_sleep_now();
}

static const char *restart_confirm(void *ud)
{
    LV_UNUSED(ud);
    static char ask[160];
    const char *why = devos_core_restart_check();
    if (why) snprintf(ask, sizeof(ask), "%s. Enter restarts anyway, Esc cancels", why);
    else snprintf(ask, sizeof(ask), "Restart the Tab5 now? Enter restarts, Esc cancels");
    return ask;
}

static void restart_run(void *ud)
{
    LV_UNUSED(ud);
    devos_core_restart();
}

static const devos_command_t s_system[] = {
    { .title = "Switch theme", .keywords = "theme dark light colours colors mode", .hint = "Sym+T",
      .icon = LV_SYMBOL_TINT, .label = theme_label, .run = theme_run },
    { .title = "System info", .keywords = "hud info battery power memory ram psram cpu load wifi ip uptime stats",
      .hint = "Sym+I", .icon = LV_SYMBOL_CHARGE, .run = info_run },
    { .title = "Turn the screen off", .keywords = "sleep screen display off lock", .icon = LV_SYMBOL_EYE_CLOSE,
      .run = sleep_run },
    { .title = "Restart the Tab5", .keywords = "reboot restart reset", .icon = LV_SYMBOL_REFRESH,
      .confirm = restart_confirm, .run = restart_run },
};

void devos_cmdpal_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    for (size_t i = 0; i < sizeof(s_system) / sizeof(s_system[0]); i++) devos_cmdpal_add(&s_system[i]);
    devos_core_add_key_hook(palette_key);
}
