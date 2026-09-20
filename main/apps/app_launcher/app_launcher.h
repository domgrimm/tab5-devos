#pragma once

#include "lvgl.h"
#include "devos_core.h"

#ifdef __cplusplus
extern "C" {
#endif

devos_app_descriptor_t *app_launcher_get_descriptor(void);
void app_launcher_update_telemetry(void);

#ifdef __cplusplus
}
#endif
