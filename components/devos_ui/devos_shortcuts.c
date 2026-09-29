/* devos_shortcuts: see devos_shortcuts.h. */
#include "devos_shortcuts.h"
#include "devos_cmdpal.h"
#include "devos_hud.h"
#include "devos_widgets.h"
#include "devos_theme.h"
#include "devos_core.h"
#include "devos_config.h"

#include <stdio.h>
#include <string.h>

#define SC_W        1230
#define SC_H        640
#define SC_PAD      16
#define SC_INNER_W  (SC_W - 2 * SC_PAD - 4)
#define SC_INNER_H  (SC_H - 2 * SC_PAD - 4)
#define SC_COL_Y    32
#define SC_COL_H    (SC_INNER_H - SC_COL_Y - 24)
#define SC_LEFT_W   560
#define SC_GAP      14
#define SC_RIGHT_W  (SC_INNER_W - SC_LEFT_W - SC_GAP)
#define SC_KEYS_W   150

/* Everywhere: the global keys (devos_core_dispatch_key, the overlays) and
 * the one key model every screen follows (AGENTS.md invariant 9). */
static const char *const GLOBAL_KEYS =
    "Anywhere\n"
    "Sym+Space\tCommand palette: find any app or command\n"
    "Sym+I\tSystem info: power, memory, network, CPU\n"
    DEVOS_SHORTCUTS_KEY_TEXT "\tThis sheet\n"
    "Sym+H\tHome Screen\n"
    "Sym+1 ... 6\tTerminal, Editor, Tailscale, WireGuard, MQTT, Settings\n"
    "Alt+Tab\tThe app before this one\n"
    "Sym+T\tDark / light theme\n"
    "Sym+- / Sym++\tScreen brightness\n"
    "Sym+L\tShow / hide the side panel (Editor, Terminal, MQTT, REST)\n"
    "Sym+Up / Down\tPage up / down\n"
    "On every screen\n"
    "Arrows\tMove the selection or focus\n"
    "Tab / Aa+Tab\tNext / previous field or area\n"
    "Enter\tOpen, connect, confirm, press\n"
    "Space\tTick a box, flip a switch, pause a live view\n"
    "Left / Right\tChange the focused setting\n"
    "Esc\tBack one level; unhandled, the Home Screen\n"
    "The keyboard\n"
    "Aa\tTap: caps lock. Hold: Shift\n"
    "Sym+punctuation\tTypes the symbol printed on the key\n";

static lv_obj_t *s_overlay, *s_box, *s_left, *s_right;

bool devos_shortcuts_is_open(void)
{
    return s_overlay && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ layout */
static lv_obj_t *column(int x, int w)
{
    lv_obj_t *c = devos_w_panel(s_box, x, SC_COL_Y, w, SC_COL_H, DEVOS_W_PANEL_ALT);
    lv_obj_set_style_radius(c, 6, 0);
    lv_obj_set_style_pad_all(c, 12, 0);
    lv_obj_set_style_pad_row(c, 5, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_AUTO);
    return c;
}

/* "heading" lines and "keys\twhat" rows into a column */
static void fill(lv_obj_t *col, const char *text)
{
    int w = lv_obj_get_width(col) - 24 - 2 - 12;       /* padding, border, scrollbar */
    bool first = true;
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        char buf[200];
        snprintf(buf, sizeof(buf), "%.*s", (int)(len < sizeof(buf) - 1 ? len : sizeof(buf) - 1), line);
        line = end ? end + 1 : NULL;
        if (!buf[0]) continue;
        char *tab = strchr(buf, '\t');
        if (!tab) {
            lv_obj_t *h = devos_w_label(col, &lv_font_montserrat_12, DEVOS_W_TEXT_ACCENT, buf);
            if (!first) lv_obj_set_style_pad_top(h, 10, 0);
            first = false;
            continue;
        }
        *tab = '\0';
        first = false;
        lv_obj_t *row = lv_obj_create(col);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, w, LV_SIZE_CONTENT);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_t *k = devos_w_label(row, &lv_font_montserrat_14, DEVOS_W_TEXT, buf);
        lv_obj_set_width(k, SC_KEYS_W - 10);
        lv_label_set_long_mode(k, LV_LABEL_LONG_WRAP);
        lv_obj_t *d = devos_w_label(row, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM, tab + 1);
        lv_obj_set_width(d, w - SC_KEYS_W);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(d, SC_KEYS_W, 0);
    }
}

static void overlay_click_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_overlay) devos_shortcuts_close();
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
    lv_obj_set_size(s_box, SC_W, SC_H);
    lv_obj_align(s_box, LV_ALIGN_CENTER, 0, DEVOS_TOP_BAR_HEIGHT / 2);
    lv_obj_set_style_border_width(s_box, 2, 0);
    lv_obj_set_style_radius(s_box, 8, 0);
    lv_obj_set_style_pad_all(s_box, SC_PAD, 0);
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_SCROLLABLE);
    devos_w_track(s_box, DEVOS_W_MODAL);

    devos_w_label(s_box, &lv_font_montserrat_16, DEVOS_W_TEXT_ACCENT, "Keyboard shortcuts");

    s_left = column(0, SC_LEFT_W);
    s_right = column(SC_LEFT_W + SC_GAP, SC_RIGHT_W);
    lv_obj_update_layout(s_box);
    fill(s_left, GLOBAL_KEYS);

    lv_obj_t *keys = devos_w_label(s_box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM,
                                   "Up / Down scroll    Esc / " DEVOS_SHORTCUTS_KEY_TEXT "  close    "
                                   "Sym+Space  commands");
    lv_obj_align(keys, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_update_layout(s_overlay);    /* real coordinates before the first show redraws them */
}

/* The right column: the app on screen now. */
static void fill_app(void)
{
    lv_obj_clean(s_right);
    devos_app_descriptor_t *app = devos_core_get_app(devos_core_get_current_app());
    const char *name = app ? (app->title ? app->title : app->name) : NULL;
    char head[80];
    snprintf(head, sizeof(head), "In %s", name ? name : "this app");
    const char *keys = app && app->get_shortcuts ? app->get_shortcuts() : NULL;
    if (keys && *keys) {
        /* the app's own headings follow; start with its name */
        lv_obj_t *h = devos_w_label(s_right, &lv_font_montserrat_12, DEVOS_W_TEXT_ACCENT, head);
        LV_UNUSED(h);
        fill(s_right, keys);
    } else {
        fill(s_right, head);
        lv_obj_t *l = devos_w_label(s_right, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM,
                                    "This app's keys are on the hint line along the bottom of its screen.");
        lv_obj_set_width(l, SC_RIGHT_W - 34);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    lv_obj_scroll_to_y(s_right, 0, LV_ANIM_OFF);
    lv_obj_scroll_to_y(s_left, 0, LV_ANIM_OFF);
}

void devos_shortcuts_open(void)
{
    if (devos_shortcuts_is_open()) return;
    devos_cmdpal_close();
    devos_hud_close();
    build();
    fill_app();
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
}

void devos_shortcuts_close(void)
{
    if (devos_shortcuts_is_open()) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ keys */
static bool shortcuts_key(uint32_t key, uint8_t mods)
{
    bool toggle = (mods & DEVOS_MOD_FN) && (key == DEVOS_SHORTCUTS_KEY || key == DEVOS_SHORTCUTS_KEY - 'a' + 'A');
    if (!devos_shortcuts_is_open()) {
        if (!toggle) return false;
        devos_shortcuts_open();
        return true;
    }
    if (toggle || key == LV_KEY_ESC || key == '\r' || key == '\n') {
        devos_shortcuts_close();
        return true;
    }
    int page = SC_COL_H - 60;
    if ((mods & DEVOS_MOD_FN) && (key == LV_KEY_UP || key == LV_KEY_DOWN)) {
        key = key == LV_KEY_UP ? DEVOS_KEY_PGUP : DEVOS_KEY_PGDN;
    } else if (mods & (DEVOS_MOD_FN | DEVOS_MOD_CTRL | DEVOS_MOD_ALT)) {
        devos_shortcuts_close();            /* another shortcut: go ahead with it */
        return false;
    }
    /* Up / Down scroll the app's column (the longer one), Left / Right the other */
    switch (key) {
    case LV_KEY_UP: lv_obj_scroll_by_bounded(s_right, 0, 60, LV_ANIM_OFF); break;
    case LV_KEY_DOWN: lv_obj_scroll_by_bounded(s_right, 0, -60, LV_ANIM_OFF); break;
    case DEVOS_KEY_PGUP: lv_obj_scroll_by_bounded(s_right, 0, page, LV_ANIM_OFF); break;
    case DEVOS_KEY_PGDN: lv_obj_scroll_by_bounded(s_right, 0, -page, LV_ANIM_OFF); break;
    case LV_KEY_LEFT: lv_obj_scroll_by_bounded(s_left, 0, 60, LV_ANIM_OFF); break;
    case LV_KEY_RIGHT: lv_obj_scroll_by_bounded(s_left, 0, -60, LV_ANIM_OFF); break;
    default: break;
    }
    return true;                            /* nothing gets typed into the app unseen */
}

void devos_shortcuts_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    devos_core_add_key_hook(shortcuts_key);
}
