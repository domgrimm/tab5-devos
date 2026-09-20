#pragma once

#include "lvgl.h"
#include "devos_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_obj_t *root;
    lv_obj_t *left_panel;
    lv_obj_t *center_panel;
    lv_obj_t *right_panel;

    devos_viewport_mode_t mode;
    devos_viewport_mode_t mode_before_focus;
    bool left_visible;
    bool right_visible;

    int theme_listener_id;
} devos_agent_viewport_t;

/* Creates the tri-pane viewport on parent */
devos_agent_viewport_t *devos_agent_viewport_create(lv_obj_t *parent);

/* Layout State Transitions */
void devos_agent_viewport_set_mode(devos_agent_viewport_t *vp, devos_viewport_mode_t mode);
void devos_agent_viewport_toggle_focus(devos_agent_viewport_t *vp);  /* Fn + F */
void devos_agent_viewport_toggle_left(devos_agent_viewport_t *vp);   /* Fn + [ */
void devos_agent_viewport_toggle_right(devos_agent_viewport_t *vp);  /* Fn + ] */

/* Container Accessors */
lv_obj_t *devos_agent_viewport_get_left(devos_agent_viewport_t *vp);
lv_obj_t *devos_agent_viewport_get_center(devos_agent_viewport_t *vp);
lv_obj_t *devos_agent_viewport_get_right(devos_agent_viewport_t *vp);

void devos_agent_viewport_destroy(devos_agent_viewport_t *vp);

#ifdef __cplusplus
}
#endif
