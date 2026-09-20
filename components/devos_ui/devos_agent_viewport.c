#include "devos_agent_viewport.h"
#include "devos_theme.h"
#include <stdlib.h>

static void apply_theme_to_viewport(devos_agent_viewport_t *vp, const devos_palette_t *p)
{
    if (!vp || !vp->root) return;

    lv_obj_set_style_bg_color(vp->root, p->bg, 0);

    /* Left Sidebar */
    lv_obj_set_style_bg_color(vp->left_panel, p->surface, 0);
    lv_obj_set_style_border_color(vp->left_panel, p->surface_border, 0);

    /* Center Main Canvas */
    lv_obj_set_style_bg_color(vp->center_panel, p->bg, 0);

    /* Right Inspector */
    lv_obj_set_style_bg_color(vp->right_panel, p->surface, 0);
    lv_obj_set_style_border_color(vp->right_panel, p->surface_border, 0);
}

static void on_viewport_theme_change(const devos_palette_t *p, void *user_data)
{
    devos_agent_viewport_t *vp = (devos_agent_viewport_t *)user_data;
    apply_theme_to_viewport(vp, p);
}

devos_agent_viewport_t *devos_agent_viewport_create(lv_obj_t *parent)
{
    devos_agent_viewport_t *vp = (devos_agent_viewport_t *)malloc(sizeof(devos_agent_viewport_t));
    if (!vp) return NULL;

    const devos_palette_t *p = devos_theme_get();

    /* Root Viewport Container */
    vp->root = lv_obj_create(parent);
    lv_obj_set_size(vp->root, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(vp->root, 0, DEVOS_TOP_BAR_HEIGHT);
    lv_obj_set_style_bg_color(vp->root, p->bg, 0);
    lv_obj_set_style_radius(vp->root, 0, 0);
    lv_obj_set_style_border_width(vp->root, 0, 0);
    lv_obj_set_style_pad_all(vp->root, 0, 0);
    lv_obj_clear_flag(vp->root, LV_OBJ_FLAG_SCROLLABLE);

    /* 1. Left Sidebar (Collapsible 260px) */
    vp->left_panel = lv_obj_create(vp->root);
    lv_obj_set_size(vp->left_panel, DEVOS_PANE_LEFT_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(vp->left_panel, 0, 0);
    lv_obj_set_style_bg_color(vp->left_panel, p->surface, 0);
    lv_obj_set_style_border_color(vp->left_panel, p->surface_border, 0);
    lv_obj_set_style_border_width(vp->left_panel, 1, 0);
    lv_obj_set_style_border_side(vp->left_panel, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_radius(vp->left_panel, 0, 0);
    lv_obj_set_style_pad_all(vp->left_panel, 8, 0);

    /* 2. Center Main Canvas (Dynamic width: starts at 720px) */
    vp->center_panel = lv_obj_create(vp->root);
    lv_obj_set_size(vp->center_panel, DEVOS_PANE_CENTER_TRIPANE, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(vp->center_panel, DEVOS_PANE_LEFT_WIDTH, 0);
    lv_obj_set_style_bg_color(vp->center_panel, p->bg, 0);
    lv_obj_set_style_border_width(vp->center_panel, 0, 0);
    lv_obj_set_style_radius(vp->center_panel, 0, 0);
    lv_obj_set_style_pad_all(vp->center_panel, 8, 0);

    /* 3. Right Inspector (Collapsible 300px) */
    vp->right_panel = lv_obj_create(vp->root);
    lv_obj_set_size(vp->right_panel, DEVOS_PANE_RIGHT_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(vp->right_panel, DEVOS_PANE_LEFT_WIDTH + DEVOS_PANE_CENTER_TRIPANE, 0);
    lv_obj_set_style_bg_color(vp->right_panel, p->surface, 0);
    lv_obj_set_style_border_color(vp->right_panel, p->surface_border, 0);
    lv_obj_set_style_border_width(vp->right_panel, 1, 0);
    lv_obj_set_style_border_side(vp->right_panel, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_radius(vp->right_panel, 0, 0);
    lv_obj_set_style_pad_all(vp->right_panel, 8, 0);

    vp->mode = DEVOS_VIEWPORT_TRIPANE;
    vp->mode_before_focus = DEVOS_VIEWPORT_TRIPANE;
    vp->left_visible = true;
    vp->right_visible = true;

    vp->theme_listener_id = devos_theme_add_listener(on_viewport_theme_change, vp);

    return vp;
}

void devos_agent_viewport_set_mode(devos_agent_viewport_t *vp, devos_viewport_mode_t mode)
{
    if (!vp) return;

    vp->mode = mode;

    switch (mode) {
        case DEVOS_VIEWPORT_TRIPANE:
            lv_obj_remove_flag(vp->left_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(vp->right_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(vp->left_panel, DEVOS_PANE_LEFT_WIDTH, DEVOS_CONTENT_HEIGHT);
            lv_obj_set_pos(vp->left_panel, 0, 0);
            lv_obj_set_size(vp->center_panel, DEVOS_PANE_CENTER_TRIPANE, DEVOS_CONTENT_HEIGHT);
            lv_obj_set_pos(vp->center_panel, DEVOS_PANE_LEFT_WIDTH, 0);
            lv_obj_set_size(vp->right_panel, DEVOS_PANE_RIGHT_WIDTH, DEVOS_CONTENT_HEIGHT);
            lv_obj_set_pos(vp->right_panel, DEVOS_PANE_LEFT_WIDTH + DEVOS_PANE_CENTER_TRIPANE, 0);
            vp->left_visible = true;
            vp->right_visible = true;
            break;

        case DEVOS_VIEWPORT_LEFT_ONLY:
            lv_obj_remove_flag(vp->left_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(vp->right_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(vp->left_panel, DEVOS_PANE_LEFT_WIDTH, DEVOS_CONTENT_HEIGHT);
            lv_obj_set_pos(vp->left_panel, 0, 0);
            lv_obj_set_size(vp->center_panel, DEVOS_PANE_CENTER_LEFTONLY, DEVOS_CONTENT_HEIGHT);
            lv_obj_set_pos(vp->center_panel, DEVOS_PANE_LEFT_WIDTH, 0);
            vp->left_visible = true;
            vp->right_visible = false;
            break;

        case DEVOS_VIEWPORT_RIGHT_ONLY:
            lv_obj_add_flag(vp->left_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(vp->right_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(vp->center_panel, DEVOS_PANE_CENTER_RIGHTONLY, DEVOS_CONTENT_HEIGHT);
            lv_obj_set_pos(vp->center_panel, 0, 0);
            lv_obj_set_size(vp->right_panel, DEVOS_PANE_RIGHT_WIDTH, DEVOS_CONTENT_HEIGHT);
            lv_obj_set_pos(vp->right_panel, DEVOS_PANE_CENTER_RIGHTONLY, 0);
            vp->left_visible = false;
            vp->right_visible = true;
            break;

        case DEVOS_VIEWPORT_FOCUS:
            lv_obj_add_flag(vp->left_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(vp->right_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(vp->center_panel, DEVOS_PANE_CENTER_FOCUS, DEVOS_CONTENT_HEIGHT);
            lv_obj_set_pos(vp->center_panel, 0, 0);
            vp->left_visible = false;
            vp->right_visible = false;
            break;
    }
}

void devos_agent_viewport_toggle_focus(devos_agent_viewport_t *vp)
{
    if (!vp) return;

    if (vp->mode == DEVOS_VIEWPORT_FOCUS) {
        /* Restore previous layout state */
        devos_agent_viewport_set_mode(vp, vp->mode_before_focus);
    } else {
        /* Save previous state and enter Focus Mode */
        vp->mode_before_focus = vp->mode;
        devos_agent_viewport_set_mode(vp, DEVOS_VIEWPORT_FOCUS);
    }
}

void devos_agent_viewport_toggle_left(devos_agent_viewport_t *vp)
{
    if (!vp) return;

    if (vp->mode == DEVOS_VIEWPORT_FOCUS) {
        devos_agent_viewport_set_mode(vp, DEVOS_VIEWPORT_LEFT_ONLY);
    } else if (vp->mode == DEVOS_VIEWPORT_TRIPANE) {
        devos_agent_viewport_set_mode(vp, DEVOS_VIEWPORT_RIGHT_ONLY);
    } else if (vp->mode == DEVOS_VIEWPORT_RIGHT_ONLY) {
        devos_agent_viewport_set_mode(vp, DEVOS_VIEWPORT_TRIPANE);
    } else if (vp->mode == DEVOS_VIEWPORT_LEFT_ONLY) {
        devos_agent_viewport_set_mode(vp, DEVOS_VIEWPORT_FOCUS);
    }
}

void devos_agent_viewport_toggle_right(devos_agent_viewport_t *vp)
{
    if (!vp) return;

    if (vp->mode == DEVOS_VIEWPORT_FOCUS) {
        devos_agent_viewport_set_mode(vp, DEVOS_VIEWPORT_RIGHT_ONLY);
    } else if (vp->mode == DEVOS_VIEWPORT_TRIPANE) {
        devos_agent_viewport_set_mode(vp, DEVOS_VIEWPORT_LEFT_ONLY);
    } else if (vp->mode == DEVOS_VIEWPORT_LEFT_ONLY) {
        devos_agent_viewport_set_mode(vp, DEVOS_VIEWPORT_TRIPANE);
    } else if (vp->mode == DEVOS_VIEWPORT_RIGHT_ONLY) {
        devos_agent_viewport_set_mode(vp, DEVOS_VIEWPORT_FOCUS);
    }
}

lv_obj_t *devos_agent_viewport_get_left(devos_agent_viewport_t *vp)
{
    return vp ? vp->left_panel : NULL;
}

lv_obj_t *devos_agent_viewport_get_center(devos_agent_viewport_t *vp)
{
    return vp ? vp->center_panel : NULL;
}

lv_obj_t *devos_agent_viewport_get_right(devos_agent_viewport_t *vp)
{
    return vp ? vp->right_panel : NULL;
}

void devos_agent_viewport_destroy(devos_agent_viewport_t *vp)
{
    if (!vp) return;
    if (vp->theme_listener_id >= 0) {
        devos_theme_remove_listener(vp->theme_listener_id);
    }
    if (vp->root) {
        lv_obj_delete(vp->root);
    }
    free(vp);
}
