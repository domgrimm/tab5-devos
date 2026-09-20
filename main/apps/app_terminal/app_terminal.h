#pragma once

#include "lvgl.h"
#include "devos_core.h"

#ifdef __cplusplus
extern "C" {
#endif

devos_app_descriptor_t *app_terminal_get_descriptor(void);

/* Toggle Collapsible Connections & Sessions Side Panel (Fn + [) */
void app_terminal_toggle_sidebar(void);

/* Request PTY resize and notify remote host */
void app_terminal_resize_pty(uint16_t cols, uint16_t rows);

#ifdef __cplusplus
}
#endif
