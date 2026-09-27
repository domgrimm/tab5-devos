/* devos_widgets: see devos_widgets.h. */
#include "devos_widgets.h"
#include "devos_theme.h"
#include "devos_config.h"

#include <string.h>

LV_FONT_DECLARE(lv_font_nimbus_mono_14);

typedef struct {
    lv_obj_t *obj;
    uint8_t kind;
} tracked_t;

static tracked_t *s_tr;
static int s_tr_n, s_tr_cap;
static bool s_listening;

const lv_font_t *devos_w_mono(void) { return &lv_font_nimbus_mono_14; }

/* ------------------------------------------------------------------ theme */
static void style_obj(lv_obj_t *o, devos_w_kind_t k, const devos_palette_t *p)
{
    switch (k) {
    case DEVOS_W_SCREEN:
        lv_obj_set_style_bg_color(o, p->bg, 0);
        break;
    case DEVOS_W_BAR:
        lv_obj_set_style_bg_color(o, p->top_bar_bg, 0);
        lv_obj_set_style_border_color(o, p->surface_border, 0);
        break;
    case DEVOS_W_PANEL:
        lv_obj_set_style_bg_color(o, p->surface, 0);
        lv_obj_set_style_border_color(o, p->surface_border, 0);
        break;
    case DEVOS_W_PANEL_ALT:
        lv_obj_set_style_bg_color(o, p->bg_alt, 0);
        lv_obj_set_style_border_color(o, p->surface_border, 0);
        break;
    case DEVOS_W_CODE:
        lv_obj_set_style_bg_color(o, p->code_bg, 0);
        lv_obj_set_style_border_color(o, p->surface_border, 0);
        break;
    case DEVOS_W_MODAL:
        lv_obj_set_style_bg_color(o, p->surface, 0);
        lv_obj_set_style_border_color(o, p->accent_primary, 0);
        break;
    case DEVOS_W_BTN:
        lv_obj_set_style_bg_color(o, p->surface, 0);
        lv_obj_set_style_border_color(o, p->surface_border, 0);
        lv_obj_set_style_text_color(o, p->text_primary, 0);
        lv_obj_set_style_bg_color(o, p->surface_active, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(o, p->text_muted, LV_STATE_DISABLED);
        break;
    case DEVOS_W_BTN_PRIMARY:
    case DEVOS_W_BTN_DANGER: {
        lv_color_t c = k == DEVOS_W_BTN_PRIMARY ? p->accent_primary : p->accent_danger;
        lv_obj_set_style_bg_color(o, c, 0);
        lv_obj_set_style_border_color(o, c, 0);
        lv_obj_set_style_text_color(o, p->bg, 0);
        lv_obj_set_style_bg_color(o, lv_color_mix(c, p->bg, 200), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(o, p->surface, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(o, p->text_muted, LV_STATE_DISABLED);
        break;
    }
    case DEVOS_W_TA:
        lv_obj_set_style_bg_color(o, p->code_bg, 0);
        lv_obj_set_style_text_color(o, p->text_primary, 0);
        lv_obj_set_style_border_color(o, p->surface_border, 0);
        lv_obj_set_style_border_color(o, p->accent_primary, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(o, p->accent_primary, LV_PART_CURSOR);
        lv_obj_set_style_text_color(o, p->text_muted, LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_set_style_bg_color(o, p->accent_primary, LV_PART_SELECTED);
        lv_obj_set_style_text_color(o, p->bg, LV_PART_SELECTED);
        break;
    case DEVOS_W_DD: {
        lv_obj_set_style_bg_color(o, p->code_bg, 0);
        lv_obj_set_style_text_color(o, p->text_primary, 0);
        lv_obj_set_style_border_color(o, p->surface_border, 0);
        lv_obj_t *list = lv_dropdown_get_list(o);
        if (list) {
            lv_obj_set_style_bg_color(list, p->surface, 0);
            lv_obj_set_style_text_color(list, p->text_primary, 0);
            lv_obj_set_style_border_color(list, p->surface_border, 0);
            lv_obj_set_style_bg_color(list, p->surface_active, LV_PART_SELECTED | LV_STATE_CHECKED);
            lv_obj_set_style_text_color(list, p->accent_primary, LV_PART_SELECTED | LV_STATE_CHECKED);
            lv_obj_set_style_bg_color(list, p->surface_active, LV_PART_SELECTED | LV_STATE_PRESSED);
        }
        break;
    }
    case DEVOS_W_CB:
        lv_obj_set_style_text_color(o, p->text_primary, 0);
        lv_obj_set_style_border_color(o, p->surface_border, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(o, p->code_bg, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(o, p->accent_primary, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(o, p->accent_primary, LV_PART_INDICATOR | LV_STATE_CHECKED);
        break;
    case DEVOS_W_TEXT: lv_obj_set_style_text_color(o, p->text_primary, 0); break;
    case DEVOS_W_TEXT_DIM: lv_obj_set_style_text_color(o, p->text_secondary, 0); break;
    case DEVOS_W_TEXT_MUTED: lv_obj_set_style_text_color(o, p->text_muted, 0); break;
    case DEVOS_W_TEXT_ACCENT: lv_obj_set_style_text_color(o, p->accent_primary, 0); break;
    case DEVOS_W_TEXT_OK: lv_obj_set_style_text_color(o, p->accent_secondary, 0); break;
    case DEVOS_W_TEXT_WARN: lv_obj_set_style_text_color(o, p->accent_warning, 0); break;
    case DEVOS_W_TEXT_ERR: lv_obj_set_style_text_color(o, p->accent_danger, 0); break;
    case DEVOS_W_TEXT_ON_ACCENT: lv_obj_set_style_text_color(o, p->bg, 0); break;
    case DEVOS_W_BADGE:
        lv_obj_set_style_bg_color(o, p->accent_primary, 0);
        lv_obj_set_style_text_color(o, p->bg, 0);
        break;
    case DEVOS_W_PROGRESS:
        lv_obj_set_style_bg_color(o, p->surface_border, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, p->accent_primary, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(o, p->accent_primary, LV_PART_KNOB);
        break;
    default:
        break;
    }
}

static void theme_cb(const devos_palette_t *p, void *ud)
{
    (void)ud;
    for (int i = 0; i < s_tr_n; i++) style_obj(s_tr[i].obj, (devos_w_kind_t)s_tr[i].kind, p);
}

static void untrack_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    for (int i = 0; i < s_tr_n; i++) {
        if (s_tr[i].obj == o) {
            s_tr[i] = s_tr[--s_tr_n];
            return;
        }
    }
}

void devos_w_track(lv_obj_t *obj, devos_w_kind_t kind)
{
    if (!obj) return;
    if (!s_listening) {
        s_listening = true;
        devos_theme_add_listener(theme_cb, NULL);
    }
    int i;
    for (i = 0; i < s_tr_n && s_tr[i].obj != obj; i++) {}
    if (i == s_tr_n) {
        if (s_tr_n == s_tr_cap) {
            int cap = s_tr_cap ? s_tr_cap * 2 : 256;
            tracked_t *n = lv_realloc(s_tr, (size_t)cap * sizeof(*n));
            if (!n) return;
            s_tr = n;
            s_tr_cap = cap;
        }
        s_tr_n++;
        lv_obj_add_event_cb(obj, untrack_cb, LV_EVENT_DELETE, NULL);
    }
    s_tr[i].obj = obj;
    s_tr[i].kind = (uint8_t)kind;
    style_obj(obj, kind, devos_theme_get());
}

void devos_w_set_text(lv_obj_t *l, const char *t)
{
    if (l && t && strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

/* ------------------------------------------------------------------ containers */
static void plain(lv_obj_t *o)
{
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *devos_w_screen(devos_app_descriptor_t *d)
{
    lv_obj_t *s = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(s, 0, DEVOS_TOP_BAR_HEIGHT);
    plain(s);
    lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
    devos_w_track(s, DEVOS_W_SCREEN);
    if (d) d->screen = s;
    return s;
}

lv_obj_t *devos_w_bar(lv_obj_t *screen, const char *title, lv_obj_t **title_out)
{
    lv_obj_t *b = lv_obj_create(screen);
    lv_obj_set_size(b, DEVOS_SCREEN_WIDTH, DEVOS_W_BAR_H);
    lv_obj_set_pos(b, 0, 0);
    plain(b);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    devos_w_track(b, DEVOS_W_BAR);
    lv_obj_t *t = devos_w_label(b, &lv_font_montserrat_16, DEVOS_W_TEXT_ACCENT, title);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 12, 0);
    if (title_out) *title_out = t;
    return b;
}

lv_obj_t *devos_w_panel(lv_obj_t *parent, int x, int y, int w, int h, devos_w_kind_t kind)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    plain(o);
    lv_obj_set_style_border_width(o, 1, 0);
    devos_w_track(o, kind);
    return o;
}

lv_obj_t *devos_w_label(lv_obj_t *parent, const lv_font_t *font, devos_w_kind_t kind, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text ? text : "");
    lv_obj_set_style_text_font(l, font ? font : &lv_font_montserrat_14, 0);
    devos_w_track(l, kind);
    return l;
}

/* ------------------------------------------------------------------ controls */
lv_obj_t *devos_w_btn_kind(lv_obj_t *parent, devos_w_kind_t kind, const char *text, int w,
                           lv_event_cb_t cb, void *ud, lv_obj_t **label_out)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, 28);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_center(l);
    devos_w_track(b, kind);         /* the label inherits the button's text colour */
    if (label_out) *label_out = l;
    return b;
}

lv_obj_t *devos_w_btn(lv_obj_t *parent, const char *text, int w, lv_event_cb_t cb, void *ud,
                      lv_obj_t **label_out)
{
    return devos_w_btn_kind(parent, DEVOS_W_BTN, text, w, cb, ud, label_out);
}

lv_obj_t *devos_w_ta(lv_obj_t *parent, bool one_line, int w, int h)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, one_line);
    lv_obj_set_size(ta, w, h);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_obj_set_style_pad_ver(ta, one_line ? 7 : 8, 0);
    lv_obj_set_style_pad_hor(ta, 10, 0);
    lv_obj_set_style_text_font(ta, one_line ? &lv_font_montserrat_14 : &lv_font_nimbus_mono_14, 0);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    devos_w_track(ta, DEVOS_W_TA);
    return ta;
}

lv_obj_t *devos_w_field(lv_obj_t *parent, const char *caption, int x, int y, int w)
{
    lv_obj_t *l = devos_w_label(parent, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, caption);
    lv_obj_set_pos(l, x, y);
    lv_obj_t *ta = devos_w_ta(parent, true, w, 36);
    lv_obj_set_pos(ta, x, y + 18);
    return ta;
}

lv_obj_t *devos_w_dd(lv_obj_t *parent, const char *options, int w)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    if (options) lv_dropdown_set_options(dd, options);
    lv_obj_set_size(dd, w, 36);
    lv_obj_set_style_radius(dd, 6, 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_pad_ver(dd, 8, 0);
    lv_obj_set_style_pad_hor(dd, 10, 0);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_14, 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_radius(list, 6, 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_14, 0);
    devos_w_track(dd, DEVOS_W_DD);
    return dd;
}

lv_obj_t *devos_w_cb(lv_obj_t *parent, const char *text)
{
    lv_obj_t *cb = lv_checkbox_create(parent);
    lv_checkbox_set_text(cb, text);
    lv_obj_set_style_text_font(cb, &lv_font_montserrat_14, 0);
    lv_obj_set_style_radius(cb, 4, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(cb, 2, LV_PART_INDICATOR);
    devos_w_track(cb, DEVOS_W_CB);
    return cb;
}

lv_obj_t *devos_w_keys(lv_obj_t *screen)
{
    lv_obj_t *l = devos_w_label(screen, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "");
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, DEVOS_SCREEN_WIDTH - 24);
    lv_obj_set_pos(l, 12, DEVOS_CONTENT_HEIGHT - DEVOS_W_KEYS_H + 3);
    return l;
}

/* ------------------------------------------------------------------ dialog */
void devos_w_dialog(devos_w_dialog_t *d, lv_obj_t *screen, int w, int h, const char *title)
{
    d->overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(d->overlay);
    lv_obj_set_size(d->overlay, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_style_bg_color(d->overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(d->overlay, LV_OPA_50, 0);
    lv_obj_add_flag(d->overlay, LV_OBJ_FLAG_CLICKABLE);     /* swallow taps behind the box */
    lv_obj_add_flag(d->overlay, LV_OBJ_FLAG_HIDDEN);
    d->box = lv_obj_create(d->overlay);
    lv_obj_set_size(d->box, w, h);
    lv_obj_align(d->box, LV_ALIGN_TOP_MID, 0, h < DEVOS_CONTENT_HEIGHT - 80 ? 40 : 10);
    lv_obj_set_style_border_width(d->box, 2, 0);
    lv_obj_set_style_radius(d->box, 8, 0);
    lv_obj_set_style_pad_all(d->box, 20, 0);
    lv_obj_remove_flag(d->box, LV_OBJ_FLAG_SCROLLABLE);
    devos_w_track(d->box, DEVOS_W_MODAL);
    d->title = devos_w_label(d->box, &lv_font_montserrat_16, DEVOS_W_TEXT_ACCENT, title);
    d->msg = devos_w_label(d->box, &lv_font_montserrat_12, DEVOS_W_TEXT_ERR, "");
    lv_obj_set_width(d->msg, w - 300);
    lv_label_set_long_mode(d->msg, LV_LABEL_LONG_WRAP);
    lv_obj_align(d->msg, LV_ALIGN_BOTTOM_LEFT, 0, -4);
}

void devos_w_dialog_show(devos_w_dialog_t *d, bool show)
{
    if (show) {
        lv_obj_remove_flag(d->overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(d->overlay);
    } else {
        lv_obj_add_flag(d->overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

bool devos_w_dialog_open(const devos_w_dialog_t *d)
{
    return d->overlay && !lv_obj_has_flag(d->overlay, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ drawing */
int devos_w_draw_text(lv_layer_t *layer, const lv_font_t *font, int x, int y, int w, const char *text,
                      lv_color_t color)
{
    if (!text || !*text) return x;
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = font ? font : &lv_font_nimbus_mono_14;
    ld.color = color;
    ld.text = text;
    ld.text_local = 1;
    int tw = lv_text_get_width(text, strlen(text), ld.font, 0);
    if (w > 0 && tw > w) {
        ld.flag = LV_TEXT_FLAG_NONE;
        tw = w;
    } else {
        ld.flag = LV_TEXT_FLAG_EXPAND;
    }
    int h = lv_font_get_line_height(ld.font);
    lv_area_t a = { x, y, x + (w > 0 ? w : tw + 8) - 1, y + h - 1 };
    lv_draw_label(layer, &ld, &a);
    return x + tw;
}

void devos_w_draw_rect(lv_layer_t *layer, int x1, int y1, int x2, int y2, lv_color_t color, lv_opa_t opa, int radius)
{
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = color;
    rd.bg_opa = opa;
    rd.radius = radius;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &rd, &a);
}

/* ------------------------------------------------------------------ vlist */
static void vl_draw_cb(lv_event_t *e)
{
    devos_vlist_t *v = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!v->count || !v->draw) return;
    const devos_palette_t *p = devos_theme_get();
    lv_area_t a;
    lv_obj_get_coords(v->view, &a);
    const lv_area_t *clip = &layer->_clip_area;
    int r0 = (clip->y1 - a.y1) / v->row_h, r1 = (clip->y2 - a.y1) / v->row_h;
    if (r0 < 0) r0 = 0;
    if (r1 >= v->count) r1 = v->count - 1;
    for (int r = r0; r <= r1; r++) {
        lv_area_t row = { a.x1, a.y1 + r * v->row_h, a.x2, a.y1 + (r + 1) * v->row_h - 1 };
        if (r == v->sel) {
            devos_w_draw_rect(layer, row.x1, row.y1, row.x2, row.y2, p->surface_active, LV_OPA_COVER, 0);
            devos_w_draw_rect(layer, row.x1, row.y1, row.x1 + 2, row.y2,
                              v->active ? p->accent_primary : p->text_muted, LV_OPA_COVER, 0);
        }
        v->draw(v, layer, &row, r);
    }
}

static void vl_click_cb(lv_event_t *e)
{
    devos_vlist_t *v = lv_event_get_user_data(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    lv_area_t a;
    lv_obj_get_coords(v->view, &a);
    int idx = (pt.y - a.y1) / v->row_h;
    if (idx < 0 || idx >= v->count) return;
    if (idx == v->sel) {
        if (v->on_activate) v->on_activate(v, idx);
    } else {
        devos_vlist_select(v, idx);
    }
}

void devos_vlist_create(devos_vlist_t *v, lv_obj_t *parent, int row_h, devos_vlist_draw_fn draw)
{
    memset(v, 0, sizeof(*v));
    v->row_h = row_h;
    v->draw = draw;
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

void devos_vlist_set_count(devos_vlist_t *v, int n)
{
    if (n < 0) n = 0;
    if (n != v->count) {
        v->count = n;
        lv_obj_set_height(v->view, n * v->row_h);
    }
    if (v->sel >= n) v->sel = n - 1;
    lv_obj_invalidate(v->view);
}

void devos_vlist_redraw(devos_vlist_t *v) { lv_obj_invalidate(v->view); }

static void vl_show(devos_vlist_t *v, int idx)
{
    if (idx < 0) return;
    lv_obj_update_layout(v->scroll);
    int h = lv_obj_get_content_height(v->scroll);
    int y = idx * v->row_h, sy = lv_obj_get_scroll_y(v->scroll);
    if (y < sy) lv_obj_scroll_to_y(v->scroll, y, LV_ANIM_OFF);
    else if (y + v->row_h > sy + h) lv_obj_scroll_to_y(v->scroll, y + v->row_h - h, LV_ANIM_OFF);
}

void devos_vlist_select(devos_vlist_t *v, int idx)
{
    if (!v->count) {
        v->sel = -1;
        return;
    }
    if (idx < 0) idx = 0;
    if (idx >= v->count) idx = v->count - 1;
    v->sel = idx;
    vl_show(v, idx);
    lv_obj_invalidate(v->view);
    if (v->on_select) v->on_select(v, idx);
}

bool devos_vlist_key(devos_vlist_t *v, uint32_t key)
{
    int page = 1;
    if (v->row_h) {
        lv_obj_update_layout(v->scroll);
        page = lv_obj_get_content_height(v->scroll) / v->row_h - 1;
        if (page < 1) page = 1;
    }
    int cur = v->sel < 0 ? -1 : v->sel;
    switch (key) {
    case LV_KEY_UP: devos_vlist_select(v, cur <= 0 ? 0 : cur - 1); return true;
    case LV_KEY_DOWN: devos_vlist_select(v, cur + 1); return true;
    case DEVOS_KEY_PGUP: devos_vlist_select(v, cur - page); return true;
    case DEVOS_KEY_PGDN: devos_vlist_select(v, cur < 0 ? page : cur + page); return true;
    case LV_KEY_HOME: devos_vlist_select(v, 0); return true;
    case LV_KEY_END: devos_vlist_select(v, v->count - 1); return true;
    case '\r': case '\n':
        if (v->sel >= 0 && v->on_activate) {
            v->on_activate(v, v->sel);
            return true;
        }
        return false;
    default:
        return false;
    }
}
