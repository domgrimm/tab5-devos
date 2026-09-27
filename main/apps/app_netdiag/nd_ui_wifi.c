/* Network > Wi-Fi survey: every access point radio the C6 hears (2.4 GHz),
 * the classic overlapping-channel graph and a per-channel waterfall of signal
 * over time. Rescans every 3 s while shown (Space pauses). SNR is estimated
 * against a nominal -95 dBm noise floor: the radio doesn't report noise. */
#include "app_netdiag_int.h"
#include "devos_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WF_ROWS   64
#define NOISE_DBM (-95)
#define RESCAN_MS 3000

static lv_obj_t *lbl_info, *graph, *wf, *btn_pause, *lbl_pause, *btn_sort, *lbl_sort, *btn_now;
static devos_vlist_t s_list;
static devos_wifi_bss_t s_bss[DEVOS_WIFI_MAX_BSS];
static int s_n, s_order[DEVOS_WIFI_MAX_BSS];
static int8_t s_wf[WF_ROWS][14];
static int s_wf_n, s_wf_head;
static uint32_t s_gen = 0xffffffff, s_next_scan;
static bool s_paused, s_visible;
static int s_sort;                              /* 0 signal, 1 channel, 2 name */
static uint8_t s_sel_bssid[6];
static bool s_have_sel;

static const char *SORTS[] = { "signal", "channel", "name" };

static const char *auth_name(uint8_t a)
{
    static const char *n[] = { "open", "WEP", "WPA", "WPA2", "WPA/2", "WPA2-E", "WPA3", "WPA2/3", "WAPI", "OWE",
                               "WPA3-E", "WPA3", "WPA3", "DPP", "WPA3-E", "WPA2/3-E" };
    return a < sizeof(n) / sizeof(n[0]) ? n[a] : "?";
}

static void phy_name(uint8_t phy, char *out, size_t cap)
{
    snprintf(out, cap, "%s%s%s%s", phy & DEVOS_WIFI_PHY_B ? "b" : "", phy & DEVOS_WIFI_PHY_G ? "g" : "",
             phy & DEVOS_WIFI_PHY_N ? "n" : "", phy & DEVOS_WIFI_PHY_AX ? "ax" : "");
}

static lv_color_t bss_color(const devos_wifi_bss_t *b)
{
    const devos_palette_t *p = devos_theme_get();
    static const int pick[] = { 9, 10, 11, 12, 13, 14, 1, 2, 3, 4, 5, 6 };
    unsigned h = 0;
    for (int i = 0; i < 6; i++) h = h * 31 + b->bssid[i];
    return p->ansi[pick[h % (sizeof(pick) / sizeof(pick[0]))]];
}

/* centre channel and half-width (in channels) of a BSS's signal */
static void span(const devos_wifi_bss_t *b, float *centre, float *half)
{
    *centre = (float)b->channel + (b->second > 0 ? 2.0f : b->second < 0 ? -2.0f : 0.0f);
    *half = b->second ? 4.0f : 2.0f;
}

static int cmp_signal(const void *a, const void *b)
{
    return s_bss[*(const int *)b].rssi - s_bss[*(const int *)a].rssi;
}
static int cmp_channel(const void *a, const void *b)
{
    const devos_wifi_bss_t *x = &s_bss[*(const int *)a], *y = &s_bss[*(const int *)b];
    return x->channel != y->channel ? x->channel - y->channel : y->rssi - x->rssi;
}
static int cmp_name(const void *a, const void *b)
{
    const devos_wifi_bss_t *x = &s_bss[*(const int *)a], *y = &s_bss[*(const int *)b];
    if (!x->ssid[0] != !y->ssid[0]) return x->ssid[0] ? -1 : 1;
    int c = strcasecmp(x->ssid, y->ssid);
    return c ? c : y->rssi - x->rssi;
}

static void sort_list(void)
{
    for (int i = 0; i < s_n; i++) s_order[i] = i;
    qsort(s_order, (size_t)s_n, sizeof(int), s_sort == 0 ? cmp_signal : s_sort == 1 ? cmp_channel : cmp_name);
    /* keep the selection on the same radio */
    s_list.sel = -1;
    if (s_have_sel) {
        for (int i = 0; i < s_n; i++) {
            if (!memcmp(s_bss[s_order[i]].bssid, s_sel_bssid, 6)) { s_list.sel = i; break; }
        }
    }
}

static const devos_wifi_bss_t *selected(void)
{
    return s_list.sel >= 0 && s_list.sel < s_n ? &s_bss[s_order[s_list.sel]] : NULL;
}

/* ---- list ---- */
static void row_draw(devos_vlist_t *v, lv_layer_t *layer, const lv_area_t *row, int idx)
{
    LV_UNUSED(v);
    if (idx >= s_n) return;
    const devos_palette_t *p = devos_theme_get();
    const devos_wifi_bss_t *b = &s_bss[s_order[idx]];
    int y = row->y1 + 3, x = row->x1 + 8;
    char buf[40];
    devos_w_draw_rect(layer, x, y + 3, x + 7, y + 12, bss_color(b), LV_OPA_COVER, 2);
    devos_w_draw_text(layer, NULL, x + 14, y, 172, b->ssid[0] ? b->ssid : "(hidden)", b->ssid[0] ? p->text_primary : p->text_muted);
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3],
             b->bssid[4], b->bssid[5]);
    devos_w_draw_text(layer, NULL, x + 200, y, 140, buf, p->text_secondary);
    snprintf(buf, sizeof(buf), "%d%s", b->channel, b->second > 0 ? "+" : b->second < 0 ? "-" : "");
    devos_w_draw_text(layer, NULL, x + 346, y, 32, buf, p->text_primary);
    snprintf(buf, sizeof(buf), "%d", b->rssi);
    lv_color_t sc = b->rssi >= -60 ? p->accent_secondary : b->rssi >= -75 ? p->accent_warning : p->accent_danger;
    devos_w_draw_text(layer, NULL, x + 382, y, 34, buf, sc);
    int bw = (b->rssi + 100) * 40 / 60;
    if (bw < 1) bw = 1;
    if (bw > 40) bw = 40;
    devos_w_draw_rect(layer, x + 418, y + 5, x + 458, y + 11, p->surface_border, LV_OPA_COVER, 2);
    devos_w_draw_rect(layer, x + 418, y + 5, x + 418 + bw, y + 11, sc, LV_OPA_COVER, 2);
    snprintf(buf, sizeof(buf), "%d", b->rssi - NOISE_DBM);
    devos_w_draw_text(layer, NULL, x + 466, y, 30, buf, p->text_secondary);
    devos_w_draw_text(layer, NULL, x + 498, y, 58, auth_name(b->authmode), b->authmode ? p->text_secondary : p->accent_warning);
    phy_name(b->phy, buf, sizeof(buf));
    devos_w_draw_text(layer, NULL, x + 554, y, 44, buf, p->text_muted);
}

static void on_select(devos_vlist_t *v, int idx)
{
    LV_UNUSED(v);
    if (idx >= 0 && idx < s_n) {
        memcpy(s_sel_bssid, s_bss[s_order[idx]].bssid, 6);
        s_have_sel = true;
    }
    lv_obj_invalidate(graph);
}

/* ---- channel graph ---- */
#define CH_MIN (-1.0f)
#define CH_MAX (16.0f)
static int ch_x(const lv_area_t *a, float ch) { return a->x1 + 44 + (int)((ch - CH_MIN) * (float)(a->x2 - a->x1 - 56) / (CH_MAX - CH_MIN)); }
static int db_y(const lv_area_t *a, int db)
{
    if (db < -100) db = -100;
    if (db > -20) db = -20;
    return a->y2 - 26 - (db + 100) * (a->y2 - a->y1 - 46) / 80;
}

static void draw_hump(lv_layer_t *layer, const lv_area_t *a, const devos_wifi_bss_t *b, bool sel)
{
    float c, h;
    span(b, &c, &h);
    lv_color_t col = bss_color(b);
    int yb = db_y(a, -100), yt = db_y(a, b->rssi);
    int xa = ch_x(a, c - h), xb = ch_x(a, c - h * 0.7f), xc = ch_x(a, c + h * 0.7f), xd = ch_x(a, c + h);
    lv_draw_triangle_dsc_t td;
    lv_draw_triangle_dsc_init(&td);
    td.bg_color = col;
    td.bg_opa = sel ? LV_OPA_50 : LV_OPA_20;
    td.p[0].x = xa; td.p[0].y = yb; td.p[1].x = xb; td.p[1].y = yt; td.p[2].x = xd; td.p[2].y = yb;
    lv_draw_triangle(layer, &td);
    td.p[0].x = xb; td.p[0].y = yt; td.p[1].x = xc; td.p[1].y = yt; td.p[2].x = xd; td.p[2].y = yb;
    lv_draw_triangle(layer, &td);
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = col;
    ld.width = sel ? 3 : 1;
    ld.opa = LV_OPA_COVER;
    int pts[4][2] = { { xa, yb }, { xb, yt }, { xc, yt }, { xd, yb } };
    for (int i = 0; i < 3; i++) {
        ld.p1.x = pts[i][0]; ld.p1.y = pts[i][1]; ld.p2.x = pts[i + 1][0]; ld.p2.y = pts[i + 1][1];
        lv_draw_line(layer, &ld);
    }
    int lw = 110;
    devos_w_draw_text(layer, &lv_font_montserrat_12, (xb + xc) / 2 - lw / 2 + 4, yt - 16, lw, b->ssid[0] ? b->ssid : "(hidden)",
                      sel ? devos_theme_get()->text_primary : col);
}

static void graph_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const devos_palette_t *p = devos_theme_get();
    lv_area_t a;
    lv_obj_get_coords(graph, &a);
    char buf[16];
    for (int db = -90; db <= -30; db += 20) {
        int y = db_y(&a, db);
        devos_w_draw_rect(layer, a.x1 + 44, y, a.x2 - 12, y, p->surface_border, LV_OPA_COVER, 0);
        snprintf(buf, sizeof(buf), "%d", db);
        devos_w_draw_text(layer, &lv_font_montserrat_12, a.x1 + 8, y - 7, 34, buf, p->text_muted);
    }
    for (int ch = 1; ch <= 14; ch++) {
        int x = ch_x(&a, (float)ch);
        snprintf(buf, sizeof(buf), "%d", ch);
        bool main_ch = ch == 1 || ch == 6 || ch == 11;
        devos_w_draw_text(layer, &lv_font_montserrat_12, x - 6, a.y2 - 20, 20, buf, main_ch ? p->text_primary : p->text_muted);
    }
    const devos_wifi_bss_t *sel = selected();
    for (int i = s_n - 1; i >= 0; i--) {         /* weakest first, strongest on top */
        const devos_wifi_bss_t *b = &s_bss[s_order[i]];
        if (b != sel) draw_hump(layer, &a, b, false);
    }
    if (sel) draw_hump(layer, &a, sel, true);
    devos_w_draw_text(layer, &lv_font_montserrat_12, a.x1 + 8, a.y1 + 6, 200, "dBm", p->text_muted);
}

/* ---- waterfall ---- */
static lv_color_t heat(int8_t v)
{
    const devos_palette_t *p = devos_theme_get();
    if (v <= -100) return p->code_bg;
    static const uint32_t stops[] = { 0x1e3a8a, 0x0ea5e9, 0x10b981, 0xf59e0b, 0xef4444 };
    float t = (float)(v + 95) / 45.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    if (t < 0.08f) return lv_color_mix(lv_color_hex(stops[0]), p->code_bg, (uint8_t)(t / 0.08f * 255));
    float f = t * 4.0f;
    int i = (int)f;
    if (i >= 4) return lv_color_hex(stops[4]);
    return lv_color_mix(lv_color_hex(stops[i + 1]), lv_color_hex(stops[i]), (uint8_t)((f - (float)i) * 255));
}

static void wf_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const devos_palette_t *p = devos_theme_get();
    lv_area_t a;
    lv_obj_get_coords(wf, &a);
    int x0 = a.x1 + 44, x1 = a.x2 - 12, y0 = a.y1 + 22, y1 = a.y2 - 6;
    float cw = (float)(x1 - x0) / 14.0f;
    float rh = (float)(y1 - y0) / WF_ROWS;
    for (int r = 0; r < s_wf_n; r++) {                       /* newest at the top */
        int8_t *row = s_wf[(s_wf_head - 1 - r + WF_ROWS) % WF_ROWS];
        int ry = y0 + (int)(r * rh), ry2 = y0 + (int)((r + 1) * rh) - 1;
        if (ry2 < ry) ry2 = ry;
        for (int c = 0; c < 14; c++) {
            int cx = x0 + (int)(c * cw), cx2 = x0 + (int)((c + 1) * cw) - 1;
            devos_w_draw_rect(layer, cx, ry, cx2, ry2, heat(row[c]), LV_OPA_COVER, 0);
        }
    }
    char buf[8];
    for (int c = 0; c < 14; c++) {
        snprintf(buf, sizeof(buf), "%d", c + 1);
        devos_w_draw_text(layer, &lv_font_montserrat_12, x0 + (int)(c * cw + cw / 2) - 6, a.y1 + 4, 20, buf,
                          c == 0 || c == 5 || c == 10 ? p->text_primary : p->text_muted);
    }
    devos_w_draw_text(layer, &lv_font_montserrat_12, a.x1 + 6, a.y1 + 4, 40, "now", p->text_muted);
    devos_w_draw_text(layer, &lv_font_montserrat_12, a.x1 + 6, a.y2 - 20, 40, "-3m", p->text_muted);
}

/* ---- data ---- */
static void ingest(void)
{
    s_n = devos_net_wifi_bss_results(s_bss, DEVOS_WIFI_MAX_BSS);
    sort_list();
    devos_vlist_set_count(&s_list, s_n);
    /* waterfall row: the strongest signal overlapping each channel */
    int8_t *row = s_wf[s_wf_head];
    for (int c = 0; c < 14; c++) row[c] = -127;
    for (int i = 0; i < s_n; i++) {
        float cen, half;
        span(&s_bss[i], &cen, &half);
        for (int c = 0; c < 14; c++) {
            float d = (float)(c + 1) - cen;
            if (d < 0) d = -d;
            if (d < half && s_bss[i].rssi > row[c]) row[c] = s_bss[i].rssi;
        }
    }
    s_wf_head = (s_wf_head + 1) % WF_ROWS;
    if (s_wf_n < WF_ROWS) s_wf_n++;
    lv_obj_invalidate(graph);
    lv_obj_invalidate(wf);
}

static void info(void)
{
    char buf[240], conn[80] = "not connected";
    devos_wifi_status_t st;
    if (devos_net_wifi_get_status(&st) == 0 && st.connected) {
        int ch = 0;
        for (int i = 0; i < s_n; i++) {
            if (!strcmp(s_bss[i].ssid, st.ssid) && (!ch || s_bss[i].rssi > -128)) { ch = s_bss[i].channel; break; }
        }
        if (ch) snprintf(conn, sizeof(conn), "on %.24s, channel %d (%d dBm)", st.ssid, ch, st.rssi);
        else snprintf(conn, sizeof(conn), "on %.24s (%d dBm)", st.ssid, st.rssi);
    }
    /* least crowded of 1 / 6 / 11: summed (linear) power overlapping it */
    const int mains[3] = { 1, 6, 11 };
    double best = 1e30;
    int best_ch = 0;
    for (int m = 0; m < 3; m++) {
        double sum = 0;
        for (int i = 0; i < s_n; i++) {
            float cen, half;
            span(&s_bss[i], &cen, &half);
            float d = (float)mains[m] - cen;
            if (d < 0) d = -d;
            if (d < half) {
                double lin = 1.0;
                for (int k = s_bss[i].rssi; k < 0; k += 10) lin /= 10.0;   /* 10^(rssi/10), roughly */
                sum += lin * (1.0 - d / half);
            }
        }
        if (sum < best) { best = sum; best_ch = mains[m]; }
    }
    int ssids = 0;
    for (int i = 0; i < s_n; i++) {
        bool dup = false;
        for (int k = 0; k < i && !dup; k++) dup = s_bss[k].ssid[0] && !strcmp(s_bss[k].ssid, s_bss[i].ssid);
        if (!dup) ssids++;
    }
    snprintf(buf, sizeof(buf), "%d network%s, %d radio%s  -  %s  -  least crowded: channel %d  -  %s",
             ssids, ssids == 1 ? "" : "s", s_n, s_n == 1 ? "" : "s", conn, best_ch,
             s_paused ? "paused" : devos_net_wifi_scan_busy() ? "scanning..." : "rescans every 3 s");
    devos_w_set_text(lbl_info, buf);
}

static void pause_cb(lv_event_t *e) { LV_UNUSED(e); s_paused = !s_paused; s_next_scan = 0; }
static void sort_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_sort = (s_sort + 1) % 3;
    sort_list();
    devos_vlist_redraw(&s_list);
}
static void now_cb(lv_event_t *e) { LV_UNUSED(e); s_next_scan = 0; if (devos_net_wifi_scan_start() != 0) nd_flash("Wi-Fi is off"); }

static void create(lv_obj_t *parent)
{
    lbl_info = devos_w_label(parent, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "");
    lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_info, 900);
    lv_obj_set_pos(lbl_info, 20, 12);
    btn_now = devos_w_btn(parent, LV_SYMBOL_REFRESH " Scan now", 96, now_cb, NULL, NULL);
    lv_obj_set_pos(btn_now, 936, 6);
    btn_sort = devos_w_btn(parent, "Sort: signal", 110, sort_cb, NULL, &lbl_sort);
    lv_obj_set_pos(btn_sort, 1038, 6);
    btn_pause = devos_w_btn(parent, LV_SYMBOL_PAUSE " Pause", 104, pause_cb, NULL, &lbl_pause);
    lv_obj_set_pos(btn_pause, 1154, 6);
    lv_obj_t *hdr = devos_w_label(parent, devos_w_mono(), DEVOS_W_TEXT_MUTED,
                                  "   SSID                  BSSID              CH   dBm         SNR SECURITY PHY");
    lv_obj_set_pos(hdr, 24, 42);
    lv_obj_t *box = devos_w_panel(parent, 16, 62, 612, ND_VIEW_H - 68, DEVOS_W_CODE);
    lv_obj_set_style_radius(box, 6, 0);
    devos_vlist_create(&s_list, box, ND_ROW_H, row_draw);
    s_list.on_select = on_select;
    s_list.active = true;
    lv_obj_set_pos(s_list.scroll, 2, 2);
    lv_obj_set_size(s_list.scroll, 608, ND_VIEW_H - 72);
    graph = devos_w_panel(parent, 640, 42, 624, 300, DEVOS_W_CODE);
    lv_obj_set_style_radius(graph, 6, 0);
    lv_obj_add_event_cb(graph, graph_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    wf = devos_w_panel(parent, 640, 350, 624, ND_VIEW_H - 356, DEVOS_W_CODE);
    lv_obj_set_style_radius(wf, 6, 0);
    lv_obj_add_event_cb(wf, wf_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    for (int r = 0; r < WF_ROWS; r++) for (int c = 0; c < 14; c++) s_wf[r][c] = -127;
}

static void show(void)
{
    s_visible = true;
    s_next_scan = 0;
    s_gen = 0xffffffff;
}

static void hide(void) { s_visible = false; }

static bool key(uint32_t k, uint8_t mods)
{
    if (mods & (DEVOS_MOD_CTRL | DEVOS_MOD_ALT | DEVOS_MOD_FN)) return false;
    if (devos_vlist_key(&s_list, k)) return true;
    switch (k) {
    case ' ': pause_cb(NULL); return true;
    case 's': case 'S': sort_cb(NULL); return true;
    case 'r': case 'R': now_cb(NULL); return true;
    case LV_KEY_ESC:
        if (s_list.sel >= 0) {
            s_list.sel = -1;
            s_have_sel = false;
            devos_vlist_redraw(&s_list);
            lv_obj_invalidate(graph);
            return true;
        }
        return false;
    default: return k >= 32 && k < 127;
    }
}

static void tick(void)
{
    if (!s_visible) return;
    uint32_t g = devos_net_wifi_scan_generation();
    if (g != s_gen) {
        bool first = s_gen == 0xffffffff;
        s_gen = g;
        if (!first || s_n == 0) ingest();
        s_next_scan = lv_tick_get() + RESCAN_MS;
    }
    if (!s_paused && !devos_net_wifi_scan_busy() && (!s_next_scan || (int32_t)(lv_tick_get() - s_next_scan) >= 0)) {
        s_next_scan = lv_tick_get() + RESCAN_MS * 2;       /* until the results land */
        devos_net_wifi_scan_start();
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "Sort: %s", SORTS[s_sort]);
    devos_w_set_text(lbl_sort, buf);
    devos_w_set_text(lbl_pause, s_paused ? LV_SYMBOL_PLAY " Resume" : LV_SYMBOL_PAUSE " Pause");
    info();
}

static const char *keys(void)
{
    return "Up / Down pick a radio    Space pause    S sort    R scan now    Esc clears the pick";
}

const nd_view_t nd_view_wifi = { "Wi-Fi survey", create, show, hide, key, tick, keys };
