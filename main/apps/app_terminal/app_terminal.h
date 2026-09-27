#pragma once

#include "lvgl.h"
#include "devos_core.h"

#ifdef __cplusplus
extern "C" {
#endif

devos_app_descriptor_t *app_terminal_get_descriptor(void);

/* Show/hide the sessions & saved hosts sidebar (Sym + L) */
void app_terminal_toggle_sidebar(void);

/* Resize the active session's remote PTY */
void app_terminal_resize_pty(uint16_t cols, uint16_t rows);

#ifdef __cplusplus
}
#endif
