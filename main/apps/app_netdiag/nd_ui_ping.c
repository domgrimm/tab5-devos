/* Network > Ping: host, count, interval, size; live stats, an RTT graph and
 * the reply log. */
#include "app_netdiag_int.h"

#include <stdio.h>
#include <string.h>

static lv_obj_t *ta_host, *dd_count, *dd_int, *dd_size, *btn_go, *lbl_go;
static lv_obj_t *stat_val[8], *graph, *lbl_graph;
static devos_vlist_t s_log;
static devos_focus_t s_f;
static uint32_t s_gen = 0xffffffff;
static float s_hist[DEVOS_PING_HIST];
static int s_hist_n;
static devos_ping_stats_t s_st;

static const int COUNTS[] = { 4, 10, 100, 0 };
static const int INTERVALS[] = { 200, 500, 1000, 2000, 5000 };
static const int SIZES[] = { 32, 56, 512, 1400 };

void nd_ui_ping_start(const char *host)
{
    if (host && host[0]) lv_textarea_set_text(ta_host, host);
    const char *h = lv_textarea_get_text(ta_host);
    while (*h == ' ') h++;
    if (!*h) {
        nd_flash("Enter a host name or IP address");
        devos_focus_set(&s_f, ta_host);
        return;
    }
    if (devos_ping_start(h, COUNTS[lv_dropdown_get_selected(dd_count)], INTERVALS[lv_dropdown_get_selected(dd_int)],
                         SIZES[lv_dropdown_get_selected(dd_size)]) != 0)
        nd_flash("Couldn't start ping");
    s_gen = 0xffffffff;
}

static void go_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_ping_stats_t st;
    devos_ping_stats(&st);
    if (st.running) devos_ping_stop();
    else nd_ui_ping_start(NULL);
}

/* ---- graph ---- */
static void graph_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const devos_palette_t *p = devos_theme_get();
    lv_area_t a;
    lv_obj_get_coords(graph, &a);
    int x0 = a.x1 + 56, x1 = a.x2 - 12, y0 = a.y1 + 12, y1 = a.y2 - 20;
    float top = 10;
    for (int i = 0; i < s_hist_n; i++) if (s_hist[i] > top) top = s_hist[i];
    /* round the scale up to 1-2-5 */
    float steps[] = { 10, 20, 50, 100, 200, 500, 1000, 2000 };
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        if (top <= steps[i]) { top = steps[i]; break; }
    }
    char buf[16];
    for (int g = 0; g <= 4; g++) {
        int y = y1 - (y1 - y0) * g / 4;
        devos_w_draw_rect(layer, x0, y, x1, y, p->surface_border, LV_OPA_COVER, 0);
        snprintf(buf, sizeof(buf), "%g ms", (double)(top * (float)g / 4));
        devos_w_draw_text(layer, &lv_font_montserrat_12, a.x1 + 6, y - 7, 48, buf, p->text_secondary);
    }
    if (!s_hist_n) return;
    int slots = DEVOS_PING_HIST / 2;            /* 120 bars across */
    int n = s_hist_n < slots ? s_hist_n : slots;
    float bw = (float)(x1 - x0) / (float)slots;
    for (int i = 0; i < n; i++) {
        float v = s_hist[s_hist_n - n + i];
        int bx = x0 + (int)(i * bw), bx2 = x0 + (int)((i + 1) * bw) - 2;
        if (bx2 < bx) bx2 = bx;
        if (v < 0) {
            devos_w_draw_rect(layer, bx, y0, bx2, y1, p->accent_danger, LV_OPA_40, 0);
            continue;
        }
        int h = (int)((float)(y1 - y0) * (v > top ? 1.0f : v / top));
        if (h < 2) h = 2;
        lv_color_t c = v > s_st.avg_ms * 2 && s_st.received > 3 ? p->accent_warning : p->accent_secondary;
        devos_w_draw_rect(layer, bx, y1 - h, bx2, y1, c, LV_OPA_COVER, 1);
    }
}

/* ---- log ---- */
static void log_draw(devos_vlist_t *v, lv_layer_t *layer, const lv_area_t *row, int idx)
{
    LV_UNUSED(v);
    const devos_palette_t *p = devos_theme_get();
    char buf[128];
    float rtt = s_hist[idx];
    int seq = s_st.sent - s_hist_n + idx + 1;
    if (rtt < 0)
        snprintf(buf, sizeof(buf), "seq %-5d  no reply from %s (timed out)", seq, s_st.ip);
    else
        snprintf(buf, sizeof(buf), "seq %-5d  %d bytes from %s  time=%.1f ms", seq, s_st.size + 8, s_st.ip, (double)rtt);
    devos_w_draw_text(layer, NULL, row->x1 + 10, row->y1 + 3, 0, buf, rtt < 0 ? p->accent_danger : p->text_primary);
}

static void refresh(bool force)
{
    uint32_t g = devos_ping_generation();
    if (!force && g == s_gen) return;
    s_gen = g;
    devos_ping_stats(&s_st);
    s_hist_n = devos_ping_history(s_hist, DEVOS_PING_HIST);
    char buf[48];
    int lost = s_st.sent - s_st.received;
    const char *vals[8];
    char v[8][24];
    snprintf(v[0], 24, "%d", s_st.sent);
    snprintf(v[1], 24, "%d", s_st.received);
    snprintf(v[2], 24, s_st.sent ? "%d%%" : "-", s_st.sent ? lost * 100 / s_st.sent : 0);
    snprintf(v[3], 24, s_st.received ? "%.1f" : "-", (double)s_st.min_ms);
    snprintf(v[4], 24, s_st.received ? "%.1f" : "-", (double)s_st.avg_ms);
    snprintf(v[5], 24, s_st.received ? "%.1f" : "-", (double)s_st.max_ms);
    snprintf(v[6], 24, s_st.received > 1 ? "%.1f" : "-", (double)s_st.jitter_ms);
    if (!s_st.sent) snprintf(v[7], 24, "-");
    else if (s_st.last_ms < 0) snprintf(v[7], 24, "lost");
    else snprintf(v[7], 24, "%.1f", (double)s_st.last_ms);
    for (int i = 0; i < 8; i++) {
        vals[i] = v[i];
        devos_w_set_text(stat_val[i], vals[i]);
    }
    devos_w_track(stat_val[2], lost && s_st.sent ? DEVOS_W_TEXT_ERR : DEVOS_W_TEXT);
    devos_w_track(stat_val[7], s_st.sent && s_st.last_ms < 0 ? DEVOS_W_TEXT_ERR : DEVOS_W_TEXT);
    if (s_st.error[0]) snprintf(buf, sizeof(buf), "%.46s", s_st.error);
    else if (s_st.running) snprintf(buf, sizeof(buf), "Pinging %.20s (%s)", s_st.target, s_st.ip[0] ? s_st.ip : "...");
    else if (s_st.sent) snprintf(buf, sizeof(buf), "%.24s: done", s_st.target);
    else snprintf(buf, sizeof(buf), "Round-trip time (ms)");
    devos_w_set_text(lbl_graph, buf);
    devos_w_track(lbl_graph, s_st.error[0] ? DEVOS_W_TEXT_ERR : DEVOS_W_TEXT_DIM);
    devos_w_set_text(lbl_go, s_st.running ? LV_SYMBOL_STOP "  Stop" : LV_SYMBOL_PLAY "  Start");
    devos_w_track(btn_go, s_st.running ? DEVOS_W_BTN_DANGER : DEVOS_W_BTN_PRIMARY);
    bool follow = s_log.sel < 0 || s_log.sel >= s_log.count - 1;
    devos_vlist_set_count(&s_log, s_hist_n);
    if (follow && s_hist_n) {
        s_log.sel = -1;
        lv_obj_scroll_to_y(s_log.scroll, LV_COORD_MAX, LV_ANIM_OFF);
    }
    lv_obj_invalidate(graph);
}

static lv_obj_t *dd_field(lv_obj_t *parent, const char *caption, const char *opts, int x, int w, int sel)
{
    lv_obj_t *l = devos_w_label(parent, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, caption);
    lv_obj_set_pos(l, x, 6);
    lv_obj_t *dd = devos_w_dd(parent, opts, w);
    lv_obj_set_pos(dd, x, 24);
    lv_dropdown_set_selected(dd, (uint32_t)sel);
    return dd;
}

static void create(lv_obj_t *parent)
{
    ta_host = devos_w_field(parent, "Host or IP address", 16, 6, 360);
    lv_textarea_set_placeholder_text(ta_host, "1.1.1.1, router.lan, 100.x.y.z ...");
    dd_count = dd_field(parent, "Count", "4\n10\n100\nUntil stopped", 392, 150, 0);
    dd_int = dd_field(parent, "Interval", "0.2 s\n0.5 s\n1 s\n2 s\n5 s", 558, 110, 2);
    dd_size = dd_field(parent, "Payload", "32 bytes\n56 bytes\n512 bytes\n1400 bytes", 684, 130, 1);
    btn_go = devos_w_btn_kind(parent, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_PLAY "  Start", 120, go_cb, NULL, &lbl_go);
    lv_obj_set_size(btn_go, 120, 36);
    lv_obj_set_pos(btn_go, 830, 24);

    static const char *caps[8] = { "Sent", "Received", "Loss", "Min ms", "Avg ms", "Max ms", "Jitter ms", "Last ms" };
    for (int i = 0; i < 8; i++) {
        lv_obj_t *box = devos_w_panel(parent, 16 + i * 157, 76, 149, 58, DEVOS_W_PANEL);
        lv_obj_set_style_radius(box, 6, 0);
        lv_obj_t *c = devos_w_label(box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, caps[i]);
        lv_obj_set_pos(c, 10, 6);
        stat_val[i] = devos_w_label(box, &lv_font_montserrat_22, DEVOS_W_TEXT, "-");
        lv_obj_set_pos(stat_val[i], 10, 24);
    }
    graph = devos_w_panel(parent, 16, 146, 1248, 250, DEVOS_W_CODE);
    lv_obj_set_style_radius(graph, 6, 0);
    lv_obj_add_event_cb(graph, graph_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lbl_graph = devos_w_label(parent, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Round-trip time (ms)");
    lv_obj_set_pos(lbl_graph, 24, 400);
    lv_obj_t *logp = devos_w_panel(parent, 16, 420, 1248, ND_VIEW_H - 426, DEVOS_W_CODE);
    lv_obj_set_style_radius(logp, 6, 0);
    devos_vlist_create(&s_log, logp, ND_ROW_H, log_draw);
    lv_obj_set_pos(s_log.scroll, 2, 2);
    lv_obj_set_size(s_log.scroll, 1244, ND_VIEW_H - 430);

    devos_focus_init(&s_f);
    lv_obj_t *order[] = { ta_host, dd_count, dd_int, dd_size, btn_go, s_log.scroll };
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) devos_focus_add(&s_f, order[i]);
}

static void show(void)
{
    if (!devos_focus_get(&s_f)) devos_focus_set(&s_f, ta_host);
    refresh(true);
}

static void hide(void) {}

static bool key(uint32_t k, uint8_t mods)
{
    if (s_f.dd_open) return nd_form_key(&s_f, &s_log, k, mods);    /* an open list takes Esc / arrows */
    devos_ping_stats_t st;
    devos_ping_stats(&st);
    if (k == LV_KEY_ESC) {
        if (st.running) {
            devos_ping_stop();
            return true;
        }
        return false;
    }
    if (nd_form_key(&s_f, &s_log, k, mods)) return true;
    if (k == '\r' || k == '\n') {                   /* Enter in the host field */
        if (st.running) devos_ping_stop();
        else nd_ui_ping_start(NULL);
        return true;
    }
    return !(mods & (DEVOS_MOD_FN | DEVOS_MOD_ALT)) && k >= 32 && k < 127;
}

static void tick(void) { refresh(false); }

static const char *keys(void)
{
    devos_ping_stats_t st;
    devos_ping_stats(&st);
    return st.running ? "Enter or Esc stops    Tab / arrows move" : "Enter pings    Tab / arrows move    Left / Right change";
}

const nd_view_t nd_view_ping = { "Ping", create, show, hide, key, tick, keys };
