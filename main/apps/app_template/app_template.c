/* App Template: the canonical drop-in app. Copy it to start a new one.
 *
 * It shows the rules every app follows:
 *   - AGENTS.md #8: implement devos_app_descriptor_t (init / show / hide /
 *     handle_key / get_telemetry_lines) and self-register; no launcher edits.
 *   - AGENTS.md #5: colours come from lv_style_t objects recoloured by a
 *     devos_theme listener, so Sym+T restyles the screen instantly.
 *   - AGENTS.md #9, keyboard first: every control is registered with
 *     devos_focus (focus ring; Tab / Up / Down move, Enter / Space press or
 *     toggle, Left / Right change a value, printable keys type into a text
 *     field), handle_key() hands keys to devos_focus_key() first, frequent
 *     actions get a letter, a hint line lists the keys, and Esc is left
 *     unhandled (after leaving a text field) so devos_core returns Home.
 *     The on-screen keyboard only appears when no hardware keyboard is
 *     attached. Touch keeps working for everything.
 */
#include "app_template.h"
#include "devos_config.h"
#include "devos_theme.h"
#include "devos_focus.h"
#include "tab5_keyboard.h"
#include <stdio.h>
#include <string.h>

/* =========================================================================
 * Styles (shared by the template and the demo apps)
 * ========================================================================= */

static bool s_styles_ready = false;
static lv_style_t st_screen, st_card, st_title, st_heading, st_text, st_muted, st_hint, st_btn, st_btn_primary,
                  st_ta, st_ta_focus, st_track, st_indicator, st_knob;
static lv_style_t *const s_styles[] = { &st_screen, &st_card, &st_title, &st_heading, &st_text, &st_muted,
                                        &st_hint, &st_btn, &st_btn_primary, &st_ta, &st_ta_focus, &st_track,
                                        &st_indicator, &st_knob };
#define N_STYLES (sizeof(s_styles) / sizeof(s_styles[0]))

static void restyle(const devos_palette_t *p)
{
    lv_style_set_bg_color(&st_screen, p->bg);
    lv_style_set_bg_color(&st_card, p->surface);
    lv_style_set_border_color(&st_card, p->surface_border);
    lv_style_set_text_color(&st_title, p->accent_primary);
    lv_style_set_text_color(&st_heading, p->accent_primary);
    lv_style_set_text_color(&st_text, p->text_primary);
    lv_style_set_text_color(&st_muted, p->text_secondary);
    lv_style_set_text_color(&st_hint, p->text_muted);
    lv_style_set_bg_color(&st_btn, p->surface_active);
    lv_style_set_border_color(&st_btn, p->surface_border);
    lv_style_set_text_color(&st_btn, p->text_primary);
    lv_style_set_bg_color(&st_btn_primary, p->accent_primary);
    lv_style_set_text_color(&st_btn_primary, p->bg);
    lv_style_set_bg_color(&st_ta, p->bg_alt);
    lv_style_set_border_color(&st_ta, p->surface_border);
    lv_style_set_text_color(&st_ta, p->text_primary);
    lv_style_set_border_color(&st_ta_focus, p->accent_primary);
    lv_style_set_bg_color(&st_track, p->surface_active);
    lv_style_set_bg_color(&st_indicator, p->accent_primary);
    lv_style_set_bg_color(&st_knob, p->text_primary);
}

static void theme_cb(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    restyle(p);
    for (size_t i = 0; i < N_STYLES; i++) lv_obj_report_style_change(s_styles[i]);
}

static void styles_init(void)
{
    if (s_styles_ready) return;
    s_styles_ready = true;
    for (size_t i = 0; i < N_STYLES; i++) lv_style_init(s_styles[i]);
    lv_style_set_bg_opa(&st_screen, LV_OPA_COVER);
    lv_style_set_radius(&st_screen, 0);
    lv_style_set_border_width(&st_screen, 0);
    lv_style_set_pad_all(&st_screen, 24);
    lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
    lv_style_set_border_width(&st_card, 1);
    lv_style_set_radius(&st_card, 8);
    lv_style_set_pad_all(&st_card, 18);             /* room for the focus ring */
    lv_style_set_text_font(&st_title, &lv_font_montserrat_20);
    lv_style_set_text_font(&st_heading, &lv_font_montserrat_16);
    lv_style_set_text_font(&st_text, &lv_font_montserrat_14);
    lv_style_set_text_font(&st_muted, &lv_font_montserrat_14);
    lv_style_set_text_font(&st_hint, &lv_font_montserrat_12);
    lv_style_set_bg_opa(&st_btn, LV_OPA_COVER);
    lv_style_set_border_width(&st_btn, 1);
    lv_style_set_radius(&st_btn, 6);
    lv_style_set_shadow_width(&st_btn, 0);
    lv_style_set_text_font(&st_btn, &lv_font_montserrat_14);
    lv_style_set_border_width(&st_btn_primary, 0);
    lv_style_set_bg_opa(&st_ta, LV_OPA_COVER);
    lv_style_set_border_width(&st_ta, 1);
    lv_style_set_radius(&st_ta, 6);
    lv_style_set_pad_all(&st_ta, 8);
    lv_style_set_text_font(&st_ta, &lv_font_montserrat_16);
    lv_style_set_border_width(&st_ta_focus, 2);
    lv_style_set_bg_opa(&st_track, LV_OPA_COVER);
    lv_style_set_bg_opa(&st_indicator, LV_OPA_COVER);
    lv_style_set_bg_opa(&st_knob, LV_OPA_COVER);
    restyle(devos_theme_get());
    devos_theme_add_listener(theme_cb, NULL);
}

static lv_obj_t *mk_screen(void)
{
    lv_obj_t *scr = lv_obj_create(lv_screen_active());
    lv_obj_add_style(scr, &st_screen, 0);
    lv_obj_set_size(scr, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(scr, 0, DEVOS_TOP_BAR_HEIGHT);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

static lv_obj_t *mk_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_add_style(c, &st_card, 0);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *mk_label(lv_obj_t *parent, lv_style_t *st, const char *txt, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_add_style(l, st, 0);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *txt, lv_style_t *extra, int x, int y, int w,
                        lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_add_style(b, &st_btn, 0);
    if (extra) lv_obj_add_style(b, extra, 0);
    lv_obj_set_size(b, w, 40);
    lv_obj_set_pos(b, x, y);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void home_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_core_switch_app(DEVOS_APP_LAUNCHER);
}

/* =========================================================================
 * 1. Primary Starter App Template Implementation
 * ========================================================================= */

/* Widgets live in the app's context struct (AGENTS.md 5.3). */
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *lbl_counter;
    lv_obj_t *lbl_step;
    lv_obj_t *btn_count;
    lv_obj_t *btn_reset;
    lv_obj_t *sw_down;
    lv_obj_t *slider_step;
    lv_obj_t *ta_name;
    lv_obj_t *btn_home;
    lv_obj_t *kb;               /* on-screen keyboard: only without a hardware keyboard */
    devos_focus_t focus;        /* keyboard focus order of the controls above */
    int count;
    char name[24];
} template_ctx_t;

static devos_app_descriptor_t app_descriptor;
static template_ctx_t s_tpl;

static void update_counter(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s: %d", s_tpl.name[0] ? s_tpl.name : "Count", s_tpl.count);
    lv_label_set_text(s_tpl.lbl_counter, buf);
}

static void do_count(void)
{
    int step = (int)lv_slider_get_value(s_tpl.slider_step);
    s_tpl.count += lv_obj_has_state(s_tpl.sw_down, LV_STATE_CHECKED) ? -step : step;
    update_counter();
}

static void do_reset(void)
{
    s_tpl.count = 0;
    update_counter();
}

static void apply_name(void)
{
    snprintf(s_tpl.name, sizeof(s_tpl.name), "%s", lv_textarea_get_text(s_tpl.ta_name));
    update_counter();
}

static void count_cb(lv_event_t *e) { LV_UNUSED(e); do_count(); }
static void reset_cb(lv_event_t *e) { LV_UNUSED(e); do_reset(); }

static void step_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    char buf[32];
    snprintf(buf, sizeof(buf), "Step: %d", (int)lv_slider_get_value(s_tpl.slider_step));
    lv_label_set_text(s_tpl.lbl_step, buf);
}

/* Tap on the text field: on-screen keyboard only if no real one is attached. */
static void name_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!tab5_keyboard_is_connected()) lv_obj_remove_flag(s_tpl.kb, LV_OBJ_FLAG_HIDDEN);
}

static void kb_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) apply_name();
    lv_obj_add_flag(s_tpl.kb, LV_OBJ_FLAG_HIDDEN);          /* READY or CANCEL */
}

static void template_init(void)
{
    styles_init();
    lv_obj_t *scr = s_tpl.screen = app_descriptor.screen = mk_screen();

    mk_label(scr, &st_title, LV_SYMBOL_FILE "  App Template - Drop-in Reference", 20, 16);
    mk_label(scr, &st_muted, "Modular Third-Party Application Architecture for devOS", 20, 46);

    /* Card 1: the rules */
    lv_obj_t *card = mk_card(scr, 20, 80, 700, 360);
    mk_label(card, &st_heading, "Drop-in Integration Rules (AGENTS.md #8 and #9):", 0, 0);
    lv_obj_t *body = mk_label(card, &st_text,
        "1. Implement devos_app_descriptor_t (init, show, hide, handle_key).\n"
        "2. Provide get_telemetry_lines(lines[3][64]) for the launcher tile.\n"
        "3. Register via devos_core_register_app() at boot. Never hardcode app IDs "
        "or modify app_launcher.c.\n"
        "4. Colour widgets through styles updated by devos_theme_add_listener() (Dark & Light).\n"
        "5. Keep network / crypto I/O on Core 0; UI interaction on Core 1.\n"
        "6. Keyboard first: register every control with devos_focus and pass keys to "
        "devos_focus_key() from handle_key; give frequent actions a letter; show the keys "
        "in a hint line; leave Esc unhandled so it goes to the Home Screen.\n"
        "7. On-screen keyboards only when tab5_keyboard_is_connected() is false.", 0, 32);
    lv_obj_set_width(body, 660);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);

    /* Card 2: live controls, all reachable from the keyboard */
    card = mk_card(scr, 740, 80, 472, 360);
    mk_label(card, &st_heading, "Interactive Demo (keyboard or touch):", 0, 0);
    s_tpl.lbl_counter = mk_label(card, &st_text, "", 0, 32);
    lv_obj_set_style_text_font(s_tpl.lbl_counter, &lv_font_montserrat_16, 0);

    s_tpl.btn_count = mk_btn(card, LV_SYMBOL_PLUS " Count  [C]", &st_btn_primary, 0, 66, 160, count_cb);
    s_tpl.btn_reset = mk_btn(card, LV_SYMBOL_REFRESH " Reset  [R]", NULL, 176, 66, 140, reset_cb);

    s_tpl.sw_down = lv_switch_create(card);
    lv_obj_set_size(s_tpl.sw_down, 56, 28);
    lv_obj_set_pos(s_tpl.sw_down, 0, 128);
    lv_obj_add_style(s_tpl.sw_down, &st_track, LV_PART_MAIN);
    lv_obj_add_style(s_tpl.sw_down, &st_indicator, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_style(s_tpl.sw_down, &st_knob, LV_PART_KNOB);
    mk_label(card, &st_text, "Count down", 70, 132);

    s_tpl.lbl_step = mk_label(card, &st_text, "Step: 1", 0, 174);
    s_tpl.slider_step = lv_slider_create(card);
    lv_slider_set_range(s_tpl.slider_step, 1, 10);
    lv_slider_set_value(s_tpl.slider_step, 1, LV_ANIM_OFF);
    lv_obj_set_size(s_tpl.slider_step, 300, 12);
    lv_obj_set_pos(s_tpl.slider_step, 8, 204);
    lv_obj_add_style(s_tpl.slider_step, &st_track, LV_PART_MAIN);
    lv_obj_add_style(s_tpl.slider_step, &st_indicator, LV_PART_INDICATOR);
    lv_obj_add_style(s_tpl.slider_step, &st_knob, LV_PART_KNOB);
    lv_obj_add_event_cb(s_tpl.slider_step, step_cb, LV_EVENT_VALUE_CHANGED, NULL);

    mk_label(card, &st_muted, "Name (Enter applies it):", 0, 236);
    s_tpl.ta_name = lv_textarea_create(card);
    lv_obj_add_style(s_tpl.ta_name, &st_ta, 0);
    lv_obj_add_style(s_tpl.ta_name, &st_ta_focus, LV_STATE_FOCUSED);
    lv_textarea_set_one_line(s_tpl.ta_name, true);
    lv_textarea_set_placeholder_text(s_tpl.ta_name, "Your name");
    lv_textarea_set_max_length(s_tpl.ta_name, sizeof(s_tpl.name) - 1);
    lv_obj_set_width(s_tpl.ta_name, 300);
    lv_obj_set_pos(s_tpl.ta_name, 0, 260);
    lv_obj_add_event_cb(s_tpl.ta_name, name_click_cb, LV_EVENT_CLICKED, NULL);

    s_tpl.btn_home = mk_btn(scr, LV_SYMBOL_LEFT " Return Home", NULL, 20, 460, 180, home_cb);

    /* The screen's keys, always visible */
    lv_obj_t *hint = mk_label(scr, &st_hint,
        "Tab / Up / Down move  |  Enter / Space press or toggle  |  Left / Right change the value  |  "
        "type into the name field, Enter applies  |  C count  |  R reset  |  Esc leave the field, then Home",
        20, 520);
    lv_obj_set_width(hint, 1190);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    s_tpl.kb = lv_keyboard_create(scr);
    lv_obj_add_style(s_tpl.kb, &st_card, 0);
    lv_obj_add_style(s_tpl.kb, &st_btn, LV_PART_ITEMS);
    lv_obj_set_size(s_tpl.kb, DEVOS_SCREEN_WIDTH - 48, 200);
    lv_obj_align(s_tpl.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_tpl.kb, s_tpl.ta_name);
    lv_obj_add_event_cb(s_tpl.kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_tpl.kb, kb_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(s_tpl.kb, LV_OBJ_FLAG_HIDDEN);

    /* Keyboard focus order = reading order. devos_focus draws the ring and
     * lets taps move the focus too. */
    devos_focus_init(&s_tpl.focus);
    devos_focus_add(&s_tpl.focus, s_tpl.btn_count);
    devos_focus_add(&s_tpl.focus, s_tpl.btn_reset);
    devos_focus_add(&s_tpl.focus, s_tpl.sw_down);
    devos_focus_add(&s_tpl.focus, s_tpl.slider_step);
    devos_focus_add(&s_tpl.focus, s_tpl.ta_name);
    devos_focus_add(&s_tpl.focus, s_tpl.btn_home);

    update_counter();
    lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN);
}

static void template_show(void)
{
    if (!s_tpl.screen) return;
    lv_obj_remove_flag(s_tpl.screen, LV_OBJ_FLAG_HIDDEN);
    devos_focus_first(&s_tpl.focus);    /* highlight what Enter will press (or restore the last focus) */
}

static void template_hide(void)
{
    if (!s_tpl.screen) return;
    lv_obj_add_flag(s_tpl.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_tpl.screen, LV_OBJ_FLAG_HIDDEN);
}

static bool template_handle_key(uint32_t key, uint8_t modifiers)
{
    /* 1. The focused control first: Tab / Up / Down move, Enter / Space press or
     *    toggle, Left / Right change a value, printable keys type into a field. */
    if (devos_focus_key(&s_tpl.focus, key, modifiers)) return true;

    lv_obj_t *cur = devos_focus_get(&s_tpl.focus);
    bool in_field = cur && lv_obj_check_type(cur, &lv_textarea_class);

    /* 2. What devos_focus hands back: Enter in a one-line field = submit ... */
    if (in_field && (key == '\r' || key == '\n')) {
        apply_name();
        return true;
    }
    /* ... and Esc: leave the field first; otherwise unhandled, so devos_core
     * goes to the Home Screen. */
    if (key == LV_KEY_ESC) {
        if (in_field) {
            devos_focus_clear(&s_tpl.focus);
            return true;
        }
        return false;
    }

    /* 3. Letter shortcuts for frequent actions (shown in the hint line and on
     *    the buttons). Sym + <key> is reserved for the system. */
    if (modifiers & (DEVOS_MOD_CTRL | DEVOS_MOD_FN | DEVOS_MOD_ALT)) return false;
    switch (key) {
    case 'c': case 'C': do_count(); return true;
    case 'r': case 'R': do_reset(); return true;
    default: return false;
    }
}

static int template_telemetry_lines(char lines[3][64])
{
    snprintf(lines[0], sizeof(lines[0]), "* Count: %d", s_tpl.count);
    snprintf(lines[1], sizeof(lines[1]), "* Modular demo");
    snprintf(lines[2], sizeof(lines[2]), "* Keyboard-first reference");
    return 3;
}

devos_app_descriptor_t *app_template_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_COUNT; /* Auto-assigned if collision */
    app_descriptor.uid = "template";
    app_descriptor.icon = LV_SYMBOL_FILE;
    app_descriptor.category = "tools";
    app_descriptor.name = "Template";
    app_descriptor.title = "App Template";
    app_descriptor.subtitle = "Modular Drop-In Starter";
    app_descriptor.screen = s_tpl.screen;
    app_descriptor.init = template_init;
    app_descriptor.show = template_show;
    app_descriptor.hide = template_hide;
    app_descriptor.handle_key = template_handle_key;
    app_descriptor.get_telemetry_lines = template_telemetry_lines;

    return &app_descriptor;
}

/* =========================================================================
 * 2. Modular Demo Apps for Multi-App Scalability Verification (12+ Apps)
 * ========================================================================= */

typedef struct {
    devos_app_descriptor_t desc;
    lv_obj_t *screen;
    lv_obj_t *btn_home;
    devos_focus_t focus;
    const char *uid;
    const char *icon;
    const char *name;
    const char *title;
    const char *subtitle;
    const char *category;
    const char *line1;
    const char *line2;
    const char *line3;
} demo_app_ctx_t;

static demo_app_ctx_t s_demo_apps[4] = {
    {
        .uid = "sysmon",
        .icon = LV_SYMBOL_CHARGE,
        .name = "SysMon",
        .title = "System Monitor",
        .subtitle = "Hardware Telemetry",
        .category = "system",
        .line1 = "* Dual-core 400MHz",
        .line2 = "* 32MB PSRAM free",
        .line3 = "* FreeRTOS pinned",
    },
    {
        .uid = "files",
        .icon = LV_SYMBOL_DIRECTORY,
        .name = "Files",
        .title = "File Explorer",
        .subtitle = "MicroSD Storage",
        .category = "storage",
        .line1 = "* /sdcard mounted",
        .line2 = "* 4-bit SDMMC",
        .line3 = "* FATFS VFS driver",
    },
    {
        .uid = "calc",
        .icon = LV_SYMBOL_EDIT,
        .name = "Calc",
        .title = "Hex Calculator",
        .subtitle = "Programmer Utils",
        .category = "tools",
        .line1 = "* Mode: 64-bit Hex",
        .line2 = "* Bitwise AND/OR/XOR",
        .line3 = "* Stack depth: 8",
    },
    {
        .uid = "tasks",
        .icon = LV_SYMBOL_LIST,
        .name = "Tasks",
        .title = "Task Manager",
        .subtitle = "FreeRTOS Tasks",
        .category = "system",
        .line1 = "* 14 Tasks running",
        .line2 = "* Core 0: Net/Crypto",
        .line3 = "* Core 1: LVGL/UI",
    },
};

static void demo_app_init(demo_app_ctx_t *ctx)
{
    styles_init();
    lv_obj_t *scr = ctx->screen = ctx->desc.screen = mk_screen();

    char buf[256];
    snprintf(buf, sizeof(buf), "%s  %s - Modular App", ctx->icon, ctx->title);
    mk_label(scr, &st_title, buf, 20, 20);
    mk_label(scr, &st_muted, ctx->subtitle, 20, 50);

    lv_obj_t *card = mk_card(scr, 20, 90, 600, 200);
    snprintf(buf, sizeof(buf), "Modular Application Demo\nCategory: %s\n\nLive Telemetry:\n%s\n%s\n%s",
             ctx->category, ctx->line1, ctx->line2, ctx->line3);
    mk_label(card, &st_text, buf, 0, 0);

    ctx->btn_home = mk_btn(scr, LV_SYMBOL_LEFT " Return Home", NULL, 20, 310, 180, home_cb);
    mk_label(scr, &st_hint, "Enter / Space press the focused button  |  Esc returns to the Home Screen", 20, 366);

    devos_focus_init(&ctx->focus);
    devos_focus_add(&ctx->focus, ctx->btn_home);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN);
}

static void demo_app_show(demo_app_ctx_t *ctx)
{
    if (!ctx->screen) return;
    lv_obj_remove_flag(ctx->screen, LV_OBJ_FLAG_HIDDEN);
    devos_focus_first(&ctx->focus);
}

static void demo_app_hide(demo_app_ctx_t *ctx)
{
    if (ctx->screen) lv_obj_add_flag(ctx->screen, LV_OBJ_FLAG_HIDDEN);
}

/* Enter / Space press the focused button; Esc is left to devos_core (Home). */
static bool demo_app_key(demo_app_ctx_t *ctx, uint32_t key, uint8_t modifiers)
{
    return devos_focus_key(&ctx->focus, key, modifiers);
}

static int demo_app_telemetry(demo_app_ctx_t *ctx, char lines[3][64])
{
    snprintf(lines[0], 64, "%s", ctx->line1);
    snprintf(lines[1], 64, "%s", ctx->line2);
    snprintf(lines[2], 64, "%s", ctx->line3);
    return 3;
}

/* Dispatchers for demo apps 0..3 (the descriptor callbacks take no context) */
#define DEF_DEMO_HANDLERS(idx) \
static void demo_init_##idx(void) { demo_app_init(&s_demo_apps[idx]); } \
static void demo_show_##idx(void) { demo_app_show(&s_demo_apps[idx]); } \
static void demo_hide_##idx(void) { demo_app_hide(&s_demo_apps[idx]); } \
static bool demo_key_##idx(uint32_t k, uint8_t m) { return demo_app_key(&s_demo_apps[idx], k, m); } \
static int demo_telemetry_##idx(char lines[3][64]) { return demo_app_telemetry(&s_demo_apps[idx], lines); }

DEF_DEMO_HANDLERS(0)
DEF_DEMO_HANDLERS(1)
DEF_DEMO_HANDLERS(2)
DEF_DEMO_HANDLERS(3)

void app_template_register_demo_apps(void)
{
    /* Register primary template app */
    devos_core_register_app(app_template_get_descriptor());

    /* Register demo apps to reach 12+ registered apps */
    void (*inits[4])(void) = {demo_init_0, demo_init_1, demo_init_2, demo_init_3};
    void (*shows[4])(void) = {demo_show_0, demo_show_1, demo_show_2, demo_show_3};
    void (*hides[4])(void) = {demo_hide_0, demo_hide_1, demo_hide_2, demo_hide_3};
    bool (*keys[4])(uint32_t, uint8_t) = {demo_key_0, demo_key_1, demo_key_2, demo_key_3};
    int (*teles[4])(char lines[3][64]) = {demo_telemetry_0, demo_telemetry_1, demo_telemetry_2, demo_telemetry_3};

    for (int i = 0; i < 4; i++) {
        s_demo_apps[i].desc.id = (devos_app_id_t)(20 + i);
        s_demo_apps[i].desc.uid = s_demo_apps[i].uid;
        s_demo_apps[i].desc.icon = s_demo_apps[i].icon;
        s_demo_apps[i].desc.category = s_demo_apps[i].category;
        s_demo_apps[i].desc.name = s_demo_apps[i].name;
        s_demo_apps[i].desc.title = s_demo_apps[i].title;
        s_demo_apps[i].desc.subtitle = s_demo_apps[i].subtitle;
        s_demo_apps[i].desc.init = inits[i];
        s_demo_apps[i].desc.show = shows[i];
        s_demo_apps[i].desc.hide = hides[i];
        s_demo_apps[i].desc.handle_key = keys[i];
        s_demo_apps[i].desc.get_telemetry_lines = teles[i];

        devos_core_register_app(&s_demo_apps[i].desc);
    }
}
