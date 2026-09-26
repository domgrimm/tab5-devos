#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEVOS_THEME_DARK = 0,   /* Dark Cyberdeck Mode (Default) */
    DEVOS_THEME_LIGHT       /* High-Contrast Light Mode (Daylight/Sunlight) */
} devos_theme_type_t;

typedef struct {
    /* Base canvases */
    lv_color_t bg;
    lv_color_t bg_alt;
    lv_color_t surface;
    lv_color_t surface_active;
    lv_color_t surface_border;
    lv_color_t border_highlight;

    /* Typography */
    lv_color_t text_primary;
    lv_color_t text_secondary;
    lv_color_t text_muted;

    /* Accents & States */
    lv_color_t accent_primary;      /* Cyan / Cobalt */
    lv_color_t accent_secondary;    /* Emerald / Teal */
    lv_color_t accent_warning;      /* Amber */
    lv_color_t accent_danger;       /* Coral / Red */

    /* Specialized Surfaces */
    lv_color_t top_bar_bg;
    lv_color_t bottom_bar_bg;
    lv_color_t telemetry_bg;
    lv_color_t code_bg;

    /* Terminal 16-color ANSI Palette */
    lv_color_t ansi[16];
} devos_palette_t;

typedef void (*devos_theme_change_cb_t)(const devos_palette_t *palette, void *user_data);

/* Theme Engine API */
void devos_theme_init(void);
devos_theme_type_t devos_theme_get_type(void);
const devos_palette_t *devos_theme_get(void);
void devos_theme_set(devos_theme_type_t type);
void devos_theme_toggle(void);      /* Bound to Fn + T */
bool devos_theme_is_dark(void);

/* Dynamic Listener Registration */
int devos_theme_add_listener(devos_theme_change_cb_t cb, void *user_data);
void devos_theme_remove_listener(int listener_id);

#ifdef __cplusplus
}
#endif
