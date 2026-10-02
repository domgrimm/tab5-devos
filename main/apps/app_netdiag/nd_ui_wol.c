/* Network > Wake-on-LAN: send a magic packet to a sleeping machine.
 *
 * A form (MAC address, optional "send to" address) with a Wake button, and a
 * list of the machines you have woken before - pick one and Enter re-wakes it
 * (or W wakes the one on screen). The list is saved to /.devos/wol.json.
 *
 * A machine on this Wi-Fi network needs no address: the packet is broadcast.
 * For another network (in a VPN, or a different subnet) type its address
 * (or a directed broadcast like 192.168.1.255). */
#include "app_netdiag_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#define WOL_MAX 24
#define WOL_JSON TAB5_SD_MOUNT_POINT "/.devos/wol.json"

typedef struct {
    char mac[18];               /* "aa:bb:cc:dd:ee:ff" */
    char addr[48];              /* last "send to" ("" = broadcast) */
    char label[32];             /* optional name */
} wol_entry_t;

static lv_obj_t *ta_mac, *ta_addr, *btn_wake, *lbl_wake, *lbl_status, *lbl_result, *box_list;
static devos_vlist_t s_list;
static devos_focus_t s_f;
static wol_entry_t *s_rows;                 /* WOL_MAX, PSRAM */
static int s_nrows;
static char s_result[160];
static bool s_loaded;

/* ---- storage ---- */
static void load(void)
{
    if (s_loaded) return;
    s_loaded = true;
    if (!s_rows) {
#ifdef ESP_PLATFORM
        s_rows = heap_caps_calloc(WOL_MAX, sizeof(*s_rows), MALLOC_CAP_SPIRAM);
        if (!s_rows) s_rows = calloc(WOL_MAX, sizeof(*s_rows));
#else
        s_rows = calloc(WOL_MAX, sizeof(*s_rows));
#endif
    }
    if (!s_rows) return;
    FILE *f = fopen(WOL_JSON, "r");
    if (!f) return;
    char buf[512];
    while (fgets(buf, sizeof(buf), f) && s_nrows < WOL_MAX) {
        char mac[40] = "", addr[64] = "", label[64] = "";
        /* one line per entry: {"mac":"..","addr":"..","label":".."} */
        const char *p;
        if ((p = strstr(buf, "\"mac\""))) sscanf(strchr(p, ':') + 1, " \"%39[^\"]\"", mac);
        if ((p = strstr(buf, "\"addr\""))) sscanf(strchr(p, ':') + 1, " \"%63[^\"]\"", addr);
        if ((p = strstr(buf, "\"label\""))) sscanf(strchr(p, ':') + 1, " \"%63[^\"]\"", label);
        if (mac[0]) {
            snprintf(s_rows[s_nrows].mac, sizeof(s_rows[s_nrows].mac), "%s", mac);
            snprintf(s_rows[s_nrows].addr, sizeof(s_rows[s_nrows].addr), "%s", addr);
            snprintf(s_rows[s_nrows].label, sizeof(s_rows[s_nrows].label), "%s", label);
            s_nrows++;
        }
    }
    fclose(f);
}

static void save(void)
{
    if (!s_rows) return;
    FILE *f = fopen(WOL_JSON, "w");
    if (!f) return;
    fprintf(f, "[\n");
    for (int i = 0; i < s_nrows; i++)
        fprintf(f, "  {\"mac\": \"%s\", \"addr\": \"%s\", \"label\": \"%s\"}%s\n", s_rows[i].mac, s_rows[i].addr,
                s_rows[i].label, i + 1 < s_nrows ? "," : "");
    fprintf(f, "]\n");
    fclose(f);
}

static void remember(const char *mac, const char *addr)
{
    if (!s_rows) return;
    for (int i = 0; i < s_nrows; i++) {
        if (!strcmp(s_rows[i].mac, mac)) {                 /* move to the top */
            snprintf(s_rows[i].addr, sizeof(s_rows[i].addr), "%s", addr);
            wol_entry_t e = s_rows[i];
            memmove(&s_rows[1], &s_rows[0], (size_t)i * sizeof(wol_entry_t));
            s_rows[0] = e;
            save();
            return;
        }
    }
    int n = s_nrows < WOL_MAX ? s_nrows : WOL_MAX - 1;
    memmove(&s_rows[1], &s_rows[0], (size_t)n * sizeof(wol_entry_t));
    snprintf(s_rows[0].mac, sizeof(s_rows[0].mac), "%s", mac);
    snprintf(s_rows[0].addr, sizeof(s_rows[0].addr), "%s", addr);
    s_rows[0].label[0] = '\0';
    s_nrows = n + 1;
    save();
}

/* ---- waking ---- */
static void wake(const char *mac_text, const char *addr)
{
    uint8_t mac[6];
    char err[64];
    if (devos_wol_parse_mac(mac_text, mac, err, sizeof(err)) != 0) {
        snprintf(s_result, sizeof(s_result), "%s", err);
        nd_flash(err);
        return;
    }
    if (devos_wol_send(mac, addr) != 0) {
        snprintf(s_result, sizeof(s_result), "Failed: %s", devos_wol_error());
        nd_flash(devos_wol_error());
        return;
    }
    char norm[18];
    devos_wol_format_mac(mac, norm, sizeof(norm));
    snprintf(s_result, sizeof(s_result), "Magic packet sent to %s via %s", norm, devos_wol_last_target());
    remember(norm, addr ? addr : "");
    nd_flash("Magic packet sent");
}

static void wake_cb(lv_event_t *e) { LV_UNUSED(e); wake(lv_textarea_get_text(ta_mac), lv_textarea_get_text(ta_addr)); }

static void wake_selected(int idx)
{
    if (idx < 0 || idx >= s_nrows) return;
    lv_textarea_set_text(ta_mac, s_rows[idx].mac);
    lv_textarea_set_text(ta_addr, s_rows[idx].addr);
    wake(s_rows[idx].mac, s_rows[idx].addr);
}

/* ---- list ---- */
static void row_draw(devos_vlist_t *v, lv_layer_t *layer, const lv_area_t *row, int idx)
{
    LV_UNUSED(v);
    if (idx >= s_nrows) return;
    const devos_palette_t *p = devos_theme_get();
    const wol_entry_t *r = &s_rows[idx];
    devos_w_draw_text(layer, NULL, row->x1 + 10, row->y1 + 3, 200, r->label[0] ? r->label : r->mac, p->text_primary);
    devos_w_draw_text(layer, NULL, row->x1 + 216, row->y1 + 3, 200, r->label[0] ? r->mac : "", p->text_secondary);
    devos_w_draw_text(layer, NULL, row->x1 + 430, row->y1 + 3, 300, r->addr[0] ? r->addr : "broadcast", p->accent_secondary);
}

static void on_activate(devos_vlist_t *v, int idx)
{
    LV_UNUSED(v);
    wake_selected(idx);
}

static void refresh(void)
{
    devos_vlist_set_count(&s_list, s_nrows);
    if (!s_nrows) devos_w_set_text(lbl_status, "No machines yet - a MAC you wake is remembered here.");
    else devos_w_set_text(lbl_status, "Up / Down pick a machine, Enter wakes it again.");
    devos_w_set_text(lbl_result, s_result);
}

/* ---- view ---- */
static void create(lv_obj_t *parent)
{
    load();
    ta_mac = devos_w_field(parent, "MAC address", 16, 6, 300);
    lv_textarea_set_placeholder_text(ta_mac, "aa:bb:cc:dd:ee:ff");
    ta_addr = devos_w_field(parent, "Send to (empty = broadcast)", 330, 6, 300);
    lv_textarea_set_placeholder_text(ta_addr, "192.168.1.255, nas.lan, 100.x.y.z");
    btn_wake = devos_w_btn_kind(parent, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_PLAY "  Wake  [W]", 120, wake_cb, NULL, &lbl_wake);
    lv_obj_set_size(btn_wake, 120, 36);
    lv_obj_set_pos(btn_wake, 650, 24);

    lbl_status = devos_w_label(parent, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM, "");
    lv_obj_set_pos(lbl_status, 16, 78);
    lbl_result = devos_w_label(parent, &lv_font_montserrat_14, DEVOS_W_TEXT_ACCENT, "");
    lv_obj_set_pos(lbl_result, 16, 100);

    box_list = devos_w_panel(parent, 16, 126, 1248, ND_VIEW_H - 132, DEVOS_W_CODE);
    lv_obj_set_style_radius(box_list, 6, 0);
    devos_vlist_create(&s_list, box_list, ND_ROW_H + 4, row_draw);
    s_list.on_activate = on_activate;
    s_list.active = true;
    lv_obj_set_pos(s_list.scroll, 2, 2);
    lv_obj_set_size(s_list.scroll, 1244, ND_VIEW_H - 136);

    devos_focus_init(&s_f);
    devos_focus_add(&s_f, ta_mac);
    devos_focus_add(&s_f, ta_addr);
    devos_focus_add(&s_f, btn_wake);
    devos_focus_add(&s_f, s_list.scroll);
    refresh();
}

static void show(void)
{
    if (!devos_focus_get(&s_f)) devos_focus_set(&s_f, ta_mac);
    refresh();
}

static void hide(void) {}

static bool key(uint32_t k, uint8_t mods)
{
    if (mods & (DEVOS_MOD_CTRL | DEVOS_MOD_ALT | DEVOS_MOD_FN)) return false;
    bool on_list = devos_focus_get(&s_f) == s_list.scroll && s_f.ring;
    if (on_list && (k == LV_KEY_UP || k == LV_KEY_DOWN || k == '\r' || k == '\n')) {
        if (k == '\r' || k == '\n') { wake_selected(s_list.sel); return true; }
        if (s_list.sel <= 0 && k == LV_KEY_UP) { devos_focus_set(&s_f, btn_wake); return true; }
        return devos_vlist_key(&s_list, k);
    }
    if (devos_focus_key(&s_f, k, mods)) return true;
    if (k == '\r' || k == '\n') { wake(lv_textarea_get_text(ta_mac), lv_textarea_get_text(ta_addr)); return true; }
    if (k == 'w' || k == 'W') { wake(lv_textarea_get_text(ta_mac), lv_textarea_get_text(ta_addr)); return true; }
    return !(mods & (DEVOS_MOD_FN | DEVOS_MOD_ALT)) && k >= 32 && k < 127;
}

static void tick(void) {}

static const char *keys(void)
{
    return "W wakes    Enter wakes    Up / Down pick a saved machine, Enter re-wakes";
}

const nd_view_t nd_view_wol = { "Wake-on-LAN", create, show, hide, key, tick, keys };
