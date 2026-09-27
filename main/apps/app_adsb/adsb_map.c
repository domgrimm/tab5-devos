/* ADS-B radar map underlay: see adsb_map.h.
 *
 * The radar is a flat projection around the receiver (x = east nm, y = north
 * nm); OSM tiles are Web Mercator. For every screen column we work out its
 * longitude and for every row its latitude, and from those the pixel in the
 * Mercator world at the chosen zoom, so each tile is painted straight into
 * place (nearest pixel) with the radar's own geometry: no drift at the edge
 * of a 400 nm view.
 */
#include "adsb_map.h"
#include "devos_config.h"
#include "devos_maptiles.h"
#include "devos_theme.h"
#include "src/libs/lodepng/lodepng.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_W        1280
#define MAX_H        720
#define PAINT_PER_TICK 2           /* PNG decodes per UI tick (~20-40 ms each on the P4) */
#define DIM_DARK     64            /* map strength /256 over the radar background */
#define DIM_LIGHT    150

enum { T_PENDING, T_DONE, T_FAILED };

static EXT_RAM_BSS_ATTR struct {
    lv_draw_buf_t *buf;
    lv_area_t area;
    int vw, vh;                     /* the area asked for (area is clipped to MAX_W x MAX_H) */
    bool shown;
    /* what the picture was built for */
    adsb_map_view_t v;
    int z;
    bool dark;
    lv_color_t base;
    /* world pixel of each column / row at zoom z (-1: off the map) */
    int32_t *col, *row;             /* MAX_W / MAX_H, LVGL heap (PSRAM) */
    devos_tile_t t[DEVOS_MAPTILES_MAX_WANT];
    uint8_t st[DEVOS_MAPTILES_MAX_WANT];
    int n, done, failed;
    uint32_t gen, gen_fail;
    bool more;                      /* tiles may be ready that we haven't painted */
    char status[112];
} M;

static bool same_view(const lv_area_t *a, const adsb_map_view_t *v)
{
    return M.buf && a->x1 == M.area.x1 && a->y1 == M.area.y1 && lv_area_get_width(a) == M.vw && lv_area_get_height(a) == M.vh && v->cx == M.v.cx && v->cy == M.v.cy && v->scale == M.v.scale &&
           v->lat0 == M.v.lat0 && v->lon0 == M.v.lon0 && devos_theme_is_dark() == M.dark &&
           lv_color_eq(devos_theme_get()->code_bg, M.base);
}

static int32_t world_x(double lon, int z)
{
    double w = (double)(256u << z);
    double x = (lon + 180.0) / 360.0 * w;
    x = fmod(x, w);
    if (x < 0) x += w;
    return (int32_t)x;
}

static int32_t world_y(double lat, int z)
{
    if (lat > 85.05 || lat < -85.05) return -1;
    double r = lat * M_PI / 180.0;
    return (int32_t)((1.0 - asinh(tan(r)) / M_PI) / 2.0 * (double)(256u << z));
}

/* Columns / rows -> world pixels at zoom z; returns the tile count. */
static int plan(int z, int w, int h)
{
    const adsb_map_view_t *v = &M.v;
    double coslat = cos(v->lat0 * M_PI / 180.0);
    for (int i = 0; i < w; i++) {
        double nm = (M.area.x1 + i - v->cx) / (double)v->scale;
        M.col[i] = world_x(v->lon0 + nm / (60.0 * coslat), z);
    }
    for (int j = 0; j < h; j++) {
        double nm = (v->cy - (M.area.y1 + j)) / (double)v->scale;
        M.row[j] = world_y(v->lat0 + nm / 60.0, z);
    }
    /* distinct tile columns (the view may wrap at 180 degrees) and rows */
    uint32_t tx[32], ty[32];
    int ntx = 0, nty = 0;
    for (int i = 0; i < w; i++) {
        uint32_t x = (uint32_t)M.col[i] >> 8;
        if (!ntx || tx[ntx - 1] != x) {
            if (ntx == 32) return 1000;
            tx[ntx++] = x;
        }
    }
    for (int j = 0; j < h; j++) {
        if (M.row[j] < 0) continue;
        uint32_t y = (uint32_t)M.row[j] >> 8;
        if (!nty || ty[nty - 1] != y) {
            if (nty == 32) return 1000;
            ty[nty++] = y;
        }
    }
    if (ntx * nty > DEVOS_MAPTILES_MAX_WANT) return ntx * nty;
    /* nearest the receiver first */
    uint32_t cxw = (uint32_t)world_x(v->lon0, z) >> 8, cyw = (uint32_t)world_y(v->lat0, z) >> 8;
    int n = 0;
    for (int a = 0; a < ntx; a++)
        for (int b = 0; b < nty; b++) M.t[n++] = (devos_tile_t){ (uint8_t)z, tx[a], ty[b] };
    for (int a = 1; a < n; a++) {
        devos_tile_t k = M.t[a];
        long dk = labs((long)k.x - (long)cxw) + labs((long)k.y - (long)cyw);
        int b = a - 1;
        while (b >= 0 && labs((long)M.t[b].x - (long)cxw) + labs((long)M.t[b].y - (long)cyw) > dk) {
            M.t[b + 1] = M.t[b];
            b--;
        }
        M.t[b + 1] = k;
    }
    return n;
}

static void clear_buf(void)
{
    lv_color_t c = M.base;
    uint16_t px = (uint16_t)(((c.red & 0xF8) << 8) | ((c.green & 0xFC) << 3) | (c.blue >> 3));
    uint32_t h = M.buf->header.h, w = M.buf->header.w, stride = M.buf->header.stride;
    for (uint32_t j = 0; j < h; j++) {
        uint16_t *d = (uint16_t *)(M.buf->data + j * stride);
        for (uint32_t i = 0; i < w; i++) d[i] = px;
    }
}

static void rebuild(const lv_area_t *a, const adsb_map_view_t *v)
{
    int w = lv_area_get_width(a), h = lv_area_get_height(a);
    if (w > MAX_W) w = MAX_W;
    if (h > MAX_H) h = MAX_H;
    if (!M.col) M.col = lv_malloc(sizeof(int32_t) * MAX_W);
    if (!M.row) M.row = lv_malloc(sizeof(int32_t) * MAX_H);
    if (!M.buf || (int)M.buf->header.w != w || (int)M.buf->header.h != h) {
        if (M.buf) {
            lv_image_cache_drop(M.buf);
            lv_draw_buf_destroy(M.buf);
        }
        M.buf = lv_draw_buf_create((uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_RGB565, 0);
    }
    M.area = *a;
    M.vw = lv_area_get_width(a);
    M.vh = lv_area_get_height(a);
    M.area.x2 = a->x1 + w - 1;
    M.area.y2 = a->y1 + h - 1;
    M.v = *v;
    M.dark = devos_theme_is_dark();
    M.base = devos_theme_get()->code_bg;
    M.n = M.done = M.failed = 0;
    M.status[0] = '\0';
    if (!M.buf || !M.col || !M.row) {
        snprintf(M.status, sizeof(M.status), "Map: out of memory");
        return;
    }
    clear_buf();
    /* zoom where a tile pixel is about a screen pixel */
    double mpp = 1852.0 / v->scale;
    int z = (int)lround(log2(156543.034 * cos(v->lat0 * M_PI / 180.0) / mpp));
    if (z > DEVOS_MAPTILES_MAX_ZOOM) z = DEVOS_MAPTILES_MAX_ZOOM;
    if (z < 1) z = 1;
    int n;
    while ((n = plan(z, w, h)) > DEVOS_MAPTILES_MAX_WANT && z > 1) z--;
    M.z = z;
    M.n = n > DEVOS_MAPTILES_MAX_WANT ? 0 : n;
    memset(M.st, T_PENDING, sizeof(M.st));
    devos_maptiles_want(M.t, M.n);
    M.more = true;
    lv_image_cache_drop(M.buf);
}

/* Paint one decoded 256x256 tile (RGBA bytes, `pitch` per row) into the picture. */
static void paint(int k, const uint8_t *rgba, uint32_t pitch)
{
    devos_tile_t t = M.t[k];
    int w = (int)M.buf->header.w, h = (int)M.buf->header.h, stride = (int)M.buf->header.stride;
    int dim = M.dark ? DIM_DARK : DIM_LIGHT;
    int br = M.base.red, bg = M.base.green, bb = M.base.blue;
    int i0 = -1, i1 = -1;
    for (int i = 0; i < w; i++) {
        if (((uint32_t)M.col[i] >> 8) != t.x) continue;
        if (i0 < 0) i0 = i;
        i1 = i;
    }
    if (i0 < 0) return;
    for (int j = 0; j < h; j++) {
        if (M.row[j] < 0 || ((uint32_t)M.row[j] >> 8) != t.y) continue;
        const uint8_t *src = rgba + (size_t)(M.row[j] & 255) * pitch;
        uint16_t *d = (uint16_t *)(M.buf->data + (size_t)j * (size_t)stride);
        for (int i = i0; i <= i1; i++) {
            if (((uint32_t)M.col[i] >> 8) != t.x) continue;   /* only at a 180 degree wrap */
            const uint8_t *s = src + (M.col[i] & 255) * 4;
            int r = br + ((s[0] - br) * dim >> 8), g = bg + ((s[1] - bg) * dim >> 8), b = bb + ((s[2] - bb) * dim >> 8);
            d[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }
}

static bool progress(void)
{
    uint32_t g = devos_maptiles_generation();
    if (g == M.gen && !M.more) return false;
    M.gen = g;
    M.more = false;
    bool changed = false;
    int painted = 0;
    for (int k = 0; k < M.n; k++) {
        if (M.st[k] != T_PENDING) continue;
        if (painted == PAINT_PER_TICK) {
            M.more = true;
            break;
        }
        uint8_t *png = NULL;
        size_t len = 0;
        int r = devos_maptiles_get(M.t[k], &png, &len);
        if (r < 0) {
            M.st[k] = T_FAILED;
            M.failed++;
            continue;
        }
        if (r == 0) continue;
        /* LVGL's lodepng hands back an lv_draw_buf_t holding RGBA bytes */
        unsigned char *out = NULL;
        unsigned tw = 0, th = 0;
        unsigned e = lodepng_decode32(&out, &tw, &th, png, len);
        free(png);
        lv_draw_buf_t *db = (lv_draw_buf_t *)out;
        if (!e && db && tw == 256 && th == 256) {
            paint(k, db->data, db->header.stride);
            changed = true;
        }
        if (db) lv_draw_buf_destroy(db);
        M.st[k] = T_DONE;
        M.done++;
        painted++;
    }
    if (changed) {
        lv_draw_buf_flush_cache(M.buf, NULL);
        lv_image_cache_drop(M.buf);
    }
    return changed;
}

static void update_status(void)
{
    if (!M.buf) return;
    if (M.n && M.failed == M.n) {
        char err[96];
        devos_maptiles_error(err, sizeof(err));
        snprintf(M.status, sizeof(M.status), "Map unavailable%s%s", err[0] ? ": " : "", err);
    } else if (M.done + M.failed < M.n) {
        snprintf(M.status, sizeof(M.status), "Map %d / %d", M.done, M.n);
    } else {
        M.status[0] = '\0';
    }
}

bool adsb_map_update(const lv_area_t *a, const adsb_map_view_t *v)
{
    bool changed = !M.shown;
    M.shown = true;
    devos_maptiles_init();
    if (!same_view(a, v)) {
        rebuild(a, v);
        changed = true;
    }
    if (M.buf && M.col && progress()) changed = true;
    /* a failed tile is retried by the engine after a while */
    uint32_t g = devos_maptiles_generation();
    for (int k = 0; k < M.n && g != M.gen_fail; k++) {
        if (M.st[k] != T_FAILED) continue;
        uint8_t *png;
        size_t len;
        int r = devos_maptiles_get(M.t[k], &png, &len);
        if (r == 1) free(png);
        if (r >= 0) {
            M.st[k] = T_PENDING;
            M.failed--;
            M.more = true;
        }
    }
    M.gen_fail = g;
    update_status();
    return changed;
}

void adsb_map_draw(lv_layer_t *layer)
{
    if (!M.shown || !M.buf) return;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = M.buf;
    lv_draw_image(layer, &d, &M.area);
}

void adsb_map_stop(void)
{
    M.shown = false;
    devos_maptiles_want(NULL, 0);
    M.status[0] = '\0';
}

const char *adsb_map_status(void) { return M.shown ? M.status : ""; }
