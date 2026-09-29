/* Docker: watch and poke your containers - straight to a Docker daemon (or
 * a socket proxy) or through Portainer.
 *
 * Left: the containers (running first) with a health dot, image and ports.
 * Right: the selected one's details and live stats (CPU, memory, network,
 * processes) with its actions. Enter or L opens its logs (full width,
 * following new lines until you scroll up; F toggles).
 *
 * Keys: Up / Down pick, Enter / L logs, R restart, S stop or start (asks
 * first), Ctrl+R refresh now, C server settings, Esc back / Home.
 */
#include "app_docker.h"
#include "devos_config.h"
#include "devos_icons.h"
#include "devos_core.h"
#include "devos_focus.h"
#include "devos_theme.h"
#include "devos_widgets.h"
#include "devos_codeview.h"
#include "devos_docker.h"
#include "devos_toast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LIST_W   700
#define ROW_H    44
#define LOG_VIEW (128 * 1024)

static devos_app_descriptor_t s_desc;
static lv_obj_t *s_screen, *lbl_status, *btn_server, *btn_refresh, *s_keys;
static lv_obj_t *list_box, *lbl_empty;
static devos_vlist_t s_list;
static lv_obj_t *detail, *lbl_name, *lbl_state, *lbl_info, *lbl_cpu, *lbl_mem, *bar_mem, *lbl_net, *lbl_note;
static lv_obj_t *btn_logs, *btn_restart, *btn_stop, *lbl_stop, *btn_ssh, *btn_web;
static lv_obj_t *logs, *lbl_logs_hdr, *logs_scroll;
static devos_codeview_t s_cv;
static devos_w_dialog_t s_dlg_cfg, s_dlg_confirm;
static lv_obj_t *dd_mode, *ta_url, *ta_key, *ta_ep, *cb_insecure, *dd_interval, *lbl_cfg_hint;
static devos_focus_t s_fcfg, s_fconfirm, s_fdetail;

static devos_docker_ct_t *s_ct;             /* DEVOS_DOCKER_MAX */
static int s_n;
static char s_sel_id[16];
static uint32_t s_gen = 0xffffffff, s_log_gen = 0xffffffff;
static bool s_logs_open, s_follow = true, s_prog_scroll;
static char *s_log_text;
static char s_confirm_id[16], s_confirm_what[12];
static bool s_inited;


static const int INTERVALS[] = { 3, 5, 10, 30 };

/* ------------------------------------------------------------------ helpers */
static const devos_docker_ct_t *sel_ct(void)
{
    return s_list.sel >= 0 && s_list.sel < s_n ? &s_ct[s_list.sel] : NULL;
}

static lv_color_t state_color(const devos_docker_ct_t *c)
{
    const devos_palette_t *p = devos_theme_get();
    if (c->health == 2) return p->accent_danger;
    if (!strcmp(c->state, "running")) return c->health == 3 ? p->accent_warning : p->accent_secondary;
    if (!strcmp(c->state, "restarting") || !strcmp(c->state, "paused")) return p->accent_warning;
    if (!strcmp(c->state, "dead")) return p->accent_danger;
    return p->text_muted;
}

static void fmt_bytes(uint64_t b, char *out, size_t cap)
{
    if (b >= 1024ULL * 1024 * 1024) snprintf(out, cap, "%.2f GB", b / 1073741824.0);
    else if (b >= 1024 * 1024) snprintf(out, cap, "%.1f MB", b / 1048576.0);
    else if (b >= 1024) snprintf(out, cap, "%.1f KB", b / 1024.0);
    else snprintf(out, cap, "%u B", (unsigned)b);
}

static void fmt_ago(int64_t t, char *out, size_t cap)
{
    int64_t d = (int64_t)time(NULL) - t;
    if (t <= 0 || d < 0) snprintf(out, cap, "-");
    else if (d < 90) snprintf(out, cap, "%d s ago", (int)d);
    else if (d < 5400) snprintf(out, cap, "%d min ago", (int)(d / 60));
    else if (d < 172800) snprintf(out, cap, "%d h ago", (int)(d / 3600));
    else snprintf(out, cap, "%d days ago", (int)(d / 86400));
}

/* ------------------------------------------------------------------ list */
static void row_draw(devos_vlist_t *v, lv_layer_t *layer, const lv_area_t *row, int idx)
{
    LV_UNUSED(v);
    if (idx >= s_n) return;
    const devos_palette_t *p = devos_theme_get();
    const devos_docker_ct_t *c = &s_ct[idx];
    int x = row->x1 + 12, y = row->y1 + 5, w = row->x2 - row->x1;
    devos_w_draw_rect(layer, x, y + 5, x + 9, y + 14, state_color(c), LV_OPA_COVER, 5);
    int nx = devos_w_draw_text(layer, &lv_font_montserrat_16, x + 18, y, 330, c->name,
                               strcmp(c->state, "running") ? p->text_secondary : p->text_primary);
    if (c->compose[0]) devos_w_draw_text(layer, &lv_font_montserrat_12, nx + 10, y + 3, 150, c->compose, p->text_muted);
    devos_w_draw_text(layer, &lv_font_montserrat_12, row->x1 + w - 250, y + 2, 240, c->status, state_color(c));
    char sub[200];
    snprintf(sub, sizeof(sub), "%s%s%s", c->image, c->ports[0] ? "   " : "", c->ports);
    devos_w_draw_text(layer, NULL, x + 18, y + 20, w - 40, sub, p->text_muted);
}

static void show_detail(void);

static void on_select(devos_vlist_t *v, int idx)
{
    LV_UNUSED(v);
    if (idx >= 0 && idx < s_n) {
        snprintf(s_sel_id, sizeof(s_sel_id), "%s", s_ct[idx].id);
        devos_docker_select(s_sel_id, false);
    }
    show_detail();
}

static void open_logs(bool open);

static void flash(const char *msg)
{
    devos_toast_show(msg, DEVOS_TOAST_WARN, 3000);
}

/* Enter on a container that publishes SSH: a session in the Terminal. */
static bool open_ssh(void)
{
    char target[112];
    const devos_docker_ct_t *c = sel_ct();
    if (!c || !devos_docker_ssh_target(c, target, sizeof(target))) return false;
    if (!devos_core_open_with("terminal", "ssh", target)) flash("The Terminal is switched off (Settings > Apps)");
    return true;
}

/* W: its web port in the REST client. */
static void open_web(void)
{
    char url[160];
    const devos_docker_ct_t *c = sel_ct();
    if (!c) return;
    if (!devos_docker_web_url(c, url, sizeof(url))) flash("No web port published (80, 443, 8080, 5000 ...)");
    else if (!devos_core_open_with("rest", "get", url)) flash("The REST app is switched off (Settings > Apps)");
}

static void on_activate(devos_vlist_t *v, int idx)
{
    LV_UNUSED(v);
    if (idx >= 0 && idx < s_n && !open_ssh()) open_logs(true);
}

/* ------------------------------------------------------------------ detail */
static void show_detail(void)
{
    const devos_docker_ct_t *c = sel_ct();
    if (!c) {
        lv_obj_add_flag(detail, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(detail, LV_OBJ_FLAG_HIDDEN);
    devos_w_set_text(lbl_name, c->name);
    char buf[400], ago[32];
    const char *health = c->health == 1 ? "  -  healthy" : c->health == 2 ? "  -  UNHEALTHY" : c->health == 3 ? "  -  health check starting" : "";
    snprintf(buf, sizeof(buf), "%s%s", c->state, health);
    devos_w_set_text(lbl_state, buf);
    lv_obj_set_style_text_color(lbl_state, state_color(c), 0);
    fmt_ago(c->created, ago, sizeof(ago));
    snprintf(buf, sizeof(buf), "Image      %s\nStatus     %s\nCreated    %s\nPorts      %s\nCompose    %s\nID         %s",
             c->image, c->status, ago, c->ports[0] ? c->ports : "-", c->compose[0] ? c->compose : "-", c->id);
    devos_w_set_text(lbl_info, buf);
    bool running = !strcmp(c->state, "running") || !strcmp(c->state, "restarting");
    devos_w_set_text(lbl_stop, running ? LV_SYMBOL_STOP "  Stop  (S)" : LV_SYMBOL_PLAY "  Start  (S)");
    devos_w_track(btn_stop, running ? DEVOS_W_BTN_DANGER : DEVOS_W_BTN_PRIMARY);
    if (running) lv_obj_remove_state(btn_restart, LV_STATE_DISABLED);
    else lv_obj_add_state(btn_restart, LV_STATE_DISABLED);
    char link[160];
    if (devos_docker_ssh_target(c, link, sizeof(link))) lv_obj_remove_flag(btn_ssh, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(btn_ssh, LV_OBJ_FLAG_HIDDEN);
    if (devos_docker_web_url(c, link, sizeof(link))) lv_obj_remove_flag(btn_web, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(btn_web, LV_OBJ_FLAG_HIDDEN);

    devos_docker_stats_t st;
    devos_docker_stats(&st);
    if (!running) {
        devos_w_set_text(lbl_cpu, "CPU        -");
        devos_w_set_text(lbl_mem, "Memory     -");
        devos_w_set_text(lbl_net, "");
        lv_bar_set_value(bar_mem, 0, LV_ANIM_OFF);
    } else if (!st.valid || strcmp(st.id, c->id)) {
        devos_w_set_text(lbl_cpu, "CPU        measuring...");
        devos_w_set_text(lbl_mem, "Memory");
        devos_w_set_text(lbl_net, "");
    } else {
        char a[24], b[24];
        snprintf(buf, sizeof(buf), "CPU        %.1f %%", (double)st.cpu_pct);
        devos_w_set_text(lbl_cpu, buf);
        fmt_bytes(st.mem_used, a, sizeof(a));
        fmt_bytes(st.mem_limit, b, sizeof(b));
        snprintf(buf, sizeof(buf), "Memory     %s of %s", a, b);
        devos_w_set_text(lbl_mem, buf);
        lv_bar_set_value(bar_mem, st.mem_limit ? (int32_t)(st.mem_used * 1000 / st.mem_limit) : 0, LV_ANIM_OFF);
        fmt_bytes(st.net_rx, a, sizeof(a));
        fmt_bytes(st.net_tx, b, sizeof(b));
        snprintf(buf, sizeof(buf), "Network    %s in, %s out\nProcesses  %d", a, b, st.pids);
        devos_w_set_text(lbl_net, buf);
    }
}

/* ------------------------------------------------------------------ logs */
static void logs_scroll_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_prog_scroll) return;
    s_follow = lv_obj_get_scroll_bottom(logs_scroll) <= DEVOS_CODEVIEW_LINE_H;
}

static void refresh_logs(bool force)
{
    uint32_t g;
    if (!s_log_text) s_log_text = malloc(LOG_VIEW);
    if (!s_log_text) return;
    devos_docker_logs(NULL, 0, &g);                 /* just the generation */
    if (!force && g == s_log_gen) return;
    s_log_gen = g;
    size_t n = devos_docker_logs(s_log_text, LOG_VIEW, NULL);
    if (!n) snprintf(s_log_text, LOG_VIEW, "Fetching logs...");
    int y = lv_obj_get_scroll_y(logs_scroll);
    devos_codeview_set(&s_cv, s_log_text);
    s_prog_scroll = true;
    lv_obj_update_layout(logs_scroll);
    if (s_follow) lv_obj_scroll_to_y(logs_scroll, LV_COORD_MAX, LV_ANIM_OFF);
    else lv_obj_scroll_to_y(logs_scroll, y, LV_ANIM_OFF);
    s_prog_scroll = false;
}

static void open_logs(bool open)
{
    const devos_docker_ct_t *c = sel_ct();
    if (open && !c) return;
    s_logs_open = open;
    if (open) {
        char buf[120];
        snprintf(buf, sizeof(buf), LV_SYMBOL_LIST "  Logs  -  %s", c->name);
        devos_w_set_text(lbl_logs_hdr, buf);
        s_follow = true;
        devos_docker_select(c->id, true);
        s_cv.plain = true;
        devos_codeview_set(&s_cv, "Fetching logs...");
        lv_obj_remove_flag(logs, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(logs);
        s_log_gen = 0xffffffff;
    } else {
        lv_obj_add_flag(logs, LV_OBJ_FLAG_HIDDEN);
        if (c) devos_docker_select(c->id, false);
    }
}

/* ------------------------------------------------------------------ actions */
static void confirm_open(const char *what)
{
    const devos_docker_ct_t *c = sel_ct();
    if (!c) return;
    bool running = !strcmp(c->state, "running") || !strcmp(c->state, "restarting");
    if (!strcmp(what, "toggle")) what = running ? "stop" : "start";
    if (!strcmp(what, "restart") && !running) return;
    if (!strcmp(what, "start")) {                   /* harmless: no question */
        devos_docker_action(c->id, "start");
        return;
    }
    snprintf(s_confirm_id, sizeof(s_confirm_id), "%s", c->id);
    snprintf(s_confirm_what, sizeof(s_confirm_what), "%s", what);
    char buf[120];
    snprintf(buf, sizeof(buf), "%s  %s %s?", !strcmp(what, "stop") ? LV_SYMBOL_STOP : LV_SYMBOL_REFRESH,
             !strcmp(what, "stop") ? "Stop" : "Restart", c->name);
    devos_w_set_text(s_dlg_confirm.title, buf);
    devos_w_dialog_show(&s_dlg_confirm, true);
    devos_focus_first(&s_fconfirm);
}

static void confirm_ok(void)
{
    devos_w_dialog_show(&s_dlg_confirm, false);
    devos_focus_clear(&s_fconfirm);
    devos_docker_action(s_confirm_id, s_confirm_what);
}

static void confirm_ok_cb(lv_event_t *e) { LV_UNUSED(e); confirm_ok(); }
static void confirm_cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_w_dialog_show(&s_dlg_confirm, false);
    devos_focus_clear(&s_fconfirm);
}
static void logs_cb(lv_event_t *e) { LV_UNUSED(e); open_logs(true); }
static void ssh_cb(lv_event_t *e) { LV_UNUSED(e); open_ssh(); }
static void web_cb(lv_event_t *e) { LV_UNUSED(e); open_web(); }
static void restart_cb(lv_event_t *e) { LV_UNUSED(e); confirm_open("restart"); }
static void stop_cb(lv_event_t *e) { LV_UNUSED(e); confirm_open("toggle"); }
static void refresh_cb(lv_event_t *e) { LV_UNUSED(e); devos_docker_refresh(); }

/* ------------------------------------------------------------------ server dialog */
static void cfg_mode_changed(void)
{
    bool portainer = lv_dropdown_get_selected(dd_mode) == 1;
    if (portainer) {
        lv_obj_remove_state(ta_key, LV_STATE_DISABLED);
        lv_obj_remove_state(ta_ep, LV_STATE_DISABLED);
        lv_textarea_set_placeholder_text(ta_url, "https://portainer.local:9443");
        devos_w_set_text(lbl_cfg_hint, "Portainer: My account > Access tokens > Add access token, paste it above.\n"
                                       "Its default certificate is self-signed: tick Skip certificate check.");
    } else {
        lv_obj_add_state(ta_key, LV_STATE_DISABLED);
        lv_obj_add_state(ta_ep, LV_STATE_DISABLED);
        lv_textarea_set_placeholder_text(ta_url, "http://nas.local:2375");
        devos_w_set_text(lbl_cfg_hint, "The Docker Engine API over TCP (dockerd -H tcp://0.0.0.0:2375) or a socket proxy\n"
                                       "(e.g. tecnativa/docker-socket-proxy with POST=1). It has no password: keep it on\n"
                                       "your LAN or tailnet.");
    }
}

static void mode_cb(lv_event_t *e) { LV_UNUSED(e); cfg_mode_changed(); }

static void cfg_open(void)
{
    devos_docker_config_t c;
    devos_docker_get_config(&c);
    lv_dropdown_set_selected(dd_mode, c.mode == DEVOS_DOCKER_PORTAINER ? 1 : 0);
    lv_textarea_set_text(ta_url, c.url);
    lv_textarea_set_text(ta_key, c.api_key);
    char ep[12] = "";
    if (c.endpoint) snprintf(ep, sizeof(ep), "%d", c.endpoint);
    lv_textarea_set_text(ta_ep, ep);
    if (c.insecure) lv_obj_add_state(cb_insecure, LV_STATE_CHECKED);
    else lv_obj_remove_state(cb_insecure, LV_STATE_CHECKED);
    int sel = 1;
    for (int i = 0; i < 4; i++) if (INTERVALS[i] == c.interval_s) sel = i;
    lv_dropdown_set_selected(dd_interval, (uint32_t)sel);
    cfg_mode_changed();
    devos_w_set_text(s_dlg_cfg.msg, "");
    devos_w_dialog_show(&s_dlg_cfg, true);
    devos_focus_set(&s_fcfg, ta_url);
}

static void cfg_ok(void)
{
    devos_docker_config_t c;
    devos_docker_get_config(&c);
    const char *url = lv_textarea_get_text(ta_url);
    while (*url == ' ') url++;
    if (!*url) {
        devos_w_set_text(s_dlg_cfg.msg, "Enter the URL");
        return;
    }
    c.mode = lv_dropdown_get_selected(dd_mode) == 1 ? DEVOS_DOCKER_PORTAINER : DEVOS_DOCKER_DIRECT;
    if (!strstr(url, "://")) snprintf(c.url, sizeof(c.url), "%s://%s", c.mode == DEVOS_DOCKER_PORTAINER ? "https" : "http", url);
    else snprintf(c.url, sizeof(c.url), "%s", url);
    snprintf(c.api_key, sizeof(c.api_key), "%s", lv_textarea_get_text(ta_key));
    if (c.mode == DEVOS_DOCKER_PORTAINER && !c.api_key[0]) {
        devos_w_set_text(s_dlg_cfg.msg, "Portainer needs an access token");
        return;
    }
    c.endpoint = atoi(lv_textarea_get_text(ta_ep));
    c.insecure = lv_obj_has_state(cb_insecure, LV_STATE_CHECKED);
    c.interval_s = INTERVALS[lv_dropdown_get_selected(dd_interval)];
    devos_docker_set_config(&c);
    devos_w_dialog_show(&s_dlg_cfg, false);
    devos_focus_clear(&s_fcfg);
    s_list.sel = -1;
    s_sel_id[0] = '\0';
    s_gen = 0xffffffff;
}

static void cfg_ok_cb(lv_event_t *e) { LV_UNUSED(e); cfg_ok(); }
static void cfg_cancel_cb(lv_event_t *e) { LV_UNUSED(e); devos_w_dialog_show(&s_dlg_cfg, false); devos_focus_clear(&s_fcfg); }
static void server_cb(lv_event_t *e) { LV_UNUSED(e); cfg_open(); }

/* ------------------------------------------------------------------ refresh */
static void refresh(bool force)
{
    devos_docker_status_t st;
    devos_docker_status(&st);
    uint32_t g = devos_docker_generation();
    if (force || g != s_gen) {
        s_gen = g;
        s_n = devos_docker_list(s_ct, DEVOS_DOCKER_MAX);
        devos_vlist_set_count(&s_list, s_n);
        int sel = -1;
        for (int i = 0; i < s_n; i++) if (!strcmp(s_ct[i].id, s_sel_id)) sel = i;
        if (sel < 0 && s_n) {
            sel = 0;
            snprintf(s_sel_id, sizeof(s_sel_id), "%s", s_ct[0].id);
            devos_docker_select(s_sel_id, s_logs_open);
        }
        s_list.sel = sel;
        devos_vlist_redraw(&s_list);
        show_detail();
    }
    char buf[240], ago[32];
    if (!st.configured) snprintf(buf, sizeof(buf), "Not set up - press C");
    else if (st.error[0]) snprintf(buf, sizeof(buf), "%s", st.error);
    else if (!st.updated) snprintf(buf, sizeof(buf), "Connecting...");
    else {
        fmt_ago(st.updated, ago, sizeof(ago));
        snprintf(buf, sizeof(buf), "%s%s%s%s%s  -  %d running of %d  -  updated %s", st.host[0] ? st.host : "Docker",
                 st.version[0] ? "  (Docker " : "", st.version, st.version[0] ? ")" : "",
                 st.endpoint_name[0] ? "  via Portainer" : "", st.running, st.total, ago);
    }
    devos_w_set_text(lbl_status, buf);
    devos_w_track(lbl_status, st.error[0] ? DEVOS_W_TEXT_ERR : DEVOS_W_TEXT_DIM);
    devos_w_set_text(lbl_note, st.note);
    if (s_n) lv_obj_add_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_remove_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
        devos_w_set_text(lbl_empty, !st.configured ? "Press C to connect to a Docker daemon or Portainer." :
                                    st.error[0] ? "Couldn't list containers - see the top line. C changes the server." :
                                    st.updated ? "No containers." : "Connecting...");
    }
    if (s_logs_open) refresh_logs(false);
    else if (sel_ct()) show_detail();
}

static const char *keys_text(void)
{
    if (devos_w_dialog_open(&s_dlg_cfg)) return "Tab / arrows move    Left / Right change    Enter saves    Esc cancels";
    if (devos_w_dialog_open(&s_dlg_confirm)) return "Enter confirms    Esc cancels";
    if (s_logs_open) return "Up / Down / PgUp / PgDn scroll    F follow new lines    R restart    Esc back to the list";
    static char k[200];
    const devos_docker_ct_t *c = sel_ct();
    char link[160];
    bool ssh = c && devos_docker_ssh_target(c, link, sizeof(link));
    bool web = c && devos_docker_web_url(c, link, sizeof(link));
    snprintf(k, sizeof(k), "Up / Down pick    %s%s    R restart    S stop / start    Ctrl+R refresh    C server    Esc home",
             ssh ? "Enter SSH    L logs" : "Enter or L logs", web ? "    W open in REST" : "");
    return k;
}

static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!s_screen || lv_obj_has_flag(s_screen, LV_OBJ_FLAG_HIDDEN)) return;
    refresh(false);
    devos_w_set_text(s_keys, keys_text());
}

/* ------------------------------------------------------------------ keys */
static bool docker_key(uint32_t key, uint8_t mods)
{
    if ((mods & DEVOS_MOD_CTRL) && key >= 1 && key <= 26 && key != '\b' && key != '\t' && key != '\n' && key != '\r')
        key += 'a' - 1;
    if (devos_w_dialog_open(&s_dlg_confirm)) {
        if (key == LV_KEY_ESC) confirm_cancel_cb(NULL);
        else if (!devos_focus_key(&s_fconfirm, key, mods) && (key == '\r' || key == '\n')) confirm_ok();
        return true;
    }
    if (devos_w_dialog_open(&s_dlg_cfg)) {
        if (key == LV_KEY_ESC && !s_fcfg.dd_open) cfg_cancel_cb(NULL);
        else if (devos_focus_key(&s_fcfg, key, mods)) {}
        else if (key == '\r' || key == '\n') cfg_ok();
        return true;
    }
    if ((mods & DEVOS_MOD_CTRL) && (key == 'r' || key == 'R')) { devos_docker_refresh(); return true; }
    if (mods & (DEVOS_MOD_CTRL | DEVOS_MOD_ALT | DEVOS_MOD_FN)) return false;

    if (s_logs_open) {
        int page = lv_obj_get_height(logs_scroll) - 40;
        switch (key) {
        case LV_KEY_ESC: open_logs(false); return true;
        case LV_KEY_UP: lv_obj_scroll_by_bounded(logs_scroll, 0, DEVOS_CODEVIEW_LINE_H * 2, LV_ANIM_OFF); s_follow = false; return true;
        case LV_KEY_DOWN: lv_obj_scroll_by_bounded(logs_scroll, 0, -DEVOS_CODEVIEW_LINE_H * 2, LV_ANIM_OFF); return true;
        case DEVOS_KEY_PGUP: lv_obj_scroll_by_bounded(logs_scroll, 0, page, LV_ANIM_OFF); s_follow = false; return true;
        case DEVOS_KEY_PGDN: lv_obj_scroll_by_bounded(logs_scroll, 0, -page, LV_ANIM_OFF); return true;
        case LV_KEY_HOME: lv_obj_scroll_to_y(logs_scroll, 0, LV_ANIM_OFF); s_follow = false; return true;
        case LV_KEY_END: case 'f': case 'F':
            s_follow = key == LV_KEY_END ? true : !s_follow;
            if (s_follow) lv_obj_scroll_to_y(logs_scroll, LV_COORD_MAX, LV_ANIM_OFF);
            return true;
        case 'r': case 'R': confirm_open("restart"); return true;
        default: return key >= 32 && key < 127;
        }
    }
    if (devos_focus_get(&s_fdetail) && s_fdetail.ring && devos_focus_key(&s_fdetail, key, mods)) return true;
    if (devos_vlist_key(&s_list, key)) return true;
    switch (key) {
    case 'l': case 'L': open_logs(true); return true;
    case 'w': case 'W': open_web(); return true;
    case 'r': case 'R': confirm_open("restart"); return true;
    case 's': case 'S': confirm_open("toggle"); return true;
    case 'c': case 'C': cfg_open(); return true;
    case '\t': devos_focus_first(&s_fdetail); return true;              /* the action buttons */
    case LV_KEY_ESC:
        if (devos_focus_get(&s_fdetail)) { devos_focus_clear(&s_fdetail); return true; }
        return false;
    default: return key >= 32 && key < 127;
    }
}

/* ------------------------------------------------------------------ init */
static void docker_init(void)
{
    if (s_inited) return;
    s_inited = true;
    devos_docker_init();
    s_ct = calloc(DEVOS_DOCKER_MAX, sizeof(*s_ct));

    s_screen = devos_w_screen(&s_desc);
    lv_obj_t *bar = devos_w_bar(s_screen, LV_SYMBOL_DRIVE "  Docker", NULL);
    lbl_status = devos_w_label(bar, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "");
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_status, 900);
    lv_obj_align(lbl_status, LV_ALIGN_LEFT_MID, 124, 0);
    btn_server = devos_w_btn(bar, LV_SYMBOL_SETTINGS " Server", 84, server_cb, NULL, NULL);
    lv_obj_align(btn_server, LV_ALIGN_RIGHT_MID, -8, 0);
    btn_refresh = devos_w_btn(bar, LV_SYMBOL_REFRESH " Refresh", 88, refresh_cb, NULL, NULL);
    lv_obj_align_to(btn_refresh, btn_server, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    int h = DEVOS_CONTENT_HEIGHT - DEVOS_W_BAR_H - DEVOS_W_KEYS_H;
    list_box = devos_w_panel(s_screen, 0, DEVOS_W_BAR_H, LIST_W, h, DEVOS_W_PANEL_ALT);
    lv_obj_set_style_border_side(list_box, LV_BORDER_SIDE_RIGHT, 0);
    devos_vlist_create(&s_list, list_box, ROW_H, row_draw);
    s_list.on_select = on_select;
    s_list.on_activate = on_activate;
    s_list.active = true;
    lv_obj_set_pos(s_list.scroll, 0, 4);
    lv_obj_set_size(s_list.scroll, LIST_W - 2, h - 8);
    lbl_empty = devos_w_label(list_box, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM, "");
    lv_obj_align(lbl_empty, LV_ALIGN_CENTER, 0, 0);

    detail = lv_obj_create(s_screen);
    lv_obj_remove_style_all(detail);
    lv_obj_set_pos(detail, LIST_W + 16, DEVOS_W_BAR_H + 12);
    lv_obj_set_size(detail, DEVOS_SCREEN_WIDTH - LIST_W - 32, h - 24);
    lv_obj_remove_flag(detail, LV_OBJ_FLAG_SCROLLABLE);
    lbl_name = devos_w_label(detail, &lv_font_montserrat_22, DEVOS_W_TEXT_ACCENT, "");
    lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_name, DEVOS_SCREEN_WIDTH - LIST_W - 40);
    lbl_state = devos_w_label(detail, &lv_font_montserrat_14, DEVOS_W_TEXT, "");
    lv_obj_set_pos(lbl_state, 0, 32);
    lbl_info = devos_w_label(detail, devos_w_mono(), DEVOS_W_TEXT, "");
    lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_DOT);
    lv_obj_set_size(lbl_info, DEVOS_SCREEN_WIDTH - LIST_W - 40, 110);
    lv_obj_set_pos(lbl_info, 0, 60);
    lbl_cpu = devos_w_label(detail, devos_w_mono(), DEVOS_W_TEXT, "");
    lv_obj_set_pos(lbl_cpu, 0, 186);
    lbl_mem = devos_w_label(detail, devos_w_mono(), DEVOS_W_TEXT, "");
    lv_obj_set_pos(lbl_mem, 0, 208);
    bar_mem = lv_bar_create(detail);
    lv_obj_set_size(bar_mem, 400, 6);
    lv_obj_set_pos(bar_mem, 88, 230);
    lv_bar_set_range(bar_mem, 0, 1000);
    devos_w_track(bar_mem, DEVOS_W_PROGRESS);
    lbl_net = devos_w_label(detail, devos_w_mono(), DEVOS_W_TEXT, "");
    lv_obj_set_pos(lbl_net, 0, 246);
    btn_logs = devos_w_btn(detail, LV_SYMBOL_LIST "  Logs  (L)", 150, logs_cb, NULL, NULL);
    lv_obj_set_size(btn_logs, 150, 36);
    lv_obj_set_pos(btn_logs, 0, 310);
    btn_restart = devos_w_btn(detail, LV_SYMBOL_REFRESH "  Restart  (R)", 160, restart_cb, NULL, NULL);
    lv_obj_set_size(btn_restart, 160, 36);
    lv_obj_set_pos(btn_restart, 162, 310);
    btn_stop = devos_w_btn_kind(detail, DEVOS_W_BTN_DANGER, LV_SYMBOL_STOP "  Stop  (S)", 150, stop_cb, NULL, &lbl_stop);
    lv_obj_set_size(btn_stop, 150, 36);
    lv_obj_set_pos(btn_stop, 334, 310);
    /* deep links, shown when the container publishes the port */
    btn_ssh = devos_w_btn(detail, LV_SYMBOL_KEYBOARD "  SSH  (Enter)", 150, ssh_cb, NULL, NULL);
    lv_obj_set_size(btn_ssh, 150, 36);
    lv_obj_set_pos(btn_ssh, 0, 354);
    btn_web = devos_w_btn(detail, LV_SYMBOL_UPLOAD "  Open in REST  (W)", 200, web_cb, NULL, NULL);
    lv_obj_set_size(btn_web, 200, 36);
    lv_obj_set_pos(btn_web, 162, 354);
    lbl_note = devos_w_label(detail, &lv_font_montserrat_12, DEVOS_W_TEXT_OK, "");
    lv_label_set_long_mode(lbl_note, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_note, DEVOS_SCREEN_WIDTH - LIST_W - 40);
    lv_obj_set_pos(lbl_note, 0, 404);
    lv_obj_add_flag(detail, LV_OBJ_FLAG_HIDDEN);
    devos_focus_init(&s_fdetail);
    devos_focus_add(&s_fdetail, btn_logs);
    devos_focus_add(&s_fdetail, btn_restart);
    devos_focus_add(&s_fdetail, btn_stop);
    devos_focus_add(&s_fdetail, btn_ssh);
    devos_focus_add(&s_fdetail, btn_web);

    logs = devos_w_panel(s_screen, 0, DEVOS_W_BAR_H, DEVOS_SCREEN_WIDTH, h, DEVOS_W_CODE);
    lbl_logs_hdr = devos_w_label(logs, &lv_font_montserrat_14, DEVOS_W_TEXT_ACCENT, "");
    lv_obj_set_pos(lbl_logs_hdr, 14, 8);
    logs_scroll = lv_obj_create(logs);
    lv_obj_set_style_bg_opa(logs_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(logs_scroll, 0, 0);
    lv_obj_set_pos(logs_scroll, 6, 32);
    lv_obj_set_size(logs_scroll, DEVOS_SCREEN_WIDTH - 12, h - 36);
    devos_codeview_create(&s_cv, logs_scroll);
    lv_obj_set_style_pad_bottom(logs_scroll, 12, 0);
    s_cv.plain = true;
    lv_obj_add_event_cb(logs_scroll, logs_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_flag(logs, LV_OBJ_FLAG_HIDDEN);

    s_keys = devos_w_keys(s_screen);

    /* server dialog */
    devos_w_dialog(&s_dlg_cfg, s_screen, 700, 420, LV_SYMBOL_SETTINGS "  Docker server");
    lv_obj_t *l = devos_w_label(s_dlg_cfg.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Connect to");
    lv_obj_set_pos(l, 0, 34);
    dd_mode = devos_w_dd(s_dlg_cfg.box, "Docker API (daemon or socket proxy)\nPortainer", 320);
    lv_obj_set_pos(dd_mode, 0, 52);
    lv_obj_add_event_cb(dd_mode, mode_cb, LV_EVENT_VALUE_CHANGED, NULL);
    l = devos_w_label(s_dlg_cfg.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Refresh every");
    lv_obj_set_pos(l, 340, 34);
    dd_interval = devos_w_dd(s_dlg_cfg.box, "3 s\n5 s\n10 s\n30 s", 110);
    lv_obj_set_pos(dd_interval, 340, 52);
    ta_url = devos_w_field(s_dlg_cfg.box, "URL", 0, 96, 656);
    ta_key = devos_w_field(s_dlg_cfg.box, "Access token (Portainer)", 0, 158, 480);
    lv_textarea_set_password_mode(ta_key, true);
    ta_ep = devos_w_field(s_dlg_cfg.box, "Environment ID (blank = first)", 496, 158, 160);
    lv_textarea_set_accepted_chars(ta_ep, "0123456789");
    cb_insecure = devos_w_cb(s_dlg_cfg.box, "Skip certificate check (self-signed)");
    lv_obj_set_pos(cb_insecure, 0, 224);
    lbl_cfg_hint = devos_w_label(s_dlg_cfg.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "");
    lv_obj_set_pos(lbl_cfg_hint, 0, 262);
    lv_obj_t *bok = devos_w_btn_kind(s_dlg_cfg.box, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_OK "  Save", 120, cfg_ok_cb, NULL, NULL);
    lv_obj_set_size(bok, 120, 36);
    lv_obj_align(bok, LV_ALIGN_BOTTOM_RIGHT, -132, 0);
    lv_obj_t *bc = devos_w_btn(s_dlg_cfg.box, "Cancel", 120, cfg_cancel_cb, NULL, NULL);
    lv_obj_set_size(bc, 120, 36);
    lv_obj_align(bc, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    devos_focus_init(&s_fcfg);
    lv_obj_t *order[] = { dd_mode, dd_interval, ta_url, ta_key, ta_ep, cb_insecure, bok, bc };
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) devos_focus_add(&s_fcfg, order[i]);

    /* confirm dialog */
    devos_w_dialog(&s_dlg_confirm, s_screen, 520, 130, "");
    lv_obj_t *cok = devos_w_btn_kind(s_dlg_confirm.box, DEVOS_W_BTN_DANGER, LV_SYMBOL_OK "  Yes", 120, confirm_ok_cb, NULL, NULL);
    lv_obj_set_size(cok, 120, 36);
    lv_obj_align(cok, LV_ALIGN_BOTTOM_RIGHT, -132, 0);
    lv_obj_t *ccl = devos_w_btn(s_dlg_confirm.box, "Cancel", 120, confirm_cancel_cb, NULL, NULL);
    lv_obj_set_size(ccl, 120, 36);
    lv_obj_align(ccl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    devos_focus_init(&s_fconfirm);
    devos_focus_add(&s_fconfirm, cok);
    devos_focus_add(&s_fconfirm, ccl);

    lv_timer_create(tick_cb, 250, NULL);
}

static void docker_show(void)
{
    devos_docker_set_active(true);
    refresh(true);
    if (!devos_docker_configured()) cfg_open();
}

static void docker_hide(void)
{
    devos_docker_set_active(false);
    devos_focus_clear(&s_fdetail);
}

static int docker_telemetry(char lines[3][64])
{
    devos_docker_status_t st;
    devos_docker_status(&st);
    if (!st.configured) {
        snprintf(lines[0], 64, "* Not set up");
        snprintf(lines[1], 64, "* Docker API or Portainer");
        return 2;
    }
    devos_docker_config_t c;
    devos_docker_get_config(&c);
    const char *host = strstr(c.url, "://");
    snprintf(lines[0], 64, "* %.50s", host ? host + 3 : c.url);
    if (st.updated) snprintf(lines[1], 64, "* %d running of %d", st.running, st.total);
    else snprintf(lines[1], 64, "* Opens to check");
    snprintf(lines[2], 64, "* %s", c.mode == DEVOS_DOCKER_PORTAINER ? "via Portainer" : "Docker API");
    return 3;
}

/* Sym+S sheet (devos_shortcuts.h) */
static const char *docker_shortcuts(void)
{
    return
        "Containers\n"
        "Up / Down\tPick a container\n"
        "Enter\tSSH into it (if it publishes port 22), else its logs\n"
        "L\tLogs\n"
        "W\tOpen its web port in REST\n"
        "R\tRestart\n"
        "S\tStop / start\n"
        "C\tServer: Docker API or Portainer\n"
        "Ctrl+R\tRefresh now\n"
        "Tab\tThe buttons beside the list\n"
        "Logs\n"
        "Up / Down, Sym+Up / Down\tScroll\n"
        "F\tFollow new lines\n"
        "R\tRestart\n"
        "Esc\tBack to the list\n";
}

devos_app_descriptor_t *app_docker_get_descriptor(void)
{
    s_desc.id = DEVOS_APP_LAUNCHER;                 /* auto-assigned */
    s_desc.uid = "docker";
    s_desc.icon = LV_SYMBOL_DRIVE;
    s_desc.draw_icon = devos_icon_docker;
    s_desc.category = "network";
    s_desc.name = "Docker";
    s_desc.title = "Docker";
    s_desc.subtitle = "Containers, logs, restarts";
    s_desc.init = docker_init;
    s_desc.show = docker_show;
    s_desc.hide = docker_hide;
    s_desc.handle_key = docker_key;
    s_desc.get_telemetry_lines = docker_telemetry;
    s_desc.get_shortcuts = docker_shortcuts;
    return &s_desc;
}
