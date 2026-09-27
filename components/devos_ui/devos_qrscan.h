#pragma once

/* devos_qrscan: the camera QR scanner as a dialog - live preview, status
 * line, Esc to cancel. The app decides whether a decoded code is useful:
 * its callback returns true to close, or false with a message to keep
 * scanning (e.g. "That's not an authenticator code").
 */

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*devos_qrscan_fn)(const char *text, char *msg, size_t msg_cap);

typedef struct {
    lv_obj_t *overlay, *panel, *title, *img, *status, *keys;
    lv_image_dsc_t dsc;
    uint8_t *buf;
    uint32_t gen, retry_at;
    devos_qrscan_fn cb;
    const char *hint;
    lv_timer_t *timer;
    bool open;
} devos_qrscan_t;

void devos_qrscan_create(devos_qrscan_t *s, lv_obj_t *screen, const char *title, const char *hint, devos_qrscan_fn cb);
void devos_qrscan_open(devos_qrscan_t *s);
void devos_qrscan_close(devos_qrscan_t *s);
bool devos_qrscan_is_open(const devos_qrscan_t *s);
/* While open it takes every key (Esc closes). */
bool devos_qrscan_key(devos_qrscan_t *s, uint32_t key);

#ifdef __cplusplus
}
#endif
