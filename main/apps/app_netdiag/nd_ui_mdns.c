/* Network > mDNS: DNS-SD browse of the local network, grouped by service
 * type, with the selected service's details. Enter opens it: SSH services in
 * the terminal, web services in the REST client. */
#include "app_netdiag_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BROWSE_MS 4000

typedef struct {
    int svc;                    /* index into s_svc, or -1 for a type heading */
    char type[40];
    int count;
} row_t;

static lv_obj_t *btn_browse, *lbl_browse, *lbl_status, *detail, *lbl_d_title, *lbl_d_body, *lbl_empty;
static devos_vlist_t s_list;
static devos_mdns_svc_t *s_svc;                 /* DEVOS_MDNS_MAX, PSRAM */
static int s_nsvc;
static row_t s_rows[DEVOS_MDNS_MAX * 2];
static int s_nrows;
static uint32_t s_gen = 0xffffffff;
static bool s_browsed;
static uint32_t s_started;

static const devos_mdns_svc_t *sel_svc(void)
{
    if (s_list.sel < 0 || s_list.sel >= s_nrows || s_rows[s_list.sel].svc < 0) return NULL;
    return &s_svc[s_rows[s_list.sel].svc];
}

static void browse(void)
{
    if (devos_mdns_busy()) return;
    if (devos_mdns_browse_start(BROWSE_MS) != 0) nd_flash(devos_mdns_error()[0] ? devos_mdns_error() : "Couldn't browse");
    s_browsed = true;
    s_started = lv_tick_get();
    s_gen = 0xffffffff;
}

static void browse_cb(lv_event_t *e) { LV_UNUSED(e); browse(); }

static bool is_web(const char *t) { return !strcmp(t, "_http._tcp") || !strcmp(t, "_https._tcp") || !strcmp(t, "_home-assistant._tcp") || !strcmp(t, "_octoprint._tcp") || !strcmp(t, "_esphomelib._tcp"); }
static bool is_ssh(const char *t) { return !strcmp(t, "_ssh._tcp") || !strcmp(t, "_sftp-ssh._tcp"); }

static void open_svc(const devos_mdns_svc_t *s)
{
    if (!s) return;
    const char *host = s->ip[0] ? s->ip : s->host;
    char url[160];
    if (is_ssh(s->type)) {
        if (!devos_core_open_with("terminal", "ssh", host)) nd_flash("No terminal app");
    } else if (is_web(s->type)) {
        /* ESPHome's native API port isn't HTTP: its web server is on 80 */
        int port = !strcmp(s->type, "_esphomelib._tcp") ? 80 : s->port;
        const char *path = "/";
        char txtpath[64] = "";
        const char *pp = strstr(s->txt, "path=");
        if (pp) {
            sscanf(pp + 5, "%63s", txtpath);
            if (txtpath[0] == '/') path = txtpath;
        }
        snprintf(url, sizeof(url), "%s://%s:%d%s", !strcmp(s->type, "_https._tcp") ? "https" : "http", host, port, path);
        if (!devos_core_open_with("rest", "get", url)) nd_flash(url);
    } else {
        nd_flash("Nothing to open it with - P pings, C scans its ports");
    }
}

static void row_draw(devos_vlist_t *v, lv_layer_t *layer, const lv_area_t *row, int idx)
{
    LV_UNUSED(v);
    if (idx >= s_nrows) return;
    const devos_palette_t *p = devos_theme_get();
    const row_t *r = &s_rows[idx];
    int y = row->y1 + 3, x = row->x1 + 10;
    char buf[120];
    if (r->svc < 0) {
        const char *lab = devos_mdns_type_label(r->type);
        snprintf(buf, sizeof(buf), "%s%s%s  (%d)", lab ? lab : r->type, lab ? "  " : "", lab ? r->type : "", r->count);
        devos_w_draw_text(layer, &lv_font_montserrat_14, x, y - 1, 0, buf, p->accent_primary);
        return;
    }
    const devos_mdns_svc_t *s = &s_svc[r->svc];
    devos_w_draw_text(layer, NULL, x + 16, y, 250, s->instance, p->text_primary);
    snprintf(buf, sizeof(buf), "%s%s%u", s->ip[0] ? s->ip : "?", s->port ? ":" : "", s->port);
    if (!s->port) snprintf(buf, sizeof(buf), "%s", s->ip[0] ? s->ip : "resolving...");
    devos_w_draw_text(layer, NULL, x + 276, y, 170, buf, p->accent_secondary);
    devos_w_draw_text(layer, NULL, x + 452, y, row->x2 - x - 460, s->host, p->text_secondary);
}

static void show_detail(void)
{
    const devos_mdns_svc_t *s = sel_svc();
    if (!s) {
        devos_w_set_text(lbl_d_title, "");
        devos_w_set_text(lbl_d_body, s_nsvc ? "Pick a service to see its details." : "");
        return;
    }
    static char body[900];
    const char *lab = devos_mdns_type_label(s->type);
    size_t o = (size_t)snprintf(body, sizeof(body), "Type      %s%s%s%s\nHost      %s\nAddress   %s\nPort      %u\n",
                                lab ? lab : "", lab ? " (" : "", s->type, lab ? ")" : "", s->host[0] ? s->host : "-",
                                s->ip[0] ? s->ip : "-", s->port);
    if (s->txt[0]) {
        o += (size_t)snprintf(body + o, sizeof(body) - o, "\nTXT\n");
        /* TXT pairs are separated by two spaces */
        const char *t = s->txt;
        while (*t && o + 4 < sizeof(body)) {
            const char *e = strstr(t, "  ");
            size_t l = e ? (size_t)(e - t) : strlen(t);
            o += (size_t)snprintf(body + o, sizeof(body) - o, "  %.*s\n", (int)l, t);
            t += l;
            while (*t == ' ') t++;
        }
    }
    o += (size_t)snprintf(body + o, sizeof(body) - o, "\n%s    P ping    C scan ports",
                          is_ssh(s->type) ? "Enter: SSH in the terminal" : is_web(s->type) ? "Enter: open in REST" : "");
    devos_w_set_text(lbl_d_title, s->instance);
    devos_w_set_text(lbl_d_body, body);
}

static void on_select(devos_vlist_t *v, int idx)
{
    LV_UNUSED(v);
    LV_UNUSED(idx);
    show_detail();
}

static void on_activate(devos_vlist_t *v, int idx)
{
    LV_UNUSED(v);
    if (idx >= 0 && idx < s_nrows && s_rows[idx].svc >= 0) open_svc(&s_svc[s_rows[idx].svc]);
}

static void rebuild(void)
{
    char keep_inst[64] = "", keep_type[40] = "";
    const devos_mdns_svc_t *cur = sel_svc();
    if (cur) {
        snprintf(keep_inst, sizeof(keep_inst), "%s", cur->instance);
        snprintf(keep_type, sizeof(keep_type), "%s", cur->type);
    }
    s_nsvc = devos_mdns_results(s_svc, DEVOS_MDNS_MAX);
    s_nrows = 0;
    int sel = -1;
    for (int i = 0; i < s_nsvc; i++) {
        if (!i || strcmp(s_svc[i].type, s_svc[i - 1].type)) {
            row_t *h = &s_rows[s_nrows++];
            h->svc = -1;
            snprintf(h->type, sizeof(h->type), "%s", s_svc[i].type);
            h->count = 0;
            for (int k = i; k < s_nsvc && !strcmp(s_svc[k].type, s_svc[i].type); k++) h->count++;
        }
        if (!strcmp(s_svc[i].instance, keep_inst) && !strcmp(s_svc[i].type, keep_type)) sel = s_nrows;
        row_t *r = &s_rows[s_nrows++];
        r->svc = i;
        snprintf(r->type, sizeof(r->type), "%s", s_svc[i].type);
    }
    devos_vlist_set_count(&s_list, s_nrows);
    s_list.sel = sel;
    if (sel < 0 && s_nrows > 1 && s_list.active) s_list.sel = 1;
    show_detail();
}

static void refresh(bool force)
{
    uint32_t g = devos_mdns_generation();
    if (force || g != s_gen) {
        s_gen = g;
        rebuild();
    }
    char buf[160];
    int hosts = 0;
    for (int i = 0; i < s_nsvc; i++) {
        bool dup = false;
        for (int k = 0; k < i && !dup; k++) dup = s_svc[k].host[0] && !strcmp(s_svc[k].host, s_svc[i].host);
        if (!dup) hosts++;
    }
    bool busy = devos_mdns_busy();
    if (busy) snprintf(buf, sizeof(buf), "Browsing...  %d service%s so far", s_nsvc, s_nsvc == 1 ? "" : "s");
    else if (devos_mdns_error()[0]) snprintf(buf, sizeof(buf), "%s", devos_mdns_error());
    else if (s_browsed) snprintf(buf, sizeof(buf), "%d service%s on %d host%s", s_nsvc, s_nsvc == 1 ? "" : "s", hosts, hosts == 1 ? "" : "s");
    else snprintf(buf, sizeof(buf), "Finds printers, ESPHome devices, SSH servers, Home Assistant... on this network");
    devos_w_set_text(lbl_status, buf);
    devos_w_set_text(lbl_browse, busy ? LV_SYMBOL_REFRESH "  Browsing" : LV_SYMBOL_REFRESH "  Browse again");
    if (s_nsvc || busy) lv_obj_add_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    devos_w_set_text(lbl_empty, s_browsed ? "Nothing answered. mDNS only reaches this Wi-Fi network (not VPN peers)."
                                          : "Press B to browse.");
}

static void create(lv_obj_t *parent)
{
    s_svc = calloc(DEVOS_MDNS_MAX, sizeof(*s_svc));
    btn_browse = devos_w_btn_kind(parent, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_REFRESH "  Browse", 140, browse_cb, NULL, &lbl_browse);
    lv_obj_set_size(btn_browse, 140, 32);
    lv_obj_set_pos(btn_browse, 16, 8);
    lbl_status = devos_w_label(parent, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "");
    lv_obj_set_pos(lbl_status, 170, 17);
    lv_obj_t *box = devos_w_panel(parent, 16, 50, 780, ND_VIEW_H - 56, DEVOS_W_CODE);
    lv_obj_set_style_radius(box, 6, 0);
    devos_vlist_create(&s_list, box, ND_ROW_H + 2, row_draw);
    s_list.on_select = on_select;
    s_list.on_activate = on_activate;
    s_list.active = true;
    lv_obj_set_pos(s_list.scroll, 2, 2);
    lv_obj_set_size(s_list.scroll, 776, ND_VIEW_H - 60);
    lbl_empty = devos_w_label(box, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM, "");
    lv_obj_align(lbl_empty, LV_ALIGN_CENTER, 0, 0);
    detail = devos_w_panel(parent, 808, 50, 456, ND_VIEW_H - 56, DEVOS_W_PANEL);
    lv_obj_set_style_radius(detail, 6, 0);
    lbl_d_title = devos_w_label(detail, &lv_font_montserrat_18, DEVOS_W_TEXT_ACCENT, "");
    lv_label_set_long_mode(lbl_d_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_d_title, 420);
    lv_obj_set_pos(lbl_d_title, 16, 14);
    lbl_d_body = devos_w_label(detail, devos_w_mono(), DEVOS_W_TEXT, "");
    lv_label_set_long_mode(lbl_d_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_d_body, 424);
    lv_obj_set_pos(lbl_d_body, 16, 48);
}

static void show(void)
{
    if (!s_browsed) browse();
    refresh(true);
}

static void hide(void) {}

static bool key(uint32_t k, uint8_t mods)
{
    if (mods & (DEVOS_MOD_CTRL | DEVOS_MOD_ALT | DEVOS_MOD_FN)) return false;
    if (k == LV_KEY_DOWN || k == LV_KEY_UP) {
        /* skip the type headings */
        int i = s_list.sel, d = k == LV_KEY_DOWN ? 1 : -1;
        do i += d; while (i >= 0 && i < s_nrows && s_rows[i].svc < 0);
        if (i >= 0 && i < s_nrows) devos_vlist_select(&s_list, i);
        return true;
    }
    if (devos_vlist_key(&s_list, k)) return true;
    const devos_mdns_svc_t *s = sel_svc();
    switch (k) {
    case 'b': case 'B': browse(); return true;
    case 'p': case 'P': if (s && s->ip[0]) nd_ping_host(s->ip); return true;
    case 'c': case 'C': if (s && s->ip[0]) nd_scan_host(s->ip); return true;
    case LV_KEY_ESC: if (devos_mdns_busy()) { devos_mdns_stop(); return true; } return false;
    default: return k >= 32 && k < 127;
    }
}

static void tick(void) { refresh(false); }

static const char *keys(void)
{
    return "Up / Down pick    Enter opens (SSH, web)    P ping    C scan ports    B browse again";
}

const nd_view_t nd_view_mdns = { "mDNS", create, show, hide, key, tick, keys };
