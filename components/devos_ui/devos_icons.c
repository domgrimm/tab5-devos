/* devos_icons: see devos_icons.h.
 *
 * Every icon is designed on a 20 x 20 grid, written here in tenths (0..200),
 * and scaled to the square it is drawn into. */
#include "devos_icons.h"

typedef struct {
    lv_layer_t *layer;
    int32_t x, y, s;            /* the square: top-left and side */
    lv_color_t c;
} pen_t;

static pen_t pen(lv_layer_t *layer, const lv_area_t *a, lv_color_t c)
{
    int32_t w = lv_area_get_width(a), h = lv_area_get_height(a);
    int32_t s = w < h ? w : h;
    pen_t p = { layer, a->x1 + (w - s) / 2, a->y1 + (h - s) / 2, s, c };
    return p;
}

static int32_t px(const pen_t *p, int32_t v) { return p->x + (v * p->s + 100) / 200; }
static int32_t py(const pen_t *p, int32_t v) { return p->y + (v * p->s + 100) / 200; }

/* a length (stroke width, radius): at least one pixel */
static int32_t len(const pen_t *p, int32_t v)
{
    int32_t l = (v * p->s + 100) / 200;
    return l < 1 ? 1 : l;
}

static void line(const pen_t *p, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t w)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = p->c;
    d.width = len(p, w);
    d.round_start = d.round_end = 1;
    d.p1.x = px(p, x1);
    d.p1.y = py(p, y1);
    d.p2.x = px(p, x2);
    d.p2.y = py(p, y2);
    lv_draw_line(p->layer, &d);
}

/* a stroke through the points, joints rounded */
static void polyline(const pen_t *p, const int16_t *xy, int n, int32_t w)
{
    for (int i = 0; i + 1 < n; i++) line(p, xy[2 * i], xy[2 * i + 1], xy[2 * i + 2], xy[2 * i + 3], w);
}

static void box(const pen_t *p, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t r, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = p->c;
    d.bg_opa = opa;
    d.radius = r < 0 ? LV_RADIUS_CIRCLE : r == 0 ? 0 : len(p, r);
    lv_area_t a = { px(p, x1), py(p, y1), px(p, x2) - 1, py(p, y2) - 1 };
    lv_draw_rect(p->layer, &d, &a);
}

static void frame(const pen_t *p, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t r, int32_t w)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = p->c;
    d.border_width = len(p, w);
    d.border_opa = LV_OPA_COVER;
    d.radius = len(p, r);
    lv_area_t a = { px(p, x1), py(p, y1), px(p, x2) - 1, py(p, y2) - 1 };
    lv_draw_rect(p->layer, &d, &a);
}

static void dot(const pen_t *p, int32_t cx, int32_t cy, int32_t r, lv_opa_t opa)
{
    box(p, cx - r, cy - r, cx + r, cy + r, -1, opa);
}

/* `r` is the outer radius; the stroke of width `w` lies inside it.
 * Angles in degrees, clockwise from 3 o'clock. */
static void arc(const pen_t *p, int32_t cx, int32_t cy, int32_t r, int32_t a0, int32_t a1, int32_t w)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = p->c;
    d.width = len(p, w);
    d.rounded = 1;
    d.center.x = px(p, cx);
    d.center.y = py(p, cy);
    d.radius = (uint16_t)len(p, r);
    d.start_angle = a0;
    d.end_angle = a1;
    lv_draw_arc(p->layer, &d);
}

static void tri(const pen_t *p, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.bg_color = p->c;
    d.bg_opa = LV_OPA_COVER;
    d.p[0].x = px(p, x1);
    d.p[0].y = py(p, y1);
    d.p[1].x = px(p, x2);
    d.p[1].y = py(p, y2);
    d.p[2].x = px(p, x3);
    d.p[2].y = py(p, y3);
    lv_draw_triangle(p->layer, &d);
}

/* ------------------------------------------------------------------ icons */

/* A terminal window with a ">_" prompt. */
void devos_icon_terminal(lv_layer_t *layer, const lv_area_t *area, lv_color_t color)
{
    pen_t p = pen(layer, area, color);
    frame(&p, 5, 25, 195, 175, 28, 16);
    line(&p, 50, 72, 85, 100, 20);
    line(&p, 85, 100, 50, 128, 20);
    line(&p, 102, 128, 150, 128, 20);
}

/* A page of text with its corner folded. */
void devos_icon_editor(lv_layer_t *layer, const lv_area_t *area, lv_color_t color)
{
    pen_t p = pen(layer, area, color);
    static const int16_t page[] = { 118, 10, 40, 10, 40, 190, 160, 190, 160, 52, 118, 10 };
    polyline(&p, page, 6, 16);
    static const int16_t fold[] = { 118, 10, 118, 52, 160, 52 };
    polyline(&p, fold, 3, 14);
    line(&p, 70, 92, 130, 92, 16);
    line(&p, 70, 124, 130, 124, 16);
    line(&p, 70, 156, 108, 156, 16);
}

/* Tailscale's 3 x 3 dots: the middle row and the bottom centre lit (a "T"). */
void devos_icon_tailscale(lv_layer_t *layer, const lv_area_t *area, lv_color_t color)
{
    pen_t p = pen(layer, area, color);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            bool lit = r == 1 || (r == 2 && c == 1);
            dot(&p, 33 + c * 67, 33 + r * 67, 27, lit ? LV_OPA_COVER : LV_OPA_30);
        }
    }
}

/* WireGuard: the red roundel with the white curled dragon, reduced to a
 * hooked stroke and an eye. Brand colours, whatever `color` is. */
void devos_icon_wireguard(lv_layer_t *layer, const lv_area_t *area, lv_color_t color)
{
    LV_UNUSED(color);
    pen_t p = pen(layer, area, lv_color_hex(0xC4262E));
    dot(&p, 100, 100, 100, LV_OPA_COVER);
    p.c = lv_color_white();
    arc(&p, 88, 112, 60, 180, 60, 26);         /* left, over the top, round to the lower right */
    line(&p, 40, 112, 58, 152, 22);             /* the tail curling back in */
    dot(&p, 136, 52, 16, LV_OPA_COVER);         /* the eye */
}

/* MQTT: a broadcast from the corner. */
void devos_icon_mqtt(lv_layer_t *layer, const lv_area_t *area, lv_color_t color)
{
    pen_t p = pen(layer, area, color);
    dot(&p, 32, 168, 22, LV_OPA_COVER);
    arc(&p, 32, 168, 84, 270, 360, 22);
    arc(&p, 32, 168, 128, 270, 360, 22);
    arc(&p, 32, 168, 172, 270, 360, 22);
}

/* Network tools: three linked hosts. */
void devos_icon_network(lv_layer_t *layer, const lv_area_t *area, lv_color_t color)
{
    pen_t p = pen(layer, area, color);
    line(&p, 100, 42, 36, 158, 14);
    line(&p, 100, 42, 164, 158, 14);
    line(&p, 36, 158, 164, 158, 14);
    dot(&p, 100, 42, 30, LV_OPA_COVER);
    dot(&p, 36, 158, 30, LV_OPA_COVER);
    dot(&p, 164, 158, 30, LV_OPA_COVER);
}

/* REST: a pair of JSON braces. */
void devos_icon_rest(lv_layer_t *layer, const lv_area_t *area, lv_color_t color)
{
    pen_t p = pen(layer, area, color);
    static const int16_t l[] = { 78, 18, 58, 32, 58, 82, 34, 100, 58, 118, 58, 168, 78, 182 };
    static const int16_t r[] = { 122, 18, 142, 32, 142, 82, 166, 100, 142, 118, 142, 168, 122, 182 };
    polyline(&p, l, 7, 18);
    polyline(&p, r, 7, 18);
    dot(&p, 100, 100, 13, LV_OPA_COVER);
}

/* Docker: containers stacked on the whale's back, its tail up behind. */
void devos_icon_docker(lv_layer_t *layer, const lv_area_t *area, lv_color_t color)
{
    pen_t p = pen(layer, area, color);
    box(&p, 76, 34, 108, 64, 4, LV_OPA_COVER);          /* top row: one */
    for (int i = 0; i < 3; i++) {                       /* second row: three */
        int32_t x = 40 + i * 36;
        box(&p, x, 70, x + 32, 100, 4, LV_OPA_COVER);
    }
    box(&p, 6, 106, 172, 160, 26, LV_OPA_COVER);        /* the body */
    line(&p, 160, 116, 190, 86, 18);                    /* tail */
    line(&p, 184, 92, 196, 104, 14);
}

/* ADS-B: an airliner seen from above. */
void devos_icon_adsb(lv_layer_t *layer, const lv_area_t *area, lv_color_t color)
{
    pen_t p = pen(layer, area, color);
    line(&p, 100, 16, 100, 178, 26);                    /* fuselage */
    tri(&p, 100, 66, 8, 124, 100, 110);                 /* wings, swept back */
    tri(&p, 100, 66, 192, 124, 100, 110);
    tri(&p, 100, 146, 56, 186, 100, 174);               /* tailplane */
    tri(&p, 100, 146, 144, 186, 100, 174);
}

/* Authenticator: a padlock. */
void devos_icon_totp(lv_layer_t *layer, const lv_area_t *area, lv_color_t color)
{
    pen_t p = pen(layer, area, color);
    arc(&p, 100, 82, 52, 180, 360, 20);                 /* shackle */
    line(&p, 58, 82, 58, 96, 20);
    line(&p, 142, 82, 142, 96, 20);
    frame(&p, 30, 92, 170, 190, 22, 18);                /* body */
    dot(&p, 100, 130, 15, LV_OPA_COVER);                /* keyhole */
    line(&p, 100, 132, 100, 160, 13);
}

/* Cricket: a bat and a ball. */
void devos_icon_cricket(lv_layer_t *layer, const lv_area_t *area, lv_color_t color)
{
    pen_t p = pen(layer, area, color);
    line(&p, 150, 22, 124, 48, 16);                     /* handle */
    line(&p, 118, 54, 44, 128, 44);                     /* blade */
    line(&p, 44, 128, 28, 144, 36);                     /* toe */
    dot(&p, 150, 150, 26, LV_OPA_COVER);                /* ball */
}

/* ------------------------------------------------------------------ widget */
static void icon_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    const devos_app_descriptor_t *app = lv_obj_get_user_data(o);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    lv_color_t c = lv_obj_get_style_text_color(o, LV_PART_MAIN);
    if (app && app->draw_icon) {
        app->draw_icon(layer, &a, c);
        return;
    }
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = app && app->icon ? app->icon : LV_SYMBOL_FILE;
    d.color = c;
    d.font = lv_obj_get_style_text_font(o, LV_PART_MAIN);
    d.align = LV_TEXT_ALIGN_CENTER;
    int32_t lh = lv_font_get_line_height(d.font);
    a.y1 += (lv_area_get_height(&a) - lh) / 2;       /* centred like the drawn icons */
    a.y2 = a.y1 + lh - 1;
    lv_draw_label(layer, &d, &a);
}

lv_obj_t *devos_icon_create(lv_obj_t *parent, int32_t size)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, icon_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    return o;
}

void devos_icon_set_app(lv_obj_t *icon, const devos_app_descriptor_t *app)
{
    if (!icon || lv_obj_get_user_data(icon) == app) return;
    lv_obj_set_user_data(icon, (void *)app);
    lv_obj_invalidate(icon);
}
