/* Network > DNS: query any record type from the DHCP server or one you name;
 * Enter on an answer looks up what it points at (IP -> PTR, name -> A). */
#include "app_netdiag_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t *ta_name, *dd_type, *ta_server, *btn_go, *lbl_head, *lbl_empty;
static devos_vlist_t s_list;
static devos_focus_t s_f;
static uint32_t s_gen = 0xffffffff;
static devos_dns_result_t *s_res;               /* PSRAM: ~16 KB */

static const uint16_t TYPES[] = { DEVOS_DNS_A, DEVOS_DNS_AAAA, DEVOS_DNS_CNAME, DEVOS_DNS_MX, DEVOS_DNS_TXT,
                                  DEVOS_DNS_NS, DEVOS_DNS_SOA, DEVOS_DNS_SRV, DEVOS_DNS_PTR, DEVOS_DNS_ANY };

static void lookup(void)
{
    const char *name = lv_textarea_get_text(ta_name);
    while (*name == ' ') name++;
    if (!*name) {
        nd_flash("Enter a name (or an IP address for a reverse lookup)");
        devos_focus_set(&s_f, ta_name);
        return;
    }
    if (devos_dns_lookup_start(lv_textarea_get_text(ta_server), name, TYPES[lv_dropdown_get_selected(dd_type)]) != 0)
        nd_flash("A lookup is already running");
    s_gen = 0xffffffff;
}

void nd_ui_dns_lookup(const char *name)
{
    if (name && name[0]) lv_textarea_set_text(ta_name, name);
    lookup();
}

static void go_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lookup();
}

static const char *SECTIONS[] = { "answer", "authority", "additional" };

static void row_draw(devos_vlist_t *v, lv_layer_t *layer, const lv_area_t *row, int idx)
{
    LV_UNUSED(v);
    if (!s_res || idx >= s_res->n) return;
    const devos_palette_t *p = devos_theme_get();
    const devos_dns_rr_t *r = &s_res->rr[idx];
    int y = row->y1 + 3, x = row->x1 + 10;
    char ttl[16];
    devos_w_draw_text(layer, NULL, x, y, 88, SECTIONS[r->section % 3], p->text_muted);
    devos_w_draw_text(layer, NULL, x + 96, y, 330, r->name, p->text_secondary);
    snprintf(ttl, sizeof(ttl), "%u", (unsigned)r->ttl);
    devos_w_draw_text(layer, NULL, x + 436, y, 64, ttl, p->text_secondary);
    devos_w_draw_text(layer, NULL, x + 506, y, 56, devos_dns_type_name(r->type), p->accent_primary);
    devos_w_draw_text(layer, NULL, x + 568, y, row->x2 - x - 580, r->data, r->section ? p->text_secondary : p->text_primary);
}

/* Enter on an answer: follow it */
static void row_activate(devos_vlist_t *v, int idx)
{
    LV_UNUSED(v);
    if (!s_res || idx >= s_res->n) return;
    const devos_dns_rr_t *r = &s_res->rr[idx];
    char target[256] = "";
    const char *d = r->data;
    switch (r->type) {
    case DEVOS_DNS_A: snprintf(target, sizeof(target), "%s", d); break;            /* -> PTR */
    case DEVOS_DNS_CNAME: case DEVOS_DNS_NS: case DEVOS_DNS_PTR: snprintf(target, sizeof(target), "%s", d); break;
    case DEVOS_DNS_MX: { const char *sp = strchr(d, ' '); if (sp) snprintf(target, sizeof(target), "%s", sp + 1); break; }
    case DEVOS_DNS_SRV: { const char *sp = strrchr(d, ' '); if (sp) snprintf(target, sizeof(target), "%s", sp + 1); break; }
    default: break;
    }
    if (!target[0]) {
        nd_flash("Nothing to follow in that record");
        return;
    }
    size_t l = strlen(target);
    if (l && target[l - 1] == '.') target[l - 1] = '\0';
    lv_dropdown_set_selected(dd_type, 0);
    lv_textarea_set_text(ta_name, target);
    lookup();
}

static void refresh(bool force)
{
    uint32_t g = devos_dns_generation();
    if (!force && g == s_gen) return;
    s_gen = g;
    devos_dns_lookup_result(s_res);
    char buf[200];
    if (s_res->busy) snprintf(buf, sizeof(buf), "Asking %s for %s %.100s ...", s_res->server,
                              devos_dns_type_name(s_res->qtype), s_res->query);
    else if (s_res->error[0]) snprintf(buf, sizeof(buf), "%s", s_res->error);
    else if (s_res->query[0]) {
        int an = 0;
        for (int i = 0; i < s_res->n; i++) an += s_res->rr[i].section == 0;
        snprintf(buf, sizeof(buf), "%s %.80s  -  %s, %d answer%s, %d ms from %s%s%s%s", devos_dns_type_name(s_res->qtype),
                 s_res->query, devos_dns_rcode_name(s_res->rcode), an, an == 1 ? "" : "s", s_res->ms, s_res->server,
                 s_res->authoritative ? ", authoritative" : "", s_res->recursion ? "" : ", no recursion",
                 s_res->truncated ? ", truncated" : "");
    } else buf[0] = '\0';
    devos_w_set_text(lbl_head, buf);
    devos_w_track(lbl_head, s_res->error[0] || (s_res->rcode && !s_res->busy) ? DEVOS_W_TEXT_ERR : DEVOS_W_TEXT_DIM);
    devos_vlist_set_count(&s_list, s_res->busy ? 0 : s_res->n);
    if (s_list.sel >= s_list.count) s_list.sel = -1;
    if (s_res->n || s_res->busy) lv_obj_add_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    devos_w_set_text(lbl_empty, s_res->query[0] && !s_res->error[0] ? "No records in the reply."
                                                                     : "Type a name and press Enter.\n"
                                                                       "An IP address does a reverse (PTR) lookup.");
}

static void create(lv_obj_t *parent)
{
    s_res = calloc(1, sizeof(*s_res));
    ta_name = devos_w_field(parent, "Name or IP address", 16, 6, 440);
    lv_textarea_set_placeholder_text(ta_name, "example.com, _mqtt._tcp.example.com, 192.168.1.1 ...");
    lv_obj_t *l = devos_w_label(parent, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Type");
    lv_obj_set_pos(l, 472, 6);
    dd_type = devos_w_dd(parent, "A\nAAAA\nCNAME\nMX\nTXT\nNS\nSOA\nSRV\nPTR\nANY", 110);
    lv_obj_set_pos(dd_type, 472, 24);
    char def[48], ph[80];
    devos_dns_default_server(def, sizeof(def));
    ta_server = devos_w_field(parent, "Server (blank = DHCP's)", 598, 6, 200);
    snprintf(ph, sizeof(ph), "%s", def);
    lv_textarea_set_placeholder_text(ta_server, ph);
    lv_textarea_set_accepted_chars(ta_server, "0123456789.");
    btn_go = devos_w_btn_kind(parent, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_REFRESH "  Look up", 120, go_cb, NULL, NULL);
    lv_obj_set_size(btn_go, 120, 36);
    lv_obj_set_pos(btn_go, 814, 24);
    lbl_head = devos_w_label(parent, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "");
    lv_label_set_long_mode(lbl_head, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_head, 1240);
    lv_obj_set_pos(lbl_head, 20, 72);
    lv_obj_t *hdr = devos_w_label(parent, devos_w_mono(), DEVOS_W_TEXT_MUTED,
                                  "SECTION      NAME                                     TTL      TYPE   DATA");
    lv_obj_set_pos(hdr, 28, 94);
    lv_obj_t *box = devos_w_panel(parent, 16, 114, 1248, ND_VIEW_H - 120, DEVOS_W_CODE);
    lv_obj_set_style_radius(box, 6, 0);
    devos_vlist_create(&s_list, box, ND_ROW_H, row_draw);
    s_list.on_activate = row_activate;
    lv_obj_set_pos(s_list.scroll, 2, 2);
    lv_obj_set_size(s_list.scroll, 1244, ND_VIEW_H - 124);
    lbl_empty = devos_w_label(box, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM, "");
    lv_obj_set_style_text_align(lbl_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_empty, LV_ALIGN_CENTER, 0, 0);
    devos_focus_init(&s_f);
    lv_obj_t *order[] = { ta_name, dd_type, ta_server, btn_go, s_list.scroll };
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) devos_focus_add(&s_f, order[i]);
}

static void show(void)
{
    if (!devos_focus_get(&s_f)) devos_focus_set(&s_f, ta_name);
    refresh(true);
}

static void hide(void) {}

static bool key(uint32_t k, uint8_t mods)
{
    if (s_f.dd_open) return nd_form_key(&s_f, &s_list, k, mods);    /* an open list takes Esc / arrows */
    if (k == LV_KEY_ESC) return false;
    if (nd_form_key(&s_f, &s_list, k, mods)) return true;
    if (k == '\r' || k == '\n') {
        lookup();
        return true;
    }
    return !(mods & (DEVOS_MOD_FN | DEVOS_MOD_ALT)) && k >= 32 && k < 127;
}

static void tick(void) { refresh(false); }

static const char *keys(void)
{
    return s_list.active ? "Up / Down pick    Enter looks up what the record points at    Tab back to the form"
                         : "Enter looks up    Tab / arrows move    Left / Right change the type";
}

const nd_view_t nd_view_dns = { "DNS", create, show, hide, key, tick, keys };
