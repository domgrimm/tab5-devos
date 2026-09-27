/* devos_focus: see devos_focus.h. */
#include "devos_focus.h"
#include "devos_theme.h"
#include "devos_config.h"
#include "devos_core.h"

static lv_style_t s_ring, s_ring_out;
static bool s_ring_ready;

static void ring_theme_cb(const devos_palette_t *p, void *ud)
{
    (void)ud;
    lv_style_set_outline_color(&s_ring, p->accent_primary);
    lv_style_set_border_color(&s_ring, p->accent_primary);
    lv_style_set_outline_color(&s_ring_out, p->accent_primary);
    lv_obj_report_style_change(&s_ring);
    lv_obj_report_style_change(&s_ring_out);
}

static void ring_init(void)
{
    if (s_ring_ready) return;
    s_ring_ready = true;
    lv_style_init(&s_ring);
    lv_style_set_outline_width(&s_ring, 2);
    lv_style_set_outline_pad(&s_ring, -2);      /* on the control's own edge: never clipped */
    lv_style_set_outline_opa(&s_ring, LV_OPA_COVER);
    lv_style_set_outline_color(&s_ring, devos_theme_get()->accent_primary);
    /* controls with a border (text fields) draw it over the inset ring: tint it too */
    lv_style_set_border_color(&s_ring, devos_theme_get()->accent_primary);
    /* switches and sliders fill with the accent, which paints over an inset
     * ring (and matches its colour): theirs sits just outside, clear of the
     * fill. They always have room for it inside their card or row. */
    lv_style_init(&s_ring_out);
    lv_style_set_outline_width(&s_ring_out, 2);
    lv_style_set_outline_pad(&s_ring_out, 2);
    lv_style_set_outline_opa(&s_ring_out, LV_OPA_COVER);
    lv_style_set_outline_color(&s_ring_out, devos_theme_get()->accent_primary);
    devos_theme_add_listener(ring_theme_cb, NULL);
}

static bool ring_outside(lv_obj_t *o)
{
    return lv_obj_check_type(o, &lv_switch_class) || lv_obj_check_type(o, &lv_slider_class);
}

static bool usable(lv_obj_t *o)
{
    if (!o || lv_obj_has_state(o, LV_STATE_DISABLED)) return false;
    for (lv_obj_t *p = o; p; p = lv_obj_get_parent(p)) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_HIDDEN)) return false;
    }
    return true;
}

static void mark(devos_focus_t *f, int idx, bool on)
{
    if (idx < 0 || idx >= f->n) return;
    lv_obj_t *o = f->items[idx];
    if (on && f->ring) lv_obj_add_state(o, LV_STATE_FOCUS_KEY);
    else lv_obj_remove_state(o, LV_STATE_FOCUS_KEY);
    /* text fields show their cursor while focused */
    if (lv_obj_check_type(o, &lv_textarea_class)) {
        if (on) lv_obj_add_state(o, LV_STATE_FOCUSED);
        else lv_obj_remove_state(o, LV_STATE_FOCUSED);
    }
}

static void close_dd(devos_focus_t *f, bool keep)
{
    if (!f->dd_open) return;
    f->dd_open = false;
    lv_obj_t *dd = devos_focus_get(f);
    if (!dd || !lv_obj_check_type(dd, &lv_dropdown_class)) return;
    uint32_t sel = lv_dropdown_get_selected(dd);
    if (!keep && sel != f->dd_orig) {
        lv_dropdown_set_selected(dd, f->dd_orig);
        sel = f->dd_orig;
    }
    lv_dropdown_close(dd);
    if (keep && sel != f->dd_orig) lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, NULL);
}

static void focus_idx(devos_focus_t *f, int idx)
{
    close_dd(f, false);
    mark(f, f->cur, false);
    f->cur = idx;
    mark(f, idx, true);
    if (idx >= 0 && idx < f->n) lv_obj_scroll_to_view_recursive(f->items[idx], LV_ANIM_OFF);
}

static void pressed_cb(lv_event_t *e)
{
    devos_focus_t *f = lv_event_get_user_data(e);
    lv_obj_t *o = lv_event_get_current_target(e);
    f->ring = false;                            /* touch: no ring */
    for (int i = 0; i < f->n; i++) {
        if (f->items[i] == o) {
            if (i != f->cur) focus_idx(f, i);
            else mark(f, i, true);
            return;
        }
    }
}

void devos_focus_init(devos_focus_t *f)
{
    ring_init();
    f->n = 0;
    f->cur = -1;
    f->ring = false;
    f->dd_open = false;
}

void devos_focus_add(devos_focus_t *f, lv_obj_t *obj)
{
    if (!obj || f->n >= DEVOS_FOCUS_MAX) return;
    f->items[f->n++] = obj;
    lv_obj_add_style(obj, ring_outside(obj) ? &s_ring_out : &s_ring, LV_STATE_FOCUS_KEY);
    lv_obj_add_event_cb(obj, pressed_cb, LV_EVENT_PRESSED, f);
}

lv_obj_t *devos_focus_get(const devos_focus_t *f)
{
    return (f->cur >= 0 && f->cur < f->n) ? f->items[f->cur] : NULL;
}

void devos_focus_set(devos_focus_t *f, lv_obj_t *obj)
{
    f->ring = true;
    for (int i = 0; i < f->n; i++) {
        if (f->items[i] == obj) {
            focus_idx(f, i);
            return;
        }
    }
    focus_idx(f, -1);
}

void devos_focus_move(devos_focus_t *f, int dir)
{
    f->ring = true;
    if (!f->n) return;
    int start = f->cur < 0 ? (dir > 0 ? -1 : f->n) : f->cur;
    for (int k = 1; k <= f->n; k++) {
        int i = ((start + dir * k) % f->n + f->n) % f->n;
        if (usable(f->items[i])) {
            focus_idx(f, i);
            return;
        }
    }
}

void devos_focus_first(devos_focus_t *f)
{
    if (f->cur >= 0 && usable(devos_focus_get(f))) {
        f->ring = true;
        mark(f, f->cur, true);
        return;
    }
    f->cur = -1;
    devos_focus_move(f, 1);
}

void devos_focus_clear(devos_focus_t *f)
{
    close_dd(f, false);
    mark(f, f->cur, false);
    f->cur = -1;
}

static void press(lv_obj_t *o)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_CHECKABLE)) {
        if (lv_obj_has_state(o, LV_STATE_CHECKED)) lv_obj_remove_state(o, LV_STATE_CHECKED);
        else lv_obj_add_state(o, LV_STATE_CHECKED);
        lv_obj_send_event(o, LV_EVENT_VALUE_CHANGED, NULL);
    }
    lv_obj_send_event(o, LV_EVENT_CLICKED, NULL);
}

static bool text_key(lv_obj_t *ta, uint32_t key)
{
    bool multi = !lv_textarea_get_one_line(ta);
    if (key == LV_KEY_DEL) lv_textarea_delete_char_forward(ta);     /* 127: Del, not Backspace */
    else if (key == '\b') lv_textarea_delete_char(ta);
    else if (key == LV_KEY_LEFT) lv_textarea_cursor_left(ta);
    else if (key == LV_KEY_RIGHT) lv_textarea_cursor_right(ta);
    else if (multi && key == LV_KEY_UP) lv_textarea_cursor_up(ta);
    else if (multi && key == LV_KEY_DOWN) lv_textarea_cursor_down(ta);
    else if (multi && (key == '\r' || key == '\n')) lv_textarea_add_char(ta, '\n');
    else if (key >= 32 && key <= 126) lv_textarea_add_char(ta, (char)key);
    else return false;
    return true;
}

bool devos_focus_key(devos_focus_t *f, uint32_t key, uint8_t mods)
{
    if (mods & (DEVOS_MOD_CTRL | DEVOS_MOD_FN | DEVOS_MOD_ALT)) return false;
    lv_obj_t *o = devos_focus_get(f);
    if (o && !usable(o)) {                      /* hidden / disabled since: drop it */
        mark(f, f->cur, false);
        f->dd_open = false;
        o = NULL;
        f->cur = -1;
    }
    /* a dropdown list may have been opened or closed by touch */
    if (o && lv_obj_check_type(o, &lv_dropdown_class)) {
        bool open = lv_dropdown_is_open(o);
        if (open && !f->dd_open) f->dd_orig = lv_dropdown_get_selected(o);
        f->dd_open = open;
    }

    /* an open dropdown list owns the keys */
    if (f->dd_open && o && lv_obj_check_type(o, &lv_dropdown_class)) {
        uint32_t sel = lv_dropdown_get_selected(o), n = lv_dropdown_get_option_count(o);
        if (key == LV_KEY_UP && sel > 0) lv_dropdown_set_selected(o, sel - 1);
        else if (key == LV_KEY_DOWN && sel + 1 < n) lv_dropdown_set_selected(o, sel + 1);
        else if (key == '\r' || key == '\n' || key == ' ') close_dd(f, true);
        else if (key == LV_KEY_ESC) close_dd(f, false);
        return true;
    }

    if (key == '\t') {
        devos_focus_move(f, (mods & DEVOS_MOD_SHIFT) ? -1 : 1);
        return true;
    }
    if (!o) {
        if (key == LV_KEY_DOWN || key == LV_KEY_UP) {
            devos_focus_move(f, key == LV_KEY_DOWN ? 1 : -1);
            return true;
        }
        return false;
    }
    f->ring = true;
    mark(f, f->cur, true);

    if (lv_obj_check_type(o, &lv_textarea_class)) {
        bool multi = !lv_textarea_get_one_line(o);
        if (!multi && (key == LV_KEY_UP || key == LV_KEY_DOWN)) {
            devos_focus_move(f, key == LV_KEY_DOWN ? 1 : -1);
            return true;
        }
        if (!multi && (key == '\r' || key == '\n')) return false;   /* app: submit */
        return text_key(o, key);
    }
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        devos_focus_move(f, key == LV_KEY_DOWN ? 1 : -1);
        return true;
    }
    if (lv_obj_check_type(o, &lv_slider_class)) {
        if (key != LV_KEY_LEFT && key != LV_KEY_RIGHT) goto activate;
        int32_t lo = lv_slider_get_min_value(o), hi = lv_slider_get_max_value(o);
        int32_t step = (hi - lo) / 20 > 0 ? (hi - lo) / 20 : 1;
        int32_t v = lv_slider_get_value(o) + (key == LV_KEY_RIGHT ? step : -step);
        lv_slider_set_value(o, v < lo ? lo : v > hi ? hi : v, LV_ANIM_OFF);
        lv_obj_send_event(o, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_send_event(o, LV_EVENT_RELEASED, NULL);   /* "done" for apps that save on release */
        return true;
    }
    if (lv_obj_check_type(o, &lv_dropdown_class)) {
        uint32_t sel = lv_dropdown_get_selected(o), n = lv_dropdown_get_option_count(o);
        if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
            uint32_t next = key == LV_KEY_RIGHT ? (sel + 1 < n ? sel + 1 : sel) : (sel > 0 ? sel - 1 : 0);
            if (next != sel) {
                lv_dropdown_set_selected(o, next);
                lv_obj_send_event(o, LV_EVENT_VALUE_CHANGED, NULL);
            }
            return true;
        }
        if (key == '\r' || key == '\n' || key == ' ') {
            f->dd_orig = sel;
            f->dd_open = true;
            lv_dropdown_open(o);
            return true;
        }
        return false;
    }
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_CHECKABLE) && (key == LV_KEY_LEFT || key == LV_KEY_RIGHT)) {
        bool want = key == LV_KEY_RIGHT;
        if (want != lv_obj_has_state(o, LV_STATE_CHECKED)) press(o);
        return true;
    }
activate:
    /* Space toggles a checkbox / switch; Enter there is left to the app (submit) */
    if (key == ' ' || ((key == '\r' || key == '\n') && !lv_obj_has_flag(o, LV_OBJ_FLAG_CHECKABLE))) {
        press(o);
        return true;
    }
    if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {     /* buttons: walk the row */
        devos_focus_move(f, key == LV_KEY_RIGHT ? 1 : -1);
        return true;
    }
    return false;
}
