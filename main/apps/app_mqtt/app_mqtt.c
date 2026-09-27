/* MQTT monitor & publisher.
 *
 * Top: connection status, message rate, Connect / Broker / Publish / Pause /
 * Clear. Left: the topics seen (with counts); picking one filters the
 * stream. Middle: the live message stream (follows new messages until you
 * scroll or move up). Bottom: the selected message, JSON pretty-printed and
 * highlighted (plain text or a hex dump otherwise) - or the Publish panel
 * with templates from /mqtt/templates.json.
 *
 * Keys: Up/Down/Sym+Up/Down move through messages, F jumps to the newest,
 * Tab switches to the topic list, Space pauses, C clears, P publish panel,
 * Enter copies the selected message into it, B broker settings,
 * Alt+1..9 sends template 1..9, Sym+L hides the topic list. In the Publish
 * panel Tab moves between fields and Ctrl+Enter sends; Esc closes.
 */
#include "app_mqtt.h"
#include "devos_config.h"
#include "devos_theme.h"
#include "devos_core.h"
#include "devos_mqtt.h"
#include "devos_json.h"
#include "devos_codeview.h"
#include "devos_focus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

LV_FONT_DECLARE(lv_font_nimbus_mono_14);

#define TOP_H       40
#define SIDE_W      DEVOS_PANE_LEFT_WIDTH
#define ROW_H       22
#define STREAM_H    320
#define MONO_W      8
#define KEYS_H      22                  /* key hints footer */
#define TPL_DIR     TAB5_SD_MOUNT_POINT "/mqtt"
#define TPL_FILE    TPL_DIR "/templates.json"
#define TPL_MAX     16
#define TPL_PAYLOAD 1024

typedef struct {
    char name[48];
    char topic[DEVOS_MQTT_TOPIC_MAX];
    char payload[TPL_PAYLOAD];
    int qos;
    bool retain;
} tpl_t;

/* A list that draws only its visible rows (fast with hundreds of rows). */
typedef void (*vl_draw_fn)(lv_layer_t *layer, int x1, int y, int x2, int idx);
typedef struct {
    lv_obj_t *scroll, *view;
    int count, sel;
    vl_draw_fn draw;
    void (*on_select)(int idx);
} vlist_t;

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen, *top_bar, *lbl_title, *led, *lbl_status, *lbl_stats;
static lv_obj_t *btn_conn, *lbl_conn, *btn_broker, *lbl_broker, *btn_pub, *lbl_pub, *btn_pause, *lbl_pause,
                *btn_clear, *lbl_clear;
static lv_obj_t *side, *lbl_side_hdr, *lbl_empty, *lbl_new;
static vlist_t s_topics_vl, s_stream_vl;
static lv_obj_t *detail, *lbl_detail_hdr, *detail_scroll;
static devos_codeview_t s_cv;
static lv_obj_t *pub_panel, *dd_tpl, *ta_topic, *ta_payload, *dd_qos, *cb_retain, *btn_send, *lbl_send,
                *btn_tpl_save, *lbl_tpl_save, *btn_tpl_del, *lbl_tpl_del, *lbl_pub_hint, *lbl_pub_msg;
static lv_obj_t *overlay, *modal, *lbl_modal_title, *lbl_modal_err, *btn_modal_ok, *lbl_modal_ok,
                *btn_modal_cancel, *lbl_modal_cancel;
static lv_obj_t *ta_host, *ta_port, *ta_user, *ta_pass, *ta_client, *ta_subs;
static lv_obj_t *lbl_keys;                      /* key hints footer */
static devos_focus_t s_pub_f, s_modal_f;        /* publish panel, broker dialog */
static bool s_pub_focus;                        /* keys go to the publish panel */

static bool s_side_visible = true;
static bool s_focus_topics;
static bool s_follow = true, s_paused;
static bool s_pub_open;
static bool s_prog_scroll;
static bool s_autostarted;
static char s_filter[DEVOS_MQTT_TOPIC_MAX];      /* "" = all topics */
static EXT_RAM_BSS_ATTR uint32_t s_rows[DEVOS_MQTT_RING];
static int s_row_n;
static uint32_t s_sel_seq, s_shown_seq;
static uint32_t s_last_gen = 0xFFFFFFFFu;
static uint32_t s_unseen;
static uint32_t s_flash_until;
static EXT_RAM_BSS_ATTR tpl_t s_tpl[TPL_MAX];     /* ~20 KB: PSRAM */
static int s_tpl_n;
static EXT_RAM_BSS_ATTR char s_detail[DEVOS_MQTT_PAYLOAD_MAX + 1];
static EXT_RAM_BSS_ATTR char s_pretty[24576];

static void refresh(bool force);
static void open_publish(bool open);
static void show_detail(uint32_t seq);

/* ------------------------------------------------------------------ helpers */
static void set_text(lv_obj_t *l, const char *t)
{
    if (l && strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *text, int w, lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, 28);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_center(l);
    if (lbl_out) *lbl_out = l;
    return b;
}

static lv_obj_t *mk_ta(lv_obj_t *parent, bool one_line, int w, int h)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, one_line);
    lv_obj_set_size(ta, w, h);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_obj_set_style_pad_ver(ta, one_line ? 7 : 8, 0);
    lv_obj_set_style_pad_hor(ta, 10, 0);
    lv_obj_set_style_text_font(ta, one_line ? &lv_font_montserrat_14 : &lv_font_nimbus_mono_14, 0);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    return ta;
}

static void style_dd(lv_obj_t *dd)
{
    lv_obj_set_style_radius(dd, 6, 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_pad_ver(dd, 8, 0);
    lv_obj_set_style_pad_hor(dd, 10, 0);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_14, 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_radius(list, 6, 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_14, 0);
}

/* Tapping a publish-panel control sends the keyboard there too. */
static void pub_touch_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_pub_focus = true;
    s_focus_topics = false;
}

static void pub_unfocus(void)
{
    s_pub_focus = false;
    devos_focus_clear(&s_pub_f);
}

static void flash(const char *msg)
{
    set_text(lbl_pub_msg, msg);
    set_text(lbl_status, msg);
    s_flash_until = lv_tick_get() + 2500;
}

static void draw_mono(lv_layer_t *layer, int x, int y, const char *text, lv_color_t col)
{
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &lv_font_nimbus_mono_14;
    ld.flag = LV_TEXT_FLAG_EXPAND;
    ld.text_local = 1;
    ld.color = col;
    ld.text = text;
    int len = (int)strlen(text);
    lv_area_t a = { x, y + 3, x + len * MONO_W + MONO_W, y + ROW_H - 1 };
    lv_draw_label(layer, &ld, &a);
}

/* Fit s into n columns: printable ASCII, whitespace runs collapsed. */
static void fit_text(const char *s, char *out, int n)
{
    int o = 0;
    bool sp = false;
    for (; *s && o < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!sp && o) out[o++] = ' ';
            sp = true;
            continue;
        }
        sp = false;
        out[o++] = (c < 32 || c > 126) ? '.' : (char)c;
    }
    out[o] = '\0';
}

/* Long topics keep their end (the leaf is the useful part). */
static void fit_topic(const char *t, char *out, int n)
{
    int len = (int)strlen(t);
    if (len <= n) snprintf(out, (size_t)n + 1, "%s", t);
    else snprintf(out, (size_t)n + 1, "...%s", t + len - (n - 3));
}

/* ------------------------------------------------------------------ vlist */
static void vl_draw_cb(lv_event_t *e)
{
    vlist_t *v = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!v->count) return;
    const devos_palette_t *p = devos_theme_get();
    lv_area_t a;
    lv_obj_get_coords(v->view, &a);
    const lv_area_t *clip = &layer->_clip_area;
    int r0 = (clip->y1 - a.y1) / ROW_H, r1 = (clip->y2 - a.y1) / ROW_H;
    if (r0 < 0) r0 = 0;
    if (r1 >= v->count) r1 = v->count - 1;
    for (int r = r0; r <= r1; r++) {
        int y = a.y1 + r * ROW_H;
        if (r == v->sel) {
            lv_draw_rect_dsc_t rd;
            lv_draw_rect_dsc_init(&rd);
            rd.bg_color = p->surface_active;
            lv_area_t ra = { a.x1, y, a.x2, y + ROW_H - 1 };
            lv_draw_rect(layer, &rd, &ra);
            rd.bg_color = p->accent_primary;
            lv_area_t bar = { a.x1, y, a.x1 + 2, y + ROW_H - 1 };
            lv_draw_rect(layer, &rd, &bar);
        }
        v->draw(layer, a.x1, y, a.x2, r);
    }
}

static void vl_click_cb(lv_event_t *e)
{
    vlist_t *v = lv_event_get_user_data(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    lv_area_t a;
    lv_obj_get_coords(v->view, &a);
    int idx = (pt.y - a.y1) / ROW_H;
    pub_unfocus();
    if (idx >= 0 && idx < v->count && v->on_select) v->on_select(idx);
}

static void vl_create(vlist_t *v, lv_obj_t *parent, vl_draw_fn draw, void (*on_select)(int))
{
    v->draw = draw;
    v->on_select = on_select;
    v->sel = -1;
    v->scroll = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(v->scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(v->scroll, 0, 0);
    lv_obj_set_style_radius(v->scroll, 0, 0);
    lv_obj_set_style_pad_all(v->scroll, 0, 0);
    lv_obj_set_scroll_dir(v->scroll, LV_DIR_VER);
    v->view = lv_obj_create(v->scroll);
    lv_obj_remove_style_all(v->view);
    lv_obj_add_flag(v->view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(v->view, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(v->view, lv_pct(100), 0);
    lv_obj_add_event_cb(v->view, vl_draw_cb, LV_EVENT_DRAW_MAIN, v);
    lv_obj_add_event_cb(v->view, vl_click_cb, LV_EVENT_CLICKED, v);
}

static void vl_set_count(vlist_t *v, int n)
{
    if (n != v->count) {
        v->count = n;
        lv_obj_set_height(v->view, n * ROW_H);
    }
    lv_obj_invalidate(v->view);
}

static void vl_show(vlist_t *v, int idx)
{
    if (idx < 0) return;
    lv_obj_update_layout(v->scroll);
    int h = lv_obj_get_content_height(v->scroll);
    int y = idx * ROW_H, sy = lv_obj_get_scroll_y(v->scroll);
    s_prog_scroll = true;
    if (y < sy) lv_obj_scroll_to_y(v->scroll, y, LV_ANIM_OFF);
    else if (y + ROW_H > sy + h) lv_obj_scroll_to_y(v->scroll, y + ROW_H - h, LV_ANIM_OFF);
    s_prog_scroll = false;
}

/* ------------------------------------------------------------------ stream */
static uint32_t row_seq(int idx)
{
    return (idx >= 0 && idx < s_row_n) ? s_rows[idx] : 0;
}

static void stream_draw(lv_layer_t *layer, int x1, int y, int x2, int idx)
{
    const devos_palette_t *p = devos_theme_get();
    devos_mqtt_msg_t m;
    char pay[200];
    if (!devos_mqtt_get(row_seq(idx), &m, pay, sizeof(pay))) return;
    char buf[160];
    time_t t = m.time_s;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    int x = x1 + 8;
    draw_mono(layer, x, y, buf, p->text_secondary);
    x += 9 * MONO_W;
    snprintf(buf, sizeof(buf), "%c%c", m.retain ? 'R' : ' ', m.qos ? (char)('0' + m.qos) : ' ');
    draw_mono(layer, x, y, buf, p->accent_warning);
    x += 3 * MONO_W;
    int cols = (x2 - x) / MONO_W;
    int tcols = cols > 70 ? 34 : cols / 2;
    char topic[DEVOS_MQTT_TOPIC_MAX];
    fit_topic(m.topic, topic, tcols);
    draw_mono(layer, x, y, topic, p->accent_primary);
    x += (tcols + 2) * MONO_W;
    int pcols = (x2 - x - 8) / MONO_W;
    if (pcols > (int)sizeof(buf) - 1) pcols = (int)sizeof(buf) - 1;
    if (pcols > 0) {
        fit_text(pay, buf, pcols);
        draw_mono(layer, x, y, buf, p->text_primary);
    }
}

static void stream_select(int idx)
{
    if (idx < 0 || idx >= s_row_n) return;
    s_stream_vl.sel = idx;
    s_sel_seq = row_seq(idx);
    s_follow = idx == s_row_n - 1;
    if (s_follow) s_unseen = 0;
    s_focus_topics = false;
    vl_show(&s_stream_vl, idx);
    lv_obj_invalidate(s_stream_vl.view);
    show_detail(s_sel_seq);
}

static void stream_scroll_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_prog_scroll) return;
    /* scrolled away from the newest message by hand: stop following */
    if (lv_obj_get_scroll_bottom(s_stream_vl.scroll) > ROW_H) s_follow = false;
}

/* ------------------------------------------------------------------ topics */
static void topics_draw(lv_layer_t *layer, int x1, int y, int x2, int idx)
{
    const devos_palette_t *p = devos_theme_get();
    char name[64], cnt[16];
    int cols = (x2 - x1 - 16) / MONO_W;
    if (idx == 0) {
        uint32_t total;
        devos_mqtt_stats(&total, NULL, NULL);
        snprintf(cnt, sizeof(cnt), "%u", (unsigned)total);
        draw_mono(layer, x1 + 8, y, "All topics", s_filter[0] ? p->text_secondary : p->accent_primary);
    } else {
        devos_mqtt_topic_t t;
        if (!devos_mqtt_topic(idx - 1, &t)) return;
        snprintf(cnt, sizeof(cnt), "%u", (unsigned)t.count);
        int ncols = cols - (int)strlen(cnt) - 1;
        if (ncols > (int)sizeof(name) - 1) ncols = (int)sizeof(name) - 1;
        fit_topic(t.topic, name, ncols);
        draw_mono(layer, x1 + 8, y, name, strcmp(t.topic, s_filter) == 0 ? p->accent_primary : p->text_primary);
    }
    draw_mono(layer, x2 - 8 - (int)strlen(cnt) * MONO_W, y, cnt, p->text_secondary);
}

static void topics_select(int idx)
{
    if (idx <= 0) {
        s_filter[0] = '\0';
    } else {
        devos_mqtt_topic_t t;
        if (!devos_mqtt_topic(idx - 1, &t)) return;
        snprintf(s_filter, sizeof(s_filter), "%s", t.topic);
    }
    s_topics_vl.sel = idx;
    s_follow = true;
    s_unseen = 0;
    vl_show(&s_topics_vl, idx);
    refresh(true);
}

/* ------------------------------------------------------------------ detail */
static bool is_binary(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0 || (c < 32 && c != '\t' && c != '\n' && c != '\r') || c == 127) return true;
    }
    return false;
}

static void hex_dump(const unsigned char *d, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = '\0';
    for (size_t i = 0; i < n && o + 80 < cap; i += 16) {
        o += (size_t)snprintf(out + o, cap - o, "%04x  ", (unsigned)i);
        for (size_t k = 0; k < 16; k++) {
            if (i + k < n) o += (size_t)snprintf(out + o, cap - o, "%02x ", d[i + k]);
            else o += (size_t)snprintf(out + o, cap - o, "   ");
        }
        o += (size_t)snprintf(out + o, cap - o, " ");
        for (size_t k = 0; k < 16 && i + k < n; k++) out[o++] = (d[i + k] >= 32 && d[i + k] < 127) ? (char)d[i + k] : '.';
        out[o++] = '\n';
        out[o] = '\0';
    }
}

static void show_detail(uint32_t seq)
{
    if (seq == s_shown_seq && seq) return;
    s_shown_seq = seq;
    devos_mqtt_msg_t m;
    if (!seq || !devos_mqtt_get(seq, &m, s_detail, sizeof(s_detail))) {
        set_text(lbl_detail_hdr, s_row_n ? "Pick a message to see it here" : "");
        s_cv.json = false;
        devos_codeview_set(&s_cv, "");
        return;
    }
    char tbuf[32], hdr[DEVOS_MQTT_TOPIC_MAX + 120];
    time_t t = m.time_s;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm);
    size_t n = m.stored;
    const char *kind;
    if (devos_json_pretty(s_detail, n, s_pretty, sizeof(s_pretty))) {
        kind = "JSON";
        s_cv.json = true;
        s_cv.plain = true;
        devos_codeview_set(&s_cv, s_pretty);
    } else if (is_binary(s_detail, n)) {
        kind = "binary";
        hex_dump((const unsigned char *)s_detail, n, s_pretty, sizeof(s_pretty));
        s_cv.json = false;
        s_cv.plain = true;
        devos_codeview_set(&s_cv, s_pretty);
    } else {
        kind = "text";
        s_cv.json = false;
        s_cv.plain = true;
        devos_codeview_set(&s_cv, s_detail);
    }
    snprintf(hdr, sizeof(hdr), "%s   %s   QoS %d%s   %u bytes%s   %s", m.topic, tbuf, m.qos,
             m.retain ? "   retained" : "", (unsigned)m.len, m.len > m.stored ? " (first 4 KB shown)" : "", kind);
    set_text(lbl_detail_hdr, hdr);
}

/* ------------------------------------------------------------------ templates */
static void tpl_defaults(void)
{
    static const tpl_t defs[] = {
        { "Hello", "devos/tab5/hello", "{\"msg\": \"hello from the Tab5\", \"ts\": {{ts}}}", 0, false },
        { "Online (retained)", "devos/tab5/status", "online", 1, true },
        { "Battery", "devos/tab5/battery", "{\"battery\": {{battery}}, \"uptime\": {{uptime}}}", 0, false },
        { "Clear status", "devos/tab5/status", "", 1, true },
    };
    s_tpl_n = (int)(sizeof(defs) / sizeof(defs[0]));
    memcpy(s_tpl, defs, sizeof(defs));
}

static void tpl_parse_one(const char *e, size_t len, void *ud)
{
    LV_UNUSED(ud);
    if (s_tpl_n >= TPL_MAX || !len || *e != '{') return;
    tpl_t *t = &s_tpl[s_tpl_n];
    memset(t, 0, sizeof(*t));
    int v;
    devos_json_get_str(e, len, "name", t->name, sizeof(t->name));
    devos_json_get_str(e, len, "topic", t->topic, sizeof(t->topic));
    devos_json_get_str(e, len, "payload", t->payload, sizeof(t->payload));
    if (devos_json_get_int(e, len, "qos", &v) == 0) t->qos = v ? 1 : 0;
    if (devos_json_get_int(e, len, "retain", &v) == 0) t->retain = v != 0;
    if (!t->topic[0]) return;
    if (!t->name[0]) {                          /* same struct: copy, don't snprintf */
        size_t n = strlen(t->topic);
        if (n > sizeof(t->name) - 1) n = sizeof(t->name) - 1;
        memcpy(t->name, t->topic, n);
        t->name[n] = '\0';
    }
    s_tpl_n++;
}

static void tpl_load(void)
{
    s_tpl_n = 0;
    FILE *f = fopen(TPL_FILE, "rb");
    if (!f) {
        tpl_defaults();
        return;
    }
    static EXT_RAM_BSS_ATTR char buf[TPL_MAX * (TPL_PAYLOAD * 2 + 512)];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    const char *a = strchr(buf, '[');
    if (a) devos_json_array_each(a, n - (size_t)(a - buf), tpl_parse_one, NULL);
}

static bool tpl_save(void)
{
    mkdir(TPL_DIR, 0755);
    FILE *f = fopen(TPL_FILE, "wb");
    if (!f) return false;
    static EXT_RAM_BSS_ATTR char esc[TPL_PAYLOAD * 6];
    fprintf(f, "[\n");
    for (int i = 0; i < s_tpl_n; i++) {
        const tpl_t *t = &s_tpl[i];
        devos_json_escape(t->name, esc, sizeof(esc));
        fprintf(f, "  {\"name\": \"%s\", ", esc);
        devos_json_escape(t->topic, esc, sizeof(esc));
        fprintf(f, "\"topic\": \"%s\", \"qos\": %d, \"retain\": %d,\n   ", esc, t->qos, t->retain ? 1 : 0);
        devos_json_escape(t->payload, esc, sizeof(esc));
        fprintf(f, "\"payload\": \"%s\"}%s\n", esc, i + 1 < s_tpl_n ? "," : "");
    }
    fprintf(f, "]\n");
    fclose(f);
    return true;
}

static void tpl_fill_dropdown(void)
{
    char opts[TPL_MAX * 52 + 16];
    size_t o = (size_t)snprintf(opts, sizeof(opts), "Templates...");
    for (int i = 0; i < s_tpl_n && o < sizeof(opts) - 60; i++) {
        o += (size_t)snprintf(opts + o, sizeof(opts) - o, "\n%d  %s", i + 1, s_tpl[i].name);
    }
    lv_dropdown_set_options(dd_tpl, opts);
    lv_dropdown_set_selected(dd_tpl, 0);
}

/* {{ts}} {{time}} {{battery}} {{uptime}} */
static void expand(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    const devos_telemetry_t *tel = devos_telemetry_get();
    while (*in && o + 1 < cap) {
        if (in[0] == '{' && in[1] == '{') {
            const char *end = strstr(in, "}}");
            if (end) {
                char key[16] = "", val[40] = "";
                size_t kl = (size_t)(end - in - 2);
                if (kl < sizeof(key)) {
                    memcpy(key, in + 2, kl);
                    key[kl] = '\0';
                }
                time_t now = time(NULL);
                if (!strcmp(key, "ts")) snprintf(val, sizeof(val), "%lld", (long long)now);
                else if (!strcmp(key, "time")) {
                    struct tm tm;
                    localtime_r(&now, &tm);
                    strftime(val, sizeof(val), "%Y-%m-%dT%H:%M:%S", &tm);
                } else if (!strcmp(key, "battery")) snprintf(val, sizeof(val), "%d", tel ? tel->battery_percent : 0);
                else if (!strcmp(key, "uptime")) snprintf(val, sizeof(val), "%u", tel ? (unsigned)tel->uptime_s : 0u);
                else {
                    out[o++] = *in++;
                    continue;
                }
                for (const char *v = val; *v && o + 1 < cap; v++) out[o++] = *v;
                in = end + 2;
                continue;
            }
        }
        out[o++] = *in++;
    }
    out[o] = '\0';
}

static bool publish_now(const char *topic, const char *payload, int qos, bool retain)
{
    static EXT_RAM_BSS_ATTR char body[DEVOS_MQTT_PAYLOAD_MAX];
    char msg[DEVOS_MQTT_TOPIC_MAX + 32];
    if (devos_mqtt_state() != DEVOS_MQTT_UP) {
        flash("Not connected");
        return false;
    }
    if (!topic[0] || strchr(topic, '#') || strchr(topic, '+')) {
        flash("Topic can't be empty or contain # or +");
        return false;
    }
    expand(payload, body, sizeof(body));
    if (devos_mqtt_publish(topic, body, strlen(body), qos, retain) != 0) {
        flash("Couldn't queue it (connection busy?)");
        return false;
    }
    snprintf(msg, sizeof(msg), "Sent to %s", topic);
    flash(msg);
    return true;
}

static void send_template(int i)
{
    if (i < 0 || i >= s_tpl_n) {
        flash("No such template");
        return;
    }
    publish_now(s_tpl[i].topic, s_tpl[i].payload, s_tpl[i].qos, s_tpl[i].retain);
}

/* ------------------------------------------------------------------ publish panel */
static void tpl_dd_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int i = (int)lv_dropdown_get_selected(dd_tpl) - 1;
    if (i < 0 || i >= s_tpl_n) return;
    lv_textarea_set_text(ta_topic, s_tpl[i].topic);
    lv_textarea_set_text(ta_payload, s_tpl[i].payload);
    lv_dropdown_set_selected(dd_qos, (uint32_t)s_tpl[i].qos);
    if (s_tpl[i].retain) lv_obj_add_state(cb_retain, LV_STATE_CHECKED);
    else lv_obj_remove_state(cb_retain, LV_STATE_CHECKED);
}

static void send_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    publish_now(lv_textarea_get_text(ta_topic), lv_textarea_get_text(ta_payload),
                (int)lv_dropdown_get_selected(dd_qos), lv_obj_has_state(cb_retain, LV_STATE_CHECKED));
}

static void tpl_save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    const char *topic = lv_textarea_get_text(ta_topic);
    if (!topic[0]) {
        flash("Enter a topic first");
        return;
    }
    int i;
    for (i = 0; i < s_tpl_n; i++) if (!strcmp(s_tpl[i].topic, topic)) break;
    if (i == s_tpl_n) {
        if (s_tpl_n >= TPL_MAX) {
            flash("Template list is full (16)");
            return;
        }
        s_tpl_n++;
        memset(&s_tpl[i], 0, sizeof(s_tpl[i]));
        const char *leaf = strrchr(topic, '/');
        snprintf(s_tpl[i].name, sizeof(s_tpl[i].name), "%.47s", leaf && leaf[1] ? leaf + 1 : topic);
    }
    snprintf(s_tpl[i].topic, sizeof(s_tpl[i].topic), "%s", topic);
    snprintf(s_tpl[i].payload, sizeof(s_tpl[i].payload), "%s", lv_textarea_get_text(ta_payload));
    s_tpl[i].qos = (int)lv_dropdown_get_selected(dd_qos);
    s_tpl[i].retain = lv_obj_has_state(cb_retain, LV_STATE_CHECKED);
    char msg[80];
    if (tpl_save()) snprintf(msg, sizeof(msg), "Saved as template %d", i + 1);
    else snprintf(msg, sizeof(msg), "Couldn't write the templates file");
    tpl_fill_dropdown();
    lv_dropdown_set_selected(dd_tpl, (uint32_t)i + 1);
    flash(msg);
}

static void tpl_del_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int i = (int)lv_dropdown_get_selected(dd_tpl) - 1;
    if (i < 0 || i >= s_tpl_n) {
        flash("Pick a template to delete");
        return;
    }
    memmove(&s_tpl[i], &s_tpl[i + 1], (size_t)(s_tpl_n - i - 1) * sizeof(s_tpl[0]));
    s_tpl_n--;
    tpl_save();
    tpl_fill_dropdown();
    flash("Template deleted");
}

static void open_publish(bool open)
{
    s_pub_open = open;
    if (open) {
        lv_obj_remove_flag(pub_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(detail, LV_OBJ_FLAG_HIDDEN);
        s_pub_focus = true;
        s_focus_topics = false;
        devos_focus_set(&s_pub_f, lv_textarea_get_text(ta_topic)[0] ? ta_payload : ta_topic);
    } else {
        lv_obj_add_flag(pub_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(detail, LV_OBJ_FLAG_HIDDEN);
        pub_unfocus();
    }
    refresh(true);
}

/* Enter on a message: copy it into the publish panel (to replay or edit). */
static void copy_to_publish(void)
{
    devos_mqtt_msg_t m;
    if (s_sel_seq && devos_mqtt_get(s_sel_seq, &m, s_detail, sizeof(s_detail))) {
        lv_textarea_set_text(ta_topic, m.topic);
        lv_textarea_set_text(ta_payload, s_detail);
        lv_dropdown_set_selected(dd_qos, m.qos ? 1 : 0);
        lv_obj_remove_state(cb_retain, LV_STATE_CHECKED);
        s_shown_seq = 0;
    }
    open_publish(true);
    devos_focus_set(&s_pub_f, ta_payload);
}

/* ------------------------------------------------------------------ broker dialog */
static void modal_show(bool show)
{
    if (show) {
        devos_mqtt_config_t c;
        devos_mqtt_get_config(&c);
        char port[8], subs[DEVOS_MQTT_MAX_SUBS * (DEVOS_MQTT_TOPIC_MAX + 2)] = "";
        snprintf(port, sizeof(port), "%d", c.port);
        for (int i = 0; i < DEVOS_MQTT_MAX_SUBS; i++) {
            if (!c.subs[i][0]) continue;
            size_t l = strlen(subs);
            snprintf(subs + l, sizeof(subs) - l, "%s%s", l ? ", " : "", c.subs[i]);
        }
        lv_textarea_set_text(ta_host, c.host);
        lv_textarea_set_text(ta_port, port);
        lv_textarea_set_text(ta_user, c.username);
        lv_textarea_set_text(ta_pass, c.password);
        lv_textarea_set_text(ta_client, c.client_id);
        lv_textarea_set_text(ta_subs, subs);
        set_text(lbl_modal_err, "");
        lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(overlay);
        devos_focus_set(&s_modal_f, ta_host);
    } else {
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        devos_focus_clear(&s_modal_f);
    }
}

static void modal_ok_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_mqtt_config_t c;
    devos_mqtt_get_config(&c);
    const char *host = lv_textarea_get_text(ta_host);
    while (*host == ' ') host++;
    if (!host[0]) {
        set_text(lbl_modal_err, "Enter the broker's address");
        return;
    }
    int port = atoi(lv_textarea_get_text(ta_port));
    if (port <= 0 || port > 65535) {
        set_text(lbl_modal_err, "Port must be 1-65535 (usually 1883)");
        return;
    }
    snprintf(c.host, sizeof(c.host), "%s", host);
    size_t hl = strlen(c.host);
    while (hl && c.host[hl - 1] == ' ') c.host[--hl] = '\0';
    c.port = port;
    snprintf(c.username, sizeof(c.username), "%s", lv_textarea_get_text(ta_user));
    snprintf(c.password, sizeof(c.password), "%s", lv_textarea_get_text(ta_pass));
    snprintf(c.client_id, sizeof(c.client_id), "%s", lv_textarea_get_text(ta_client));
    /* subscriptions: comma separated */
    char subs[DEVOS_MQTT_MAX_SUBS * (DEVOS_MQTT_TOPIC_MAX + 2)];
    snprintf(subs, sizeof(subs), "%s", lv_textarea_get_text(ta_subs));
    int n = 0;
    for (char *tok = strtok(subs, ","); tok && n < DEVOS_MQTT_MAX_SUBS; tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        size_t l = strlen(tok);
        while (l && tok[l - 1] == ' ') tok[--l] = '\0';
        if (l) snprintf(c.subs[n++], sizeof(c.subs[0]), "%s", tok);
    }
    for (int i = n; i < DEVOS_MQTT_MAX_SUBS; i++) c.subs[i][0] = '\0';
    devos_mqtt_set_config(&c);
    modal_show(false);
    devos_mqtt_start();
    refresh(true);
}

static void modal_cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    modal_show(false);
}

static lv_obj_t *modal_field(lv_obj_t *parent, const char *label, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, label);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, devos_theme_get()->text_secondary, 0);
    lv_obj_t *ta = mk_ta(parent, true, w, 36);
    lv_obj_set_pos(ta, x, y + 18);
    return ta;
}

/* ------------------------------------------------------------------ buttons */
static void conn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (devos_mqtt_state() != DEVOS_MQTT_IDLE) devos_mqtt_stop();
    else if (!devos_mqtt_configured()) modal_show(true);
    else devos_mqtt_start();
    refresh(true);
}

static void broker_cb(lv_event_t *e) { LV_UNUSED(e); modal_show(true); }
static void pub_cb(lv_event_t *e) { LV_UNUSED(e); open_publish(!s_pub_open); }

static void pause_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_paused = !s_paused;
    if (!s_paused) s_follow = true;
    refresh(true);
}

static void clear_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_mqtt_clear();
    s_filter[0] = '\0';
    s_sel_seq = s_shown_seq = 0;
    s_follow = true;
    s_unseen = 0;
    s_topics_vl.sel = 0;
    refresh(true);
    show_detail(0);
}

/* ------------------------------------------------------------------ layout & refresh */
static void apply_layout(void)
{
    int x = s_side_visible ? SIDE_W : 0;
    int w = DEVOS_SCREEN_WIDTH - x;
    if (s_side_visible) lv_obj_remove_flag(side, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(side, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_stream_vl.scroll, x, TOP_H);
    lv_obj_set_size(s_stream_vl.scroll, w, STREAM_H);
    lv_obj_set_pos(lbl_empty, x + 20, TOP_H + 20);
    lv_obj_set_width(lbl_empty, w - 40);
    lv_obj_set_pos(detail, x, TOP_H + STREAM_H);
    lv_obj_set_size(detail, w, DEVOS_CONTENT_HEIGHT - TOP_H - STREAM_H - KEYS_H);
    lv_obj_set_pos(pub_panel, x, TOP_H + STREAM_H);
    lv_obj_set_size(pub_panel, w, DEVOS_CONTENT_HEIGHT - TOP_H - STREAM_H - KEYS_H);
    lv_obj_set_pos(lbl_keys, x + 12, DEVOS_CONTENT_HEIGHT - KEYS_H + 3);
    lv_obj_set_width(lbl_keys, w - 24);
    lv_obj_set_width(ta_payload, w - 32);
    lv_obj_align(lbl_new, LV_ALIGN_TOP_RIGHT, -16, TOP_H + STREAM_H - 34);
}

static void rebuild_rows(void)
{
    uint32_t first = devos_mqtt_seq_first(), next = devos_mqtt_seq_next();
    int n = 0;
    devos_mqtt_msg_t m;
    for (uint32_t s = first; s < next && n < DEVOS_MQTT_RING; s++) {
        if (s_filter[0]) {
            if (!devos_mqtt_get(s, &m, NULL, 0) || strcmp(m.topic, s_filter) != 0) continue;
        }
        s_rows[n++] = s;
    }
    int old_n = s_row_n;
    s_row_n = n;
    if (n > old_n && !s_follow) s_unseen += (uint32_t)(n - old_n);
    vl_set_count(&s_stream_vl, n);
    /* keep the selection on the same message */
    int sel = -1;
    if (s_follow && n) sel = n - 1;
    else if (s_sel_seq) {
        for (int i = n - 1; i >= 0; i--) if (s_rows[i] == s_sel_seq) { sel = i; break; }
    }
    s_stream_vl.sel = sel;
    s_sel_seq = row_seq(sel);
    if (s_follow && n) vl_show(&s_stream_vl, n - 1);
}

static void refresh(bool force)
{
    const devos_palette_t *p = devos_theme_get();
    devos_mqtt_state_t st = devos_mqtt_state();
    char buf[200];
    uint32_t gen = devos_mqtt_generation();
    if ((force || gen != s_last_gen) && !s_paused) {
        s_last_gen = gen;
        rebuild_rows();
        int nt = devos_mqtt_topic_count();
        vl_set_count(&s_topics_vl, nt + 1);
        int sel = 0;
        if (s_filter[0]) {
            devos_mqtt_topic_t t;
            for (int i = 0; i < nt; i++) {
                if (devos_mqtt_topic(i, &t) && !strcmp(t.topic, s_filter)) { sel = i + 1; break; }
            }
        }
        s_topics_vl.sel = sel;
        snprintf(buf, sizeof(buf), "TOPICS (%d)", nt);
        set_text(lbl_side_hdr, buf);
        show_detail(s_sel_seq);
    }
    /* status line (unless a message is showing there) */
    if ((int32_t)(lv_tick_get() - s_flash_until) >= 0) {
        devos_mqtt_status_text(buf, sizeof(buf));
        if (!devos_mqtt_configured()) snprintf(buf, sizeof(buf), "No broker yet: press Broker to set one up");
        set_text(lbl_status, buf);
        set_text(lbl_pub_msg, "");
    }
    lv_color_t lc = st == DEVOS_MQTT_UP ? p->accent_secondary
                  : st == DEVOS_MQTT_IDLE ? p->text_secondary : p->accent_warning;
    lv_obj_set_style_bg_color(led, lc, 0);
    set_text(lbl_conn, st == DEVOS_MQTT_IDLE ? LV_SYMBOL_PLAY " Connect" : LV_SYMBOL_STOP " Disconnect");
    set_text(lbl_pause, s_paused ? LV_SYMBOL_PLAY " Resume" : LV_SYMBOL_PAUSE " Pause");
    lv_obj_set_style_text_color(lbl_pause, s_paused ? p->accent_warning : p->text_primary, 0);
    lv_obj_set_style_text_color(lbl_pub, s_pub_open ? p->accent_primary : p->text_primary, 0);
    uint32_t msgs, bytes;
    float rate;
    devos_mqtt_stats(&msgs, &bytes, &rate);
    if (bytes >= 1024 * 1024) snprintf(buf, sizeof(buf), "%u msgs  %.1f MB  %.1f/s", (unsigned)msgs, bytes / 1048576.0, (double)rate);
    else snprintf(buf, sizeof(buf), "%u msgs  %.1f KB  %.1f/s", (unsigned)msgs, bytes / 1024.0, (double)rate);
    set_text(lbl_stats, buf);
    /* empty stream hint */
    if (s_row_n) lv_obj_add_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_remove_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
        if (!devos_mqtt_configured())
            set_text(lbl_empty, "Press Broker to connect to an MQTT broker (e.g. mosquitto on your LAN or tailnet).\n"
                                "Everything on # shows up here as it arrives.");
        else if (st == DEVOS_MQTT_UP)
            set_text(lbl_empty, s_filter[0] ? "No messages on this topic yet." : "Connected - waiting for messages...");
        else set_text(lbl_empty, "Not connected. Press Connect.");
    }
    if (s_paused) {
        uint32_t next = devos_mqtt_seq_next();
        uint32_t last = s_row_n ? s_rows[s_row_n - 1] + 1 : next;
        snprintf(buf, sizeof(buf), LV_SYMBOL_PAUSE "  Paused  -  %u new", (unsigned)(next - last));
        set_text(lbl_new, buf);
        lv_obj_remove_flag(lbl_new, LV_OBJ_FLAG_HIDDEN);
    } else if (!s_follow && s_unseen) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_DOWN "  %u new  (F)", (unsigned)s_unseen);
        set_text(lbl_new, buf);
        lv_obj_remove_flag(lbl_new, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_new, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_border_color(side, s_focus_topics ? p->accent_primary : p->surface_border, 0);

    /* what the keys do right now */
    const char *keys;
    if (!lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN))
        keys = "Tab / Up / Down move    Enter connects    Esc cancels";
    else if (s_pub_open && s_pub_focus)
        keys = "Tab / Up / Down move    Left / Right change    Enter presses    Ctrl+Enter sends    "
               "Alt+1..9 sends a template    Esc leaves the panel";
    else if (s_focus_topics)
        keys = "Up / Down pick a topic    Tab or Enter back to messages    Esc back";
    else
        keys = "Up / Down messages    Tab topics    Enter copy to publish    P publish    B broker    "
               "O connect    Space pause    C clear    F newest    Sym+L topics    Esc home";
    set_text(lbl_keys, keys);
}

static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!screen || lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN)) return;
    refresh(false);
}

static void style_btn(lv_obj_t *b, lv_obj_t *l, const devos_palette_t *p)
{
    lv_obj_set_style_bg_color(b, p->surface, 0);
    lv_obj_set_style_border_color(b, p->surface_border, 0);
    if (l) lv_obj_set_style_text_color(l, p->text_primary, 0);
}

static void style_ta(lv_obj_t *ta, const devos_palette_t *p)
{
    lv_obj_set_style_bg_color(ta, p->code_bg, 0);
    lv_obj_set_style_text_color(ta, p->text_primary, 0);
    lv_obj_set_style_border_color(ta, p->surface_border, 0);
    lv_obj_set_style_border_color(ta, p->accent_primary, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(ta, p->accent_primary, LV_PART_CURSOR);
}

static void apply_theme(const devos_palette_t *p, void *ud)
{
    LV_UNUSED(ud);
    lv_obj_set_style_bg_color(screen, p->bg, 0);
    lv_obj_set_style_bg_color(top_bar, p->top_bar_bg, 0);
    lv_obj_set_style_border_color(top_bar, p->surface_border, 0);
    lv_obj_set_style_text_color(lbl_title, p->accent_primary, 0);
    lv_obj_set_style_text_color(lbl_status, p->text_secondary, 0);
    lv_obj_set_style_text_color(lbl_stats, p->text_secondary, 0);
    lv_obj_t *bs[][2] = {{btn_conn, lbl_conn}, {btn_broker, lbl_broker}, {btn_pub, lbl_pub}, {btn_pause, lbl_pause},
                         {btn_clear, lbl_clear}, {btn_send, NULL}, {btn_tpl_save, lbl_tpl_save},
                         {btn_tpl_del, lbl_tpl_del}, {btn_modal_cancel, lbl_modal_cancel}};
    for (unsigned i = 0; i < sizeof(bs) / sizeof(bs[0]); i++) style_btn(bs[i][0], bs[i][1], p);
    lv_obj_set_style_bg_color(btn_send, p->accent_primary, 0);
    lv_obj_set_style_text_color(lbl_send, p->bg, 0);
    lv_obj_set_style_bg_color(btn_modal_ok, p->accent_primary, 0);
    lv_obj_set_style_text_color(lbl_modal_ok, p->bg, 0);
    lv_obj_set_style_bg_color(side, p->surface, 0);
    lv_obj_set_style_text_color(lbl_side_hdr, p->text_secondary, 0);
    lv_obj_set_style_text_color(lbl_empty, p->text_secondary, 0);
    lv_obj_set_style_bg_color(lbl_new, p->accent_primary, 0);
    lv_obj_set_style_text_color(lbl_new, p->bg, 0);
    lv_obj_set_style_bg_color(detail, p->code_bg, 0);
    lv_obj_set_style_border_color(detail, p->surface_border, 0);
    lv_obj_set_style_text_color(lbl_detail_hdr, p->text_secondary, 0);
    lv_obj_set_style_bg_color(pub_panel, p->bg_alt, 0);
    lv_obj_set_style_border_color(pub_panel, p->surface_border, 0);
    lv_obj_set_style_text_color(lbl_pub_hint, p->text_secondary, 0);
    lv_obj_set_style_text_color(lbl_keys, p->text_secondary, 0);
    lv_obj_set_style_text_color(lbl_pub_msg, p->accent_secondary, 0);
    lv_obj_set_style_text_color(cb_retain, p->text_primary, 0);
    lv_obj_t *tas[] = {ta_topic, ta_payload, ta_host, ta_port, ta_user, ta_pass, ta_client, ta_subs};
    for (unsigned i = 0; i < sizeof(tas) / sizeof(tas[0]); i++) style_ta(tas[i], p);
    lv_obj_t *dds[] = {dd_tpl, dd_qos};
    for (unsigned i = 0; i < 2; i++) {
        lv_obj_set_style_bg_color(dds[i], p->code_bg, 0);
        lv_obj_set_style_text_color(dds[i], p->text_primary, 0);
        lv_obj_set_style_border_color(dds[i], p->surface_border, 0);
        lv_obj_t *list = lv_dropdown_get_list(dds[i]);
        lv_obj_set_style_bg_color(list, p->surface, 0);
        lv_obj_set_style_text_color(list, p->text_primary, 0);
        lv_obj_set_style_border_color(list, p->surface_border, 0);
        lv_obj_set_style_bg_color(list, p->surface_active, LV_PART_SELECTED | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(list, p->accent_primary, LV_PART_SELECTED | LV_STATE_CHECKED);
    }
    lv_obj_set_style_border_color(cb_retain, p->surface_border, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(cb_retain, p->code_bg, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(cb_retain, p->accent_primary, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(cb_retain, p->accent_primary, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(modal, p->surface, 0);
    lv_obj_set_style_border_color(modal, p->accent_primary, 0);
    lv_obj_set_style_text_color(lbl_modal_title, p->accent_primary, 0);
    lv_obj_set_style_text_color(lbl_modal_err, p->accent_danger, 0);
    lv_obj_invalidate(screen);
}

/* ------------------------------------------------------------------ keys */
static void stream_move(int delta)
{
    if (!s_row_n) return;
    int i = s_stream_vl.sel < 0 ? s_row_n - 1 : s_stream_vl.sel + delta;
    if (i < 0) i = 0;
    if (i >= s_row_n) i = s_row_n - 1;
    stream_select(i);
}

static bool mqtt_handle_key(uint32_t key, uint8_t mods)
{
    if ((mods & DEVOS_MOD_CTRL) && key >= 1 && key <= 26 && key != '\b' && key != '\t' && key != '\n' && key != '\r')
        key += 'a' - 1;
    bool modal_open = !lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    if (modal_open) {
        if (key == LV_KEY_ESC) modal_show(false);
        else if (devos_focus_key(&s_modal_f, key, mods)) { /* moved, typed, pressed a button */ }
        else if (key == '\r' || key == '\n') modal_ok_cb(NULL);   /* Enter in a field */
        return true;
    }

    if ((mods & DEVOS_MOD_FN) && (key == 'l' || key == 'L')) {
        s_side_visible = !s_side_visible;
        apply_layout();
        return true;
    }
    if ((mods & DEVOS_MOD_ALT) && key >= '1' && key <= '9') {
        send_template((int)(key - '1'));
        return true;
    }

    if (s_pub_open && s_pub_focus) {
        if (key == LV_KEY_ESC) {                         /* leave the panel (still open) */
            pub_unfocus();
            refresh(true);
            return true;
        }
        if ((mods & DEVOS_MOD_CTRL) && (key == '\r' || key == '\n')) { send_cb(NULL); return true; }
        if (devos_focus_key(&s_pub_f, key, mods)) return true;
        if (key == '\r' || key == '\n') {                 /* Enter in the topic field / on Retain */
            if (devos_focus_get(&s_pub_f) == ta_topic) devos_focus_set(&s_pub_f, ta_payload);
            return true;
        }
        return (mods & DEVOS_MOD_FN) == 0;
    }

    if (key == LV_KEY_ESC) {
        if (s_pub_open) { open_publish(false); return true; }
        if (s_focus_topics) { s_focus_topics = false; refresh(true); return true; }
        return false;                                   /* home */
    }
    if (mods & DEVOS_MOD_CTRL) {
        if (key == '\r' || key == '\n') { if (s_pub_open) send_cb(NULL); return true; }
        return false;
    }

    if (s_focus_topics) {
        int n = s_topics_vl.count;
        int i = s_topics_vl.sel < 0 ? 0 : s_topics_vl.sel;
        if (key == LV_KEY_UP && i > 0) topics_select(i - 1);
        else if (key == LV_KEY_DOWN && i + 1 < n) topics_select(i + 1);
        else if (key == DEVOS_KEY_PGUP) topics_select(i > 10 ? i - 10 : 0);
        else if (key == DEVOS_KEY_PGDN) topics_select(i + 10 < n ? i + 10 : n - 1);
        else if (key == '\t' || key == LV_KEY_RIGHT || key == '\r') { s_focus_topics = false; refresh(true); }
        return true;
    }

    switch (key) {
    case LV_KEY_UP: stream_move(-1); return true;
    case LV_KEY_DOWN: stream_move(1); return true;
    case DEVOS_KEY_PGUP: stream_move(-(STREAM_H / ROW_H - 1)); return true;
    case DEVOS_KEY_PGDN: stream_move(STREAM_H / ROW_H - 1); return true;
    case '\t': case LV_KEY_LEFT:
        if (s_side_visible) { s_focus_topics = true; refresh(true); }
        return true;
    case ' ': pause_cb(NULL); return true;
    case 'c': case 'C': clear_cb(NULL); return true;
    case 'p': case 'P':
        if (!s_pub_open) open_publish(true);
        else {                                          /* open but not focused: go there */
            s_pub_focus = true;
            s_focus_topics = false;
            devos_focus_first(&s_pub_f);
            refresh(true);
        }
        return true;
    case 'o': case 'O': conn_cb(NULL); return true;
    case 'b': case 'B': modal_show(true); return true;
    case 'f': case 'F':
        s_follow = true;
        s_paused = false;
        s_unseen = 0;
        refresh(true);
        return true;
    case '\r': case '\n': copy_to_publish(); return true;
    default: return (mods & DEVOS_MOD_FN) == 0 && key >= 32 && key <= 126;
    }
}

/* ------------------------------------------------------------------ init */
static void mqtt_init(void)
{
    const devos_palette_t *p = devos_theme_get();
    devos_mqtt_init();
    tpl_load();

    screen = lv_obj_create(lv_screen_active());
    app_descriptor.screen = screen;
    lv_obj_set_size(screen, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(screen, 0, DEVOS_TOP_BAR_HEIGHT);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);

    /* ---- top bar ---- */
    top_bar = lv_obj_create(screen);
    lv_obj_set_size(top_bar, DEVOS_SCREEN_WIDTH, TOP_H);
    lv_obj_set_pos(top_bar, 0, 0);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_border_width(top_bar, 0, 0);
    lv_obj_set_style_border_side(top_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(top_bar, 1, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_remove_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);
    lbl_title = lv_label_create(top_bar);
    lv_label_set_text(lbl_title, "MQTT");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_title, LV_ALIGN_LEFT_MID, 12, 0);
    led = lv_obj_create(top_bar);
    lv_obj_remove_style_all(led);
    lv_obj_set_size(led, 10, 10);
    lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
    lv_obj_align(led, LV_ALIGN_LEFT_MID, 70, 0);
    lbl_status = lv_label_create(top_bar);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_status, 380);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_status, LV_ALIGN_LEFT_MID, 88, 0);
    btn_clear = mk_btn(top_bar, LV_SYMBOL_TRASH " Clear", 70, clear_cb, &lbl_clear);
    lv_obj_align(btn_clear, LV_ALIGN_RIGHT_MID, -8, 0);
    btn_pause = mk_btn(top_bar, LV_SYMBOL_PAUSE " Pause", 80, pause_cb, &lbl_pause);
    lv_obj_align_to(btn_pause, btn_clear, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    btn_pub = mk_btn(top_bar, LV_SYMBOL_UPLOAD " Publish", 84, pub_cb, &lbl_pub);
    lv_obj_align_to(btn_pub, btn_pause, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    btn_broker = mk_btn(top_bar, LV_SYMBOL_SETTINGS " Broker", 80, broker_cb, &lbl_broker);
    lv_obj_align_to(btn_broker, btn_pub, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    btn_conn = mk_btn(top_bar, LV_SYMBOL_PLAY " Connect", 100, conn_cb, &lbl_conn);
    lv_obj_align_to(btn_conn, btn_broker, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    lbl_stats = lv_label_create(top_bar);
    lv_obj_set_style_text_font(lbl_stats, &lv_font_montserrat_12, 0);
    lv_label_set_text(lbl_stats, "");
    lv_label_set_long_mode(lbl_stats, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl_stats, 190);
    lv_obj_set_style_text_align(lbl_stats, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align_to(lbl_stats, btn_conn, LV_ALIGN_OUT_LEFT_MID, -10, 0);

    /* ---- topics ---- */
    side = lv_obj_create(screen);
    lv_obj_set_pos(side, 0, TOP_H);
    lv_obj_set_size(side, SIDE_W, DEVOS_CONTENT_HEIGHT - TOP_H);
    lv_obj_set_style_radius(side, 0, 0);
    lv_obj_set_style_border_width(side, 1, 0);
    lv_obj_set_style_pad_all(side, 0, 0);
    lv_obj_remove_flag(side, LV_OBJ_FLAG_SCROLLABLE);
    lbl_side_hdr = lv_label_create(side);
    lv_label_set_text(lbl_side_hdr, "TOPICS");
    lv_obj_set_style_text_font(lbl_side_hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lbl_side_hdr, 10, 8);
    vl_create(&s_topics_vl, side, topics_draw, topics_select);
    lv_obj_set_pos(s_topics_vl.scroll, 0, 30);
    lv_obj_set_size(s_topics_vl.scroll, SIDE_W - 2, DEVOS_CONTENT_HEIGHT - TOP_H - 32);
    s_topics_vl.sel = 0;

    /* ---- stream ---- */
    vl_create(&s_stream_vl, screen, stream_draw, stream_select);
    lv_obj_add_event_cb(s_stream_vl.scroll, stream_scroll_cb, LV_EVENT_SCROLL, NULL);
    lbl_empty = lv_label_create(screen);
    lv_label_set_long_mode(lbl_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(lbl_empty, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(lbl_empty, LV_OBJ_FLAG_CLICKABLE);
    lbl_new = lv_label_create(screen);
    lv_obj_set_style_bg_opa(lbl_new, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl_new, 10, 0);
    lv_obj_set_style_pad_hor(lbl_new, 10, 0);
    lv_obj_set_style_pad_ver(lbl_new, 4, 0);
    lv_obj_set_style_text_font(lbl_new, &lv_font_montserrat_12, 0);
    lv_obj_add_flag(lbl_new, LV_OBJ_FLAG_HIDDEN);

    /* ---- message detail ---- */
    detail = lv_obj_create(screen);
    lv_obj_set_style_radius(detail, 0, 0);
    lv_obj_set_style_border_width(detail, 1, 0);
    lv_obj_set_style_border_side(detail, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_all(detail, 0, 0);
    lv_obj_remove_flag(detail, LV_OBJ_FLAG_SCROLLABLE);
    lbl_detail_hdr = lv_label_create(detail);
    lv_label_set_long_mode(lbl_detail_hdr, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_detail_hdr, lv_pct(96));
    lv_obj_set_style_text_font(lbl_detail_hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lbl_detail_hdr, 12, 8);
    lv_label_set_text(lbl_detail_hdr, "");
    detail_scroll = lv_obj_create(detail);
    lv_obj_set_style_bg_opa(detail_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(detail_scroll, 0, 0);
    lv_obj_set_style_radius(detail_scroll, 0, 0);
    lv_obj_set_pos(detail_scroll, 4, 30);
    lv_obj_set_size(detail_scroll, lv_pct(99), DEVOS_CONTENT_HEIGHT - TOP_H - STREAM_H - KEYS_H - 34);
    devos_codeview_create(&s_cv, detail_scroll);
    s_cv.plain = true;

    /* ---- publish panel ---- */
    pub_panel = lv_obj_create(screen);
    lv_obj_set_style_radius(pub_panel, 0, 0);
    lv_obj_set_style_border_width(pub_panel, 1, 0);
    lv_obj_set_style_border_side(pub_panel, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_all(pub_panel, 0, 0);
    lv_obj_remove_flag(pub_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pub_panel, LV_OBJ_FLAG_HIDDEN);
    dd_tpl = lv_dropdown_create(pub_panel);
    lv_obj_set_size(dd_tpl, 260, 36);
    lv_obj_set_pos(dd_tpl, 16, 10);
    lv_obj_add_event_cb(dd_tpl, tpl_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);
    style_dd(dd_tpl);
    btn_tpl_save = mk_btn(pub_panel, LV_SYMBOL_SAVE " Save as template", 150, tpl_save_cb, &lbl_tpl_save);
    lv_obj_set_pos(btn_tpl_save, 288, 14);
    btn_tpl_del = mk_btn(pub_panel, LV_SYMBOL_TRASH " Delete", 80, tpl_del_cb, &lbl_tpl_del);
    lv_obj_set_pos(btn_tpl_del, 446, 14);
    ta_topic = mk_ta(pub_panel, true, 520, 36);
    lv_textarea_set_placeholder_text(ta_topic, "Topic, e.g. home/lounge/light/set");
    lv_obj_set_pos(ta_topic, 16, 54);
    dd_qos = lv_dropdown_create(pub_panel);
    lv_dropdown_set_options(dd_qos, "QoS 0\nQoS 1");
    lv_obj_set_size(dd_qos, 110, 36);
    lv_obj_set_pos(dd_qos, 548, 54);
    style_dd(dd_qos);
    cb_retain = lv_checkbox_create(pub_panel);
    lv_checkbox_set_text(cb_retain, "Retain");
    lv_obj_set_pos(cb_retain, 672, 62);
    lv_obj_set_style_radius(cb_retain, 4, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(cb_retain, 2, LV_PART_INDICATOR);
    btn_send = mk_btn(pub_panel, LV_SYMBOL_UPLOAD "  Send", 110, send_cb, &lbl_send);
    lv_obj_set_size(btn_send, 110, 36);
    lv_obj_align(btn_send, LV_ALIGN_TOP_RIGHT, -16, 54);
    ta_payload = mk_ta(pub_panel, false, 600, 150);
    lv_textarea_set_placeholder_text(ta_payload, "Payload");
    lv_obj_set_pos(ta_payload, 16, 98);
    lbl_pub_hint = lv_label_create(pub_panel);
    lv_label_set_text(lbl_pub_hint, "{{ts}} {{time}} {{battery}} {{uptime}} in the topic or payload are filled in when sent");
    lv_obj_set_style_text_font(lbl_pub_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lbl_pub_hint, 16, 256);
    lbl_pub_msg = lv_label_create(pub_panel);
    lv_label_set_text(lbl_pub_msg, "");
    lv_obj_set_style_text_font(lbl_pub_msg, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_pub_msg, LV_ALIGN_TOP_RIGHT, -16, 20);
    tpl_fill_dropdown();
    devos_focus_init(&s_pub_f);
    lv_obj_t *pub_order[] = {dd_tpl, btn_tpl_save, btn_tpl_del, ta_topic, dd_qos, cb_retain, btn_send, ta_payload};
    for (unsigned i = 0; i < sizeof(pub_order) / sizeof(pub_order[0]); i++) {
        devos_focus_add(&s_pub_f, pub_order[i]);
        lv_obj_add_event_cb(pub_order[i], pub_touch_cb, LV_EVENT_PRESSED, NULL);
    }

    /* ---- key hints ---- */
    lbl_keys = lv_label_create(screen);
    lv_label_set_long_mode(lbl_keys, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl_keys, &lv_font_montserrat_12, 0);
    lv_label_set_text(lbl_keys, "");

    /* ---- broker dialog ---- */
    overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    modal = lv_obj_create(overlay);
    lv_obj_set_size(modal, 640, 350);
    lv_obj_align(modal, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_border_width(modal, 2, 0);
    lv_obj_set_style_radius(modal, 8, 0);
    lv_obj_set_style_pad_all(modal, 20, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lbl_modal_title = lv_label_create(modal);
    lv_label_set_text(lbl_modal_title, LV_SYMBOL_SETTINGS "  MQTT broker");
    lv_obj_set_style_text_font(lbl_modal_title, &lv_font_montserrat_16, 0);
    ta_host = modal_field(modal, "Address (IP, hostname or tailnet name)", 0, 30, 440);
    lv_textarea_set_placeholder_text(ta_host, "192.168.1.10");
    ta_port = modal_field(modal, "Port", 456, 30, 140);
    lv_textarea_set_accepted_chars(ta_port, "0123456789");
    ta_user = modal_field(modal, "Username (optional)", 0, 92, 290);
    ta_pass = modal_field(modal, "Password", 306, 92, 290);
    lv_textarea_set_password_mode(ta_pass, true);
    ta_client = modal_field(modal, "Client ID (blank = automatic)", 0, 154, 290);
    ta_subs = modal_field(modal, "Subscribe to (comma separated, max 4)", 306, 154, 290);
    lv_textarea_set_placeholder_text(ta_subs, "#");
    lbl_modal_err = lv_label_create(modal);
    lv_label_set_text(lbl_modal_err, "");
    lv_obj_set_style_text_font(lbl_modal_err, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lbl_modal_err, 0, 222);
    lv_obj_t *hint = lv_label_create(modal);
    lv_label_set_text(hint, "Tab / arrows move between fields   Enter connects   Esc cancels.  Plain MQTT (no TLS).");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, p->text_secondary, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, -6);
    btn_modal_ok = mk_btn(modal, LV_SYMBOL_OK "  Connect", 120, modal_ok_cb, &lbl_modal_ok);
    lv_obj_set_size(btn_modal_ok, 120, 36);
    lv_obj_align(btn_modal_ok, LV_ALIGN_TOP_RIGHT, -136, 236);
    btn_modal_cancel = mk_btn(modal, "Cancel", 120, modal_cancel_cb, &lbl_modal_cancel);
    lv_obj_set_size(btn_modal_cancel, 120, 36);
    lv_obj_align(btn_modal_cancel, LV_ALIGN_TOP_RIGHT, 0, 236);
    devos_focus_init(&s_modal_f);
    lv_obj_t *modal_order[] = {ta_host, ta_port, ta_user, ta_pass, ta_client, ta_subs, btn_modal_ok, btn_modal_cancel};
    for (unsigned i = 0; i < sizeof(modal_order) / sizeof(modal_order[0]); i++) devos_focus_add(&s_modal_f, modal_order[i]);

    apply_layout();
    apply_theme(p, NULL);
    devos_theme_add_listener(apply_theme, NULL);
    lv_timer_create(tick_cb, 250, NULL);
}

static void mqtt_show(void)
{
    /* connect on first open if a broker is set */
    if (!s_autostarted && devos_mqtt_configured() && devos_mqtt_state() == DEVOS_MQTT_IDLE) devos_mqtt_start();
    s_autostarted = true;
    apply_layout();
    refresh(true);
}

static void mqtt_hide(void)
{
    pub_unfocus();
}

static int mqtt_telemetry_lines(char lines[3][64])
{
    devos_mqtt_config_t c;
    devos_mqtt_get_config(&c);
    uint32_t msgs;
    float rate;
    devos_mqtt_stats(&msgs, NULL, &rate);
    devos_mqtt_state_t st = devos_mqtt_state();
    if (!c.host[0]) snprintf(lines[0], sizeof(lines[0]), "* No broker set");
    else snprintf(lines[0], sizeof(lines[0]), "* %s %.40s",
                  st == DEVOS_MQTT_UP ? "Connected to" : st == DEVOS_MQTT_IDLE ? "Off:" : "Connecting to", c.host);
    snprintf(lines[1], sizeof(lines[1]), "* %u msgs, %.1f/s", (unsigned)msgs, (double)rate);
    snprintf(lines[2], sizeof(lines[2]), "* %d topics", devos_mqtt_topic_count());
    return 3;
}

devos_app_descriptor_t *app_mqtt_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_MQTT;
    app_descriptor.uid = "mqtt";
    app_descriptor.icon = LV_SYMBOL_SHUFFLE;
    app_descriptor.category = "network";
    app_descriptor.name = "MQTT";
    app_descriptor.title = "MQTT";
    app_descriptor.subtitle = "Watch and publish broker traffic";
    app_descriptor.screen = screen;
    app_descriptor.init = mqtt_init;
    app_descriptor.show = mqtt_show;
    app_descriptor.hide = mqtt_hide;
    app_descriptor.handle_key = mqtt_handle_key;
    app_descriptor.get_telemetry_lines = mqtt_telemetry_lines;
    return &app_descriptor;
}
