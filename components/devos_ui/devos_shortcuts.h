#pragma once

/* devos_shortcuts: the keyboard cheat sheet. Sym + S from any screen shows
 * the shortcuts that work everywhere on the left and the current app's on
 * the right (its descriptor's get_shortcuts(), which can follow what the app
 * is doing). Up / Down scroll, Esc / Enter / Sym + S close it; any other Sym
 * shortcut closes it and goes ahead. A tap outside the box closes it too.
 * (? and / are Sym-layer symbols on the Tab5 keyboard, so not those.) */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The key that opens it (with Sym), and how hint lines write it. */
#define DEVOS_SHORTCUTS_KEY      's'
#define DEVOS_SHORTCUTS_KEY_TEXT "Sym+S"

/* Hook the key (after the top bar exists). */
void devos_shortcuts_init(void);
void devos_shortcuts_open(void);
void devos_shortcuts_close(void);
bool devos_shortcuts_is_open(void);

#ifdef __cplusplus
}
#endif
