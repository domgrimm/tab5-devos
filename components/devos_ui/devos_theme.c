#ifdef ESP_PLATFORM
#include "nvs.h"
#define THEME_NVS_NS "devos"
#endif
#include "devos_theme.h"
#include "devos_core.h"
#include <stdio.h>
#include <string.h>

#define MAX_THEME_LISTENERS 32

typedef struct {
    devos_theme_change_cb_t cb;
    void *user_data;
    bool active;
} theme_listener_t;

static devos_theme_type_t current_theme = DEVOS_THEME_DARK;
static theme_listener_t listeners[MAX_THEME_LISTENERS] = {0};

/* --------------------------------------------------------------------------
 * 1. Dark Cyberdeck Palette
 * -------------------------------------------------------------------------- */
static const devos_palette_t palette_dark = {
    .bg               = LV_COLOR_MAKE(0x12, 0x14, 0x17),
    .bg_alt           = LV_COLOR_MAKE(0x16, 0x19, 0x20),
    .surface          = LV_COLOR_MAKE(0x1A, 0x1D, 0x24),
    .surface_active   = LV_COLOR_MAKE(0x24, 0x29, 0x33),
    .surface_border   = LV_COLOR_MAKE(0x2E, 0x38, 0x4D),
    .border_highlight = LV_COLOR_MAKE(0x00, 0xE5, 0xFF),

    .text_primary     = LV_COLOR_MAKE(0xEC, 0xEF, 0xF4),
    .text_secondary   = LV_COLOR_MAKE(0x9A, 0xA5, 0xB8),
    .text_muted       = LV_COLOR_MAKE(0x5A, 0x65, 0x78),

    .accent_primary   = LV_COLOR_MAKE(0x00, 0xE5, 0xFF),   /* Neon Cyan */
    .accent_secondary = LV_COLOR_MAKE(0x00, 0xE6, 0x76),   /* Emerald Green */
    .accent_warning   = LV_COLOR_MAKE(0xFF, 0xB3, 0x00),   /* Amber */
    .accent_danger    = LV_COLOR_MAKE(0xFF, 0x52, 0x52),   /* Coral Red */

    .top_bar_bg       = LV_COLOR_MAKE(0x0E, 0x11, 0x16),
    .bottom_bar_bg    = LV_COLOR_MAKE(0x0E, 0x11, 0x16),
    .telemetry_bg     = LV_COLOR_MAKE(0x16, 0x19, 0x22),
    .code_bg          = LV_COLOR_MAKE(0x0B, 0x0D, 0x11),

    .ansi = {
        LV_COLOR_MAKE(0x1E, 0x1E, 0x1E), /* Black */
        LV_COLOR_MAKE(0xF4, 0x43, 0x36), /* Red */
        LV_COLOR_MAKE(0x4C, 0xAF, 0x50), /* Green */
        LV_COLOR_MAKE(0xFF, 0xEB, 0x3B), /* Yellow */
        LV_COLOR_MAKE(0x21, 0x96, 0xF3), /* Blue */
        LV_COLOR_MAKE(0x9C, 0x27, 0xB0), /* Magenta */
        LV_COLOR_MAKE(0x00, 0xBC, 0xD4), /* Cyan */
        LV_COLOR_MAKE(0xEE, 0xEE, 0xEE), /* White */
        LV_COLOR_MAKE(0x75, 0x75, 0x75), /* Bright Black */
        LV_COLOR_MAKE(0xEF, 0x53, 0x50), /* Bright Red */
        LV_COLOR_MAKE(0x66, 0xBB, 0x6A), /* Bright Green */
        LV_COLOR_MAKE(0xFF, 0xEE, 0x58), /* Bright Yellow */
        LV_COLOR_MAKE(0x42, 0xA5, 0xF5), /* Bright Blue */
        LV_COLOR_MAKE(0xAB, 0x47, 0xBC), /* Bright Magenta */
        LV_COLOR_MAKE(0x26, 0xC6, 0xDA), /* Bright Cyan */
        LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), /* Bright White */
    }
};

/* --------------------------------------------------------------------------
 * 2. High-Contrast Light Palette (Outdoor / Daylight)
 * -------------------------------------------------------------------------- */
static const devos_palette_t palette_light = {
    .bg               = LV_COLOR_MAKE(0xF8, 0xFA, 0xFC),
    .bg_alt           = LV_COLOR_MAKE(0xED, 0xF2, 0xF7),
    .surface          = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
    .surface_active   = LV_COLOR_MAKE(0xF1, 0xF5, 0xF9),
    .surface_border   = LV_COLOR_MAKE(0xCB, 0xD5, 0xE1),
    .border_highlight = LV_COLOR_MAKE(0x25, 0x63, 0xEB),

    .text_primary     = LV_COLOR_MAKE(0x0F, 0x17, 0x2A),
    .text_secondary   = LV_COLOR_MAKE(0x47, 0x55, 0x69),
    .text_muted       = LV_COLOR_MAKE(0x94, 0xA3, 0xB8),

    .accent_primary   = LV_COLOR_MAKE(0x25, 0x63, 0xEB),   /* Cobalt Blue */
    .accent_secondary = LV_COLOR_MAKE(0x0D, 0x94, 0x88),   /* Deep Teal */
    .accent_warning   = LV_COLOR_MAKE(0xD9, 0x77, 0x06),   /* Deep Amber */
    .accent_danger    = LV_COLOR_MAKE(0xDC, 0x26, 0x26),   /* Crimson Red */

    .top_bar_bg       = LV_COLOR_MAKE(0xE2, 0xE8, 0xF0),
    .bottom_bar_bg    = LV_COLOR_MAKE(0xE2, 0xE8, 0xF0),
    .telemetry_bg     = LV_COLOR_MAKE(0xEA, 0xEE, 0xF4),
    .code_bg          = LV_COLOR_MAKE(0xEA, 0xEE, 0xF3),

    .ansi = {
        LV_COLOR_MAKE(0x00, 0x00, 0x00), /* Black */
        LV_COLOR_MAKE(0xC6, 0x28, 0x28), /* Red */
        LV_COLOR_MAKE(0x2E, 0x7D, 0x32), /* Green */
        LV_COLOR_MAKE(0xF5, 0x7F, 0x17), /* Yellow */
        LV_COLOR_MAKE(0x15, 0x65, 0xC0), /* Blue */
        LV_COLOR_MAKE(0x6A, 0x1B, 0x9A), /* Magenta */
        LV_COLOR_MAKE(0x00, 0x83, 0x8F), /* Cyan */
        LV_COLOR_MAKE(0x75, 0x75, 0x75), /* White */
        LV_COLOR_MAKE(0x42, 0x42, 0x42), /* Bright Black */
        LV_COLOR_MAKE(0xD3, 0x2F, 0x2F), /* Bright Red */
        LV_COLOR_MAKE(0x38, 0x8E, 0x3C), /* Bright Green */
        LV_COLOR_MAKE(0xF9, 0xA8, 0x25), /* Bright Yellow */
        LV_COLOR_MAKE(0x19, 0x76, 0xD2), /* Bright Blue */
        LV_COLOR_MAKE(0x7B, 0x1F, 0xA2), /* Bright Magenta */
        LV_COLOR_MAKE(0x00, 0x97, 0xA7), /* Bright Cyan */
        LV_COLOR_MAKE(0x21, 0x21, 0x21), /* Bright White */
    }
};

void devos_theme_init(void)
{
    /* Dark Cyberdeck mode by default; the user's last choice wins on target. */
    current_theme = DEVOS_THEME_DARK;
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open(THEME_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "theme", &v) == ESP_OK && v == DEVOS_THEME_LIGHT) {
            current_theme = DEVOS_THEME_LIGHT;
        }
        nvs_close(h);
    }
#endif
    devos_core_set_theme_toggle_cb(devos_theme_toggle);
}

devos_theme_type_t devos_theme_get_type(void)
{
    return current_theme;
}

const devos_palette_t *devos_theme_get(void)
{
    return (current_theme == DEVOS_THEME_DARK) ? &palette_dark : &palette_light;
}

bool devos_theme_is_dark(void)
{
    return current_theme == DEVOS_THEME_DARK;
}

static void notify_listeners(void)
{
    const devos_palette_t *p = devos_theme_get();
    for (int i = 0; i < MAX_THEME_LISTENERS; i++) {
        if (listeners[i].active && listeners[i].cb) {
            listeners[i].cb(p, listeners[i].user_data);
        }
    }
}

void devos_theme_set(devos_theme_type_t type)
{
    if (current_theme != type) {
        current_theme = type;
        notify_listeners();
#ifdef ESP_PLATFORM
        nvs_handle_t h;
        if (nvs_open(THEME_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "theme", (uint8_t)type);
            nvs_commit(h);
            nvs_close(h);
        }
#endif
    }
}

void devos_theme_toggle(void)
{
    devos_theme_set((current_theme == DEVOS_THEME_DARK) ? DEVOS_THEME_LIGHT : DEVOS_THEME_DARK);
}

int devos_theme_add_listener(devos_theme_change_cb_t cb, void *user_data)
{
    for (int i = 0; i < MAX_THEME_LISTENERS; i++) {
        if (!listeners[i].active) {
            listeners[i].cb = cb;
            listeners[i].user_data = user_data;
            listeners[i].active = true;
            return i;
        }
    }
    return -1;
}

void devos_theme_remove_listener(int listener_id)
{
    if (listener_id >= 0 && listener_id < MAX_THEME_LISTENERS) {
        listeners[listener_id].active = false;
        listeners[listener_id].cb = NULL;
        listeners[listener_id].user_data = NULL;
    }
}
