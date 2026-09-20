#pragma once

#include "lvgl.h"
#include "devos_core.h"
#include "devos_agent_viewport.h"

#ifdef __cplusplus
extern "C" {
#endif

devos_app_descriptor_t *app_antigravity_get_descriptor(void);

/* Viewport Actions */
void app_antigravity_toggle_focus(void);    /* Fn + F */
void app_antigravity_toggle_left(void);     /* Fn + [ */
void app_antigravity_toggle_right(void);    /* Fn + ] */

#ifdef __cplusplus
}
#endif
