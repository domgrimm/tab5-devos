/* Host unit test for components/devos_vterm.
 *
 *   gcc -Wall -Wextra -o /tmp/vterm_test tools/vterm_test.c \
 *       components/devos_vterm/devos_vterm.c -Icomponents/devos_vterm && /tmp/vterm_test
 */
#include "devos_vterm.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char s_reply[64];
static void on_reply(const char *d, size_t n, void *u)
{
    (void)u;
    snprintf(s_reply, sizeof(s_reply), "%.*s", (int)n, d);
}

static void feed(devos_vterm_t *vt, const char *s) { devos_vterm_feed(vt, s, strlen(s)); }

/* Row text (trailing blanks trimmed), code points > 0x7F shown as '#'. */
static const char *row_text(devos_vterm_t *vt, int r, int off)
{
    static char buf[DEVOS_VT_MAX_COLS + 1];
    const devos_vt_cell_t *row = devos_vterm_view_row(vt, r, off);
    int n = devos_vterm_cols(vt);
    for (int c = 0; c < n; c++) {
        uint16_t ch = row[c].ch;
        buf[c] = (ch == 0) ? ' ' : (ch < 0x80 ? (char)ch : '#');
    }
    buf[n] = '\0';
    for (int c = n - 1; c >= 0 && buf[c] == ' '; c--) buf[c] = '\0';
    return buf;
}

int main(void)
{
    devos_vterm_t *vt = devos_vterm_create(20, 5, 50);
    CHECK(vt != NULL);
    devos_vterm_set_output_cb(vt, on_reply, NULL);
    int cx, cy;
    bool vis;

    /* text, CR/LF */
    feed(vt, "hello\r\nworld");
    CHECK(strcmp(row_text(vt, 0, 0), "hello") == 0);
    CHECK(strcmp(row_text(vt, 1, 0), "world") == 0);
    devos_vterm_cursor(vt, &cx, &cy, &vis);
    CHECK(cx == 5 && cy == 1 && vis);

    /* CUP + overwrite, EL */
    feed(vt, "\033[1;3HXY\033[K");
    CHECK(strcmp(row_text(vt, 0, 0), "heXY") == 0);

    /* SGR: red fg, bold; reset */
    feed(vt, "\033[2;1H\033[1;31mR\033[0mN");
    const devos_vt_cell_t *r1 = devos_vterm_view_row(vt, 1, 0);
    CHECK(r1[0].ch == 'R' && r1[0].fg == 1 && (r1[0].attr & VT_ATTR_BOLD) && !(r1[0].attr & VT_ATTR_FG_DEFAULT));
    CHECK(r1[1].ch == 'N' && (r1[1].attr & VT_ATTR_FG_DEFAULT) && !(r1[1].attr & VT_ATTR_BOLD));
    /* 256-colour and truecolour */
    feed(vt, "\033[38;5;202mA\033[48;2;255;255;255mB\033[0m");
    CHECK(r1[2].fg == 202);
    CHECK(r1[3].bg == 231 && !(r1[3].attr & VT_ATTR_BG_DEFAULT));

    /* ED 2 clears; deferred autowrap at the right margin */
    feed(vt, "\033[2J\033[H01234567890123456789");
    devos_vterm_cursor(vt, &cx, &cy, NULL);
    CHECK(cx == 19 && cy == 0);                  /* wrap pending, not yet moved */
    feed(vt, "Z");
    CHECK(strcmp(row_text(vt, 0, 0), "01234567890123456789") == 0);
    CHECK(strcmp(row_text(vt, 1, 0), "Z") == 0);

    /* scrolling into history */
    feed(vt, "\033[2J\033[Hl0\r\nl1\r\nl2\r\nl3\r\nl4\r\nl5\r\nl6");
    CHECK(strcmp(row_text(vt, 0, 0), "l2") == 0);
    CHECK(strcmp(row_text(vt, 4, 0), "l6") == 0);
    CHECK(devos_vterm_scrollback_lines(vt) >= 2);
    CHECK(strcmp(row_text(vt, 0, 2), "l0") == 0);  /* view 2 lines back */
    CHECK(strcmp(row_text(vt, 2, 2), "l2") == 0);

    /* alternate screen keeps the primary intact */
    feed(vt, "\033[?1049h\033[HALT");
    CHECK(devos_vterm_alt_screen(vt));
    CHECK(strcmp(row_text(vt, 0, 0), "ALT") == 0);
    CHECK(devos_vterm_scrollback_lines(vt) == 0);
    feed(vt, "\033[?1049l");
    CHECK(!devos_vterm_alt_screen(vt));
    CHECK(strcmp(row_text(vt, 4, 0), "l6") == 0);

    /* DSR cursor position report */
    s_reply[0] = '\0';
    feed(vt, "\033[3;4H\033[6n");
    CHECK(strcmp(s_reply, "\033[3;4R") == 0);
    feed(vt, "\033[c");
    CHECK(strcmp(s_reply, "\033[?1;2c") == 0);

    /* UTF-8 and DEC line drawing */
    feed(vt, "\033[2J\033[H\xC3\xA9\033(0q\033(Bq");
    const devos_vt_cell_t *r0 = devos_vterm_view_row(vt, 0, 0);
    CHECK(r0[0].ch == 0x00E9);
    CHECK(r0[1].ch == 0x2500);
    CHECK(r0[2].ch == 'q');

    /* scroll region + delete line stays inside the region */
    feed(vt, "\033[2J\033[Ha\r\nb\r\nc\r\nd\r\ne\033[2;4r\033[2;1H\033[M");
    CHECK(strcmp(row_text(vt, 0, 0), "a") == 0);
    CHECK(strcmp(row_text(vt, 1, 0), "c") == 0);
    CHECK(strcmp(row_text(vt, 2, 0), "d") == 0);
    CHECK(strcmp(row_text(vt, 3, 0), "") == 0);
    CHECK(strcmp(row_text(vt, 4, 0), "e") == 0);
    feed(vt, "\033[r");

    /* insert chars, erase chars */
    feed(vt, "\033[2J\033[Habcdef\033[1;2H\033[2@\033[1;7H\033[2X");
    CHECK(strcmp(row_text(vt, 0, 0), "a  bcd") == 0);

    /* resize keeps the cursor row visible */
    feed(vt, "\033[2J\033[Hr0\r\nr1\r\nr2\r\nr3\r\nr4");
    devos_vterm_resize(vt, 30, 3);
    CHECK(devos_vterm_rows(vt) == 3 && devos_vterm_cols(vt) == 30);
    CHECK(strcmp(row_text(vt, 2, 0), "r4") == 0);
    devos_vterm_cursor(vt, &cx, &cy, NULL);
    CHECK(cy == 2);

    /* dirty tracking */
    uint8_t d[8];
    devos_vterm_take_dirty(vt, d, 8);
    CHECK(!devos_vterm_take_dirty(vt, d, 8));
    feed(vt, "\033[1;1Hx");
    CHECK(devos_vterm_take_dirty(vt, d, 8) && d[0] == 1 && d[1] == 0);

    /* OSC title, cursor hide */
    feed(vt, "\033]0;my title\007\033[?25l");
    CHECK(strcmp(devos_vterm_title(vt), "my title") == 0);
    devos_vterm_cursor(vt, NULL, NULL, &vis);
    CHECK(!vis);

    devos_vterm_destroy(vt);
    if (failures == 0) printf("vterm tests: ALL PASS\n");
    return failures ? 1 : 0;
}
