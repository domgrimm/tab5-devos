/* Network: diagnostics suite - Ping, DNS, Port scan, Wi-Fi survey, mDNS.
 *
 * The bar holds one tab per tool (Alt+1..5, or Alt+Left / Right); each tool is
 * a view in its own file (nd_ui_*.c) with a form along the top and its
 * results below. Keys inside a tool follow the usual model: Tab / arrows move
 * through the form and into the results list, Enter runs or opens, Esc stops
 * what's running, then leaves (Home).
 *
 * Tools hand work to each other: a scan result can be pinged, an mDNS
 * service opened in the terminal, a DNS answer looked up in turn.
 */
#include "devos_toast.h"
#include "app_netdiag.h"
#include "app_netdiag_int.h"
#include "devos_net.h"
#include "devos_icons.h"

#include <stdio.h>
#include <string.h>

static devos_app_descriptor_t s_desc;
static lv_obj_t *s_screen, *s_bar, *s_keys;
static lv_obj_t *s_tabs[ND_VIEWS], *s_tab_lbl[ND_VIEWS];
static lv_obj_t *s_view_obj[ND_VIEWS];
static const nd_view_t *s_views[ND_VIEWS] = { &nd_view_ping, &nd_view_dns, &nd_view_scan, &nd_view_wifi,
                                              &nd_view_mdns };
static int s_cur = -1;
static bool s_inited;

void nd_flash(const char *msg)
{
    devos_toast_show(msg, DEVOS_TOAST_WARN, 3000);
}

bool nd_form_key(devos_focus_t *f, devos_vlist_t *v, uint32_t key, uint8_t mods)
{
    bool on_list = v && devos_focus_get(f) == v->scroll && f->ring;
    if (v && v->active != on_list) {
        v->active = on_list;
        devos_vlist_redraw(v);
    }
    if (on_list && !(mods & (DEVOS_MOD_CTRL | DEVOS_MOD_ALT | DEVOS_MOD_FN))) {
        if (v->sel < 0 && v->count && (key == LV_KEY_DOWN || key == LV_KEY_UP)) {
            devos_vlist_select(v, 0);
            return true;
        }
        if (key == LV_KEY_UP && v->sel <= 0) return devos_focus_key(f, key, mods);   /* back into the form */
        if (devos_vlist_key(v, key)) return true;
    }
    bool used = devos_focus_key(f, key, mods);
    if (v) {
        bool now = devos_focus_get(f) == v->scroll && f->ring;
        if (now != v->active) {
            v->active = now;
            if (now && v->sel < 0 && v->count) devos_vlist_select(v, 0);
            devos_vlist_redraw(v);
        }
    }
    return used;
}

void nd_local_subnet(char *out, size_t cap)
{
    out[0] = '\0';
    devos_wifi_status_t st;
    unsigned a, b, c, d, m1, m2, m3, m4;
    if (devos_net_wifi_get_status(&st) != 0 || !st.connected) return;
    if (sscanf(st.ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return;
    if (sscanf(st.netmask, "%u.%u.%u.%u", &m1, &m2, &m3, &m4) == 4 && m1 == 255 && m2 == 255 && m3 == 255)
        snprintf(out, cap, "%u.%u.%u.0/24", a, b, c);
    else if (sscanf(st.netmask, "%u.%u.%u.%u", &m1, &m2, &m3, &m4) == 4 && m1 == 255 && m2 == 255 && m3 >= 252)
        snprintf(out, cap, "%u.%u.%u.0/%d", a, b, c & m3, m3 == 252 ? 22 : m3 == 254 ? 23 : 22);
    else
        snprintf(out, cap, "%u.%u.%u.0/24", a, b, c);
}

static void style_tabs(void)
{
    for (int i = 0; i < ND_VIEWS; i++) devos_w_track(s_tabs[i], i == s_cur ? DEVOS_W_BTN_PRIMARY : DEVOS_W_BTN);
}

void nd_goto(int v)
{
    if (v < 0 || v >= ND_VIEWS || v == s_cur) return;
    if (s_cur >= 0) {
        if (s_views[s_cur]->hide) s_views[s_cur]->hide();
        lv_obj_add_flag(s_view_obj[s_cur], LV_OBJ_FLAG_HIDDEN);
    }
    s_cur = v;
    lv_obj_remove_flag(s_view_obj[v], LV_OBJ_FLAG_HIDDEN);
    style_tabs();
    if (s_views[v]->show) s_views[v]->show();
}

static void tab_cb(lv_event_t *e) { nd_goto((int)(intptr_t)lv_event_get_user_data(e)); }

static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!s_screen || lv_obj_has_flag(s_screen, LV_OBJ_FLAG_HIDDEN) || s_cur < 0) return;
    if (s_views[s_cur]->tick) s_views[s_cur]->tick();
    char keys[240];
    snprintf(keys, sizeof(keys), "%s    Alt+1..5 tools    Esc home", s_views[s_cur]->keys ? s_views[s_cur]->keys() : "");
    devos_w_set_text(s_keys, keys);
}

void nd_ping_host(const char *host)
{
    nd_goto(ND_PING);
    extern void nd_ui_ping_start(const char *host);
    nd_ui_ping_start(host);
}

void nd_scan_host(const char *host)
{
    nd_goto(ND_SCAN);
    extern void nd_ui_scan_set_target(const char *host);
    nd_ui_scan_set_target(host);
}

void nd_dns_name(const char *name)
{
    nd_goto(ND_DNS);
    extern void nd_ui_dns_lookup(const char *name);
    nd_ui_dns_lookup(name);
}

static bool nd_key(uint32_t key, uint8_t mods)
{
    if ((mods & DEVOS_MOD_ALT) && key >= '1' && key <= '0' + ND_VIEWS) {
        nd_goto((int)(key - '1'));
        return true;
    }
    if ((mods & DEVOS_MOD_ALT) && (key == LV_KEY_LEFT || key == LV_KEY_RIGHT)) {
        nd_goto((s_cur + (key == LV_KEY_RIGHT ? 1 : ND_VIEWS - 1)) % ND_VIEWS);
        return true;
    }
    if (s_cur >= 0 && s_views[s_cur]->key && s_views[s_cur]->key(key, mods)) return true;
    return false;
}

static void nd_init(void)
{
    if (s_inited) return;
    s_inited = true;
    devos_netdiag_init();
    s_screen = devos_w_screen(&s_desc);
    s_bar = devos_w_bar(s_screen, LV_SYMBOL_WIFI "  Network", NULL);
    static const char *names[ND_VIEWS] = { "1  Ping", "2  DNS", "3  Port scan", "4  Wi-Fi survey", "5  mDNS" };
    static const int widths[ND_VIEWS] = { 70, 70, 100, 120, 76 };
    int x = 150;
    for (int i = 0; i < ND_VIEWS; i++) {
        s_tabs[i] = devos_w_btn(s_bar, names[i], widths[i], tab_cb, (void *)(intptr_t)i, &s_tab_lbl[i]);
        lv_obj_align(s_tabs[i], LV_ALIGN_LEFT_MID, x, 0);
        x += widths[i] + 6;
    }
    for (int i = 0; i < ND_VIEWS; i++) {
        lv_obj_t *v = lv_obj_create(s_screen);
        lv_obj_remove_style_all(v);
        lv_obj_set_pos(v, 0, ND_VIEW_Y);
        lv_obj_set_size(v, DEVOS_SCREEN_WIDTH, ND_VIEW_H);
        lv_obj_remove_flag(v, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);
        s_view_obj[i] = v;
        s_views[i]->create(v);
    }
    s_keys = devos_w_keys(s_screen);
    lv_timer_create(tick_cb, 200, NULL);
    nd_goto(ND_PING);
}

static void nd_show(void)
{
    char action[24], arg[256];
    if (devos_core_take_intent("netdiag", action, sizeof(action), arg, sizeof(arg))) {
        if (!strcmp(action, "ping")) nd_ping_host(arg);
        else if (!strcmp(action, "scan")) nd_scan_host(arg);
        else if (!strcmp(action, "dns")) nd_dns_name(arg);
    }
    if (s_cur >= 0 && s_views[s_cur]->show) s_views[s_cur]->show();
}

static void nd_hide(void)
{
    if (s_cur >= 0 && s_views[s_cur]->hide) s_views[s_cur]->hide();
}

static int nd_telemetry(char lines[3][64])
{
    devos_ping_stats_t ps;
    devos_ping_stats(&ps);
    devos_scan_status_t ss;
    devos_scan_status(&ss);
    int n = 0;
    if (ps.running || ps.sent) {
        if (ps.received) snprintf(lines[n++], 64, "* Ping %.24s %.1f ms", ps.target, (double)ps.avg_ms);
        else snprintf(lines[n++], 64, "* Ping %.24s: no reply", ps.target);
    }
    if (ss.running) snprintf(lines[n++], 64, "* Scanning: %d/%d probes", ss.probes_done, ss.probes_total);
    else if (ss.hosts_total) snprintf(lines[n++], 64, "* Last scan: %d hosts, %d open ports", ss.hosts_alive, ss.open_ports);
    if (!n) {
        snprintf(lines[0], 64, "* Ping, DNS, port scan");
        snprintf(lines[1], 64, "* Wi-Fi survey, mDNS browser");
        return 2;
    }
    return n;
}

/* Sym+S sheet (devos_shortcuts.h): the tool on screen first */
static const char *nd_shortcuts(void)
{
    static const char *const tool[ND_VIEWS] = {
        [ND_PING] = "Ping\n"
                    "Enter\tStart / stop\n"
                    "Tab / arrows\tMove between the fields\n"
                    "Left / Right\tChange a setting\n",
        [ND_DNS] = "DNS\n"
                   "Enter\tLook up (on an answer: follow what it points at)\n"
                   "Left / Right\tRecord type\n"
                   "Tab\tBetween the form and the answers\n",
        [ND_SCAN] = "Port scan\n"
                    "Enter\tStart / stop the scan\n"
                    "Space\tTick \"ping first\"\n"
                    "Enter / P\tPing the host picked in the results\n"
                    "S\tSSH to it in the Terminal\n"
                    "D\tReverse DNS\n",
        [ND_WIFI] = "Wi-Fi survey\n"
                    "Up / Down\tPick a radio\n"
                    "Space\tPause\n"
                    "S\tSort by signal, channel or name\n"
                    "R\tScan now\n"
                    "Esc\tClear the pick\n",
        [ND_MDNS] = "mDNS\n"
                    "Up / Down\tPick a service\n"
                    "Enter\tOpen it (SSH in the Terminal, web in REST)\n"
                    "P\tPing it\n"
                    "C\tScan its ports\n"
                    "B\tBrowse again\n",
    };
    static char text[1200];
    snprintf(text, sizeof(text), "%s"
             "Every tool\n"
             "Alt+1 ... 5\tPing, DNS, Port scan, Wi-Fi survey, mDNS\n"
             "Alt+Left / Right\tPrevious / next tool\n"
             "Esc\tStop what's running, then Home\n",
             s_cur >= 0 && s_cur < ND_VIEWS ? tool[s_cur] : "");
    return text;
}

devos_app_descriptor_t *app_netdiag_get_descriptor(void)
{
    s_desc.id = DEVOS_APP_LAUNCHER;                 /* auto-assigned */
    s_desc.uid = "netdiag";
    s_desc.icon = LV_SYMBOL_WIFI;
    s_desc.draw_icon = devos_icon_network;
    s_desc.category = "network";
    s_desc.name = "Network";
    s_desc.title = "Network";
    s_desc.subtitle = "Ping, DNS, port scan, Wi-Fi survey, mDNS";
    s_desc.init = nd_init;
    s_desc.show = nd_show;
    s_desc.hide = nd_hide;
    s_desc.handle_key = nd_key;
    s_desc.get_telemetry_lines = nd_telemetry;
    s_desc.get_shortcuts = nd_shortcuts;
    return &s_desc;
}
