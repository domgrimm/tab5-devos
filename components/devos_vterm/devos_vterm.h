#pragma once

/* devos_vterm: a small VT100 / xterm-compatible terminal emulator.
 *
 * Platform-neutral (no LVGL): it turns a byte stream from a PTY into a grid of
 * cells that a renderer draws. Covers what shells and full-screen TUIs (vim,
 * less, htop, tmux, nano) rely on: cursor addressing, scroll regions, insert/
 * delete line/char, erase, the alternate screen, SGR attributes with 16 / 256 /
 * truecolor (mapped to 256), DEC special graphics (line drawing), deferred
 * autowrap, tab stops, UTF-8, device-status/attribute replies, and a
 * scrollback ring for the primary screen.
 *
 * Host unit test: tools/vterm_test.c.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_VT_MAX_COLS 160          /* widest grid (1280 px / 8 px cells) */

#define VT_ATTR_BOLD       0x01
#define VT_ATTR_UNDERLINE  0x02
#define VT_ATTR_REVERSE    0x04
#define VT_ATTR_DIM        0x08
#define VT_ATTR_ITALIC     0x10
#define VT_ATTR_FG_DEFAULT 0x20        /* use the theme foreground, ignore fg */
#define VT_ATTR_BG_DEFAULT 0x40        /* use the theme background, ignore bg */

typedef struct {
    uint16_t ch;                       /* BMP code point; 0 or ' ' = blank */
    uint8_t  fg;                       /* 0-255 xterm palette index */
    uint8_t  bg;
    uint8_t  attr;                     /* VT_ATTR_* */
    uint8_t  _pad;
} devos_vt_cell_t;

/* Bytes the terminal must send back to the host (DA / DSR replies). */
typedef void (*devos_vterm_output_fn)(const char *data, size_t len, void *user);

typedef struct devos_vterm devos_vterm_t;

devos_vterm_t *devos_vterm_create(int cols, int rows, int scrollback_lines);
void devos_vterm_destroy(devos_vterm_t *vt);
void devos_vterm_reset(devos_vterm_t *vt);
/* Keeps the bottom of the screen (where the prompt is); overflow lines go to
 * scrollback. Clamped to DEVOS_VT_MAX_COLS. */
void devos_vterm_resize(devos_vterm_t *vt, int cols, int rows);
void devos_vterm_feed(devos_vterm_t *vt, const char *data, size_t len);
void devos_vterm_set_output_cb(devos_vterm_t *vt, devos_vterm_output_fn cb, void *user);

int  devos_vterm_cols(const devos_vterm_t *vt);
int  devos_vterm_rows(const devos_vterm_t *vt);
/* Row `row` of the view `view_offset` lines back into history (0 = live
 * screen). Always `cols` cells long. */
const devos_vt_cell_t *devos_vterm_view_row(const devos_vterm_t *vt, int row, int view_offset);
/* History lines available to scroll back into (0 while the alternate screen
 * is active, like other terminals). */
int  devos_vterm_scrollback_lines(const devos_vterm_t *vt);
/* Total lines ever pushed to history (lets a scrolled-back view stay put). */
uint32_t devos_vterm_scrolled_total(const devos_vterm_t *vt);
void devos_vterm_cursor(const devos_vterm_t *vt, int *col, int *row, bool *visible);
bool devos_vterm_app_cursor_keys(const devos_vterm_t *vt);
bool devos_vterm_alt_screen(const devos_vterm_t *vt);
const char *devos_vterm_title(const devos_vterm_t *vt);

/* Rows changed since the last call (rows_dirty[r] = 1). Returns true if any. */
bool devos_vterm_take_dirty(devos_vterm_t *vt, uint8_t *rows_dirty, int max_rows);

#ifdef __cplusplus
}
#endif
