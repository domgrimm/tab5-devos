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

/* Get current modifier state */
uint8_t tab5_keyboard_get_modifiers(void);

#ifdef __cplusplus
}
#endif
