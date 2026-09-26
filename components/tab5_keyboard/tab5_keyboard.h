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

/* The keyboard's two RGB indicator lights (left, right). */
typedef enum {
    TAB5_KBD_LIGHTS_OFF = 0,
    TAB5_KBD_LIGHTS_STATUS,     /* keyboard firmware drives them (power-on default) */
    TAB5_KBD_LIGHTS_ACCENT,     /* the theme's accent colour */
    TAB5_KBD_LIGHTS_CYAN,
    TAB5_KBD_LIGHTS_GREEN,
    TAB5_KBD_LIGHTS_AMBER,
    TAB5_KBD_LIGHTS_RED,
    TAB5_KBD_LIGHTS_PURPLE,
    TAB5_KBD_LIGHTS_WHITE,
    TAB5_KBD_LIGHTS_COUNT
} tab5_kbd_lights_t;

/* Choose the lights (brightness 0-100). In the colour modes the left light
 * turns amber (red when the colour is amber) while caps lock is on. `save`
 * persists the choice (NVS); call from the GUI task. */
void tab5_keyboard_set_lights(tab5_kbd_lights_t mode, uint8_t brightness, bool save);
void tab5_keyboard_get_lights(tab5_kbd_lights_t *mode, uint8_t *brightness);
/* Accent colour 0xRRGGBB for TAB5_KBD_LIGHTS_ACCENT (main follows the theme). */
void tab5_keyboard_set_accent(uint32_t rgb);
/* Lights off while the screen sleeps. */
void tab5_keyboard_lights_suspend(bool off);

#ifdef __cplusplus
}
#endif
