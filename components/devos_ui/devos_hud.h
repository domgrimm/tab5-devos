#pragma once

/* devos_hud: the system info panel. Sym + I from any screen shows power,
 * memory, network and CPU over the current app, updated every second.
 * Esc, Enter or Sym + I (or a tap) closes it; the app underneath keeps its
 * state and doesn't see the keys meanwhile, apart from Sym shortcuts. */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hook Sym + I (after the top bar exists). */
void devos_hud_init(void);
void devos_hud_open(void);
void devos_hud_close(void);
bool devos_hud_is_open(void);

#ifdef __cplusplus
}
#endif
