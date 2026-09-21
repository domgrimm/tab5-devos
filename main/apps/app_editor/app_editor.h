#pragma once

#include "lvgl.h"
#include "devos_core.h"

#ifdef __cplusplus
extern "C" {
#endif

devos_app_descriptor_t *app_editor_get_descriptor(void);

/* Agent Synergy API */
const char *app_editor_get_active_filename(void);
const char *app_editor_get_active_text(void);
bool app_editor_save_plan(const char *title, const char *content);

#ifdef __cplusplus
}
#endif
