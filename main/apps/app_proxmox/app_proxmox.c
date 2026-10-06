/* app_proxmox: Proxmox VE nodes and guests - a dashboard list of the cluster's
 * VMs and containers on the left, the selected guest's detail and actions on
 * the right, a config dialog for the server and its API token.
 *
 * The engine (devos_proxmox) does the talking; this only drives it and draws.
 * Rules (AGENTS.md): widgets/theme (#5), keyboard first (#9) via devos_focus and
 * devos_vlist, a 260 px side panel on Sym+L (#3), modular app (#8). */
#include "app_proxmox.h"
#include "devos_proxmox.h"
#include "devos_config.h"
#include "devos_widgets.h"
#include "devos_focus.h"
#include "devos_theme.h"
#include "devos_toast.h"
#include "devos_cmdpal.h"
#include "devos_icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LIST_W  DEVOS_PANE_LEFT_WIDTH
#define ROW_H   46

static devos_app_descriptor_t s_desc;
static lv_obj_t *s_screen, *s_bar, *lbl_status, *btn_refresh, *btn_server;
static lv_obj_t *s_list_box, *s_empty;
static devos_vlist_t s_list;
static lv_obj_t *s_detail;
static lv_obj_t *lbl_name, *lbl_state, *lbl_info, *lbl_cpu, *lbl_mem, *lbl_disk, *lbl_uptime, *lbl_nodes;
static lv_obj_t *bar_mem;
static lv_obj_t *btn_start, *btn_shutdown, *btn_stop, *btn_reboot, *btn_console, *btn_ssh;
static devos_w_dialog_t s_dlg_cfg, s_dlg_confirm;
static lv_obj_t *ta_url, *ta_tid, *ta_secret, *dd_insecure, *ta_interval;
static devos_focus_t s_fcfg, s_fconfirm;
static devos_proxmox_guest_t *s_g;
static int s_n, s_sel = -1;
static bool s_side_open = true, s_inited;
static uint32_t s_gen;
static char s_pending[12];                  /* the action awaiting confirmation */

static void show_detail(void);
static void layout(void);

static void flash(const char *msg) { devos_toast_show(msg, DEVOS_TOAST_WARN, 3000); }

static const devos_proxmox_guest_t *sel_guest(void)
{
    return (s_sel >= 0 && s_sel < s_n) ? &s_g[s_sel] : NULL;
}

static void fmt_bytes(uint64_t b, char *out, size_t cap)
{
    if (b >= 1024ULL * 1024 * 1024) snprintf(out, cap, "%.1f GiB", (double)b / (1024.0 * 1024 * 1024));
    else if (b >= 1024ULL * 1024) snprintf(out, cap, "%.0f MiB", (double)b / (1024.0 * 1024));
    else if (b >= 1024) snprintf(out, cap, "%.0f KiB", (double)b / 1024.0);
    else snprintf(out, cap, "%llu B", (unsigned long long)b);
}

static void fmt_uptime(int64_t s, char *out, size_t cap)
{
    if (s <= 0) { snprintf(out, cap, "-"); return; }
    int64_t d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
    if (d) snprintf(out, cap, "%lldd %lldh", (long long)d, (long long)h);
    else if (h) snprintf(out, cap, "%lldh %lldm", (long long)h, (long long)m);
    else snprintf(out, cap, "%lldm", (long long)m);
}

static lv_color_t state_color(const devos_proxmox_guest_t *g)
{
    const devos_palette_t *p = devos_theme_get();
    if (!strcmp(g->status, "running")) return p->accent_secondary;
    if (!strcmp(g->status, "paused") || !strcmp(g->status, "suspended")) return p->accent_warning;
    if (g->template_guest) return p->text_muted;
    return p->text_secondary;
}

/* ------------------------------------------------------------------ list */
static void row_draw(devos_vlist_t *v, lv_layer_t *layer, const lv_area_t *row, int idx)
{
    LV_UNUSED(v);
    if (idx >= s_n) return;
    const devos_palette_t *p = devos_theme_get();
    const devos_proxmox_guest_t *g = &s_g[idx];
    int x = row->x1 + 12, y = row->y1 + 5, w = row->x2 - row->x1;
    devos_w_draw_rect(layer, x, y + 5, x + 9, y + 14, state_color(g), LV_OPA_COVER, 5);
    char nm[72];
    snprintf(nm, sizeof(nm), "%s%s", g->template_guest ? "[t] " : "", g->name);
    devos_w_draw_text(layer, &lv_font_montserrat_16, x + 18, y, w - 120, nm,
                      strcmp(g->status, "running") ? p->text_secondary : p->text_primary);
    devos_w_draw_text(layer, &lv_font_montserrat_12, row->x1 + w - 90, y + 2, 82, g->status, state_color(g));
    char sub[120];
    snprintf(sub, sizeof(sub), "%d  %s  %s", g->vmid,
             g->kind == DEVOS_PROXMOX_LXC ? "LXC" : "QEMU", g->node);
    devos_w_draw_text(layer, NULL, x + 18, y + 20, w - 40, sub, p->text_muted);
}

static void on_select(devos_vlist_t *v, int idx)
{
    LV_UNUSED(v);
    if (idx >= 0 && idx < s_n) s_sel = idx;
    show_detail();
}

static void on_activate(devos_vlist_t *v, int idx)
{
    LV_UNUSED(v);
    if (idx >= 0 && idx < s_n) { s_sel = idx; show_detail(); }
}

/* ------------------------------------------------------------------ actions */
static int do_action(const char *action)
{
    const devos_proxmox_guest_t *g = sel_guest();
    if (!g) { flash("Pick a guest first"); return -1; }
    if (devos_proxmox_action(g->vmid, action) != 0) {
        flash("Couldn't queue that (is Proxmox configured?)");
        return -1;
    }
    char m[64];
    snprintf(m, sizeof(m), "%s: vmid %d", action, g->vmid);
    devos_toast_show(m, DEVOS_TOAST_OK, 0);
    return 0;
}

static void confirm_close(void)
{
    devos_w_dialog_show(&s_dlg_confirm, false);
    devos_focus_clear(&s_fconfirm);
}
static void confirm_ok(void)
{
    confirm_close();
    if (s_pending[0]) do_action(s_pending);
    s_pending[0] = '\0';
}
static void confirm_ok_cb(lv_event_t *e) { LV_UNUSED(e); confirm_ok(); }
static void confirm_cancel_cb(lv_event_t *e) { LV_UNUSED(e); s_pending[0] = '\0'; confirm_close(); }

/* Stop and reboot interrupt a running guest, so they ask first (invariant 9:
 * Enter confirms, Esc cancels). */
static void confirm_open(const char *action, const char *what)
{
    snprintf(s_pending, sizeof(s_pending), "%s", action);
    const devos_proxmox_guest_t *g = sel_guest();
    char t[128];
    snprintf(t, sizeof(t), LV_SYMBOL_WARNING "  %s \"%.48s\"?", what, g ? g->name : "the guest");
    lv_label_set_text(s_dlg_confirm.title, t);
    devos_w_dialog_show(&s_dlg_confirm, true);
    devos_focus_first(&s_fconfirm);
}

static void start_cb(lv_event_t *e)    { LV_UNUSED(e); do_action("start"); }
static void shutdown_cb(lv_event_t *e) { LV_UNUSED(e); do_action("shutdown"); }
static void stop_cb(lv_event_t *e)     { LV_UNUSED(e); confirm_open("stop", "Force stop"); }
static void reboot_cb(lv_event_t *e)   { LV_UNUSED(e); confirm_open("reboot", "Reboot"); }

/* The guest's console in the REST app (Proxmox's noVNC URL). */
static void console_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    char url[256];
    const devos_proxmox_guest_t *g = sel_guest();
    if (!g || !devos_proxmox_console_url(g, url, sizeof(url))) { flash("No console URL (is the server set?)"); return; }
    if (!devos_core_open_with("rest", "get", url)) flash("The REST app is switched off (Settings > Apps)");
}

/* SSH to the Proxmox host in the Terminal. */
static void ssh_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    char target[112];
    if (!devos_proxmox_ssh_target(target, sizeof(target))) { flash("No server set"); return; }
    if (!devos_core_open_with("terminal", "ssh", target)) flash("The Terminal is switched off (Settings > Apps)");
}

static void refresh_cb(lv_event_t *e) { LV_UNUSED(e); devos_proxmox_refresh(); }

/* ------------------------------------------------------------------ detail */
static void show_detail(void)
{
    if (!s_detail) return;
    const devos_proxmox_guest_t *g = sel_guest();
    bool has = g != NULL;
    devos_w_set_text(lbl_name, has ? g->name : "No guest selected");
    if (!has) {
        devos_w_set_text(lbl_state, "");
        devos_w_set_text(lbl_info, "");
        devos_w_set_text(lbl_cpu, "");
        devos_w_set_text(lbl_mem, "");
        devos_w_set_text(lbl_disk, "");
        devos_w_set_text(lbl_uptime, "");
        devos_w_set_text(lbl_nodes, "");
        if (bar_mem) {
            lv_bar_set_value(bar_mem, 0, LV_ANIM_OFF);
            lv_obj_add_flag(bar_mem, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (bar_mem) lv_obj_remove_flag(bar_mem, LV_OBJ_FLAG_HIDDEN);
    char b[160];
    snprintf(b, sizeof(b), "%s  -  %s %d on %s", g->status,
             g->kind == DEVOS_PROXMOX_LXC ? "LXC" : "QEMU", g->vmid, g->node);
    devos_w_set_text(lbl_state, b);
    if (g->template_guest) devos_w_set_text(lbl_info, "A template - start it by cloning it");
    else if (!strcmp(g->status, "running")) devos_w_set_text(lbl_info, "Running");
    else if (!strcmp(g->status, "stopped")) devos_w_set_text(lbl_info, "Stopped");
    else devos_w_set_text(lbl_info, g->status);

    snprintf(b, sizeof(b), "CPU   %.1f%%", (double)g->cpu * 100.0);
    devos_w_set_text(lbl_cpu, b);
    char mu[24], mt[24];
    fmt_bytes(g->mem, mu, sizeof(mu));
    fmt_bytes(g->maxmem, mt, sizeof(mt));
    snprintf(b, sizeof(b), "MEM   %s / %s", mu, mt);
    devos_w_set_text(lbl_mem, b);
    lv_bar_set_value(bar_mem, g->maxmem ? (int32_t)((double)g->mem * 1000.0 / (double)g->maxmem) : 0, LV_ANIM_OFF);
    char du[24], dt[24], up[24];
    fmt_bytes(g->disk, du, sizeof(du));
    fmt_bytes(g->maxdisk, dt, sizeof(dt));
    fmt_uptime(g->uptime_s, up, sizeof(up));
    snprintf(b, sizeof(b), "DISK  %s / %s", du, dt);
    devos_w_set_text(lbl_disk, b);
    snprintf(b, sizeof(b), "UP    %s", up);
    devos_w_set_text(lbl_uptime, b);
}

/* ------------------------------------------------------------------ config */
static void cfg_close(void)
{
    devos_w_dialog_show(&s_dlg_cfg, false);
    devos_focus_clear(&s_fcfg);
}
static void cfg_open(void)
{
    devos_proxmox_config_t c;
    devos_proxmox_get_config(&c);
    lv_textarea_set_text(ta_url, c.url);
    lv_textarea_set_text(ta_tid, c.token_id);
    lv_textarea_set_text(ta_secret, c.secret);
    char iv[12];
    snprintf(iv, sizeof(iv), "%d", c.interval_s);
    lv_textarea_set_text(ta_interval, iv);
    lv_dropdown_set_selected(dd_insecure, c.insecure ? 1 : 0);
    devos_w_set_text(s_dlg_cfg.msg, "Enter = save      Esc = cancel");
    devos_w_dialog_show(&s_dlg_cfg, true);
    devos_focus_first(&s_fcfg);
}
static void cfg_ok(void)
{
    devos_proxmox_config_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.url, sizeof(c.url), "%s", lv_textarea_get_text(ta_url));
    snprintf(c.token_id, sizeof(c.token_id), "%s", lv_textarea_get_text(ta_tid));
    snprintf(c.secret, sizeof(c.secret), "%s", lv_textarea_get_text(ta_secret));
    c.insecure = lv_dropdown_get_selected(dd_insecure) == 1;
    c.interval_s = atoi(lv_textarea_get_text(ta_interval));
    if (c.interval_s < 2) c.interval_s = 10;
    /* A server without a URL or token cannot be reached; say so instead of
     * saving something that will only fail later. */
    if (c.url[0] && strncmp(c.url, "http", 4) != 0) {
        devos_w_set_text(s_dlg_cfg.msg, "The URL must start with https:// (Proxmox is HTTPS)");
        return;
    }
    if (c.url[0] && !c.token_id[0]) {
        devos_w_set_text(s_dlg_cfg.msg, "An API token id is needed: user@realm!tokenid");
        return;
    }
    if (c.token_id[0] && !c.secret[0]) {
        devos_w_set_text(s_dlg_cfg.msg, "Enter the API token's secret (it is stored in NVS)");
        return;
    }
    if (c.url[0] && strchr(c.token_id, '!') == NULL && c.token_id[0]) {
        devos_w_set_text(s_dlg_cfg.msg, "The token id looks like user@realm!tokenid");
        return;
    }
    devos_proxmox_set_config(&c);
    cfg_close();
    devos_toast_show(c.url[0] ? "Proxmox server saved" : "Proxmox server cleared", DEVOS_TOAST_OK, 0);
}
static void cfg_ok_cb(lv_event_t *e) { LV_UNUSED(e); cfg_ok(); }
static void cfg_cancel_cb(lv_event_t *e) { LV_UNUSED(e); cfg_close(); }
static void server_cb(lv_event_t *e) { LV_UNUSED(e); cfg_open(); }

/* ------------------------------------------------------------------ layout */
static void layout(void)
{
    int x = s_side_open ? LIST_W + 16 : 8;
    int w = DEVOS_SCREEN_WIDTH - x - 16;
    if (s_list_box) {
        if (s_side_open) lv_obj_remove_flag(s_list_box, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_list_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (!s_detail) return;
    lv_obj_set_x(s_detail, x);
    lv_obj_set_width(s_detail, w);
    if (lbl_name) lv_obj_set_width(lbl_name, w);
    if (lbl_info) lv_obj_set_width(lbl_info, w);
    if (lbl_nodes) lv_obj_set_width(lbl_nodes, w);
    if (bar_mem) lv_obj_set_width(bar_mem, w > 420 ? 420 : w - 8);
}

static void side_toggle(void)
{
    s_side_open = !s_side_open;
    layout();
    devos_toast_show(s_side_open ? "Guest list shown" : "Guest list hidden", DEVOS_TOAST_INFO, 0);
}

/* ------------------------------------------------------------------ refresh */
static void refresh(bool force)
{
    uint32_t gen = devos_proxmox_generation();
    if (!force && gen == s_gen) return;
    s_gen = gen;
    s_n = devos_proxmox_list(s_g, DEVOS_PROXMOX_MAX);
    if (s_sel >= s_n) s_sel = s_n ? s_n - 1 : -1;
    if (s_sel < 0 && s_n) s_sel = 0;             /* pick the first guest for the detail panel */
    devos_vlist_set_count(&s_list, s_n);
    if (s_sel >= 0) s_list.sel = s_sel;
    devos_vlist_redraw(&s_list);
    if (s_empty) {
        if (s_n) lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    }

    devos_proxmox_status_t st;
    devos_proxmox_status(&st);
    char b[220];
    if (!st.configured) snprintf(b, sizeof(b), "No server - press C (or the Server button)");
    else if (st.error[0]) snprintf(b, sizeof(b), "%s", st.error);
    else if (st.note[0]) snprintf(b, sizeof(b), "%s", st.note);
    else snprintf(b, sizeof(b), "Proxmox %s  -  %d node%s  -  %d/%d running",
                  st.version[0] ? st.version : "?", st.nodes, st.nodes == 1 ? "" : "s",
                  st.running, st.total);
    devos_w_set_text(lbl_status, b);
    if (s_empty) {
        devos_w_set_text(s_empty, st.configured ? (st.error[0] ? "Nothing to show - see the status above"
                                                                  : "No guests in the cluster")
                                                : "Set the Proxmox server (C)");
    }

    devos_proxmox_node_t nd[DEVOS_PROXMOX_MAX_NODES];
    int nn = devos_proxmox_nodes(nd, DEVOS_PROXMOX_MAX_NODES);
    char nb[220];
    size_t o = 0;
    nb[0] = '\0';
    for (int i = 0; i < nn && o < sizeof(nb) - 40; i++) {
        char m[24];
        fmt_bytes(nd[i].mem, m, sizeof(m));
        o += (size_t)snprintf(nb + o, sizeof(nb) - o, "%s%s %s %.0f%%",
                              i ? "   " : "", nd[i].node, nd[i].status, (double)nd[i].cpu * 100.0);
    }
    devos_w_set_text(lbl_nodes, nb);
    show_detail();
}

static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!s_screen || lv_obj_has_flag(s_screen, LV_OBJ_FLAG_HIDDEN)) return;
    refresh(false);
}

/* ------------------------------------------------------------------ keys */
static bool prox_key(uint32_t key, uint8_t mods)
{
    /* dialogs own the keyboard while open */
    if (devos_w_dialog_open(&s_dlg_cfg)) {
        if (key == '\r' || key == '\n') { cfg_ok(); return true; }
        if (key == LV_KEY_ESC) { cfg_close(); return true; }
        devos_focus_key(&s_fcfg, key, mods);
        return true;
    }
    if (devos_w_dialog_open(&s_dlg_confirm)) {
        if (key == '\r' || key == '\n') { confirm_ok(); return true; }
        if (key == LV_KEY_ESC) { s_pending[0] = '\0'; confirm_close(); return true; }
        devos_focus_key(&s_fconfirm, key, mods);
        return true;
    }

    if ((mods & DEVOS_MOD_FN) && (key == 'l' || key == 'L')) { side_toggle(); return true; }
    if (mods & (DEVOS_MOD_CTRL | DEVOS_MOD_ALT)) return false;

    /* The guest list always has the keyboard: Up/Down pick a guest, Enter
     * opens it, and the letters act on the selection (invariant 9 - everything
     * is reachable from the keyboard alone). */
    if (devos_vlist_key(&s_list, key)) return true;
    switch (key) {
    case 'r': case 'R': devos_proxmox_refresh(); return true;
    case 's': case 'S': do_action("start"); return true;
    case 'h': case 'H': do_action("shutdown"); return true;
    case 'x': case 'X': confirm_open("stop", "Force stop"); return true;
    case 'b': case 'B': confirm_open("reboot", "Reboot"); return true;
    case 'w': case 'W': console_cb(NULL); return true;
    case 'k': case 'K': ssh_cb(NULL); return true;
    case 'c': case 'C': cfg_open(); return true;
    case LV_KEY_ESC: return false;                 /* Home */
    default: return false;
    }
}

/* ------------------------------------------------------------------ palette */
static void cmd_refresh(void *ud) { (void)ud; devos_core_open_with("proxmox", "refresh", NULL); }
static const devos_command_t CMD_REFRESH = {
    .title = "Proxmox: Refresh", .keywords = "proxmox refresh reload cluster guests",
    .hint = "Proxmox", .icon = LV_SYMBOL_REFRESH, .run = cmd_refresh,
};
static void cmd_settings(void *ud) { (void)ud; devos_core_open_with("proxmox", "settings", NULL); }
static const devos_command_t CMD_SETTINGS = {
    .title = "Proxmox: Server settings", .keywords = "proxmox server url token api settings configure",
    .hint = "Proxmox", .icon = LV_SYMBOL_SETTINGS, .run = cmd_settings,
};

/* ------------------------------------------------------------------ lifecycle */
static void prox_init(void)
{
    if (s_inited) return;
    s_inited = true;
    devos_proxmox_init();
    s_g = calloc(DEVOS_PROXMOX_MAX, sizeof(*s_g));
    if (!s_g) return;

    s_screen = devos_w_screen(&s_desc);
    s_bar = devos_w_bar(s_screen, LV_SYMBOL_DRIVE "  Proxmox", NULL);
    lbl_status = devos_w_label(s_bar, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "");
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_status, 820);
    lv_obj_align(lbl_status, LV_ALIGN_LEFT_MID, 130, 0);
    btn_server = devos_w_btn(s_bar, LV_SYMBOL_SETTINGS " Server", 90, server_cb, NULL, NULL);
    lv_obj_align(btn_server, LV_ALIGN_RIGHT_MID, -8, 0);
    btn_refresh = devos_w_btn(s_bar, LV_SYMBOL_REFRESH " Refresh", 92, refresh_cb, NULL, NULL);
    lv_obj_align_to(btn_refresh, btn_server, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    int h = DEVOS_CONTENT_HEIGHT - DEVOS_W_BAR_H - DEVOS_W_KEYS_H;
    s_list_box = devos_w_panel(s_screen, 0, DEVOS_W_BAR_H, LIST_W, h, DEVOS_W_PANEL_ALT);
    lv_obj_set_style_border_width(s_list_box, 1, 0);
    lv_obj_set_style_border_side(s_list_box, LV_BORDER_SIDE_RIGHT, 0);
    devos_vlist_create(&s_list, s_list_box, ROW_H, row_draw);
    s_list.on_select = on_select;
    s_list.active = true;
    lv_obj_set_pos(s_list.scroll, 0, 4);
    lv_obj_set_size(s_list.scroll, LIST_W - 2, h - 8);
    s_empty = devos_w_label(s_list_box, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM, "");
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_empty, LIST_W - 24);
    lv_obj_align(s_empty, LV_ALIGN_CENTER, 0, 0);

    s_detail = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_detail);
    lv_obj_set_pos(s_detail, LIST_W + 16, DEVOS_W_BAR_H + 12);
    lv_obj_set_size(s_detail, DEVOS_SCREEN_WIDTH - LIST_W - 32, h - 24);
    lv_obj_remove_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);
    lbl_name = devos_w_label(s_detail, &lv_font_montserrat_22, DEVOS_W_TEXT_ACCENT, "");
    lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_name, DEVOS_SCREEN_WIDTH - LIST_W - 40);
    lbl_state = devos_w_label(s_detail, &lv_font_montserrat_14, DEVOS_W_TEXT, "");
    lv_obj_set_pos(lbl_state, 0, 32);
    lbl_info = devos_w_label(s_detail, devos_w_mono(), DEVOS_W_TEXT_DIM, "");
    lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_info, DEVOS_SCREEN_WIDTH - LIST_W - 40);
    lv_obj_set_pos(lbl_info, 0, 56);
    lbl_cpu = devos_w_label(s_detail, devos_w_mono(), DEVOS_W_TEXT, "");
    lv_obj_set_pos(lbl_cpu, 0, 100);
    lbl_mem = devos_w_label(s_detail, devos_w_mono(), DEVOS_W_TEXT, "");
    lv_obj_set_pos(lbl_mem, 0, 122);
    bar_mem = lv_bar_create(s_detail);
    lv_obj_set_size(bar_mem, 420, 6);
    lv_obj_set_pos(bar_mem, 88, 144);
    lv_bar_set_range(bar_mem, 0, 1000);
    devos_w_track(bar_mem, DEVOS_W_PROGRESS);
    lbl_disk = devos_w_label(s_detail, devos_w_mono(), DEVOS_W_TEXT, "");
    lv_obj_set_pos(lbl_disk, 0, 162);
    lbl_uptime = devos_w_label(s_detail, devos_w_mono(), DEVOS_W_TEXT, "");
    lv_obj_set_pos(lbl_uptime, 0, 184);
    lbl_nodes = devos_w_label(s_detail, devos_w_mono(), DEVOS_W_TEXT_DIM, "");
    lv_obj_set_pos(lbl_nodes, 0, 218);
    lv_obj_set_width(lbl_nodes, DEVOS_SCREEN_WIDTH - LIST_W - 40);

    btn_start = devos_w_btn(s_detail, "Start  (S)", 130, start_cb, NULL, NULL);
    lv_obj_set_size(btn_start, 130, 36);
    lv_obj_set_pos(btn_start, 0, 300);
    btn_shutdown = devos_w_btn(s_detail, "Shutdown  (H)", 150, shutdown_cb, NULL, NULL);
    lv_obj_set_size(btn_shutdown, 150, 36);
    lv_obj_set_pos(btn_shutdown, 138, 300);
    btn_stop = devos_w_btn_kind(s_detail, DEVOS_W_BTN_DANGER, "Stop  (X)", 120, stop_cb, NULL, NULL);
    lv_obj_set_size(btn_stop, 120, 36);
    lv_obj_set_pos(btn_stop, 296, 300);
    btn_reboot = devos_w_btn(s_detail, "Reboot  (B)", 130, reboot_cb, NULL, NULL);
    lv_obj_set_size(btn_reboot, 130, 36);
    lv_obj_set_pos(btn_reboot, 424, 300);
    btn_console = devos_w_btn(s_detail, "Console  (W)", 140, console_cb, NULL, NULL);
    lv_obj_set_size(btn_console, 140, 36);
    lv_obj_set_pos(btn_console, 0, 346);
    btn_ssh = devos_w_btn(s_detail, "SSH  (K)", 120, ssh_cb, NULL, NULL);
    lv_obj_set_size(btn_ssh, 120, 36);
    lv_obj_set_pos(btn_ssh, 148, 346);

    /* config dialog */
    devos_w_dialog(&s_dlg_cfg, s_screen, 620, 400, LV_SYMBOL_SETTINGS "  Proxmox server");
    int cy = 44;
    devos_w_label(s_dlg_cfg.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Server URL  (https://host:8006)");
    lv_obj_set_pos(lv_obj_get_child(s_dlg_cfg.box, lv_obj_get_child_count(s_dlg_cfg.box) - 1), 0, cy);
    ta_url = devos_w_ta(s_dlg_cfg.box, true, 560, 34);
    lv_obj_set_pos(ta_url, 0, cy + 18);
    lv_textarea_set_max_length(ta_url, 159);
    cy += 62;
    devos_w_label(s_dlg_cfg.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "API token id  (user@realm!tokenid)");
    lv_obj_set_pos(lv_obj_get_child(s_dlg_cfg.box, lv_obj_get_child_count(s_dlg_cfg.box) - 1), 0, cy);
    ta_tid = devos_w_ta(s_dlg_cfg.box, true, 560, 34);
    lv_obj_set_pos(ta_tid, 0, cy + 18);
    lv_textarea_set_max_length(ta_tid, 95);
    cy += 62;
    devos_w_label(s_dlg_cfg.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Token secret  (kept in NVS)");
    lv_obj_set_pos(lv_obj_get_child(s_dlg_cfg.box, lv_obj_get_child_count(s_dlg_cfg.box) - 1), 0, cy);
    ta_secret = devos_w_ta(s_dlg_cfg.box, true, 560, 34);
    lv_obj_set_pos(ta_secret, 0, cy + 18);
    lv_textarea_set_max_length(ta_secret, 95);
    cy += 62;
    devos_w_label(s_dlg_cfg.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "TLS");
    lv_obj_set_pos(lv_obj_get_child(s_dlg_cfg.box, lv_obj_get_child_count(s_dlg_cfg.box) - 1), 0, cy);
    dd_insecure = devos_w_dd(s_dlg_cfg.box, "Verify the certificate\nAccept any certificate", 260);
    lv_obj_set_pos(dd_insecure, 0, cy + 18);
    devos_w_label(s_dlg_cfg.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Refresh (s)");
    lv_obj_set_pos(lv_obj_get_child(s_dlg_cfg.box, lv_obj_get_child_count(s_dlg_cfg.box) - 1), 280, cy);
    ta_interval = devos_w_ta(s_dlg_cfg.box, true, 80, 34);
    lv_obj_set_pos(ta_interval, 280, cy + 18);
    lv_textarea_set_max_length(ta_interval, 4);
    devos_w_set_text(s_dlg_cfg.msg, "Enter = save      Esc = cancel");
    lv_obj_t *cok = devos_w_btn_kind(s_dlg_cfg.box, DEVOS_W_BTN_PRIMARY, "Save", 120, cfg_ok_cb, NULL, NULL);
    lv_obj_set_size(cok, 120, 36);
    lv_obj_align(cok, LV_ALIGN_BOTTOM_RIGHT, -132, 0);
    lv_obj_t *ccl = devos_w_btn(s_dlg_cfg.box, "Cancel", 120, cfg_cancel_cb, NULL, NULL);
    lv_obj_set_size(ccl, 120, 36);
    lv_obj_align(ccl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    devos_focus_init(&s_fcfg);
    devos_focus_add(&s_fcfg, ta_url);
    devos_focus_add(&s_fcfg, ta_tid);
    devos_focus_add(&s_fcfg, ta_secret);
    devos_focus_add(&s_fcfg, dd_insecure);
    devos_focus_add(&s_fcfg, ta_interval);
    devos_focus_add(&s_fcfg, cok);
    devos_focus_add(&s_fcfg, ccl);

    /* confirm dialog */
    devos_w_dialog(&s_dlg_confirm, s_screen, 560, 170, LV_SYMBOL_WARNING "  Confirm");
    lv_obj_t *qok = devos_w_btn_kind(s_dlg_confirm.box, DEVOS_W_BTN_DANGER, "Do it", 130, confirm_ok_cb, NULL, NULL);
    lv_obj_set_size(qok, 130, 36);
    lv_obj_align(qok, LV_ALIGN_BOTTOM_RIGHT, -142, 0);
    lv_obj_t *qcl = devos_w_btn(s_dlg_confirm.box, "Cancel", 130, confirm_cancel_cb, NULL, NULL);
    lv_obj_set_size(qcl, 130, 36);
    lv_obj_align(qcl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    devos_w_set_text(s_dlg_confirm.msg, "Enter = confirm      Esc = cancel");
    devos_focus_init(&s_fconfirm);
    devos_focus_add(&s_fconfirm, qok);
    devos_focus_add(&s_fconfirm, qcl);

    devos_w_set_text(devos_w_keys(s_screen),
                     "Up/Down guests  Enter open  |  S start  H shutdown  X stop  B reboot  "
                     "W console  K ssh  R refresh  C server  Sym+L list  Sym+S keys");

    devos_cmdpal_add(&CMD_REFRESH);
    devos_cmdpal_add(&CMD_SETTINGS);

    layout();
    refresh(true);
    lv_timer_create(tick_cb, 250, NULL);
    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
}

static void prox_show(void)
{
    if (!s_screen) return;
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
    char action[24] = "", arg[64] = "";
    if (devos_core_take_intent("proxmox", action, sizeof(action), arg, sizeof(arg))) {
        if (!strcmp(action, "refresh")) devos_proxmox_refresh();
        else if (!strcmp(action, "settings")) cfg_open();
    }
    devos_proxmox_set_active(true);
    refresh(true);
    s_list.active = true;
    devos_vlist_redraw(&s_list);
}

static void prox_hide(void)
{
    devos_proxmox_set_active(false);
    if (s_screen) lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
}

static int prox_telemetry(char lines[3][64])
{
    devos_proxmox_status_t st;
    devos_proxmox_status(&st);
    if (!st.configured) snprintf(lines[0], 64, "* no server");
    else snprintf(lines[0], 64, "* %d/%d running", st.running, st.total);
    snprintf(lines[1], 64, "* %s", st.error[0] ? st.error : (st.version[0] ? st.version : "Proxmox VE"));
    snprintf(lines[2], 64, "* nodes and guests");
    return 3;
}

static const char *prox_shortcuts(void)
{
    return "Proxmox\n"
           "Guests\n"
           "Up / Down\tPick a guest\n"
           "Enter / Tab\tOpen it\n"
           "S\tStart it\n"
           "H\tShut it down cleanly\n"
           "X\tForce stop (asks first)\n"
           "B\tReboot (asks first)\n"
           "W\tIts console in the REST app\n"
           "K\tSSH to the Proxmox host\n"
           "Anywhere\n"
           "R\tRefresh the cluster now\n"
           "C\tServer settings (URL, API token)\n"
           "Sym+L\tShow / hide the guest list\n"
           "Esc\tBack to the list, then the Home Screen\n";
}

devos_app_descriptor_t *app_proxmox_get_descriptor(void)
{
    s_desc.id = DEVOS_APP_LAUNCHER;             /* auto-assigned */
    s_desc.uid = "proxmox";
    s_desc.name = "Proxmox";
    s_desc.title = "Proxmox";
    s_desc.subtitle = "Proxmox VE nodes and guests";
    s_desc.icon = LV_SYMBOL_DRIVE;
    s_desc.draw_icon = devos_icon_proxmox;
    s_desc.category = "containers";
    s_desc.init = prox_init;
    s_desc.show = prox_show;
    s_desc.hide = prox_hide;
    s_desc.handle_key = prox_key;
    s_desc.get_telemetry_lines = prox_telemetry;
    s_desc.get_shortcuts = prox_shortcuts;
    return &s_desc;
}
