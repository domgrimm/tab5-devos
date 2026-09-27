/* devos_qrscan: see devos_qrscan.h. */
#include "devos_qrscan.h"
#include "devos_widgets.h"
#include "devos_qr.h"
#include "devos_config.h"

#include <stdlib.h>
#include <string.h>

static void status(devos_qrscan_t *s, const char *msg, bool err)
{
    devos_w_set_text(s->status, msg);
    devos_w_track(s->status, err ? DEVOS_W_TEXT_ERR : DEVOS_W_TEXT_DIM);
}

static void start(devos_qrscan_t *s)
{
    s->gen = 0;
    if (devos_qr_start() == 0) status(s, s->hint, false);
    else status(s, devos_qr_error()[0] ? devos_qr_error() : "The camera didn't start", true);
}

static void tick_cb(lv_timer_t *t)
{
    devos_qrscan_t *s = lv_timer_get_user_data(t);
    if (!s->open) return;
    uint32_t g = devos_qr_preview(s->buf);
    if (g && g != s->gen) {
        s->gen = g;
        lv_image_cache_drop(&s->dsc);
        lv_obj_invalidate(s->img);
    }
    devos_qr_state_t st = devos_qr_state();
    if (st == DEVOS_QR_FOUND) {
        static char text[DEVOS_QR_TEXT_MAX];
        if (devos_qr_take_result(text, sizeof(text))) {
            char msg[160] = "";
            bool done = s->cb ? s->cb(text, msg, sizeof(msg)) : true;
            memset(text, 0, sizeof(text));              /* may hold secrets */
            if (done) {
                devos_qrscan_close(s);
                return;
            }
            status(s, msg[0] ? msg : "Not the code we're after - try another", true);
            s->retry_at = lv_tick_get() + 1500;
        }
    } else if (st == DEVOS_QR_ERROR) {
        status(s, devos_qr_error(), true);
    }
    if (s->retry_at && (int32_t)(lv_tick_get() - s->retry_at) >= 0) {
        s->retry_at = 0;
        start(s);
    }
}

void devos_qrscan_create(devos_qrscan_t *s, lv_obj_t *screen, const char *title, const char *hint, devos_qrscan_fn cb)
{
    memset(s, 0, sizeof(*s));
    s->cb = cb;
    s->hint = hint;
    s->buf = malloc(DEVOS_QR_PREVIEW * DEVOS_QR_PREVIEW);
    if (s->buf) memset(s->buf, 0x20, DEVOS_QR_PREVIEW * DEVOS_QR_PREVIEW);
    s->overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(s->overlay);
    lv_obj_set_size(s->overlay, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_style_bg_color(s->overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s->overlay, LV_OPA_60, 0);
    lv_obj_add_flag(s->overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s->overlay, LV_OBJ_FLAG_HIDDEN);
    s->panel = lv_obj_create(s->overlay);
    lv_obj_set_size(s->panel, 640, 560);
    lv_obj_center(s->panel);
    lv_obj_set_style_radius(s->panel, 8, 0);
    lv_obj_set_style_border_width(s->panel, 2, 0);
    lv_obj_set_style_pad_all(s->panel, 16, 0);
    lv_obj_remove_flag(s->panel, LV_OBJ_FLAG_SCROLLABLE);
    devos_w_track(s->panel, DEVOS_W_MODAL);
    s->title = devos_w_label(s->panel, &lv_font_montserrat_16, DEVOS_W_TEXT_ACCENT, title);
    s->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s->dsc.header.cf = LV_COLOR_FORMAT_L8;
    s->dsc.header.w = DEVOS_QR_PREVIEW;
    s->dsc.header.h = DEVOS_QR_PREVIEW;
    s->dsc.header.stride = DEVOS_QR_PREVIEW;
    s->dsc.data_size = DEVOS_QR_PREVIEW * DEVOS_QR_PREVIEW;
    s->dsc.data = s->buf;
    s->img = lv_image_create(s->panel);
    if (s->buf) lv_image_set_src(s->img, &s->dsc);
    lv_obj_align(s->img, LV_ALIGN_TOP_MID, 0, 34);
    s->status = devos_w_label(s->panel, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM, "");
    lv_obj_set_width(s->status, 600);
    lv_label_set_long_mode(s->status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s->status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s->status, LV_ALIGN_TOP_MID, 0, 404);
    s->keys = devos_w_label(s->panel, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Esc  cancel");
    lv_obj_align(s->keys, LV_ALIGN_BOTTOM_MID, 0, 0);
    s->timer = lv_timer_create(tick_cb, 100, s);
}

void devos_qrscan_open(devos_qrscan_t *s)
{
    s->open = true;
    s->retry_at = 0;
    if (s->buf) memset(s->buf, 0x20, DEVOS_QR_PREVIEW * DEVOS_QR_PREVIEW);
    lv_image_cache_drop(&s->dsc);
    lv_obj_invalidate(s->img);
    lv_obj_remove_flag(s->overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s->overlay);
    start(s);
}

void devos_qrscan_close(devos_qrscan_t *s)
{
    if (!s->open) return;
    s->open = false;
    s->retry_at = 0;
    devos_qr_stop();
    lv_obj_add_flag(s->overlay, LV_OBJ_FLAG_HIDDEN);
}

bool devos_qrscan_is_open(const devos_qrscan_t *s) { return s->open; }

bool devos_qrscan_key(devos_qrscan_t *s, uint32_t key)
{
    if (!s->open) return false;
    if (key == LV_KEY_ESC) devos_qrscan_close(s);
    return true;
}
