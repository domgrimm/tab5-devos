#pragma once

/* devos_maptiles: OpenStreetMap raster tiles (no LVGL).
 *
 * The caller says which tiles it wants right now (nearest first); a Core 0
 * worker downloads the missing ones one at a time from tile.openstreetmap.org
 * and keeps them on the SD card (/.devos/maps/osm/z/x/y.png) and in a small
 * PSRAM cache, so a view is only downloaded once and works offline after.
 *
 * OSM tile usage policy: one request at a time, an identifying User-Agent,
 * tiles cached locally (re-fetched after 30 days) and the attribution
 * (DEVOS_MAPTILES_ATTRIBUTION) shown on the map.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_MAPTILES_ATTRIBUTION "Map data (c) OpenStreetMap contributors"   /* the UI fonts have no \u00a9 */
#define DEVOS_MAPTILES_MAX_ZOOM    17
#define DEVOS_MAPTILES_MAX_WANT    64

typedef struct {
    uint8_t z;
    uint32_t x, y;
} devos_tile_t;

void devos_maptiles_init(void);

/* Replace the wanted list (n = 0 stops fetching). Order = fetch order. */
void devos_maptiles_want(const devos_tile_t *tiles, int n);

/* 1: the tile's PNG in *png (malloc'd, caller frees) and *len;
 * 0: not here yet (being fetched if wanted); -1: download failed. */
int devos_maptiles_get(devos_tile_t t, uint8_t **png, size_t *len);

/* Bumps whenever a tile arrives or fails. */
uint32_t devos_maptiles_generation(void);

/* Last download error ("" if none). */
void devos_maptiles_error(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
