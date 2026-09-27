/* Network > Port scan: hosts (IP, name, range or subnet) x ports, optionally
 * only the hosts that answer a ping first. Results list the live hosts with
 * their open ports; P pings the selected host, S opens SSH in the terminal. */
#include "app_netdiag_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t *ta_hosts, *ta_ports, *dd_timeout, *cb_ping, *btn_go, *lbl_go, *bar, *lbl_status, *lbl_empty;
static devos_vlist_t s_list;
static devos_focus_t s_f;
static uint32_t s_gen = 0xffffffff;
static devos_scan_host_t *s_hosts;              /* DEVOS_SCAN_MAX_HOSTS, PSRAM */
static int s_n;
static bool s_prefilled;

static const int TIMEOUTS[] = { 200, 500, 1000, 2000 };

static void start(void)
{
    char err[96];
    const char *h = lv_textarea_get_text(ta_hosts);
    const char *pt = lv_textarea_get_text(ta_ports);
    int nh = devos_scan_count_hosts(h, err, sizeof(err));
    if (nh < 0) {
        nd_flash(err);
        devos_focus_set(&s_f, ta_hosts);
        return;
    }
    if (devos_scan_count_ports(pt, err, sizeof(err)) < 0) {
        nd_flash(err);
        devos_focus_set(&s_f, ta_ports);
        return;
    }
    if (devos_scan_start(h, pt, TIMEOUTS[lv_dropdown_get_selected(dd_timeout)],
                         lv_obj_has_state(cb_ping, LV_STATE_CHECKED)) != 0) {
        devos_scan_status_t st;
        devos_scan_status(&st);
        nd_flash(st.error[0] ? st.error : "A scan is already running");
    }
    s_list.sel = -1;
    s_gen = 0xffffffff;
}

void nd_ui_scan_set_target(const char *host)
{
    if (!host || !host[0]) return;
    lv_textarea_set_text(ta_hosts, host);
    lv_obj_remove_state(cb_ping, LV_STATE_CHECKED);     /* one host: scan it even if it ignores ping */
    s_prefilled = true;
    devos_focus_set(&s_f, ta_ports);
}

static void go_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_scan_status_t st;
    devos_scan_status(&st);
    if (st.running) devos_scan_stop();
    else start();
}

static void row_draw(devos_vlist_t *v, lv_layer_t *layer, const lv_area_t *row, int idx)
{
    LV_UNUSED(v);
    if (idx >= s_n) return;
    const devos_palette_t *p = devos_theme_get();
    const devos_scan_host_t *h = &s_hosts[idx];
    int y = row->y1 + 3, x = row->x1 + 10;
    char buf[400];
    devos_w_draw_text(layer, NULL, x, y, 130, h->ip, p->text_primary);
    devos_w_draw_text(layer, NULL, x + 136, y, 250, h->name[0] ? h->name : "", p->text_secondary);
    if (h->rtt_ms >= 0) snprintf(buf, sizeof(buf), "%d ms", h->rtt_ms);
    else snprintf(buf, sizeof(buf), "%s", h->alive ? "up" : "");
    devos_w_draw_text(layer, NULL, x + 396, y, 70, buf, p->accent_secondary);
    size_t o = 0;
    buf[0] = '\0';
    for (int i = 0; i < h->n_open && o + 24 < sizeof(buf); i++) {
        const char *svc = devos_scan_service(h->open[i]);
        o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%s%u%s%s", i ? "  " : "", h->open[i], svc ? " " : "", svc ? svc : "");
    }
    if (!h->n_open) snprintf(buf, sizeof(buf), "no open ports found");
    devos_w_draw_text(layer, NULL, x + 476, y, row->x2 - x - 486, buf, h->n_open ? p->accent_primary : p->text_muted);
}

static void row_activate(devos_vlist_t *v, int idx)
{
    LV_UNUSED(v);
    if (idx < s_n) nd_ping_host(s_hosts[idx].ip);
}

static void refresh(bool force)
{
    uint32_t g = devos_scan_generation();
    if (!force && g == s_gen) return;
    s_gen = g;
    devos_scan_status_t st;
    devos_scan_status(&st);
    s_n = devos_scan_results(s_hosts, DEVOS_SCAN_MAX_HOSTS);
    devos_vlist_set_count(&s_list, s_n);
    char buf[200];
    if (st.error[0]) snprintf(buf, sizeof(buf), "%s", st.error);
    else if (!st.phase[0]) snprintf(buf, sizeof(buf), "Not scanned yet");
    else snprintf(buf, sizeof(buf), "%s  -  %d of %d host%s up, %d open port%s, %d / %d probes, %.1f s", st.phase,
                  st.hosts_alive, st.hosts_total, st.hosts_total == 1 ? "" : "s", st.open_ports,
                  st.open_ports == 1 ? "" : "s", st.probes_done, st.probes_total, st.elapsed_ms / 1000.0);
    devos_w_set_text(lbl_status, buf);
    devos_w_track(lbl_status, st.error[0] ? DEVOS_W_TEXT_ERR : DEVOS_W_TEXT_DIM);
    lv_bar_set_value(bar, st.probes_total ? st.probes_done * 1000 / st.probes_total : (st.running ? 0 : 0), LV_ANIM_OFF);
    devos_w_set_text(lbl_go, st.running ? LV_SYMBOL_STOP "  Stop" : LV_SYMBOL_PLAY "  Scan");
    devos_w_track(btn_go, st.running ? DEVOS_W_BTN_DANGER : DEVOS_W_BTN_PRIMARY);
    if (s_n || st.running) lv_obj_add_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    devos_w_set_text(lbl_empty, st.phase[0] && !st.error[0] ? "Nothing answered." :
                     "Hosts: 192.168.1.20, printer.lan, 192.168.1.10-40 or 192.168.1.0/24 (up to /22)\n"
                     "Ports: 22,80,443 or 8000-8100, or \"common\" for the usual LAN services");
}

static void create(lv_obj_t *parent)
{
    s_hosts = calloc(DEVOS_SCAN_MAX_HOSTS, sizeof(*s_hosts));
    ta_hosts = devos_w_field(parent, "Hosts", 16, 6, 360);
    lv_textarea_set_placeholder_text(ta_hosts, "192.168.1.0/24, 10.0.0.5-20, nas.lan");
    ta_ports = devos_w_field(parent, "Ports", 392, 6, 280);
    lv_textarea_set_placeholder_text(ta_ports, "common");
    lv_obj_t *l = devos_w_label(parent, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Timeout");
    lv_obj_set_pos(l, 688, 6);
    dd_timeout = devos_w_dd(parent, "200 ms\n500 ms\n1 s\n2 s", 100);
    lv_obj_set_pos(dd_timeout, 688, 24);
    lv_dropdown_set_selected(dd_timeout, 1);
    cb_ping = devos_w_cb(parent, "Ping first");
    lv_obj_set_pos(cb_ping, 804, 32);
    lv_obj_add_state(cb_ping, LV_STATE_CHECKED);
    btn_go = devos_w_btn_kind(parent, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_PLAY "  Scan", 110, go_cb, NULL, &lbl_go);
    lv_obj_set_size(btn_go, 110, 36);
    lv_obj_set_pos(btn_go, 930, 24);
    bar = lv_bar_create(parent);
    lv_obj_set_size(bar, 1248, 6);
    lv_obj_set_pos(bar, 16, 70);
    lv_bar_set_range(bar, 0, 1000);
    devos_w_track(bar, DEVOS_W_PROGRESS);
    lbl_status = devos_w_label(parent, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "");
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_status, 1240);
    lv_obj_set_pos(lbl_status, 20, 82);
    lv_obj_t *hdr = devos_w_label(parent, devos_w_mono(), DEVOS_W_TEXT_MUTED,
                                  "ADDRESS          NAME                            PING     OPEN PORTS");
    lv_obj_set_pos(hdr, 28, 102);
    lv_obj_t *box = devos_w_panel(parent, 16, 122, 1248, ND_VIEW_H - 128, DEVOS_W_CODE);
    lv_obj_set_style_radius(box, 6, 0);
    devos_vlist_create(&s_list, box, ND_ROW_H, row_draw);
    s_list.on_activate = row_activate;
    lv_obj_set_pos(s_list.scroll, 2, 2);
    lv_obj_set_size(s_list.scroll, 1244, ND_VIEW_H - 132);
    lbl_empty = devos_w_label(box, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM, "");
    lv_obj_set_style_text_align(lbl_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_empty, LV_ALIGN_CENTER, 0, 0);
    devos_focus_init(&s_f);
    lv_obj_t *order[] = { ta_hosts, ta_ports, dd_timeout, cb_ping, btn_go, s_list.scroll };
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) devos_focus_add(&s_f, order[i]);
}

static void show(void)
{
    if (!s_prefilled && !lv_textarea_get_text(ta_hosts)[0]) {
        char sub[32];
        nd_local_subnet(sub, sizeof(sub));
        if (sub[0]) lv_textarea_set_text(ta_hosts, sub);
        s_prefilled = true;
    }
    if (!devos_focus_get(&s_f)) devos_focus_set(&s_f, ta_hosts);
    refresh(true);
}

static void hide(void) {}

static bool key(uint32_t k, uint8_t mods)
{
    if (s_f.dd_open) return nd_form_key(&s_f, &s_list, k, mods);    /* an open list takes Esc / arrows */
    devos_scan_status_t st;
    devos_scan_status(&st);
    if (k == LV_KEY_ESC) {
        if (st.running) {
            devos_scan_stop();
            return true;
        }
        return false;
    }
    if (s_list.active && s_list.sel >= 0 && s_list.sel < s_n && !(mods & (DEVOS_MOD_CTRL | DEVOS_MOD_ALT))) {
        const devos_scan_host_t *h = &s_hosts[s_list.sel];
        if (k == 'p' || k == 'P') { nd_ping_host(h->ip); return true; }
        if (k == 's' || k == 'S') {
            if (!devos_core_open_with("terminal", "ssh", h->ip)) nd_flash("The Terminal is switched off (Settings > Apps)");
            return true;
        }
        if (k == 'd' || k == 'D') { nd_dns_name(h->ip); return true; }
    }
    if (nd_form_key(&s_f, &s_list, k, mods)) return true;
    if (k == '\r' || k == '\n') {
        if (st.running) devos_scan_stop();
        else start();
        return true;
    }
    return !(mods & (DEVOS_MOD_FN | DEVOS_MOD_ALT)) && k >= 32 && k < 127;
}

static void tick(void) { refresh(false); }

static const char *keys(void)
{
    devos_scan_status_t st;
    devos_scan_status(&st);
    if (s_list.active) return "Up / Down pick    Enter or P ping it    S SSH to it    D reverse DNS    Tab back";
    return st.running ? "Esc or Enter stops    Tab / arrows move" : "Enter scans    Tab / arrows move    Space toggles Ping first";
}

const nd_view_t nd_view_scan = { "Port scan", create, show, hide, key, tick, keys };
