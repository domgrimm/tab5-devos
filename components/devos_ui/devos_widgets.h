#pragma once

/* devos_widgets: the building blocks of an app screen - buttons, text fields,
 * dropdowns, panels, dialogs, the key hint footer and a virtual list - created
 * already styled and restyled automatically when the theme changes
 * (invariant 5), so an app needs no apply_theme() of its own for them.
 *
 * Anything else can join the theme with devos_w_track(obj, kind). Tracked
 * objects are forgotten when deleted.
 *
 * Keyboard (invariant 9): register the controls with devos_focus as usual;
 * devos_vlist_key() gives lists the standard Up / Down / PgUp / PgDn keys.
 */

#include "lvgl.h"
#include "devos_core.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEVOS_W_SCREEN = 0,     /* app background */
    DEVOS_W_BAR,            /* app top bar: bottom border */
    DEVOS_W_PANEL,          /* surface + border */
    DEVOS_W_PANEL_ALT,      /* bg_alt + border */
    DEVOS_W_CODE,           /* code background + border (output areas) */
    DEVOS_W_MODAL,          /* dialog box: surface + accent border */
    DEVOS_W_BTN,
    DEVOS_W_BTN_PRIMARY,    /* accent filled (the default action) */
    DEVOS_W_BTN_DANGER,     /* danger filled (delete, stop) */
    DEVOS_W_TA,             /* text field */
    DEVOS_W_DD,             /* dropdown (and its list) */
    DEVOS_W_CB,             /* checkbox */
    DEVOS_W_TEXT,           /* label colours */
    DEVOS_W_TEXT_DIM,
    DEVOS_W_TEXT_MUTED,
    DEVOS_W_TEXT_ACCENT,
    DEVOS_W_TEXT_OK,
    DEVOS_W_TEXT_WARN,
    DEVOS_W_TEXT_ERR,
    DEVOS_W_TEXT_ON_ACCENT, /* text on an accent fill */
    DEVOS_W_BADGE,          /* accent pill with bg-coloured text */
    DEVOS_W_PROGRESS,       /* lv_bar / lv_slider: accent indicator on a dim track */
    DEVOS_W_KIND_COUNT
} devos_w_kind_t;

/* Style obj as `kind` now and on every theme change. Calling it again with
 * another kind retints it (e.g. a status label going from OK to error). */
void devos_w_track(lv_obj_t *obj, devos_w_kind_t kind);

/* Set a label's text only if it changed (avoids needless redraws). */
void devos_w_set_text(lv_obj_t *label, const char *text);

/* The app's screen: the content area below the system top bar, hidden until
 * the app is shown. Stores it in d->screen. */
lv_obj_t *devos_w_screen(devos_app_descriptor_t *d);

/* A 40 px bar across the top of an app screen with its title on the left
 * (returned in *title if non-NULL). Put status text and buttons on it. */
#define DEVOS_W_BAR_H 40
lv_obj_t *devos_w_bar(lv_obj_t *screen, const char *title, lv_obj_t **title_out);

lv_obj_t *devos_w_panel(lv_obj_t *parent, int x, int y, int w, int h, devos_w_kind_t kind);
lv_obj_t *devos_w_label(lv_obj_t *parent, const lv_font_t *font, devos_w_kind_t kind, const char *text);

/* 28 px high button (resize freely). cb gets `ud` as event user data. */
lv_obj_t *devos_w_btn(lv_obj_t *parent, const char *text, int w, lv_event_cb_t cb, void *ud,
                      lv_obj_t **label_out);
/* Same, filled with the accent (primary) or danger colour. */
lv_obj_t *devos_w_btn_kind(lv_obj_t *parent, devos_w_kind_t kind, const char *text, int w,
                           lv_event_cb_t cb, void *ud, lv_obj_t **label_out);

/* Text field; one-line fields use the UI font, multi-line the mono font. */
lv_obj_t *devos_w_ta(lv_obj_t *parent, bool one_line, int w, int h);
/* A caption at (x, y) with a 36 px one-line field under it. */
lv_obj_t *devos_w_field(lv_obj_t *parent, const char *caption, int x, int y, int w);
lv_obj_t *devos_w_dd(lv_obj_t *parent, const char *options, int w);
lv_obj_t *devos_w_cb(lv_obj_t *parent, const char *text);

/* Key hint footer along the bottom of the screen (22 px). */
#define DEVOS_W_KEYS_H 22
lv_obj_t *devos_w_keys(lv_obj_t *screen);

/* ---- dialog: dimmed overlay + centred box ---- */
typedef struct {
    lv_obj_t *overlay, *box, *title, *msg;
} devos_w_dialog_t;

/* Box of w x h near the top of the screen with `title`; add fields to d->box.
 * d->msg is an empty error / info line at the bottom left of the box. */
void devos_w_dialog(devos_w_dialog_t *d, lv_obj_t *screen, int w, int h, const char *title);
void devos_w_dialog_show(devos_w_dialog_t *d, bool show);
bool devos_w_dialog_open(const devos_w_dialog_t *d);

/* ---- virtual list: draws only the visible rows, fast with thousands ---- */
struct devos_vlist;
typedef void (*devos_vlist_draw_fn)(struct devos_vlist *v, lv_layer_t *layer, const lv_area_t *row, int idx);
typedef void (*devos_vlist_select_fn)(struct devos_vlist *v, int idx);

typedef struct devos_vlist {
    lv_obj_t *scroll, *view;
    int row_h, count, sel;
    bool active;            /* has the keyboard: selection drawn in the accent */
    devos_vlist_draw_fn draw;
    devos_vlist_select_fn on_select;   /* selection changed (key or tap); may be NULL */
    devos_vlist_select_fn on_activate; /* Enter or tap on the selected row; may be NULL */
    void *user;
} devos_vlist_t;

void devos_vlist_create(devos_vlist_t *v, lv_obj_t *parent, int row_h, devos_vlist_draw_fn draw);
void devos_vlist_set_count(devos_vlist_t *v, int n);   /* also redraws */
void devos_vlist_select(devos_vlist_t *v, int idx);    /* scrolls it into view, calls on_select */
/* Up / Down / PgUp / PgDn / Enter; true if used. */
bool devos_vlist_key(devos_vlist_t *v, uint32_t key);
void devos_vlist_redraw(devos_vlist_t *v);

/* Draw text in a list row (mono font unless `font` is given), clipped to w px
 * (w <= 0: no limit). Returns the x after the text. */
int devos_w_draw_text(lv_layer_t *layer, const lv_font_t *font, int x, int y, int w, const char *text,
                      lv_color_t color);
void devos_w_draw_rect(lv_layer_t *layer, int x1, int y1, int x2, int y2, lv_color_t color, lv_opa_t opa, int radius);

/* Mono font used for lists and output (8 px per character). */
const lv_font_t *devos_w_mono(void);
#define DEVOS_W_MONO_W 8

#ifdef __cplusplus
}
#endif
