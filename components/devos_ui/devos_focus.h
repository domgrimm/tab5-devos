#pragma once

/* devos_focus: keyboard focus for forms, dialogs and button rows - the
 * keyboard-first rule (AGENTS.md invariant 9). Register a screen's or a
 * dialog's controls in reading order and pass keys to devos_focus_key()
 * from the app's handle_key. The focused control gets an accent focus ring
 * (shown after a key press, hidden again when the user taps).
 *
 *   Up / Down, Tab, Aa+Tab  move focus (hidden and disabled controls are skipped)
 *   Left / Right            change the focused slider, dropdown, switch or checkbox;
 *                           move the cursor in a text field; on a button, move along
 *                           the row (like Up / Down)
 *   Enter / Space           press a button (or any clickable object), open a dropdown -
 *                           then Up / Down pick, Enter confirms, Esc cancels
 *   Space                   toggle a checkbox / switch (Enter there goes to the app,
 *                           which usually submits the form)
 *   printable keys          type into a focused text field (Backspace / Del (127) edit;
 *                           in a multi-line field Up / Down / Enter edit too)
 *   Sym+V / Ctrl+V          paste the system clipboard into a focused text field
 *                           (a one-line field takes its first line)
 *
 * Returned false (left to the app): Esc (unless a dropdown list is open),
 * Enter on a one-line text field or a checkbox / switch (submit), keys with
 * Ctrl / Sym / Alt, and
 * anything else the focused control doesn't use.
 */

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Controls one focus ring can hold. The Jobs Builder registers a widget per
 * parameter row (12 rows x field + dropdown + Literal/Expr toggle) plus its
 * toolbar, which came to 72 and silently lost the tail - the toolbar buttons
 * and the Text view - making them unreachable by keyboard (invariant 9). */
#define DEVOS_FOCUS_MAX 96

typedef struct {
    lv_obj_t *items[DEVOS_FOCUS_MAX];
    int n;
    int cur;                    /* index into items, -1 = nothing focused */
    bool ring;                  /* focus ring visible (keyboard in use) */
    bool dd_open;               /* the focused dropdown's list is open */
    uint32_t dd_orig;           /* its selection before opening */
} devos_focus_t;

void devos_focus_init(devos_focus_t *f);
/* Register a control (order = focus order). Taps move focus to it too. */
void devos_focus_add(devos_focus_t *f, lv_obj_t *obj);
/* Focus obj (NULL = none); shows the ring. */
void devos_focus_set(devos_focus_t *f, lv_obj_t *obj);
lv_obj_t *devos_focus_get(const devos_focus_t *f);
/* Next / previous focusable control (dir +1 / -1), wrapping. */
void devos_focus_move(devos_focus_t *f, int dir);
/* Focus the first focusable control if nothing is focused. */
void devos_focus_first(devos_focus_t *f);
/* Handle a key for the focused control; true if consumed. */
bool devos_focus_key(devos_focus_t *f, uint32_t key, uint8_t mods);
/* Remove the ring and forget the focus (e.g. when a dialog closes). */
void devos_focus_clear(devos_focus_t *f);

#ifdef __cplusplus
}
#endif
