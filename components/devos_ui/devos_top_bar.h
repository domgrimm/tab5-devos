#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize and create persistent top status bar */
lv_obj_t *devos_top_bar_create(lv_obj_t *parent);

/* Update status labels from system telemetry */
void devos_top_bar_update(void);

#ifdef __cplusplus
}
#endif
