#pragma once

/* ADS-B radar map underlay: OpenStreetMap tiles (devos_maptiles) resampled
 * onto the radar's own projection, so roads and coastlines line up with the
 * aircraft at every range, and tinted to the theme. */

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int cx, cy;                     /* receiver on screen */
    float scale;                    /* px per nm */
    double lat0, lon0;
} adsb_map_view_t;

/* Call from the UI timer. `area` is the radar's drawing area. Fetches,
 * decodes and paints tiles a few at a time; true when the picture changed. */
bool adsb_map_update(const lv_area_t *area, const adsb_map_view_t *v);
/* Draw the underlay (call first in the radar's draw callback). */
void adsb_map_draw(lv_layer_t *layer);
/* Stop fetching and hide the underlay (app hidden or map switched off). */
void adsb_map_stop(void);
/* "Map 3 / 12", an error, or "" when complete. */
const char *adsb_map_status(void);

#ifdef __cplusplus
}
#endif
