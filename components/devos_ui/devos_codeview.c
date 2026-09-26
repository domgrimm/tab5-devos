/* devos_codeview: see devos_codeview.h. */
#include "devos_codeview.h"
#include "devos_theme.h"
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_nimbus_mono_14);

#define CV_LINE_H    DEVOS_CODEVIEW_LINE_H
#define CV_MAX_LINES 6000

static void cv_draw_cb(lv_event_t *e)
{
    devos_codeview_t *cv = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!cv->text || cv->n == 0) return;
    const devos_palette_t *p = devos_theme_get();
    lv_area_t a;
    lv_obj_get_coords(cv->view, &a);
    const lv_area_t *clip = &layer->_clip_area;
    int r0 = (clip->y1 - a.y1) / CV_LINE_H, r1 = (clip->y2 - a.y1) / CV_LINE_H;
    if (r0 < 0) r0 = 0;
    if (r1 >= cv->n) r1 = cv->n - 1;
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &lv_font_nimbus_mono_14;
    ld.flag = LV_TEXT_FLAG_EXPAND;
    ld.text_local = 1;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_opa = LV_OPA_20;
    char line[300];
    for (int r = r0; r <= r1; r++) {
        const char *s = cv->text + cv->off[r];
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        if (len > sizeof(line) - 1) len = sizeof(line) - 1;
        for (size_t i = 0; i < len; i++) line[i] = (s[i] == '\t' || (unsigned char)s[i] < 32) ? ' ' : s[i];
        line[len] = '\0';
        lv_color_t col = p->text_primary;
        bool band = false;
        if (cv->plain) { /* ordinary text: no diff colours */ }
        else if (line[0] == '+' && line[1] != '+') { col = p->accent_secondary; band = true; }
        else if (line[0] == '-' && line[1] != '-') { col = p->accent_danger; band = true; }
        else if (line[0] == '@' && line[1] == '@') col = p->accent_primary;
        else if (!strncmp(line, "diff ", 5) || !strncmp(line, "---", 3) || !strncmp(line, "+++", 3) ||
                 !strncmp(line, "index ", 6)) col = p->text_secondary;
        int y = a.y1 + r * CV_LINE_H;
        if (band) {
            rd.bg_color = col;
            lv_area_t br = { a.x1, y, a.x2, y + CV_LINE_H - 1 };
            lv_draw_rect(layer, &rd, &br);
        }
        ld.color = col;
        ld.text = line;
        lv_area_t tr = { a.x1 + 4, y, a.x1 + 12 + (int32_t)len * 8, y + CV_LINE_H - 1 };
        lv_draw_label(layer, &ld, &tr);
    }
}

void devos_codeview_set(devos_codeview_t *cv, const char *text)
{
    if (!cv->view) return;
    if (!cv->off) cv->off = malloc(sizeof(int) * CV_MAX_LINES);
    cv->text = text;
    cv->n = 0;
    int longest = 0;
    if (text && *text && cv->off) {
        cv->off[cv->n++] = 0;
        const char *line = text;
        for (const char *q = text; *q && cv->n < CV_MAX_LINES; q++) {
            if (*q == '\n') {
                if ((int)(q - line) > longest) longest = (int)(q - line);
                line = q + 1;
                if (q[1]) cv->off[cv->n++] = (int)(q + 1 - text);
            }
        }
        if ((int)strlen(line) > longest) longest = (int)strlen(line);
    }
    if (longest > 299) longest = 299;
    int w = lv_obj_get_content_width(cv->scroll);
    int want = longest * 8 + 16;
    lv_obj_set_size(cv->view, want > w ? want : w, cv->n * CV_LINE_H + 8);
    lv_obj_scroll_to(cv->scroll, 0, 0, LV_ANIM_OFF);
    lv_obj_invalidate(cv->view);
}

void devos_codeview_create(devos_codeview_t *cv, lv_obj_t *scroll)
{
    cv->scroll = scroll;
    lv_obj_set_scroll_dir(scroll, LV_DIR_ALL);
    lv_obj_set_style_pad_all(scroll, 0, 0);
    cv->view = lv_obj_create(scroll);
    lv_obj_remove_style_all(cv->view);
    lv_obj_add_flag(cv->view, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(cv->view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(cv->view, 10, 10);
    lv_obj_add_event_cb(cv->view, cv_draw_cb, LV_EVENT_DRAW_MAIN, cv);
}
