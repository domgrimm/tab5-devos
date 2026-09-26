#pragma once
/* devos_codeview: monospace text/diff viewer that draws only the visible
 * lines, so 6000-line diffs scroll smoothly. Diff lines are coloured
 * (+ green band, - red band, @@ accent, headers dimmed). */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_CODEVIEW_LINE_H 16   /* px per line, for keyboard scrolling */

typedef struct {
    lv_obj_t *scroll;       /* scrollable container (caller's) */
    lv_obj_t *view;         /* tall child drawn line by line */
    const char *text;
    int *off;               /* line start offsets */
    int n;
    bool plain;             /* true: plain text (no diff colours) */
} devos_codeview_t;

/* Turn `scroll` (an empty lv_obj) into a code view. */
void devos_codeview_create(devos_codeview_t *cv, lv_obj_t *scroll);
/* Show `text` (kept by pointer: it must stay valid) and scroll to the top.
 * Set cv->plain first for ordinary text files. */
void devos_codeview_set(devos_codeview_t *cv, const char *text);

#ifdef __cplusplus
}
#endif
