#pragma once

#include "lvgl.h"
#include "devos_core.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

devos_app_descriptor_t *app_launcher_get_descriptor(void);
void app_launcher_update_telemetry(void);

/* Tile / Widget Re-arrangement Mode */
void app_launcher_set_arrange_mode(bool active);
void app_launcher_toggle_arrange_mode(void);
bool app_launcher_is_arrange_mode(void);
void app_launcher_swap_slots(int slot_a, int slot_b);
void app_launcher_reset_layout(void);
int app_launcher_get_app_in_slot(int slot);
const char *app_launcher_get_app_uid_in_slot(int slot);
int app_launcher_get_active_app_count(void);

/* Multi-Page Carousel & Pagination */
int app_launcher_get_current_page(void);
int app_launcher_get_total_pages(void);
void app_launcher_set_page(int page, bool animate);

#ifdef __cplusplus
}
#endif
