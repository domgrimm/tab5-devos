#pragma once

/* devos_powerdlg: the confirm/cancel dialog for restarting or shutting the
 * Tab5 down. One dialog, shown over whatever app is in front: the global
 * shortcuts (Sym + Shift + R / Sym + Shift + Q) and the Settings > Power
 * buttons both raise it, so they behave identically.
 *
 * It names anything a restart would cut off (devos_core_restart_check) before
 * going ahead. Enter presses the focused button, Esc cancels (or press
 * Cancel); Up / Down / Left / Right / Tab move between them. While it is open
 * it has the keyboard, so nothing reaches the app underneath. */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the key hook (after the top bar exists). */
void devos_powerdlg_init(void);
void devos_powerdlg_restart(void);      /* ask, then restart */
void devos_powerdlg_shutdown(void);     /* ask, then shut down */
void devos_powerdlg_close(void);
bool devos_powerdlg_is_open(void);

#ifdef __cplusplus
}
#endif
