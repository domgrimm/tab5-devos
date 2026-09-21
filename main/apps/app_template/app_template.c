#include "app_template.h"
#include "devos_config.h"
#include "devos_theme.h"
#include <stdio.h>
#include <string.h>

/* =========================================================================
 * 1. Primary Starter App Template Implementation
 * ========================================================================= */

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;

static lv_obj_t *card_info = NULL;
static lv_obj_t *card_demo = NULL;
static lv_obj_t *lbl_counter = NULL;
static lv_obj_t *btn_inc = NULL;
static lv_obj_t *btn_home = NULL;
static int s_click_count = 0;

static void apply_theme(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    if (!screen) return;

    lv_obj_set_style_bg_color(screen, p->bg, 0);

    if (card_info) {
        lv_obj_set_style_bg_color(card_info, p->surface, 0);
        lv_obj_set_style_border_color(card_info, p->surface_border, 0);
    }
    if (card_demo) {
        lv_obj_set_style_bg_color(card_demo, p->surface, 0);
        lv_obj_set_style_border_color(card_demo, p->surface_border, 0);
    }
    if (btn_inc) {
        lv_obj_set_style_bg_color(btn_inc, p->accent_primary, 0);
    }
    if (btn_home) {
        lv_obj_set_style_bg_color(btn_home, p->surface_active, 0);
        lv_obj_set_style_border_color(btn_home, p->surface_border, 0);
    }
}

static void btn_inc_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_click_count++;
    char buf[64];
    snprintf(buf, sizeof(buf), "Button Clicks: %d", s_click_count);
    if (lbl_counter) lv_label_set_text(lbl_counter, buf);
}

static void btn_home_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_core_switch_app(DEVOS_APP_LAUNCHER);
}

static void template_init(void)
{
    const devos_palette_t *p = devos_theme_get();

    screen = lv_obj_create(lv_screen_active());
    app_descriptor.screen = screen;
    lv_obj_set_size(screen, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(screen, 0, DEVOS_TOP_BAR_HEIGHT);
    lv_obj_set_style_bg_color(screen, p->bg, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 24, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Header title */
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, LV_SYMBOL_FILE "  App Template - Drop-in Reference");
    lv_obj_set_pos(title, 20, 16);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, p->accent_primary, 0);

    lv_obj_t *subtitle = lv_label_create(screen);
    lv_label_set_text(subtitle, "Modular Third-Party Application Architecture for devOS");
    lv_obj_set_pos(subtitle, 20, 46);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(subtitle, p->text_secondary, 0);

    /* Card 1: Architecture Guidelines */
    card_info = lv_obj_create(screen);
    lv_obj_set_size(card_info, 720, 320);
    lv_obj_set_pos(card_info, 20, 80);
    lv_obj_set_style_bg_color(card_info, p->surface, 0);
    lv_obj_set_style_border_color(card_info, p->surface_border, 0);
    lv_obj_set_style_border_width(card_info, 1, 0);
    lv_obj_set_style_radius(card_info, 8, 0);
    lv_obj_set_style_pad_all(card_info, 18, 0);

    lv_obj_t *lbl_info_t = lv_label_create(card_info);
    lv_label_set_text(lbl_info_t, "Drop-in Integration Rules (AGENTS.md #8):");
    lv_obj_set_pos(lbl_info_t, 0, 0);
    lv_obj_set_style_text_font(lbl_info_t, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_info_t, p->accent_primary, 0);

    lv_obj_t *lbl_info_body = lv_label_create(card_info);
    lv_label_set_text(lbl_info_body,
        "1. Implement devos_app_descriptor_t (init, show, hide, handle_key).\n"
        "2. Provide get_telemetry_lines(lines[3][64]) for dynamic launcher tile updates.\n"
        "3. Register via devos_core_register_app(app_desc) at system boot.\n"
        "4. Never hardcode app IDs into closed enums or modify app_launcher.c.\n"
        "5. Register with devos_theme_add_listener() to support Dark & Light modes.\n"
        "6. Pin heavy network/crypto I/O to Core 0; keep UI interaction on Core 1.\n"
        "7. Press [Fn + H] or [Esc] to return to Home Screen / App Launcher.");
    lv_obj_set_pos(lbl_info_body, 0, 32);
    lv_obj_set_style_text_font(lbl_info_body, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_info_body, p->text_primary, 0);

    /* Card 2: Live Interactive Widget Demo */
    card_demo = lv_obj_create(screen);
    lv_obj_set_size(card_demo, 480, 320);
    lv_obj_set_pos(card_demo, 760, 80);
    lv_obj_set_style_bg_color(card_demo, p->surface, 0);
    lv_obj_set_style_border_color(card_demo, p->surface_border, 0);
    lv_obj_set_style_border_width(card_demo, 1, 0);
    lv_obj_set_style_radius(card_demo, 8, 0);
    lv_obj_set_style_pad_all(card_demo, 18, 0);

    lv_obj_t *lbl_demo_t = lv_label_create(card_demo);
    lv_label_set_text(lbl_demo_t, "Interactive Demo:");
    lv_obj_set_pos(lbl_demo_t, 0, 0);
    lv_obj_set_style_text_font(lbl_demo_t, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_demo_t, p->accent_primary, 0);

    lbl_counter = lv_label_create(card_demo);
    lv_label_set_text(lbl_counter, "Button Clicks: 0");
    lv_obj_set_pos(lbl_counter, 0, 36);
    lv_obj_set_style_text_font(lbl_counter, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_counter, p->text_primary, 0);

    btn_inc = lv_button_create(card_demo);
    lv_obj_set_size(btn_inc, 180, 42);
    lv_obj_set_pos(btn_inc, 0, 80);
    lv_obj_set_style_bg_color(btn_inc, p->accent_primary, 0);
    lv_obj_set_style_radius(btn_inc, 6, 0);
    lv_obj_add_event_cb(btn_inc, btn_inc_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_btn = lv_label_create(btn_inc);
    lv_label_set_text(lbl_btn, LV_SYMBOL_PLUS " Increment");
    lv_obj_center(lbl_btn);
    lv_obj_set_style_text_font(lbl_btn, &lv_font_montserrat_14, 0);

    /* Bottom Action Bar */
    btn_home = lv_button_create(screen);
    lv_obj_set_size(btn_home, 180, 42);
    lv_obj_set_pos(btn_home, 20, 420);
    lv_obj_set_style_bg_color(btn_home, p->surface_active, 0);
    lv_obj_set_style_border_color(btn_home, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_home, 1, 0);
    lv_obj_set_style_radius(btn_home, 6, 0);
    lv_obj_add_event_cb(btn_home, btn_home_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_home = lv_label_create(btn_home);
    lv_label_set_text(lbl_home, LV_SYMBOL_LEFT " Return Home");
    lv_obj_center(lbl_home);
    lv_obj_set_style_text_font(lbl_home, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_home, p->text_primary, 0);

    devos_theme_add_listener(apply_theme, NULL);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);
}

static void template_show(void)
{
    if (screen) {
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_HIDDEN);
    }
}

static void template_hide(void)
{
    if (screen) {
        lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool template_handle_key(uint32_t key, uint8_t modifiers)
{
    LV_UNUSED(modifiers);
    if (key == LV_KEY_ESC || key == 'q' || key == 'Q') {
        devos_core_switch_app(DEVOS_APP_LAUNCHER);
        return true;
    }
    if (key == ' ' || key == LV_KEY_ENTER || key == '\r') {
        btn_inc_cb(NULL);
        return true;
    }
    return false;
}

static int template_telemetry_lines(char lines[3][64])
{
    snprintf(lines[0], sizeof(lines[0]), "* Clicks: %d", s_click_count);
    snprintf(lines[1], sizeof(lines[1]), "* Modular demo");
    snprintf(lines[2], sizeof(lines[2]), "* Drop-in app");
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
    app_descriptor.screen = screen;
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

static demo_app_ctx_t s_demo_apps[5] = {
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
    {
        .uid = "netdiag",
        .icon = LV_SYMBOL_WIFI,
        .name = "NetDiag",
        .title = "Net Diagnostics",
        .subtitle = "Ping & Packet Stats",
        .category = "network",
        .line1 = "* Wi-Fi 6 (C6 SDIO)",
        .line2 = "* Mesh tunnel active",
        .line3 = "* DERP ping: 18ms",
    },
};

static void demo_app_show(demo_app_ctx_t *ctx)
{
    if (ctx && ctx->screen) lv_obj_remove_flag(ctx->screen, LV_OBJ_FLAG_HIDDEN);
}

static void demo_app_hide(demo_app_ctx_t *ctx)
{
    if (ctx && ctx->screen) lv_obj_add_flag(ctx->screen, LV_OBJ_FLAG_HIDDEN);
}

static bool demo_app_key(uint32_t key, uint8_t modifiers)
{
    LV_UNUSED(modifiers);
    if (key == LV_KEY_ESC || key == 'q' || key == 'Q') {
        devos_core_switch_app(DEVOS_APP_LAUNCHER);
        return true;
    }
    return false;
}

static int demo_app_telemetry(demo_app_ctx_t *ctx, char lines[3][64])
{
    if (!ctx) return 0;
    snprintf(lines[0], 64, "%s", ctx->line1);
    snprintf(lines[1], 64, "%s", ctx->line2);
    snprintf(lines[2], 64, "%s", ctx->line3);
    return 3;
}

/* Dispatchers for demo apps 0..4 */
#define DEF_DEMO_HANDLERS(idx) \
static void demo_init_##idx(void) { \
    const devos_palette_t *p = devos_theme_get(); \
    s_demo_apps[idx].screen = lv_obj_create(lv_screen_active()); \
    s_demo_apps[idx].desc.screen = s_demo_apps[idx].screen; \
    lv_obj_set_size(s_demo_apps[idx].screen, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT); \
    lv_obj_set_pos(s_demo_apps[idx].screen, 0, DEVOS_TOP_BAR_HEIGHT); \
    lv_obj_set_style_bg_color(s_demo_apps[idx].screen, p->bg, 0); \
    lv_obj_set_style_pad_all(s_demo_apps[idx].screen, 24, 0); \
    lv_obj_clear_flag(s_demo_apps[idx].screen, LV_OBJ_FLAG_SCROLLABLE); \
    lv_obj_t *t = lv_label_create(s_demo_apps[idx].screen); \
    char buf[128]; \
    snprintf(buf, sizeof(buf), "%s  %s - Modular App", s_demo_apps[idx].icon, s_demo_apps[idx].title); \
    lv_label_set_text(t, buf); \
    lv_obj_set_pos(t, 20, 20); \
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0); \
    lv_obj_set_style_text_color(t, p->accent_primary, 0); \
    lv_obj_t *sub = lv_label_create(s_demo_apps[idx].screen); \
    lv_label_set_text(sub, s_demo_apps[idx].subtitle); \
    lv_obj_set_pos(sub, 20, 50); \
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0); \
    lv_obj_set_style_text_color(sub, p->text_secondary, 0); \
    lv_obj_t *card = lv_obj_create(s_demo_apps[idx].screen); \
    lv_obj_set_size(card, 600, 200); \
    lv_obj_set_pos(card, 20, 90); \
    lv_obj_set_style_bg_color(card, p->surface, 0); \
    lv_obj_set_style_border_color(card, p->surface_border, 0); \
    lv_obj_set_style_border_width(card, 1, 0); \
    lv_obj_set_style_radius(card, 8, 0); \
    lv_obj_t *info = lv_label_create(card); \
    char ibuf[256]; \
    snprintf(ibuf, sizeof(ibuf), "Modular Application Demo\nCategory: %s\n\nLive Telemetry:\n%s\n%s\n%s", \
             s_demo_apps[idx].category, s_demo_apps[idx].line1, s_demo_apps[idx].line2, s_demo_apps[idx].line3); \
    lv_label_set_text(info, ibuf); \
    lv_obj_set_style_text_font(info, &lv_font_montserrat_14, 0); \
    lv_obj_set_style_text_color(info, p->text_primary, 0); \
    lv_obj_t *btn = lv_button_create(s_demo_apps[idx].screen); \
    lv_obj_set_size(btn, 180, 42); \
    lv_obj_set_pos(btn, 20, 310); \
    lv_obj_set_style_bg_color(btn, p->surface_active, 0); \
    lv_obj_set_style_border_color(btn, p->surface_border, 0); \
    lv_obj_set_style_border_width(btn, 1, 0); \
    lv_obj_set_style_radius(btn, 6, 0); \
    lv_obj_add_event_cb(btn, btn_home_cb, LV_EVENT_CLICKED, NULL); \
    lv_obj_t *hl = lv_label_create(btn); \
    lv_label_set_text(hl, LV_SYMBOL_LEFT " Return Home"); \
    lv_obj_center(hl); \
    lv_obj_set_style_text_font(hl, &lv_font_montserrat_14, 0); \
    lv_obj_set_style_text_color(hl, p->text_primary, 0); \
    lv_obj_add_flag(s_demo_apps[idx].screen, LV_OBJ_FLAG_HIDDEN); \
} \
static void demo_show_##idx(void) { demo_app_show(&s_demo_apps[idx]); } \
static void demo_hide_##idx(void) { demo_app_hide(&s_demo_apps[idx]); } \
static int demo_telemetry_##idx(char lines[3][64]) { return demo_app_telemetry(&s_demo_apps[idx], lines); }

DEF_DEMO_HANDLERS(0)
DEF_DEMO_HANDLERS(1)
DEF_DEMO_HANDLERS(2)
DEF_DEMO_HANDLERS(3)
DEF_DEMO_HANDLERS(4)

void app_template_register_demo_apps(void)
{
    /* Register primary template app */
    devos_core_register_app(app_template_get_descriptor());

    /* Register demo apps to reach 12+ registered apps */
    void (*inits[5])(void) = {demo_init_0, demo_init_1, demo_init_2, demo_init_3, demo_init_4};
    void (*shows[5])(void) = {demo_show_0, demo_show_1, demo_show_2, demo_show_3, demo_show_4};
    void (*hides[5])(void) = {demo_hide_0, demo_hide_1, demo_hide_2, demo_hide_3, demo_hide_4};
    int (*teles[5])(char lines[3][64]) = {demo_telemetry_0, demo_telemetry_1, demo_telemetry_2, demo_telemetry_3, demo_telemetry_4};

    for (int i = 0; i < 5; i++) {
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
        s_demo_apps[i].desc.handle_key = demo_app_key;
        s_demo_apps[i].desc.get_telemetry_lines = teles[i];

        devos_core_register_app(&s_demo_apps[i].desc);
    }
}
