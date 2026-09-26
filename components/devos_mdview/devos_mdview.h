#pragma once

/* devos_mdview: shared CommonMark-subset renderer for LVGL.
 *
 * Used by the editor preview and any future rich-text surfaces. Single-threaded LVGL use only (shared buffers).
 */

#include "lvgl.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Render `text` into parent, stacking blocks from y=0. Returns the end y
 * (for sizing fixed-height cards). Does NOT clear parent. */
int devos_md_render(lv_obj_t *parent, const char *text);

/* UTF-8-safe truncation used by file I/O paths. */
size_t devos_md_trunc_ok(const char *s, size_t n);

#ifdef __cplusplus
}
#endif
