#pragma once

#include "devos_config.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t modifiers;      /* DEVOS_MOD_CTRL, DEVOS_MOD_SHIFT, DEVOS_MOD_ALT, DEVOS_MOD_FN */
    uint32_t keycode;       /* ASCII or LV_KEY_* */
    bool pressed;           /* true = pressed, false = released */
} tab5_key_event_t;

/* Initialize A164 Keyboard hardware or simulator input hook */
bool tab5_keyboard_init(void);

/* Process a raw key input event */
void tab5_keyboard_inject_key(uint32_t key, uint8_t modifiers, bool pressed);

/* Non-blocking: pull one queued key for the GUI task to dispatch. Returns true
 * if a key was dequeued. Keys are dispatched on the GUI task (not the keyboard
 * task) so app/LVGL code runs on the right stack and single-threaded. */
bool tab5_keyboard_get_key(uint32_t *key, uint8_t *modifiers);

/* Get current modifier state */
uint8_t tab5_keyboard_get_modifiers(void);

/* Keyboard attached and answering on I2C (always true in the simulator). */
bool tab5_keyboard_is_connected(void);

/* Caps lock (tap Aa) state. */
bool tab5_keyboard_caps_lock(void);

/* The keyboard's two RGB indicator lights: 0 = left, 1 = right. */
typedef enum {
    TAB5_KBD_LIGHT_OFF = 0,
    TAB5_KBD_LIGHT_COLOUR,      /* the light's rgb */
    TAB5_KBD_LIGHT_ACCENT,      /* the theme's accent colour */
    TAB5_KBD_LIGHT_BATTERY,     /* green >= 50%, amber >= 20%, red below (blinks
                                 * under 10%), blue while charging / on USB */
} tab5_kbd_light_src_t;

typedef struct {
    uint8_t source;             /* tab5_kbd_light_src_t */
    uint8_t brightness;         /* 0-100 */
    bool caps_lock;             /* turn amber (red over amber) while caps lock is on */
    uint32_t rgb;               /* 0xRRGGBB for TAB5_KBD_LIGHT_COLOUR */
} tab5_kbd_light_t;

typedef struct {
    bool custom;                /* false: the keyboard firmware drives both lights */
    tab5_kbd_light_t light[2];
} tab5_kbd_lights_t;

/* Apply (and with `save`, persist in NVS) the lights. GUI task. */
void tab5_keyboard_set_lights(const tab5_kbd_lights_t *cfg, bool save);
void tab5_keyboard_get_lights(tab5_kbd_lights_t *cfg);
/* Accent colour 0xRRGGBB for TAB5_KBD_LIGHT_ACCENT (main follows the theme). */
void tab5_keyboard_set_accent(uint32_t rgb);
/* Lights off while the screen sleeps. */
void tab5_keyboard_lights_suspend(bool off);
/* What light idx shows now (0xRRGGBB before brightness; for previews). */
uint32_t tab5_keyboard_light_colour(int idx);
/* Both lights show rgb at full brightness for `ms`, then go back to their
 * settings (the terminal bell). Any task; not while the screen sleeps. */
void tab5_keyboard_lights_pulse(uint32_t rgb, uint32_t ms);

#ifdef __cplusplus
}
#endif
