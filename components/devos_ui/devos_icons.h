#pragma once

/* devos_icons: vector app icons, drawn with LVGL primitives so one design
 * serves every size (16 px in the top bar, 22 px on launcher tiles) and both
 * themes. Each draws into the largest square centred in `area`, in `color`;
 * brand marks (WireGuard's red roundel) keep their own colours. Set one as an
 * app's descriptor draw_icon; apps without one show their `icon` symbol. */

#include "lvgl.h"
#include "devos_core.h"

#ifdef __cplusplus
extern "C" {
#endif

void devos_icon_terminal(lv_layer_t *layer, const lv_area_t *area, lv_color_t color);
void devos_icon_editor(lv_layer_t *layer, const lv_area_t *area, lv_color_t color);
void devos_icon_tailscale(lv_layer_t *layer, const lv_area_t *area, lv_color_t color);
void devos_icon_wireguard(lv_layer_t *layer, const lv_area_t *area, lv_color_t color);
void devos_icon_mqtt(lv_layer_t *layer, const lv_area_t *area, lv_color_t color);
void devos_icon_network(lv_layer_t *layer, const lv_area_t *area, lv_color_t color);
void devos_icon_rest(lv_layer_t *layer, const lv_area_t *area, lv_color_t color);
void devos_icon_docker(lv_layer_t *layer, const lv_area_t *area, lv_color_t color);
void devos_icon_adsb(lv_layer_t *layer, const lv_area_t *area, lv_color_t color);
void devos_icon_cricket(lv_layer_t *layer, const lv_area_t *area, lv_color_t color);
void devos_icon_totp(lv_layer_t *layer, const lv_area_t *area, lv_color_t color);

/* A size x size object that shows an app's icon: its draw_icon, else its
 * `icon` symbol in the object's text font. The colour is the object's text
 * colour (lv_obj_set_style_text_color / a style), so theme changes apply as
 * for a label. Not clickable: taps go to the parent. */
lv_obj_t *devos_icon_create(lv_obj_t *parent, int32_t size);
void devos_icon_set_app(lv_obj_t *icon, const devos_app_descriptor_t *app);

#ifdef __cplusplus
}
#endif
