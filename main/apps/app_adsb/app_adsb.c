/* ADS-B: a live radar of the aircraft your local receiver hears (the
 * aircraft.json of dump1090-fa / readsb / tar1090 / PiAware).
 *
 * Left: the radar - range rings around the receiver, each aircraft a
 * triangle pointing along its track, coloured by altitude, with a fading
 * trail and its callsign / flight level. Emergency squawks (7500 hijack,
 * 7600 radio failure, 7700 emergency) blink red. Right: the aircraft by
 * distance and the selected one's details.
 *
 * Under it, optionally, an OpenStreetMap underlay (adsb_map.c).
 *
 * Keys: Up / Down pick an aircraft, + / - zoom, 0 back to the set range,
 * L labels, T trails, M map, Space freezes the picture, C settings, Esc home.
 * Tap the radar to pick the aircraft nearest your finger.
 */
#include "app_adsb.h"
#include "devos_config.h"
#include "devos_icons.h"
#include "devos_core.h"
#include "devos_focus.h"
#include "devos_theme.h"
#include "devos_widgets.h"
#include "devos_adsb.h"
#include "devos_maptiles.h"
#include "adsb_map.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RADAR_W  780
#define SIDE_X   (RADAR_W + 8)
#define SIDE_W   (DEVOS_SCREEN_WIDTH - SIDE_X - 8)
#define LIST_H   300

static devos_app_descriptor_t s_desc;
static lv_obj_t *s_screen, *lbl_status, *s_keys, *radar, *lbl_detail_title, *lbl_detail, *lbl_empty;
static devos_vlist_t s_list;
static devos_w_dialog_t s_dlg;
static lv_obj_t *ta_url, *ta_lat, *ta_lon, *dd_range, *cb_map;
static devos_focus_t s_fdlg;

static devos_adsb_ac_t *s_ac;               /* DEVOS_ADSB_MAX, PSRAM */
static int s_n, s_order[DEVOS_ADSB_MAX];
static float s_dist[DEVOS_ADSB_MAX], s_brg[DEVOS_ADSB_MAX];
static devos_adsb_status_t s_st;
static uint32_t s_gen = 0xffffffff;
static char s_sel_hex[8];
static int s_range = 100;
static bool s_labels = true, s_trails = true, s_map = true, s_frozen, s_blink, s_inited;
static char s_map_status[112];

static const int RANGES[] = { 5, 10, 25, 50, 100, 150, 200, 300, 400 };
#define NRANGES ((int)(sizeof(RANGES) / sizeof(RANGES[0])))

/* ------------------------------------------------------------------ helpers */
static lv_color_t alt_color(const devos_adsb_ac_t *a)
{
    if (a->emergency) return lv_color_hex(0xff3b3b);
    if (a->ground) return lv_color_hex(0x9aa3ad);
    if (!a->has_alt) return lv_color_hex(0xcfd6dd);
    /* green (low) -> yellow -> orange -> red -> magenta (high), like most radar sites */
    static const struct { int ft; uint32_t c; } st[] = {
        { 0, 0x4cd964 }, { 5000, 0xb4e03c }, { 10000, 0xffd60a }, { 20000, 0xff9500 }, { 30000, 0xff453a }, { 40000, 0xd65cff },
    };
    int ft = a->alt_ft;
    if (ft <= 0) return lv_color_hex(st[0].c);
    for (int i = 1; i < 6; i++) {
        if (ft <= st[i].ft) {
            int span = st[i].ft - st[i - 1].ft;
            return lv_color_mix(lv_color_hex(st[i].c), lv_color_hex(st[i - 1].c), (uint8_t)((ft - st[i - 1].ft) * 255 / span));
        }
    }
    return lv_color_hex(st[5].c);
}

static void alt_text(const devos_adsb_ac_t *a, char *out, size_t cap)
{
    if (a->ground) snprintf(out, cap, "GND");
    else if (!a->has_alt) snprintf(out, cap, "-");
    else if (a->alt_ft >= 18000) snprintf(out, cap, "FL%03d", (a->alt_ft + 50) / 100);
    else snprintf(out, cap, "%d ft", a->alt_ft);
}

static const devos_adsb_ac_t *selected(void)
{
    for (int i = 0; i < s_n; i++) if (!strcmp(s_ac[i].hex, s_sel_hex)) return &s_ac[i];
    return NULL;
}

static int sel_row(void)
{
    for (int i = 0; i < s_n; i++) if (!strcmp(s_ac[s_order[i]].hex, s_sel_hex)) return i;
    return -1;
}

static int cmp_dist(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    bool px = s_ac[x].has_pos, py = s_ac[y].has_pos;
    if (px != py) return px ? -1 : 1;
    return s_dist[x] < s_dist[y] ? -1 : s_dist[x] > s_dist[y];
}

/* ------------------------------------------------------------------ radar */
typedef struct {
    int cx, cy;
    float scale;                    /* px per nm */
    double lat0, lon0, coslat;
} proj_t;

static void proj_make(proj_t *p, const lv_area_t *a)
{
    p->cx = (a->x1 + a->x2) / 2;
    p->cy = (a->y1 + a->y2) / 2;
    int r = ((a->x2 - a->x1) < (a->y2 - a->y1) ? (a->x2 - a->x1) : (a->y2 - a->y1)) / 2 - 16;
    p->scale = (float)r / (float)s_range;
    p->lat0 = s_st.lat;
    p->lon0 = s_st.lon;
    p->coslat = cos(p->lat0 * M_PI / 180);
}

static void proj(const proj_t *p, double lat, double lon, int *x, int *y)
{
    double dx = (lon - p->lon0) * 60.0 * p->coslat, dy = (lat - p->lat0) * 60.0;
    *x = p->cx + (int)(dx * p->scale);
    *y = p->cy - (int)(dy * p->scale);
}

static void draw_line(lv_layer_t *layer, int x1, int y1, int x2, int y2, lv_color_t c, lv_opa_t opa, int w)
{
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = c;
    ld.opa = opa;
    ld.width = w;
    ld.p1.x = x1; ld.p1.y = y1; ld.p2.x = x2; ld.p2.y = y2;
    lv_draw_line(layer, &ld);
}

static void draw_ring(lv_layer_t *layer, int cx, int cy, int r, lv_color_t c, lv_opa_t opa, int w)
{
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.center.x = cx;
    ad.center.y = cy;
    ad.radius = (uint16_t)r;
    ad.start_angle = 0;
    ad.end_angle = 360;
    ad.width = w;
    ad.color = c;
    ad.opa = opa;
    lv_draw_arc(layer, &ad);
}

static void draw_aircraft(lv_layer_t *layer, const proj_t *pj, const lv_area_t *clip, const devos_adsb_ac_t *a, bool sel)
{
    int x, y;
    proj(pj, a->lat, a->lon, &x, &y);
    if (x < clip->x1 - 20 || x > clip->x2 + 20 || y < clip->y1 - 20 || y > clip->y2 + 20) return;
    lv_color_t c = alt_color(a);
    const devos_palette_t *p = devos_theme_get();
    if (s_trails && a->trail_n > 1) {
        int px, py;
        proj(pj, a->trail_lat[0], a->trail_lon[0], &px, &py);
        for (int i = 1; i < a->trail_n; i++) {
            int qx, qy;
            proj(pj, a->trail_lat[i], a->trail_lon[i], &qx, &qy);
            lv_opa_t opa = (lv_opa_t)(40 + 160 * i / a->trail_n);
            draw_line(layer, px, py, qx, qy, c, sel ? LV_OPA_COVER : opa, sel ? 2 : 1);
            px = qx;
            py = qy;
        }
    }
    if (a->emergency && s_blink) draw_ring(layer, x, y, 16, lv_color_hex(0xff3b3b), LV_OPA_COVER, 2);
    if (sel) draw_ring(layer, x, y, 14, p->accent_primary, LV_OPA_COVER, 2);
    if (a->has_track && !a->ground) {
        float t = a->track_deg * (float)M_PI / 180.0f, s = sinf(t), co = cosf(t);
        /* nose, left and right corners of a narrow triangle */
        float pts[3][2] = { { 0, -10 }, { -6, 7 }, { 6, 7 } };
        lv_draw_triangle_dsc_t td;
        lv_draw_triangle_dsc_init(&td);
        td.bg_color = c;
        td.bg_opa = LV_OPA_COVER;
        for (int i = 0; i < 3; i++) {
            td.p[i].x = x + (int)(pts[i][0] * co - pts[i][1] * s);
            td.p[i].y = y + (int)(pts[i][0] * s + pts[i][1] * co);
        }
        lv_draw_triangle(layer, &td);
    } else {
        devos_w_draw_rect(layer, x - 4, y - 4, x + 4, y + 4, c, LV_OPA_COVER, 4);
    }
    if (s_labels || sel || a->emergency) {
        char buf[32], alt[12];
        alt_text(a, alt, sizeof(alt));
        snprintf(buf, sizeof(buf), "%s %s%s", a->flight[0] ? a->flight : a->hex, alt,
                 a->vrate_fpm > 300 ? "+" : a->vrate_fpm < -300 ? "-" : "");
        devos_w_draw_text(layer, &lv_font_montserrat_12, x + 11, y - 16, 0, buf, sel ? p->text_primary : c);
        if (a->emergency) devos_w_draw_text(layer, &lv_font_montserrat_12, x + 11, y - 2, 0, a->squawk, lv_color_hex(0xff3b3b));
    }
}

/* The radar's drawing area inside its (right-hand) border. */
static void map_area(const lv_area_t *a, lv_area_t *out)
{
    *out = *a;
    out->x2 -= lv_obj_get_style_border_width(radar, LV_PART_MAIN);
}

static void radar_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const devos_palette_t *p = devos_theme_get();
    lv_area_t a;
    lv_obj_get_coords(radar, &a);
    if (!s_st.have_pos) return;
    proj_t pj;
    proj_make(&pj, &a);
    if (s_map) {
        adsb_map_draw(layer);
        lv_area_t ma;
        map_area(&a, &ma);
        devos_w_draw_text(layer, &lv_font_montserrat_10, ma.x2 - 232, ma.y2 - 16, 0, DEVOS_MAPTILES_ATTRIBUTION,
                          p->text_muted);
        if (s_map_status[0])
            devos_w_draw_text(layer, &lv_font_montserrat_12, a.x1 + 12, a.y1 + 8, 400, s_map_status, p->text_muted);
    }
    int r = (int)(pj.scale * s_range);
    char buf[24];
    lv_color_t ring = s_map ? p->text_muted : p->surface_border;     /* stays visible over the map */
    for (int i = 1; i <= 4; i++) {
        int rr = r * i / 4;
        draw_ring(layer, pj.cx, pj.cy, rr, ring, s_map ? LV_OPA_70 : LV_OPA_COVER, 1);
        int v = s_range * i / 4;
        snprintf(buf, sizeof(buf), "%d nm", v);
        devos_w_draw_text(layer, &lv_font_montserrat_12, pj.cx + rr + 3, pj.cy + 2, 0, buf, p->text_muted);
    }
    draw_line(layer, pj.cx - r, pj.cy, pj.cx + r, pj.cy, ring, LV_OPA_50, 1);
    draw_line(layer, pj.cx, pj.cy - r, pj.cx, pj.cy + r, ring, LV_OPA_50, 1);
    devos_w_draw_text(layer, &lv_font_montserrat_14, pj.cx - 5, a.y1 + 2, 0, "N", p->text_secondary);
    devos_w_draw_rect(layer, pj.cx - 3, pj.cy - 3, pj.cx + 3, pj.cy + 3, p->accent_primary, LV_OPA_COVER, 3);
    const devos_adsb_ac_t *sel = selected();
    /* high flyers on top: draw by altitude */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < s_n; i++) {
            const devos_adsb_ac_t *ac = &s_ac[i];
            if (!ac->has_pos || ac == sel) continue;
            bool high = ac->has_alt && ac->alt_ft >= 20000;
            if (high != (pass == 1)) continue;
            draw_aircraft(layer, &pj, &a, ac, false);
        }
    }
    if (sel && sel->has_pos) draw_aircraft(layer, &pj, &a, sel, true);
    if (s_frozen) devos_w_draw_text(layer, &lv_font_montserrat_14, a.x1 + 12, a.y2 - 24, 0, LV_SYMBOL_PAUSE "  Frozen (Space)",
                                    p->accent_warning);
}

static void radar_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    lv_area_t a;
    lv_obj_get_coords(radar, &a);
    proj_t pj;
    proj_make(&pj, &a);
    int best = -1, bd = 30 * 30;
    for (int i = 0; i < s_n; i++) {
        if (!s_ac[i].has_pos) continue;
        int x, y;
        proj(&pj, s_ac[i].lat, s_ac[i].lon, &x, &y);
        int d = (x - pt.x) * (x - pt.x) + (y - pt.y) * (y - pt.y);
        if (d < bd) { bd = d; best = i; }
    }
    if (best >= 0) {
        snprintf(s_sel_hex, sizeof(s_sel_hex), "%s", s_ac[best].hex);
        s_list.sel = sel_row();
        devos_vlist_select(&s_list, s_list.sel);
    }
}

/* ------------------------------------------------------------------ list + detail */
static void row_draw(devos_vlist_t *v, lv_layer_t *layer, const lv_area_t *row, int idx)
{
    LV_UNUSED(v);
    if (idx >= s_n) return;
    const devos_palette_t *p = devos_theme_get();
    int i = s_order[idx];
    const devos_adsb_ac_t *a = &s_ac[i];
    int x = row->x1 + 10, y = row->y1 + 3;
    char buf[24];
    devos_w_draw_rect(layer, x, y + 4, x + 7, y + 11, alt_color(a), LV_OPA_COVER, 2);
    devos_w_draw_text(layer, NULL, x + 14, y, 90, a->flight[0] ? a->flight : a->hex, a->flight[0] ? p->text_primary : p->text_secondary);
    alt_text(a, buf, sizeof(buf));
    devos_w_draw_text(layer, NULL, x + 110, y, 80, buf, p->text_primary);
    if (a->has_gs) snprintf(buf, sizeof(buf), "%d kt", (int)(a->gs_kt + 0.5f));
    else snprintf(buf, sizeof(buf), "-");
    devos_w_draw_text(layer, NULL, x + 196, y, 70, buf, p->text_secondary);
    if (a->has_pos && s_st.have_pos) snprintf(buf, sizeof(buf), "%.1f nm", (double)s_dist[i]);
    else snprintf(buf, sizeof(buf), "no pos");
    devos_w_draw_text(layer, NULL, x + 272, y, 80, buf, p->text_secondary);
    devos_w_draw_text(layer, NULL, x + 358, y, 60, a->squawk, a->emergency ? lv_color_hex(0xff3b3b) : p->text_muted);
}

static void show_detail(void)
{
    const devos_adsb_ac_t *a = selected();
    if (!a) {
        devos_w_set_text(lbl_detail_title, "");
        devos_w_set_text(lbl_detail, s_n ? "Up / Down or tap the radar to pick an aircraft." : "");
        return;
    }
    char title[64], alt[16], geo[24], body[600];
    const char *cat = devos_adsb_category_name(a->category);
    snprintf(title, sizeof(title), "%s%s%s", a->flight[0] ? a->flight : a->hex, a->emergency ? "   EMERGENCY " : "",
             a->emergency ? a->squawk : "");
    alt_text(a, alt, sizeof(alt));
    if (a->alt_geom_ft) snprintf(geo, sizeof(geo), "  (GPS %d ft)", a->alt_geom_ft);
    else geo[0] = '\0';
    int i = (int)(a - s_ac);
    char pos[64] = "no position", vr[24] = "level";
    if (a->has_pos) {
        if (s_st.have_pos) snprintf(pos, sizeof(pos), "%.4f, %.4f  -  %.1f nm at %03.0f deg", a->lat, a->lon, (double)s_dist[i], (double)s_brg[i]);
        else snprintf(pos, sizeof(pos), "%.4f, %.4f", a->lat, a->lon);
    }
    if (a->vrate_fpm > 100) snprintf(vr, sizeof(vr), "climbing %d ft/min", a->vrate_fpm);
    else if (a->vrate_fpm < -100) snprintf(vr, sizeof(vr), "descending %d ft/min", -a->vrate_fpm);
    snprintf(body, sizeof(body),
             "ICAO       %s%s\nType       %s%s%s\nSquawk     %s\nAltitude   %s%s\nVertical   %s\nSpeed      %s%.0f kt\nTrack      %s%.0f deg\n"
             "Position   %s\nSignal     %.1f dBFS, seen %.1f s ago\nMessages   %u",
             a->hex, a->mlat ? "  (MLAT)" : "", cat ? cat : "-", a->category[0] ? "  " : "", a->category,
             a->squawk[0] ? a->squawk : "-", alt, geo, vr, a->has_gs ? "" : "- ", (double)a->gs_kt, a->has_track ? "" : "- ",
             (double)a->track_deg, pos, (double)a->rssi, (double)a->seen_s, (unsigned)a->messages);
    devos_w_set_text(lbl_detail_title, title);
    devos_w_track(lbl_detail_title, a->emergency ? DEVOS_W_TEXT_ERR : DEVOS_W_TEXT_ACCENT);
    devos_w_set_text(lbl_detail, body);
}

static void on_select(devos_vlist_t *v, int idx)
{
    LV_UNUSED(v);
    if (idx >= 0 && idx < s_n) snprintf(s_sel_hex, sizeof(s_sel_hex), "%s", s_ac[s_order[idx]].hex);
    show_detail();
    lv_obj_invalidate(radar);
}

/* ------------------------------------------------------------------ data */
static void ingest(void)
{
    devos_adsb_status(&s_st);
    if (s_frozen) return;
    s_n = devos_adsb_list(s_ac, DEVOS_ADSB_MAX);
    for (int i = 0; i < s_n; i++) {
        s_order[i] = i;
        s_dist[i] = 1e9f;
        s_brg[i] = 0;
        if (s_ac[i].has_pos && s_st.have_pos) devos_adsb_range_bearing(s_st.lat, s_st.lon, s_ac[i].lat, s_ac[i].lon, &s_dist[i], &s_brg[i]);
    }
    qsort(s_order, (size_t)s_n, sizeof(int), cmp_dist);
    devos_vlist_set_count(&s_list, s_n);
    s_list.sel = sel_row();
    show_detail();
    lv_obj_invalidate(radar);
}

static void status_line(void)
{
    char buf[240];
    if (!s_st.configured) snprintf(buf, sizeof(buf), "Not set up - press C");
    else if (s_st.error[0]) snprintf(buf, sizeof(buf), "%s", s_st.error);
    else if (!s_st.updated) snprintf(buf, sizeof(buf), "Connecting to the receiver...");
    else if (!s_st.have_pos) snprintf(buf, sizeof(buf), "%d aircraft - set the receiver's position (C) to see them on the radar", s_st.total);
    else snprintf(buf, sizeof(buf), "%d aircraft, %d with a position  -  %.0f msg/s  -  furthest %.0f nm  -  %d nm range%s",
                  s_st.total, s_st.with_pos, (double)s_st.msg_rate, (double)s_st.max_range_nm, s_range,
                  (int64_t)time(NULL) - s_st.updated > 5 ? "  -  STALE" : "");
    devos_w_set_text(lbl_status, buf);
    devos_w_track(lbl_status, s_st.error[0] ? DEVOS_W_TEXT_ERR : DEVOS_W_TEXT_DIM);
    if (s_st.configured && s_st.have_pos) lv_obj_add_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_remove_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
        devos_w_set_text(lbl_empty, !s_st.configured ? "Press C and enter your receiver's aircraft.json URL."
                                                     : "Waiting for the receiver's position\n(from receiver.json, or set it with C).");
    }
}

/* ------------------------------------------------------------------ settings */
static void dlg_open(void)
{
    devos_adsb_config_t c;
    devos_adsb_get_config(&c);
    lv_textarea_set_text(ta_url, c.url);
    char b[24] = "";
    if (c.lat || c.lon) snprintf(b, sizeof(b), "%.5f", c.lat);
    lv_textarea_set_text(ta_lat, b);
    b[0] = '\0';
    if (c.lat || c.lon) snprintf(b, sizeof(b), "%.5f", c.lon);
    lv_textarea_set_text(ta_lon, b);
    int sel = 4;
    for (int i = 0; i < NRANGES; i++) if (RANGES[i] == c.range_nm) sel = i;
    lv_dropdown_set_selected(dd_range, (uint32_t)sel);
    if (c.map) lv_obj_add_state(cb_map, LV_STATE_CHECKED);
    else lv_obj_remove_state(cb_map, LV_STATE_CHECKED);
    devos_w_set_text(s_dlg.msg, "");
    devos_w_dialog_show(&s_dlg, true);
    devos_focus_set(&s_fdlg, ta_url);
}

static void dlg_ok(void)
{
    devos_adsb_config_t c;
    memset(&c, 0, sizeof(c));
    const char *u = lv_textarea_get_text(ta_url);
    while (*u == ' ') u++;
    if (!*u) {
        devos_w_set_text(s_dlg.msg, "Enter the aircraft.json URL");
        return;
    }
    if (!strstr(u, "://")) snprintf(c.url, sizeof(c.url), "http://%s", u);
    else snprintf(c.url, sizeof(c.url), "%s", u);
    const char *la = lv_textarea_get_text(ta_lat), *lo = lv_textarea_get_text(ta_lon);
    if (la[0] || lo[0]) {
        c.lat = atof(la);
        c.lon = atof(lo);
        if (c.lat < -90 || c.lat > 90 || c.lon < -180 || c.lon > 180 || (!c.lat && !c.lon)) {
            devos_w_set_text(s_dlg.msg, "Latitude -90..90, longitude -180..180 (or leave both blank)");
            return;
        }
    }
    c.range_nm = RANGES[lv_dropdown_get_selected(dd_range)];
    c.map = lv_obj_has_state(cb_map, LV_STATE_CHECKED);
    s_range = c.range_nm;
    s_map = c.map;
    devos_adsb_set_config(&c);
    devos_w_dialog_show(&s_dlg, false);
    devos_focus_clear(&s_fdlg);
    s_gen = 0xffffffff;
}

static void dlg_ok_cb(lv_event_t *e) { LV_UNUSED(e); dlg_ok(); }
static void dlg_cancel_cb(lv_event_t *e) { LV_UNUSED(e); devos_w_dialog_show(&s_dlg, false); devos_focus_clear(&s_fdlg); }
static void set_cb(lv_event_t *e) { LV_UNUSED(e); dlg_open(); }

static void zoom(int dir)
{
    int i;
    for (i = 0; i < NRANGES && RANGES[i] < s_range; i++) {}
    i += dir;
    if (i < 0) i = 0;
    if (i >= NRANGES) i = NRANGES - 1;
    s_range = RANGES[i];
    lv_obj_invalidate(radar);
}
static void zin_cb(lv_event_t *e) { LV_UNUSED(e); zoom(-1); }
static void zout_cb(lv_event_t *e) { LV_UNUSED(e); zoom(1); }

/* ------------------------------------------------------------------ tick + keys */
static const char *keys_text(void)
{
    if (devos_w_dialog_open(&s_dlg)) return "Tab / arrows move    Enter saves    Esc cancels";
    return "Up / Down pick    + / - zoom    0 set range    L labels    T trails    M map    Space freeze    C settings    Esc home";
}

/* Keep the map underlay in step with the radar's view. */
static void map_tick(void)
{
    if (!s_map || !s_st.have_pos) {
        adsb_map_stop();
        s_map_status[0] = '\0';
        return;
    }
    lv_area_t a, ma;
    lv_obj_get_coords(radar, &a);
    if (lv_area_get_width(&a) <= 1) return;         /* not laid out yet */
    proj_t pj;
    proj_make(&pj, &a);
    map_area(&a, &ma);
    adsb_map_view_t v = { pj.cx, pj.cy, pj.scale, pj.lat0, pj.lon0 };
    bool changed = adsb_map_update(&ma, &v);
    if (strcmp(s_map_status, adsb_map_status())) {
        snprintf(s_map_status, sizeof(s_map_status), "%s", adsb_map_status());
        changed = true;
    }
    if (changed) lv_obj_invalidate(radar);
}

static void set_map(bool on)
{
    s_map = on;
    devos_adsb_set_map(on);
    if (!on) adsb_map_stop();
    s_map_status[0] = '\0';
    lv_obj_invalidate(radar);
}

static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!s_screen || lv_obj_has_flag(s_screen, LV_OBJ_FLAG_HIDDEN)) return;
    uint32_t g = devos_adsb_generation();
    if (g != s_gen) {
        s_gen = g;
        ingest();
    }
    bool emerg = false;
    for (int i = 0; i < s_n && !emerg; i++) emerg = s_ac[i].emergency;
    if (emerg) {
        s_blink = (lv_tick_get() / 500) % 2;
        lv_obj_invalidate(radar);
    }
    map_tick();
    status_line();
    devos_w_set_text(s_keys, keys_text());
}

static bool adsb_key(uint32_t key, uint8_t mods)
{
    if (devos_w_dialog_open(&s_dlg)) {
        if (key == LV_KEY_ESC && !s_fdlg.dd_open) dlg_cancel_cb(NULL);
        else if (devos_focus_key(&s_fdlg, key, mods)) {}
        else if (key == '\r' || key == '\n') dlg_ok();
        return true;
    }
    if (mods & (DEVOS_MOD_CTRL | DEVOS_MOD_ALT | DEVOS_MOD_FN)) return false;
    if (devos_vlist_key(&s_list, key)) return true;
    switch (key) {
    case '+': case '=': zoom(-1); return true;
    case '-': case '_': zoom(1); return true;
    case '0': {
        devos_adsb_config_t c;
        devos_adsb_get_config(&c);
        s_range = c.range_nm;
        lv_obj_invalidate(radar);
        return true;
    }
    case 'l': case 'L': s_labels = !s_labels; lv_obj_invalidate(radar); return true;
    case 't': case 'T': s_trails = !s_trails; lv_obj_invalidate(radar); return true;
    case 'm': case 'M': set_map(!s_map); return true;
    case ' ':
        s_frozen = !s_frozen;
        if (!s_frozen) s_gen = 0xffffffff;
        lv_obj_invalidate(radar);
        return true;
    case 'c': case 'C': dlg_open(); return true;
    case LV_KEY_ESC:
        if (s_sel_hex[0]) {
            s_sel_hex[0] = '\0';
            s_list.sel = -1;
            devos_vlist_redraw(&s_list);
            show_detail();
            lv_obj_invalidate(radar);
            return true;
        }
        return false;
    default: return key >= 32 && key < 127;
    }
}

/* ------------------------------------------------------------------ init */
static void adsb_init(void)
{
    if (s_inited) return;
    s_inited = true;
    devos_adsb_init();
    devos_adsb_config_t c;
    devos_adsb_get_config(&c);
    s_range = c.range_nm;
    s_map = c.map;
    s_ac = calloc(DEVOS_ADSB_MAX, sizeof(*s_ac));

    s_screen = devos_w_screen(&s_desc);
    lv_obj_t *bar = devos_w_bar(s_screen, LV_SYMBOL_GPS "  ADS-B", NULL);
    lbl_status = devos_w_label(bar, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "");
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_status, 860);
    lv_obj_align(lbl_status, LV_ALIGN_LEFT_MID, 118, 0);
    lv_obj_t *bset = devos_w_btn(bar, LV_SYMBOL_SETTINGS " Settings", 90, set_cb, NULL, NULL);
    lv_obj_align(bset, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_t *bzo = devos_w_btn(bar, LV_SYMBOL_MINUS, 36, zout_cb, NULL, NULL);
    lv_obj_align_to(bzo, bset, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    lv_obj_t *bzi = devos_w_btn(bar, LV_SYMBOL_PLUS, 36, zin_cb, NULL, NULL);
    lv_obj_align_to(bzi, bzo, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    int h = DEVOS_CONTENT_HEIGHT - DEVOS_W_BAR_H - DEVOS_W_KEYS_H;
    radar = devos_w_panel(s_screen, 0, DEVOS_W_BAR_H, RADAR_W, h, DEVOS_W_CODE);
    lv_obj_set_style_border_side(radar, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_add_flag(radar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(radar, radar_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(radar, radar_click_cb, LV_EVENT_CLICKED, NULL);
    lbl_empty = devos_w_label(radar, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM, "");
    lv_obj_set_style_text_align(lbl_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_empty, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *hdr = devos_w_label(s_screen, devos_w_mono(), DEVOS_W_TEXT_MUTED, "   FLIGHT       ALT        SPEED     DIST      SQWK");
    lv_obj_set_pos(hdr, SIDE_X + 6, DEVOS_W_BAR_H + 6);
    lv_obj_t *lb = devos_w_panel(s_screen, SIDE_X, DEVOS_W_BAR_H + 26, SIDE_W, LIST_H, DEVOS_W_CODE);
    lv_obj_set_style_radius(lb, 6, 0);
    devos_vlist_create(&s_list, lb, 22, row_draw);
    s_list.on_select = on_select;
    s_list.active = true;
    lv_obj_set_pos(s_list.scroll, 2, 2);
    lv_obj_set_size(s_list.scroll, SIDE_W - 4, LIST_H - 4);
    lv_obj_t *dp = devos_w_panel(s_screen, SIDE_X, DEVOS_W_BAR_H + 34 + LIST_H, SIDE_W, h - LIST_H - 40, DEVOS_W_PANEL);
    lv_obj_set_style_radius(dp, 6, 0);
    lbl_detail_title = devos_w_label(dp, &lv_font_montserrat_20, DEVOS_W_TEXT_ACCENT, "");
    lv_obj_set_pos(lbl_detail_title, 14, 10);
    lbl_detail = devos_w_label(dp, devos_w_mono(), DEVOS_W_TEXT, "");
    lv_obj_set_pos(lbl_detail, 14, 42);

    s_keys = devos_w_keys(s_screen);

    devos_w_dialog(&s_dlg, s_screen, 720, 400, LV_SYMBOL_GPS "  ADS-B receiver");
    ta_url = devos_w_field(s_dlg.box, "aircraft.json URL", 0, 34, 676);
    lv_textarea_set_placeholder_text(ta_url, "http://piaware.local:8080/data/aircraft.json");
    ta_lat = devos_w_field(s_dlg.box, "Receiver latitude (blank = ask receiver.json)", 0, 96, 320);
    ta_lon = devos_w_field(s_dlg.box, "Longitude", 336, 96, 200);
    lv_textarea_set_accepted_chars(ta_lat, "0123456789.-");
    lv_textarea_set_accepted_chars(ta_lon, "0123456789.-");
    lv_obj_t *lr = devos_w_label(s_dlg.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Range");
    lv_obj_set_pos(lr, 552, 96);
    dd_range = devos_w_dd(s_dlg.box, "5 nm\n10 nm\n25 nm\n50 nm\n100 nm\n150 nm\n200 nm\n300 nm\n400 nm", 124);
    lv_obj_set_pos(dd_range, 552, 114);
    lv_obj_t *hint = devos_w_label(s_dlg.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM,
                                   "PiAware / dump1090-fa:   http://piaware.local:8080/data/aircraft.json\n"
                                   "    (or http://piaware.local/skyaware/data/aircraft.json)\n"
                                   "tar1090 / readsb:           http://<host>/tar1090/data/aircraft.json\n"
                                   "ultrafeeder / adsb.im:      http://<host>:8080/data/aircraft.json");
    lv_obj_set_pos(hint, 0, 166);
    cb_map = devos_w_cb(s_dlg.box, "Map underlay (OpenStreetMap; tiles are cached on the SD card)");
    lv_obj_set_pos(cb_map, 0, 246);
    lv_obj_t *bok = devos_w_btn_kind(s_dlg.box, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_OK "  Save", 120, dlg_ok_cb, NULL, NULL);
    lv_obj_set_size(bok, 120, 36);
    lv_obj_align(bok, LV_ALIGN_BOTTOM_RIGHT, -132, 0);
    lv_obj_t *bc = devos_w_btn(s_dlg.box, "Cancel", 120, dlg_cancel_cb, NULL, NULL);
    lv_obj_set_size(bc, 120, 36);
    lv_obj_align(bc, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    devos_focus_init(&s_fdlg);
    lv_obj_t *order[] = { ta_url, ta_lat, ta_lon, dd_range, cb_map, bok, bc };
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) devos_focus_add(&s_fdlg, order[i]);

    lv_timer_create(tick_cb, 250, NULL);
}

static void adsb_show(void)
{
    devos_adsb_set_active(true);
    s_gen = 0xffffffff;
    if (!devos_adsb_configured()) dlg_open();
}

static void adsb_hide(void)
{
    devos_adsb_set_active(false);
    adsb_map_stop();
}

static int adsb_telemetry(char lines[3][64])
{
    devos_adsb_status_t st;
    devos_adsb_status(&st);
    if (!st.configured) {
        snprintf(lines[0], 64, "* Not set up");
        snprintf(lines[1], 64, "* dump1090 / readsb / tar1090");
        return 2;
    }
    if (st.updated) {
        snprintf(lines[0], 64, "* %d aircraft (%d on radar)", st.total, st.with_pos);
        snprintf(lines[1], 64, "* furthest %.0f nm", (double)st.max_range_nm);
        return 2;
    }
    snprintf(lines[0], 64, "* Opens to track aircraft");
    return 1;
}

/* Sym+S sheet (devos_shortcuts.h) */
static const char *adsb_shortcuts(void)
{
    return
        "Radar\n"
        "Up / Down\tPick an aircraft\n"
        "+ / -\tZoom in / out\n"
        "0\tBack to the set range\n"
        "L\tLabels\n"
        "T\tTrails\n"
        "M\tMap underlay\n"
        "Space\tFreeze\n"
        "C\tSettings: feed URL, position, range\n"
        "Esc\tClear the pick, then Home\n";
}

devos_app_descriptor_t *app_adsb_get_descriptor(void)
{
    s_desc.id = DEVOS_APP_LAUNCHER;                 /* auto-assigned */
    s_desc.uid = "adsb";
    s_desc.icon = LV_SYMBOL_GPS;
    s_desc.draw_icon = devos_icon_adsb;
    s_desc.category = "network";
    s_desc.name = "ADS-B";
    s_desc.title = "ADS-B Radar";
    s_desc.subtitle = "Aircraft from your receiver";
    s_desc.init = adsb_init;
    s_desc.show = adsb_show;
    s_desc.hide = adsb_hide;
    s_desc.handle_key = adsb_key;
    s_desc.get_telemetry_lines = adsb_telemetry;
    s_desc.get_shortcuts = adsb_shortcuts;
    return &s_desc;
}
