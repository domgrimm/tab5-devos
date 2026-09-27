/* WireGuard: tunnels from wg-quick configs.
 *
 * Top: tunnel state, address, endpoint, handshake age and our public key
 * (to add on the server). Left: saved tunnels (kept in NVS). Right: the
 * selected tunnel's details, and .conf files found on the SD card (in
 * /wireguard or the card's root) to import.
 *
 * Keys: Up/Down pick, Tab switches between tunnels and SD files, Enter
 * connects / disconnects (or imports a file), D deletes a tunnel, R rescans
 * the SD card, Esc goes home.
 */
#include "app_wireguard.h"
#include "devos_config.h"
#include "devos_theme.h"
#include "devos_core.h"
#include "devos_wireguard.h"
#include "devos_qr.h"
#include "devos_focus.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

LV_FONT_DECLARE(lv_font_nimbus_mono_14);

#define WG_DIR      TAB5_SD_MOUNT_POINT "/wireguard"
#define MAX_FILES   10
#define ROW_H       52
#define KEYS_H      24                  /* key hints footer */

typedef struct {
    char path[160];
    char name[DEVOS_WG_NAME_MAX];
} conf_file_t;

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen, *card_top, *lbl_title, *lbl_state, *lbl_line2, *lbl_msg, *lbl_pub;
static lv_obj_t *btn_conn, *lbl_conn, *btn_del, *lbl_del;
static lv_obj_t *card_tun, *lbl_tun_hdr, *tun_rows[DEVOS_WG_MAX_TUNNELS], *tun_name[DEVOS_WG_MAX_TUNNELS],
                *tun_sub[DEVOS_WG_MAX_TUNNELS], *lbl_tun_empty;
static lv_obj_t *card_det, *lbl_det_hdr, *lbl_det;
static lv_obj_t *card_imp, *lbl_imp_hdr, *file_rows[MAX_FILES], *file_lbl[MAX_FILES], *lbl_imp_hint, *btn_rescan,
                *lbl_rescan;
static lv_obj_t *overlay, *modal, *lbl_modal, *btn_m_ok, *lbl_m_ok, *btn_m_cancel, *lbl_m_cancel;
static lv_obj_t *lbl_keys;
static lv_obj_t *btn_scan, *lbl_scan;

/* QR scan: camera preview, then name the tunnel */
static lv_obj_t *scan_overlay, *scan_panel, *lbl_scan_title, *scan_img, *lbl_scan_status, *lbl_scan_keys;
static lv_obj_t *name_box, *ta_name, *btn_name_save, *lbl_name_save, *btn_name_cancel, *lbl_name_cancel, *lbl_name_err;
static enum { SCAN_OFF, SCAN_RUNNING, SCAN_NAMING } s_scan;
static EXT_RAM_BSS_ATTR uint8_t s_prev_buf[DEVOS_QR_PREVIEW * DEVOS_QR_PREVIEW];
static lv_image_dsc_t s_prev_dsc;
static uint32_t s_prev_gen, s_scan_retry_at;
static EXT_RAM_BSS_ATTR char s_scanned[DEVOS_QR_TEXT_MAX];
static devos_focus_t s_name_f;

static conf_file_t s_files[MAX_FILES];
static int s_file_n;
static int s_sel_tun, s_sel_file = -1;
static bool s_focus_files;
static uint32_t s_last_gen = 0xFFFFFFFFu;
static enum { M_NONE, M_DELETE_TUNNEL, M_DELETE_FILE } s_modal;
static char s_modal_path[160];
static char s_note[200];                 /* last action result */
static bool s_note_err;

static void refresh(bool force);

/* ------------------------------------------------------------------ helpers */
static void set_text(lv_obj_t *l, const char *t)
{
    if (l && strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *text, int w, lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, 36);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    if (lbl_out) *lbl_out = l;
    return b;
}

static lv_obj_t *mk_card(int x, int y, int w, int h, const char *hdr, lv_obj_t **hdr_out)
{
    lv_obj_t *c = lv_obj_create(screen);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    if (hdr) {
        lv_obj_t *l = mk_label(c, &lv_font_montserrat_12, hdr);
        if (hdr_out) *hdr_out = l;
    }
    return c;
}

/* Addresses are in network byte order. */
static void fmt_ip(uint32_t nbo, char *out, size_t cap)
{
    const uint8_t *b = (const uint8_t *)&nbo;
    snprintf(out, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static void fmt_cidr(const devos_wg_cidr_t *c, char *out, size_t cap)
{
    char ip[16];
    fmt_ip(c->ip, ip, sizeof(ip));
    const uint8_t *m = (const uint8_t *)&c->mask;
    int bits = 0;
    for (int i = 0; i < 4; i++) for (int k = 7; k >= 0 && (m[i] >> k) & 1; k--) bits++;
    snprintf(out, cap, "%s/%d", ip, bits);
}

/* ------------------------------------------------------------------ SD files */
static bool has_conf_ext(const char *n)
{
    size_t l = strlen(n);
    return l > 5 && !strcasecmp(n + l - 5, ".conf") && n[0] != '.';
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_file_n < MAX_FILES) {
        if (!has_conf_ext(e->d_name)) continue;
        conf_file_t *f = &s_files[s_file_n++];
        snprintf(f->path, sizeof(f->path), "%s/%.100s", dir, e->d_name);
        size_t l = strlen(e->d_name) - 5;
        if (l > sizeof(f->name) - 1) l = sizeof(f->name) - 1;
        memcpy(f->name, e->d_name, l);
        f->name[l] = '\0';
    }
    closedir(d);
}

static void scan_files(void)
{
    s_file_n = 0;
    scan_dir(WG_DIR);
    scan_dir(TAB5_SD_MOUNT_POINT);
    if (s_sel_file >= s_file_n) s_sel_file = s_file_n - 1;
}

static void import_file(int i)
{
    if (i < 0 || i >= s_file_n) return;
    char *text = calloc(1, DEVOS_WG_CONF_MAX + 1);
    if (!text) return;
    FILE *f = fopen(s_files[i].path, "rb");
    size_t n = f ? fread(text, 1, DEVOS_WG_CONF_MAX, f) : 0;
    if (f) fclose(f);
    char err[160] = "";
    int idx = -1;
    if (!f) snprintf(err, sizeof(err), "Can't open %s", s_files[i].path);
    else if (n >= DEVOS_WG_CONF_MAX) snprintf(err, sizeof(err), "%s is too big for a WireGuard config", s_files[i].name);
    else idx = devos_wg_add(s_files[i].name, text, err, sizeof(err));
    memset(text, 0, DEVOS_WG_CONF_MAX + 1);
    free(text);
    if (idx < 0) {
        snprintf(s_note, sizeof(s_note), "Couldn't import %s: %s", s_files[i].name, err);
        s_note_err = true;
        refresh(true);
        return;
    }
    s_sel_tun = idx;
    s_focus_files = false;
    snprintf(s_note, sizeof(s_note), "Imported \"%s\"", s_files[i].name);
    s_note_err = false;
    /* the file holds the private key: offer to delete it */
    s_modal = M_DELETE_FILE;
    snprintf(s_modal_path, sizeof(s_modal_path), "%s", s_files[i].path);
    char msg[260];
    snprintf(msg, sizeof(msg), "Imported \"%s\" - it's stored on the Tab5 now.\n\n"
             "%s on the SD card holds the tunnel's private key. Delete it?", s_files[i].name,
             s_files[i].path + strlen(TAB5_SD_MOUNT_POINT));
    set_text(lbl_modal, msg);
    set_text(lbl_m_ok, LV_SYMBOL_TRASH "  Delete file");
    set_text(lbl_m_cancel, "Keep it");
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay);
    refresh(true);
}

/* ------------------------------------------------------------------ actions */
static void toggle_conn(void)
{
    devos_wg_info_t in;
    devos_wg_get_info(&in);
    if (devos_wg_active() && in.active == s_sel_tun) {
        devos_wg_disconnect();
        snprintf(s_note, sizeof(s_note), "Disconnected");
        s_note_err = false;
    } else if (s_sel_tun >= 0 && s_sel_tun < devos_wg_count()) {
        if (devos_wg_active()) devos_wg_disconnect();
        s_note[0] = '\0';
        devos_wg_connect(s_sel_tun);
    }
    refresh(true);
}

static void ask_delete_tunnel(void)
{
    if (s_sel_tun < 0 || s_sel_tun >= devos_wg_count()) return;
    devos_wg_info_t in;
    devos_wg_get_info(&in);
    if (devos_wg_active() && in.active == s_sel_tun) {
        snprintf(s_note, sizeof(s_note), "Disconnect it first");
        s_note_err = true;
        refresh(true);
        return;
    }
    s_modal = M_DELETE_TUNNEL;
    char msg[160];
    snprintf(msg, sizeof(msg), "Delete the tunnel \"%s\" from the Tab5?\nIts keys are erased.", devos_wg_name(s_sel_tun));
    set_text(lbl_modal, msg);
    set_text(lbl_m_ok, LV_SYMBOL_TRASH "  Delete");
    set_text(lbl_m_cancel, "Cancel");
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay);
}

static void modal_close(void)
{
    s_modal = M_NONE;
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

static void modal_ok(void)
{
    if (s_modal == M_DELETE_TUNNEL) {
        char name[DEVOS_WG_NAME_MAX];
        snprintf(name, sizeof(name), "%s", devos_wg_name(s_sel_tun));
        if (devos_wg_remove(s_sel_tun) == 0) snprintf(s_note, sizeof(s_note), "Deleted \"%s\"", name);
        s_note_err = false;
        if (s_sel_tun >= devos_wg_count()) s_sel_tun = devos_wg_count() - 1;
    } else if (s_modal == M_DELETE_FILE) {
        const char *shown = s_modal_path + strlen(TAB5_SD_MOUNT_POINT);
        s_note_err = remove(s_modal_path) != 0;
        snprintf(s_note, sizeof(s_note), s_note_err ? "Imported, but couldn't delete %s" : "Imported, and deleted %s", shown);
        scan_files();
    }
    modal_close();
    refresh(true);
}

static void conn_cb(lv_event_t *e) { LV_UNUSED(e); toggle_conn(); }
static void del_cb(lv_event_t *e) { LV_UNUSED(e); ask_delete_tunnel(); }
static void m_ok_cb(lv_event_t *e) { LV_UNUSED(e); modal_ok(); }
static void m_cancel_cb(lv_event_t *e) { LV_UNUSED(e); modal_close(); refresh(true); }

static void rescan_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    scan_files();
    refresh(true);
}

static void tun_row_cb(lv_event_t *e)
{
    s_sel_tun = (int)(intptr_t)lv_event_get_user_data(e);
    s_focus_files = false;
    refresh(true);
}

static void file_row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    s_focus_files = true;
    s_sel_file = i;
    import_file(i);
}

/* ------------------------------------------------------------------ refresh */
/* Bounded append (ignores overflow; the text is just cut). */
static void app(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void app(char *out, size_t cap, const char *fmt, ...)
{
    size_t o = strlen(out);
    if (o + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + o, cap - o, fmt, ap);
    va_end(ap);
}

static void describe(int idx, char *out, size_t cap)
{
    devos_wg_config_t c;
    out[0] = '\0';
    if (devos_wg_get_config(idx, &c) != 0) {
        app(out, cap, "Can't read this tunnel's config.");
        return;
    }
    char a[24], dns[20] = "Wi-Fi's DNS";
    fmt_cidr(&c.address, a, sizeof(a));
    if (c.dns) fmt_ip(c.dns, dns, sizeof(dns));
    app(out, cap, "Address    %s\nDNS        %s%s\nMTU        %d", a, dns,
        !c.dns ? "" : c.full_tunnel ? " (used while connected)" : " (unused: split tunnel)", c.mtu ? c.mtu : 1420);
    bool v6 = false;
    for (int i = 0; i < c.peer_n; i++) {
        const devos_wg_peer_t *p = &c.peers[i];
        app(out, cap, "\n\nPeer %.10s...\n  Endpoint  ", p->public_key);
        if (p->endpoint_host[0]) app(out, cap, "%.60s:%d", p->endpoint_host, p->endpoint_port);
        else app(out, cap, "(none)");
        app(out, cap, "\n  Allowed   ");
        for (int k = 0; k < p->allowed_n; k++) {
            char x[24];
            fmt_cidr(&p->allowed[k], x, sizeof(x));
            app(out, cap, "%s%s", k ? ", " : "", x);
        }
        if (p->keepalive_s) app(out, cap, "\n  Keepalive %d s", p->keepalive_s);
        if (p->allowed_ipv6_skipped) v6 = true;
    }
    app(out, cap, "\n\n%s", c.full_tunnel ? "Full tunnel: Tab5 app traffic (SSH, MQTT...) goes through it."
                                          : "Split tunnel: only the addresses above go through it.");
    if (v6) app(out, cap, "\nIPv6 entries are ignored (IPv4 only).");
    memset(&c, 0, sizeof(c));
}

static void refresh(bool force)
{
    const devos_palette_t *p = devos_theme_get();
    devos_wg_info_t in;
    devos_wg_get_info(&in);
    char buf[400];
    int n = devos_wg_count();
    if (s_sel_tun >= n) s_sel_tun = n - 1;
    if (s_sel_tun < 0 && n) s_sel_tun = 0;

    /* header */
    lv_color_t sc = p->text_secondary;
    switch (in.state) {
    case DEVOS_WG_UP:
        snprintf(buf, sizeof(buf), LV_SYMBOL_OK "  Connected to %s", in.name);
        sc = p->accent_secondary;
        break;
    case DEVOS_WG_CONNECTING:
        snprintf(buf, sizeof(buf), LV_SYMBOL_REFRESH "  Connecting to %s...", in.name);
        sc = p->accent_warning;
        break;
    case DEVOS_WG_ERROR:
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING "  %s didn't connect", in.name[0] ? in.name : "Tunnel");
        sc = p->accent_danger;
        break;
    default:
        snprintf(buf, sizeof(buf), n ? "Off" : "No tunnels yet");
        break;
    }
    set_text(lbl_state, buf);
    lv_obj_set_style_text_color(lbl_state, sc, 0);
    if (devos_wg_active()) {
        char hs[40] = "no handshake yet", rx[40] = "";
        if (in.handshake_age_s >= 0) snprintf(hs, sizeof(hs), "handshake %d s ago", in.handshake_age_s);
        if (in.last_rx_age_s >= 0) snprintf(rx, sizeof(rx), "   last packet %d s ago", in.last_rx_age_s);
        snprintf(buf, sizeof(buf), "%s   via %s   %s%s", in.address, in.endpoint[0] ? in.endpoint : "...", hs, rx);
    } else if (n) {
        snprintf(buf, sizeof(buf), "Pick a tunnel and press Connect (Enter). Only one runs at a time, and not with Tailscale.");
    } else {
        snprintf(buf, sizeof(buf), "Copy a wg-quick .conf file to /wireguard on the SD card, then import it on the right.");
    }
    set_text(lbl_line2, buf);
    const char *msg = in.state == DEVOS_WG_ERROR || (devos_wg_active() && in.error[0]) ? in.error : s_note;
    set_text(lbl_msg, msg);
    lv_obj_set_style_text_color(lbl_msg, in.state == DEVOS_WG_ERROR ? p->accent_danger
                                         : msg == in.error ? p->accent_warning
                                         : s_note_err ? p->accent_danger : p->accent_secondary, 0);
    if (in.public_key[0] && devos_wg_active()) snprintf(buf, sizeof(buf), "Our public key: %s", in.public_key);
    else buf[0] = '\0';
    set_text(lbl_pub, buf);
    bool running_sel = devos_wg_active() && in.active == s_sel_tun;
    set_text(lbl_conn, running_sel ? LV_SYMBOL_STOP "  Disconnect" : LV_SYMBOL_PLAY "  Connect");
    if (n) lv_obj_remove_state(btn_conn, LV_STATE_DISABLED);
    else lv_obj_add_state(btn_conn, LV_STATE_DISABLED);

    /* tunnels */
    snprintf(buf, sizeof(buf), "TUNNELS (%d/%d)", n, DEVOS_WG_MAX_TUNNELS);
    set_text(lbl_tun_hdr, buf);
    if (n) lv_obj_add_flag(lbl_tun_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(lbl_tun_empty, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < DEVOS_WG_MAX_TUNNELS; i++) {
        if (i >= n) {
            lv_obj_add_flag(tun_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(tun_rows[i], LV_OBJ_FLAG_HIDDEN);
        bool on = devos_wg_active() && in.active == i;
        snprintf(buf, sizeof(buf), "%s%s", on ? (in.state == DEVOS_WG_UP ? LV_SYMBOL_OK "  " : LV_SYMBOL_REFRESH "  ") : "",
                 devos_wg_name(i));
        set_text(tun_name[i], buf);
        lv_obj_set_style_text_color(tun_name[i], on ? p->accent_secondary : p->text_primary, 0);
        if (force || s_last_gen != devos_wg_generation()) {
            devos_wg_config_t c;
            if (devos_wg_get_config(i, &c) == 0) {
                char a[24];
                fmt_cidr(&c.address, a, sizeof(a));
                snprintf(buf, sizeof(buf), "%s   %s:%d%s", a, c.peers[0].endpoint_host, c.peers[0].endpoint_port,
                         c.full_tunnel ? "   full tunnel" : "");
                memset(&c, 0, sizeof(c));
                set_text(tun_sub[i], buf);
            }
        }
        bool sel = i == s_sel_tun;
        lv_obj_set_style_border_color(tun_rows[i], sel ? (s_focus_files ? p->text_secondary : p->accent_primary)
                                                       : p->surface_border, 0);
        lv_obj_set_style_border_width(tun_rows[i], sel ? 2 : 1, 0);
    }

    /* details */
    if (force || s_last_gen != devos_wg_generation()) {
        static char det[1200];
        if (s_sel_tun >= 0 && s_sel_tun < n) {
            snprintf(buf, sizeof(buf), "DETAILS  -  %s", devos_wg_name(s_sel_tun));
            describe(s_sel_tun, det, sizeof(det));
        } else {
            snprintf(buf, sizeof(buf), "DETAILS");
            snprintf(det, sizeof(det), "Tunnels you import show up here.\n\nThe config is stored on the Tab5 (with the "
                                       "private key), so the .conf file isn't needed afterwards.");
        }
        set_text(lbl_det_hdr, buf);
        set_text(lbl_det, det);
    }
    s_last_gen = devos_wg_generation();

    /* SD files */
    snprintf(buf, sizeof(buf), "IMPORT FROM SD CARD (%d)", s_file_n);
    set_text(lbl_imp_hdr, buf);
    for (int i = 0; i < MAX_FILES; i++) {
        if (i >= s_file_n) {
            lv_obj_add_flag(file_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(file_rows[i], LV_OBJ_FLAG_HIDDEN);
        snprintf(buf, sizeof(buf), LV_SYMBOL_FILE "  %s", s_files[i].path + strlen(TAB5_SD_MOUNT_POINT));
        set_text(file_lbl[i], buf);
        bool sel = s_focus_files && i == s_sel_file;
        lv_obj_set_style_border_color(file_rows[i], sel ? p->accent_primary : p->surface_border, 0);
        lv_obj_set_style_border_width(file_rows[i], sel ? 2 : 1, 0);
    }
    set_text(lbl_imp_hint, s_file_n ? "Tab to the files, then Enter (or tap one) to import it."
                                    : "No .conf files found. Copy wg-quick configs to /wireguard on the SD card.");

    /* what the keys do right now */
    const char *keys;
    if (s_modal == M_DELETE_FILE) keys = "Enter or Y  delete the file        Esc or N  keep it";
    else if (s_modal == M_DELETE_TUNNEL) keys = "Enter or Y  delete the tunnel        Esc or N  cancel";
    else if (s_focus_files) keys = "Up / Down  pick a file    Enter  import    Tab  tunnels    Q  scan QR    R  rescan    Esc  home";
    else keys = "Up / Down  pick a tunnel    Enter  connect / disconnect    D  delete    Tab  SD files    "
                "Q  scan QR    R  rescan    Esc  home";
    set_text(lbl_keys, keys);
}

static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!screen || lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN)) return;
    refresh(false);
}

static void style_card(lv_obj_t *c, lv_obj_t *hdr, const devos_palette_t *p)
{
    lv_obj_set_style_bg_color(c, p->surface, 0);
    lv_obj_set_style_border_color(c, p->surface_border, 0);
    if (hdr) lv_obj_set_style_text_color(hdr, p->accent_primary, 0);
}

static void style_btn(lv_obj_t *b, lv_obj_t *l, const devos_palette_t *p, bool primary)
{
    lv_obj_set_style_bg_color(b, primary ? p->accent_primary : p->surface_active, 0);
    lv_obj_set_style_border_color(b, primary ? p->accent_primary : p->surface_border, 0);
    lv_obj_set_style_text_color(l, primary ? p->bg : p->text_primary, 0);
}

static void apply_theme(const devos_palette_t *p, void *ud)
{
    LV_UNUSED(ud);
    lv_obj_set_style_bg_color(screen, p->bg, 0);
    style_card(card_top, NULL, p);
    style_card(card_tun, lbl_tun_hdr, p);
    style_card(card_det, lbl_det_hdr, p);
    style_card(card_imp, lbl_imp_hdr, p);
    lv_obj_set_style_text_color(lbl_title, p->text_primary, 0);
    lv_obj_set_style_text_color(lbl_line2, p->text_secondary, 0);
    lv_obj_set_style_text_color(lbl_pub, p->text_secondary, 0);
    lv_obj_set_style_text_color(lbl_det, p->text_primary, 0);
    lv_obj_set_style_text_color(lbl_imp_hint, p->text_secondary, 0);
    lv_obj_set_style_text_color(lbl_tun_empty, p->text_secondary, 0);
    lv_obj_set_style_text_color(lbl_keys, p->text_secondary, 0);
    style_btn(btn_conn, lbl_conn, p, true);
    style_btn(btn_del, lbl_del, p, false);
    style_btn(btn_rescan, lbl_rescan, p, false);
    style_btn(btn_scan, lbl_scan, p, false);
    style_btn(btn_name_save, lbl_name_save, p, true);
    style_btn(btn_name_cancel, lbl_name_cancel, p, false);
    lv_obj_set_style_bg_color(scan_panel, p->surface, 0);
    lv_obj_set_style_border_color(scan_panel, p->accent_primary, 0);
    lv_obj_set_style_text_color(lbl_scan_title, p->accent_primary, 0);
    lv_obj_set_style_text_color(lbl_scan_keys, p->text_secondary, 0);
    lv_obj_set_style_text_color(lbl_name_err, p->accent_danger, 0);
    lv_obj_set_style_bg_color(ta_name, p->code_bg, 0);
    lv_obj_set_style_text_color(ta_name, p->text_primary, 0);
    lv_obj_set_style_border_color(ta_name, p->surface_border, 0);
    style_btn(btn_m_ok, lbl_m_ok, p, false);
    style_btn(btn_m_cancel, lbl_m_cancel, p, false);
    lv_obj_set_style_text_color(lbl_m_ok, p->accent_danger, 0);
    for (int i = 0; i < DEVOS_WG_MAX_TUNNELS; i++) {
        lv_obj_set_style_bg_color(tun_rows[i], p->surface_active, 0);
        lv_obj_set_style_text_color(tun_sub[i], p->text_secondary, 0);
    }
    for (int i = 0; i < MAX_FILES; i++) {
        lv_obj_set_style_bg_color(file_rows[i], p->surface_active, 0);
        lv_obj_set_style_text_color(file_lbl[i], p->text_primary, 0);
    }
    lv_obj_set_style_bg_color(modal, p->surface, 0);
    lv_obj_set_style_border_color(modal, p->accent_primary, 0);
    lv_obj_set_style_text_color(lbl_modal, p->text_primary, 0);
    refresh(true);
}

/* ------------------------------------------------------------------ QR scan */
static void scan_status(const char *msg, bool err)
{
    const devos_palette_t *p = devos_theme_get();
    set_text(lbl_scan_status, msg);
    lv_obj_set_style_text_color(lbl_scan_status, err ? p->accent_danger : p->text_secondary, 0);
}

static void scan_open(void)
{
    s_scan = SCAN_RUNNING;
    s_prev_gen = 0;
    s_scan_retry_at = 0;
    memset(s_prev_buf, 0x20, sizeof(s_prev_buf));
    lv_image_cache_drop(&s_prev_dsc);
    lv_obj_invalidate(scan_img);
    lv_obj_add_flag(name_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(scan_img, LV_OBJ_FLAG_HIDDEN);
    set_text(lbl_scan_keys, "Esc  cancel");
    lv_obj_remove_flag(scan_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(scan_overlay);
    if (devos_qr_start() == 0) scan_status("Hold the WireGuard QR code 20 to 40 cm from the camera, steady. It doesn't need to fill the frame: sharp matters more than big.", false);
    else scan_status(devos_qr_error(), true);
}

static void scan_close(void)
{
    devos_qr_stop();
    s_scan = SCAN_OFF;
    memset(s_scanned, 0, sizeof(s_scanned));
    devos_focus_clear(&s_name_f);
    lv_obj_add_flag(scan_overlay, LV_OBJ_FLAG_HIDDEN);
    refresh(true);
}

/* A name for a scanned tunnel: the endpoint's first label, made unique. */
static void default_name(const devos_wg_config_t *c, char *out, size_t cap)
{
    char base[DEVOS_WG_NAME_MAX] = "tunnel";
    const char *h = c->peer_n ? c->peers[0].endpoint_host : "";
    size_t l = strcspn(h, ".");
    if (l && !isdigit((unsigned char)h[0])) {
        if (l > sizeof(base) - 4) l = sizeof(base) - 4;
        memcpy(base, h, l);
        base[l] = '\0';
    }
    snprintf(out, cap, "%s", base);
    for (int k = 2; k < 100; k++) {
        bool taken = false;
        for (int i = 0; i < devos_wg_count(); i++) if (!strcmp(devos_wg_name(i), out)) taken = true;
        if (!taken) return;
        snprintf(out, cap, "%.26s-%d", base, k);
    }
}

static void scan_found(void)
{
    devos_wg_config_t c;
    char err[160];
    if (devos_wg_parse(s_scanned, &c, err, sizeof(err)) != 0) {
        char msg[220];
        snprintf(msg, sizeof(msg), "That QR code isn't a WireGuard config (%s). Still looking...", err);
        scan_status(msg, true);
        memset(s_scanned, 0, sizeof(s_scanned));
        s_scan_retry_at = lv_tick_get() + 1500;         /* keep scanning after a pause */
        return;
    }
    char name[DEVOS_WG_NAME_MAX];
    default_name(&c, name, sizeof(name));
    memset(&c, 0, sizeof(c));
    s_scan = SCAN_NAMING;
    lv_obj_add_flag(scan_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(name_box, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(ta_name, name);
    set_text(lbl_name_err, "");
    set_text(lbl_scan_keys, "Type a name    Enter  save    Tab  buttons    Esc  cancel");
    scan_status("Got it - a valid WireGuard config. Give the tunnel a name:", false);
    devos_focus_set(&s_name_f, ta_name);
}

static void name_save(void)
{
    char err[160] = "";
    const char *name = lv_textarea_get_text(ta_name);
    int idx = devos_wg_add(name, s_scanned, err, sizeof(err));
    if (idx < 0) {
        set_text(lbl_name_err, err);
        return;
    }
    s_sel_tun = idx;
    s_focus_files = false;
    snprintf(s_note, sizeof(s_note), "Imported \"%s\" from a QR code", name);
    s_note_err = false;
    s_last_gen = 0xFFFFFFFFu;
    scan_close();
}

static void name_save_cb(lv_event_t *e) { LV_UNUSED(e); name_save(); }
static void name_cancel_cb(lv_event_t *e) { LV_UNUSED(e); scan_close(); }
static void scan_btn_cb(lv_event_t *e) { LV_UNUSED(e); scan_open(); }
static void scan_overlay_cb(lv_event_t *e) { LV_UNUSED(e); }

static void scan_tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (s_scan == SCAN_OFF) return;
    if (s_scan == SCAN_RUNNING) {
        uint32_t g = devos_qr_preview(s_prev_buf);
        if (g && g != s_prev_gen) {
            s_prev_gen = g;
            lv_image_cache_drop(&s_prev_dsc);
            lv_obj_invalidate(scan_img);
        }
        devos_qr_state_t st = devos_qr_state();
        if (st == DEVOS_QR_FOUND && devos_qr_take_result(s_scanned, sizeof(s_scanned))) scan_found();
        else if (st == DEVOS_QR_ERROR) scan_status(devos_qr_error(), true);
        if (s_scan_retry_at && (int32_t)(lv_tick_get() - s_scan_retry_at) >= 0) {
            s_scan_retry_at = 0;
            if (devos_qr_start() != 0) scan_status(devos_qr_error(), true);
        }
    }
}

static bool scan_key(uint32_t key, uint8_t mods)
{
    if (s_scan == SCAN_RUNNING) {
        if (key == LV_KEY_ESC) scan_close();
        return true;
    }
    /* naming */
    if (key == LV_KEY_ESC) {
        scan_close();
        return true;
    }
    if (devos_focus_key(&s_name_f, key, mods)) return true;
    if (key == '\r' || key == '\n') name_save();         /* Enter in the name field */
    return true;
}

/* ------------------------------------------------------------------ keys */
static bool wg_handle_key(uint32_t key, uint8_t mods)
{
    if (s_scan != SCAN_OFF) return scan_key(key, mods);
    if (s_modal != M_NONE) {
        if (key == LV_KEY_ESC || key == 'n' || key == 'N') { modal_close(); refresh(true); }
        else if (key == '\r' || key == '\n' || key == 'y' || key == 'Y') modal_ok();
        return true;
    }
    if (mods & (DEVOS_MOD_CTRL | DEVOS_MOD_FN)) return false;
    int n = devos_wg_count();
    switch (key) {
    case LV_KEY_ESC: return false;
    case '\t':
        s_focus_files = !s_focus_files && s_file_n > 0;
        if (s_focus_files && s_sel_file < 0) s_sel_file = 0;
        break;
    case LV_KEY_UP:
        if (s_focus_files) { if (s_sel_file > 0) s_sel_file--; }
        else if (s_sel_tun > 0) s_sel_tun--;
        s_last_gen = 0xFFFFFFFFu;
        break;
    case LV_KEY_DOWN:
        if (s_focus_files) { if (s_sel_file + 1 < s_file_n) s_sel_file++; }
        else if (s_sel_tun + 1 < n) s_sel_tun++;
        s_last_gen = 0xFFFFFFFFu;
        break;
    case '\r': case '\n':
        if (s_focus_files) import_file(s_sel_file);
        else toggle_conn();
        return true;
    case 'd': case 'D': case LV_KEY_DEL:
        if (!s_focus_files) ask_delete_tunnel();
        break;
    case 'r': case 'R':
        scan_files();
        break;
    case 'q': case 'Q':
        scan_open();
        return true;
    default:
        return key >= 32 && key <= 126;
    }
    refresh(true);
    return true;
}

/* ------------------------------------------------------------------ init */
static void wg_init(void)
{
    const devos_palette_t *p = devos_theme_get();
    devos_wg_init();

    screen = lv_obj_create(lv_screen_active());
    app_descriptor.screen = screen;
    lv_obj_set_size(screen, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(screen, 0, DEVOS_TOP_BAR_HEIGHT);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);

    /* status */
    card_top = mk_card(16, 12, DEVOS_SCREEN_WIDTH - 32, 150, NULL, NULL);
    lbl_title = mk_label(card_top, &lv_font_montserrat_14, LV_SYMBOL_EYE_CLOSE "  WireGuard");
    lbl_state = mk_label(card_top, &lv_font_montserrat_20, "");
    lv_obj_set_pos(lbl_state, 0, 24);
    lbl_line2 = mk_label(card_top, &lv_font_montserrat_14, "");
    lv_obj_set_pos(lbl_line2, 0, 58);
    lv_obj_set_width(lbl_line2, DEVOS_SCREEN_WIDTH - 340);
    lv_label_set_long_mode(lbl_line2, LV_LABEL_LONG_DOT);
    lbl_msg = mk_label(card_top, &lv_font_montserrat_14, "");
    lv_obj_set_pos(lbl_msg, 0, 82);
    lv_obj_set_width(lbl_msg, DEVOS_SCREEN_WIDTH - 340);
    lv_label_set_long_mode(lbl_msg, LV_LABEL_LONG_DOT);
    lbl_pub = mk_label(card_top, &lv_font_nimbus_mono_14, "");
    lv_obj_set_pos(lbl_pub, 0, 106);
    btn_conn = mk_btn(card_top, LV_SYMBOL_PLAY "  Connect", 150, conn_cb, &lbl_conn);
    lv_obj_align(btn_conn, LV_ALIGN_TOP_RIGHT, 0, 20);
    btn_del = mk_btn(card_top, LV_SYMBOL_TRASH "  Delete", 150, del_cb, &lbl_del);
    lv_obj_align(btn_del, LV_ALIGN_TOP_RIGHT, 0, 66);

    /* tunnels */
    card_tun = mk_card(16, 174, 560, DEVOS_CONTENT_HEIGHT - 186 - KEYS_H, "TUNNELS", &lbl_tun_hdr);
    for (int i = 0; i < DEVOS_WG_MAX_TUNNELS; i++) {
        lv_obj_t *r = lv_obj_create(card_tun);
        lv_obj_set_size(r, lv_pct(100), ROW_H);
        lv_obj_set_pos(r, 0, 26 + i * (ROW_H + 8));
        lv_obj_set_style_radius(r, 6, 0);
        lv_obj_set_style_pad_all(r, 8, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, tun_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        tun_name[i] = mk_label(r, &lv_font_montserrat_14, "");
        tun_sub[i] = mk_label(r, &lv_font_montserrat_12, "");
        lv_obj_set_pos(tun_sub[i], 0, 18);
        lv_obj_set_width(tun_sub[i], 500);
        lv_label_set_long_mode(tun_sub[i], LV_LABEL_LONG_DOT);
        tun_rows[i] = r;
    }
    lbl_tun_empty = mk_label(card_tun, &lv_font_montserrat_14,
                             "No tunnels yet.\n\nImport a wg-quick .conf from the SD card (right). "
                             "Up to 4 tunnels are kept on the Tab5.");
    lv_obj_set_pos(lbl_tun_empty, 0, 34);
    lv_obj_set_width(lbl_tun_empty, 520);
    lv_label_set_long_mode(lbl_tun_empty, LV_LABEL_LONG_WRAP);

    /* details */
    card_det = mk_card(592, 174, DEVOS_SCREEN_WIDTH - 608, 262, "DETAILS", &lbl_det_hdr);
    lbl_det = mk_label(card_det, &lv_font_nimbus_mono_14, "");
    lv_obj_set_pos(lbl_det, 0, 24);
    lv_obj_set_width(lbl_det, DEVOS_SCREEN_WIDTH - 640);
    lv_label_set_long_mode(lbl_det, LV_LABEL_LONG_WRAP);

    /* SD import */
    card_imp = mk_card(592, 448, DEVOS_SCREEN_WIDTH - 608, DEVOS_CONTENT_HEIGHT - 460 - KEYS_H, "IMPORT FROM SD CARD", &lbl_imp_hdr);
    lv_obj_add_flag(card_imp, LV_OBJ_FLAG_SCROLLABLE);
    btn_rescan = mk_btn(card_imp, LV_SYMBOL_REFRESH "  Rescan", 110, rescan_cb, &lbl_rescan);
    lv_obj_set_height(btn_rescan, 28);
    lv_obj_align(btn_rescan, LV_ALIGN_TOP_RIGHT, 0, -6);
    btn_scan = mk_btn(card_imp, LV_SYMBOL_IMAGE "  Scan QR", 120, scan_btn_cb, &lbl_scan);
    lv_obj_set_height(btn_scan, 28);
    lv_obj_align_to(btn_scan, btn_rescan, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    lbl_imp_hint = mk_label(card_imp, &lv_font_montserrat_12, "");
    lv_obj_set_pos(lbl_imp_hint, 0, 22);
    for (int i = 0; i < MAX_FILES; i++) {
        lv_obj_t *r = lv_obj_create(card_imp);
        lv_obj_set_size(r, lv_pct(100), 34);
        lv_obj_set_pos(r, 0, 44 + i * 40);
        lv_obj_set_style_radius(r, 6, 0);
        lv_obj_set_style_pad_hor(r, 10, 0);
        lv_obj_set_style_pad_ver(r, 0, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, file_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        file_lbl[i] = mk_label(r, &lv_font_montserrat_14, "");
        lv_obj_align(file_lbl[i], LV_ALIGN_LEFT_MID, 0, 0);
        file_rows[i] = r;
    }

    lbl_keys = mk_label(screen, &lv_font_montserrat_12, "");
    lv_obj_set_pos(lbl_keys, 20, DEVOS_CONTENT_HEIGHT - KEYS_H + 2);

    /* confirm dialog */
    overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    modal = lv_obj_create(overlay);
    lv_obj_set_size(modal, 600, 210);
    lv_obj_center(modal);
    lv_obj_set_style_border_width(modal, 2, 0);
    lv_obj_set_style_radius(modal, 8, 0);
    lv_obj_set_style_pad_all(modal, 20, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lbl_modal = mk_label(modal, &lv_font_montserrat_14, "");
    lv_obj_set_width(lbl_modal, 556);
    lv_label_set_long_mode(lbl_modal, LV_LABEL_LONG_WRAP);
    btn_m_ok = mk_btn(modal, "OK", 150, m_ok_cb, &lbl_m_ok);
    lv_obj_align(btn_m_ok, LV_ALIGN_BOTTOM_RIGHT, -166, 0);
    btn_m_cancel = mk_btn(modal, "Cancel", 150, m_cancel_cb, &lbl_m_cancel);
    lv_obj_align(btn_m_cancel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_t *mk = mk_label(modal, &lv_font_montserrat_12, "Enter or Y: yes\nEsc or N: no");
    lv_obj_align(mk, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_text_color(mk, p->text_secondary, 0);

    /* QR scan screen */
    scan_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(scan_overlay);
    lv_obj_set_size(scan_overlay, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_style_bg_color(scan_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scan_overlay, LV_OPA_70, 0);
    lv_obj_add_flag(scan_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scan_overlay, scan_overlay_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(scan_overlay, LV_OBJ_FLAG_HIDDEN);
    scan_panel = lv_obj_create(scan_overlay);
    lv_obj_set_size(scan_panel, 640, 560);
    lv_obj_center(scan_panel);
    lv_obj_set_style_radius(scan_panel, 8, 0);
    lv_obj_set_style_border_width(scan_panel, 2, 0);
    lv_obj_set_style_pad_all(scan_panel, 16, 0);
    lv_obj_remove_flag(scan_panel, LV_OBJ_FLAG_SCROLLABLE);
    lbl_scan_title = mk_label(scan_panel, &lv_font_montserrat_16, LV_SYMBOL_IMAGE "  Scan a WireGuard QR code");
    s_prev_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_prev_dsc.header.cf = LV_COLOR_FORMAT_L8;
    s_prev_dsc.header.w = DEVOS_QR_PREVIEW;
    s_prev_dsc.header.h = DEVOS_QR_PREVIEW;
    s_prev_dsc.header.stride = DEVOS_QR_PREVIEW;
    s_prev_dsc.data_size = sizeof(s_prev_buf);
    s_prev_dsc.data = s_prev_buf;
    scan_img = lv_image_create(scan_panel);
    lv_image_set_src(scan_img, &s_prev_dsc);
    lv_obj_align(scan_img, LV_ALIGN_TOP_MID, 0, 34);
    lbl_scan_status = mk_label(scan_panel, &lv_font_montserrat_14, "");
    lv_obj_set_width(lbl_scan_status, 600);
    lv_label_set_long_mode(lbl_scan_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl_scan_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_scan_status, LV_ALIGN_TOP_MID, 0, 404);
    lbl_scan_keys = mk_label(scan_panel, &lv_font_montserrat_12, "Esc  cancel");
    lv_obj_align(lbl_scan_keys, LV_ALIGN_BOTTOM_MID, 0, 0);
    name_box = lv_obj_create(scan_panel);
    lv_obj_remove_style_all(name_box);
    lv_obj_set_size(name_box, 600, 330);
    lv_obj_align(name_box, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_add_flag(name_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *nl = mk_label(name_box, &lv_font_montserrat_14, "Tunnel name");
    lv_obj_set_pos(nl, 0, 90);
    ta_name = lv_textarea_create(name_box);
    lv_textarea_set_one_line(ta_name, true);
    lv_textarea_set_max_length(ta_name, DEVOS_WG_NAME_MAX - 1);
    lv_obj_set_size(ta_name, 600, 40);
    lv_obj_set_pos(ta_name, 0, 114);
    lv_obj_set_style_radius(ta_name, 6, 0);
    lv_obj_set_style_pad_ver(ta_name, 9, 0);
    lv_obj_set_style_pad_hor(ta_name, 12, 0);
    lv_obj_set_style_text_font(ta_name, &lv_font_montserrat_16, 0);
    lv_obj_set_scrollbar_mode(ta_name, LV_SCROLLBAR_MODE_OFF);
    lbl_name_err = mk_label(name_box, &lv_font_montserrat_12, "");
    lv_obj_set_pos(lbl_name_err, 0, 162);
    btn_name_save = mk_btn(name_box, LV_SYMBOL_SAVE "  Save tunnel", 170, name_save_cb, &lbl_name_save);
    lv_obj_set_pos(btn_name_save, 250, 200);
    btn_name_cancel = mk_btn(name_box, "Cancel", 150, name_cancel_cb, &lbl_name_cancel);
    lv_obj_set_pos(btn_name_cancel, 440, 200);
    devos_focus_init(&s_name_f);
    devos_focus_add(&s_name_f, ta_name);
    devos_focus_add(&s_name_f, btn_name_save);
    devos_focus_add(&s_name_f, btn_name_cancel);

    scan_files();
    apply_theme(p, NULL);
    devos_theme_add_listener(apply_theme, NULL);
    lv_timer_create(tick_cb, 500, NULL);
    lv_timer_create(scan_tick_cb, 100, NULL);
}

static void wg_hide(void)
{
    if (s_scan != SCAN_OFF) scan_close();       /* camera off when leaving */
}

static void wg_show(void)
{
    scan_files();
    refresh(true);
}

static int wg_telemetry_lines(char lines[3][64])
{
    devos_wg_info_t in;
    devos_wg_get_info(&in);
    int n = devos_wg_count();
    if (in.state == DEVOS_WG_UP) snprintf(lines[0], sizeof(lines[0]), "* Up: %.50s", in.name);
    else if (in.state == DEVOS_WG_CONNECTING) snprintf(lines[0], sizeof(lines[0]), "* Connecting: %.40s", in.name);
    else snprintf(lines[0], sizeof(lines[0]), "* Off");
    if (devos_wg_active()) snprintf(lines[1], sizeof(lines[1]), "* %s", in.address);
    else snprintf(lines[1], sizeof(lines[1]), "* %d tunnel%s saved", n, n == 1 ? "" : "s");
    snprintf(lines[2], sizeof(lines[2]), "* wg-quick configs from SD");
    return 3;
}

devos_app_descriptor_t *app_wireguard_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_WIREGUARD;
    app_descriptor.uid = "wireguard";
    app_descriptor.icon = LV_SYMBOL_EYE_CLOSE;
    app_descriptor.category = "network";
    app_descriptor.name = "WireGuard";
    app_descriptor.title = "WireGuard";
    app_descriptor.subtitle = "VPN tunnel from a wg-quick config";
    app_descriptor.screen = screen;
    app_descriptor.init = wg_init;
    app_descriptor.show = wg_show;
    app_descriptor.hide = wg_hide;
    app_descriptor.handle_key = wg_handle_key;
    app_descriptor.get_telemetry_lines = wg_telemetry_lines;
    return &app_descriptor;
}
