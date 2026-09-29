/* devos_vterm: see devos_vterm.h. */
#include "devos_vterm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
/* Grids and scrollback are large and only touched by the UI: keep them in PSRAM. */
static void *vt_alloc(size_t n) { return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
#else
static void *vt_alloc(size_t n) { return calloc(1, n); }
#endif

enum {
    ST_GROUND = 0, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC, ST_CHARSET_G0, ST_CHARSET_G1, ST_ESC_HASH,
    ST_DCS, ST_DCS_ESC,
};

#define VT_MAX_PARAMS 16

struct devos_vterm {
    int cols, rows;
    devos_vt_cell_t *primary;           /* rows * DEVOS_VT_MAX_COLS */
    devos_vt_cell_t *alternate;
    devos_vt_cell_t *screen;            /* primary or alternate */
    bool alt_active;

    devos_vt_cell_t *sb;                /* scrollback ring: sb_cap * DEVOS_VT_MAX_COLS */
    int sb_cap, sb_count, sb_head;      /* head = next slot to write */
    uint32_t sb_total;

    int cx, cy;
    bool wrap_pending;
    devos_vt_cell_t pen;                /* current attributes (ch unused) */
    int top, bottom;                    /* scroll region, inclusive */
    bool autowrap, cursor_visible, app_cursor, origin, insert;
    bool bracketed_paste;               /* ?2004: the host wants pastes marked */
    uint32_t bells;                     /* BELs received */
    uint8_t tabs[DEVOS_VT_MAX_COLS];
    int g0, g1, gl;                     /* charsets: 0 = ASCII, 1 = DEC special graphics */

    struct { int cx, cy; devos_vt_cell_t pen; bool origin, wrap_pending; int g0, g1, gl; } saved;

    int state;
    int params[VT_MAX_PARAMS];
    int nparams;
    bool have_param;
    char priv;                          /* '?', '>', '=' or 0 */
    char inter;                         /* intermediate byte (e.g. ' ' or '!') */
    char osc[96];
    int osc_len;
    uint32_t utf8_cp;
    int utf8_need;
    uint16_t last_char;

    uint8_t *dirty;                     /* rows */
    bool any_dirty;
    char title[64];

    devos_vterm_output_fn out;
    void *out_user;
};

/* ------------------------------------------------------------------------ */
static devos_vt_cell_t *row_ptr(devos_vterm_t *vt, int r)
{
    return vt->screen + (size_t)r * DEVOS_VT_MAX_COLS;
}

static void mark(devos_vterm_t *vt, int r)
{
    if (r >= 0 && r < vt->rows) {
        vt->dirty[r] = 1;
        vt->any_dirty = true;
    }
}

static void mark_all(devos_vterm_t *vt)
{
    memset(vt->dirty, 1, (size_t)vt->rows);
    vt->any_dirty = true;
}

static devos_vt_cell_t blank_cell(const devos_vterm_t *vt)
{
    /* Erase with the current background ("bce"), default foreground. */
    devos_vt_cell_t c = { ' ', 7, vt->pen.bg, (uint8_t)(VT_ATTR_FG_DEFAULT | (vt->pen.attr & VT_ATTR_BG_DEFAULT)), 0 };
    return c;
}

static void clear_cells(devos_vterm_t *vt, int r, int c0, int c1)
{
    if (r < 0 || r >= vt->rows) return;
    if (c0 < 0) c0 = 0;
    if (c1 > vt->cols) c1 = vt->cols;
    devos_vt_cell_t b = blank_cell(vt);
    devos_vt_cell_t *row = row_ptr(vt, r);
    for (int c = c0; c < c1; c++) row[c] = b;
    mark(vt, r);
}

static void clear_row(devos_vterm_t *vt, int r)
{
    clear_cells(vt, r, 0, vt->cols);
}

static void push_scrollback(devos_vterm_t *vt, int r)
{
    if (!vt->sb || vt->sb_cap <= 0) return;
    memcpy(vt->sb + (size_t)vt->sb_head * DEVOS_VT_MAX_COLS, row_ptr(vt, r),
           sizeof(devos_vt_cell_t) * DEVOS_VT_MAX_COLS);
    vt->sb_head = (vt->sb_head + 1) % vt->sb_cap;
    if (vt->sb_count < vt->sb_cap) vt->sb_count++;
    vt->sb_total++;
}

static void scroll_up(devos_vterm_t *vt, int n)
{
    int h = vt->bottom - vt->top + 1;
    if (n <= 0) return;
    if (n > h) n = h;
    bool to_history = !vt->alt_active && vt->top == 0;
    for (int i = 0; i < n && to_history; i++) push_scrollback(vt, vt->top + i);
    memmove(row_ptr(vt, vt->top), row_ptr(vt, vt->top + n),
            sizeof(devos_vt_cell_t) * DEVOS_VT_MAX_COLS * (size_t)(h - n));
    for (int r = vt->bottom - n + 1; r <= vt->bottom; r++) clear_row(vt, r);
    for (int r = vt->top; r <= vt->bottom; r++) mark(vt, r);
}

static void scroll_down(devos_vterm_t *vt, int n)
{
    int h = vt->bottom - vt->top + 1;
    if (n <= 0) return;
    if (n > h) n = h;
    memmove(row_ptr(vt, vt->top + n), row_ptr(vt, vt->top),
            sizeof(devos_vt_cell_t) * DEVOS_VT_MAX_COLS * (size_t)(h - n));
    for (int r = vt->top; r < vt->top + n; r++) clear_row(vt, r);
    for (int r = vt->top; r <= vt->bottom; r++) mark(vt, r);
}

static void linefeed(devos_vterm_t *vt)
{
    vt->wrap_pending = false;
    if (vt->cy == vt->bottom) {
        scroll_up(vt, 1);
    } else if (vt->cy < vt->rows - 1) {
        vt->cy++;
    }
}

static void reverse_index(devos_vterm_t *vt)
{
    vt->wrap_pending = false;
    if (vt->cy == vt->top) {
        scroll_down(vt, 1);
    } else if (vt->cy > 0) {
        vt->cy--;
    }
}

static void set_cursor(devos_vterm_t *vt, int col, int row)
{
    int rmin = vt->origin ? vt->top : 0;
    int rmax = vt->origin ? vt->bottom : vt->rows - 1;
    if (vt->origin) row += vt->top;
    if (row < rmin) row = rmin;
    if (row > rmax) row = rmax;
    if (col < 0) col = 0;
    if (col > vt->cols - 1) col = vt->cols - 1;
    mark(vt, vt->cy);
    vt->cx = col;
    vt->cy = row;
    vt->wrap_pending = false;
    mark(vt, vt->cy);
}

static void reply(devos_vterm_t *vt, const char *s)
{
    if (vt->out) vt->out(s, strlen(s), vt->out_user);
}

/* DEC special graphics (ESC ( 0) for 0x60-0x7E. */
static uint16_t dec_graphics(uint16_t c)
{
    static const uint16_t map[31] = {
        0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, /* ` a b c d e f g */
        0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, /* h i j k l m n o */
        0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C, /* p q r s t u v w */
        0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,         /* x y z { | } ~   */
    };
    if (c >= 0x60 && c <= 0x7E) return map[c - 0x60];
    return c;
}

static void put_char(devos_vterm_t *vt, uint32_t cp)
{
    if (cp > 0xFFFF) cp = '?';
    if (cp >= 0x0300 && cp <= 0x036F) return;              /* combining marks: drop */
    int cs = vt->gl ? vt->g1 : vt->g0;
    if (cs == 1) cp = dec_graphics((uint16_t)cp);

    if (vt->wrap_pending) {
        if (vt->autowrap) {
            vt->cx = 0;
            linefeed(vt);
        }
        vt->wrap_pending = false;
    }
    devos_vt_cell_t *row = row_ptr(vt, vt->cy);
    if (vt->insert && vt->cx < vt->cols - 1) {
        memmove(&row[vt->cx + 1], &row[vt->cx], sizeof(devos_vt_cell_t) * (size_t)(vt->cols - vt->cx - 1));
    }
    devos_vt_cell_t c = vt->pen;
    c.ch = (uint16_t)cp;
    row[vt->cx] = c;
    vt->last_char = (uint16_t)cp;
    mark(vt, vt->cy);
    if (vt->cx >= vt->cols - 1) {
        vt->wrap_pending = true;
    } else {
        vt->cx++;
    }
}

static int param(const devos_vterm_t *vt, int i, int def)
{
    if (i >= vt->nparams || vt->params[i] <= 0) return def;
    return vt->params[i];
}

static uint8_t rgb_to_256(int r, int g, int b)
{
    if (r == g && g == b) {
        if (r < 8) return 16;
        if (r > 238) return 231;
        return (uint8_t)(232 + (r - 8) / 10);
    }
    int ri = (r * 5 + 127) / 255, gi = (g * 5 + 127) / 255, bi = (b * 5 + 127) / 255;
    return (uint8_t)(16 + 36 * ri + 6 * gi + bi);
}

static void sgr(devos_vterm_t *vt)
{
    if (vt->nparams == 0) {
        vt->nparams = 1;
        vt->params[0] = 0;
    }
    for (int i = 0; i < vt->nparams; i++) {
        int p = vt->params[i];
        if (p < 0) p = 0;
        switch (p) {
        case 0:
            vt->pen.attr = VT_ATTR_FG_DEFAULT | VT_ATTR_BG_DEFAULT;
            vt->pen.fg = 7;
            vt->pen.bg = 0;
            break;
        case 1: vt->pen.attr |= VT_ATTR_BOLD; break;
        case 2: vt->pen.attr |= VT_ATTR_DIM; break;
        case 3: vt->pen.attr |= VT_ATTR_ITALIC; break;
        case 4: vt->pen.attr |= VT_ATTR_UNDERLINE; break;
        case 7: vt->pen.attr |= VT_ATTR_REVERSE; break;
        case 21: case 22: vt->pen.attr &= (uint8_t)~(VT_ATTR_BOLD | VT_ATTR_DIM); break;
        case 23: vt->pen.attr &= (uint8_t)~VT_ATTR_ITALIC; break;
        case 24: vt->pen.attr &= (uint8_t)~VT_ATTR_UNDERLINE; break;
        case 27: vt->pen.attr &= (uint8_t)~VT_ATTR_REVERSE; break;
        case 39: vt->pen.attr |= VT_ATTR_FG_DEFAULT; break;
        case 49: vt->pen.attr |= VT_ATTR_BG_DEFAULT; break;
        case 38: case 48: {
            uint8_t idx = 0;
            bool ok = false;
            if (i + 2 < vt->nparams && vt->params[i + 1] == 5) {
                idx = (uint8_t)(vt->params[i + 2] & 0xFF);
                i += 2;
                ok = true;
            } else if (i + 4 < vt->nparams && vt->params[i + 1] == 2) {
                idx = rgb_to_256(vt->params[i + 2], vt->params[i + 3], vt->params[i + 4]);
                i += 4;
                ok = true;
            }
            if (ok) {
                if (p == 38) { vt->pen.fg = idx; vt->pen.attr &= (uint8_t)~VT_ATTR_FG_DEFAULT; }
                else         { vt->pen.bg = idx; vt->pen.attr &= (uint8_t)~VT_ATTR_BG_DEFAULT; }
            }
            break;
        }
        default:
            if (p >= 30 && p <= 37) { vt->pen.fg = (uint8_t)(p - 30); vt->pen.attr &= (uint8_t)~VT_ATTR_FG_DEFAULT; }
            else if (p >= 40 && p <= 47) { vt->pen.bg = (uint8_t)(p - 40); vt->pen.attr &= (uint8_t)~VT_ATTR_BG_DEFAULT; }
            else if (p >= 90 && p <= 97) { vt->pen.fg = (uint8_t)(p - 90 + 8); vt->pen.attr &= (uint8_t)~VT_ATTR_FG_DEFAULT; }
            else if (p >= 100 && p <= 107) { vt->pen.bg = (uint8_t)(p - 100 + 8); vt->pen.attr &= (uint8_t)~VT_ATTR_BG_DEFAULT; }
            break;
        }
    }
}

static void save_cursor(devos_vterm_t *vt)
{
    vt->saved.cx = vt->cx;
    vt->saved.cy = vt->cy;
    vt->saved.pen = vt->pen;
    vt->saved.origin = vt->origin;
    vt->saved.wrap_pending = vt->wrap_pending;
    vt->saved.g0 = vt->g0;
    vt->saved.g1 = vt->g1;
    vt->saved.gl = vt->gl;
}

static void restore_cursor(devos_vterm_t *vt)
{
    mark(vt, vt->cy);
    vt->cx = vt->saved.cx < vt->cols ? vt->saved.cx : vt->cols - 1;
    vt->cy = vt->saved.cy < vt->rows ? vt->saved.cy : vt->rows - 1;
    vt->pen = vt->saved.pen;
    vt->origin = vt->saved.origin;
    vt->wrap_pending = vt->saved.wrap_pending;
    vt->g0 = vt->saved.g0;
    vt->g1 = vt->saved.g1;
    vt->gl = vt->saved.gl;
    mark(vt, vt->cy);
}

static void clear_screen(devos_vterm_t *vt)
{
    for (int r = 0; r < vt->rows; r++) clear_row(vt, r);
}

static void set_alt_screen(devos_vterm_t *vt, bool on, bool clear)
{
    if (on == vt->alt_active) return;
    vt->alt_active = on;
    vt->screen = on ? vt->alternate : vt->primary;
    if (on && clear) clear_screen(vt);
    mark_all(vt);
}

static void set_mode(devos_vterm_t *vt, bool on)
{
    for (int i = 0; i < (vt->nparams ? vt->nparams : 1); i++) {
        int m = vt->nparams ? vt->params[i] : 0;
        if (vt->priv == '?') {
            switch (m) {
            case 1:    vt->app_cursor = on; break;
            case 6:    vt->origin = on; set_cursor(vt, 0, 0); break;
            case 7:    vt->autowrap = on; break;
            case 25:   vt->cursor_visible = on; mark(vt, vt->cy); break;
            case 2004: vt->bracketed_paste = on; break;
            case 47:
            case 1047: set_alt_screen(vt, on, true); break;
            case 1048: if (on) save_cursor(vt); else restore_cursor(vt); break;
            case 1049:
                if (on) { save_cursor(vt); set_alt_screen(vt, true, true); }
                else    { set_alt_screen(vt, false, false); restore_cursor(vt); }
                break;
            default: break;                                  /* mouse, blink: ignored */
            }
        } else if (m == 4) {
            vt->insert = on;
        }
    }
}

static void csi_dispatch(devos_vterm_t *vt, char f)
{
    int n;
    devos_vt_cell_t *row;
    if (vt->inter == ' ' || vt->inter == '!') {               /* DECSCUSR / DECSTR */
        if (vt->inter == '!' && f == 'p') devos_vterm_reset(vt);
        return;
    }
    switch (f) {
    case 'A': set_cursor(vt, vt->cx, (vt->cy - (vt->origin ? vt->top : 0)) - param(vt, 0, 1)); break;
    case 'B': case 'e': set_cursor(vt, vt->cx, (vt->cy - (vt->origin ? vt->top : 0)) + param(vt, 0, 1)); break;
    case 'C': case 'a': set_cursor(vt, vt->cx + param(vt, 0, 1), vt->cy - (vt->origin ? vt->top : 0)); break;
    case 'D': set_cursor(vt, vt->cx - param(vt, 0, 1), vt->cy - (vt->origin ? vt->top : 0)); break;
    case 'E': set_cursor(vt, 0, (vt->cy - (vt->origin ? vt->top : 0)) + param(vt, 0, 1)); break;
    case 'F': set_cursor(vt, 0, (vt->cy - (vt->origin ? vt->top : 0)) - param(vt, 0, 1)); break;
    case 'G': case '`': set_cursor(vt, param(vt, 0, 1) - 1, vt->cy - (vt->origin ? vt->top : 0)); break;
    case 'd': set_cursor(vt, vt->cx, param(vt, 0, 1) - 1); break;
    case 'H': case 'f': set_cursor(vt, param(vt, 1, 1) - 1, param(vt, 0, 1) - 1); break;
    case 'J':
        n = vt->nparams ? vt->params[0] : 0;
        if (n == 0) {
            clear_cells(vt, vt->cy, vt->cx, vt->cols);
            for (int r = vt->cy + 1; r < vt->rows; r++) clear_row(vt, r);
        } else if (n == 1) {
            for (int r = 0; r < vt->cy; r++) clear_row(vt, r);
            clear_cells(vt, vt->cy, 0, vt->cx + 1);
        } else if (n == 2 || n == 3) {
            clear_screen(vt);
            if (n == 3) { vt->sb_count = 0; vt->sb_head = 0; }
        }
        break;
    case 'K':
        n = vt->nparams ? vt->params[0] : 0;
        if (n == 0) clear_cells(vt, vt->cy, vt->cx, vt->cols);
        else if (n == 1) clear_cells(vt, vt->cy, 0, vt->cx + 1);
        else clear_row(vt, vt->cy);
        break;
    case 'L':
        if (vt->cy >= vt->top && vt->cy <= vt->bottom) {
            int t = vt->top;
            vt->top = vt->cy;
            scroll_down(vt, param(vt, 0, 1));
            vt->top = t;
            vt->cx = 0;
        }
        break;
    case 'M':
        if (vt->cy >= vt->top && vt->cy <= vt->bottom) {
            int t = vt->top;
            bool alt = vt->alt_active;
            vt->top = vt->cy;
            vt->alt_active = true;                           /* deleted lines never go to history */
            scroll_up(vt, param(vt, 0, 1));
            vt->alt_active = alt;
            vt->top = t;
            vt->cx = 0;
        }
        break;
    case '@':
        n = param(vt, 0, 1);
        row = row_ptr(vt, vt->cy);
        if (n > vt->cols - vt->cx) n = vt->cols - vt->cx;
        memmove(&row[vt->cx + n], &row[vt->cx], sizeof(devos_vt_cell_t) * (size_t)(vt->cols - vt->cx - n));
        clear_cells(vt, vt->cy, vt->cx, vt->cx + n);
        break;
    case 'P':
        n = param(vt, 0, 1);
        row = row_ptr(vt, vt->cy);
        if (n > vt->cols - vt->cx) n = vt->cols - vt->cx;
        memmove(&row[vt->cx], &row[vt->cx + n], sizeof(devos_vt_cell_t) * (size_t)(vt->cols - vt->cx - n));
        clear_cells(vt, vt->cy, vt->cols - n, vt->cols);
        break;
    case 'X':
        clear_cells(vt, vt->cy, vt->cx, vt->cx + param(vt, 0, 1));
        break;
    case 'S': scroll_up(vt, param(vt, 0, 1)); break;
    case 'T': if (vt->priv == 0) scroll_down(vt, param(vt, 0, 1)); break;
    case 'b':
        n = param(vt, 0, 1);
        for (int i = 0; i < n && i < DEVOS_VT_MAX_COLS; i++) put_char(vt, vt->last_char ? vt->last_char : ' ');
        break;
    case 'g':
        n = vt->nparams ? vt->params[0] : 0;
        if (n == 0 && vt->cx < DEVOS_VT_MAX_COLS) vt->tabs[vt->cx] = 0;
        else if (n == 3) memset(vt->tabs, 0, sizeof(vt->tabs));
        break;
    case 'h': set_mode(vt, true); break;
    case 'l': set_mode(vt, false); break;
    case 'm': if (vt->priv == 0) sgr(vt); break;
    case 'n':
        if (vt->priv == 0 && param(vt, 0, 0) == 5) {
            reply(vt, "\033[0n");
        } else if (param(vt, 0, 0) == 6) {
            char b[32];
            snprintf(b, sizeof(b), "\033[%d;%dR", vt->cy - (vt->origin ? vt->top : 0) + 1, vt->cx + 1);
            reply(vt, b);
        }
        break;
    case 'c':
        if (vt->priv == '>') reply(vt, "\033[>0;276;0c");
        else if (vt->priv == 0 && param(vt, 0, 0) == 0) reply(vt, "\033[?1;2c");
        break;
    case 'r':
        if (vt->priv == 0) {
            int t = param(vt, 0, 1) - 1, b = param(vt, 1, vt->rows) - 1;
            if (t < 0) t = 0;
            if (b > vt->rows - 1) b = vt->rows - 1;
            if (t < b) { vt->top = t; vt->bottom = b; }
            set_cursor(vt, 0, 0);
        }
        break;
    case 's': if (vt->priv == 0) save_cursor(vt); break;
    case 'u': if (vt->priv == 0) restore_cursor(vt); break;
    default: break;                                          /* t, q, p, ...: ignored */
    }
}

static void esc_dispatch(devos_vterm_t *vt, char c)
{
    switch (c) {
    case '[': vt->state = ST_CSI; vt->nparams = 0; vt->have_param = false; vt->priv = 0; vt->inter = 0;
              memset(vt->params, 0, sizeof(vt->params)); return;
    case ']': vt->state = ST_OSC; vt->osc_len = 0; return;
    case 'P': vt->state = ST_DCS; return;
    case '(': vt->state = ST_CHARSET_G0; return;
    case ')': vt->state = ST_CHARSET_G1; return;
    case '#': vt->state = ST_ESC_HASH; return;
    case '7': save_cursor(vt); break;
    case '8': restore_cursor(vt); break;
    case 'D': linefeed(vt); break;
    case 'E': vt->cx = 0; linefeed(vt); break;
    case 'M': reverse_index(vt); break;
    case 'H': if (vt->cx < DEVOS_VT_MAX_COLS) vt->tabs[vt->cx] = 1; break;
    case 'c': devos_vterm_reset(vt); break;
    default: break;                                          /* '=', '>', etc.: keypad modes */
    }
    vt->state = ST_GROUND;
}

static void osc_dispatch(devos_vterm_t *vt)
{
    vt->osc[vt->osc_len] = '\0';
    if ((vt->osc[0] == '0' || vt->osc[0] == '2') && vt->osc[1] == ';') {
        snprintf(vt->title, sizeof(vt->title), "%s", vt->osc + 2);
    }
}

static void control(devos_vterm_t *vt, uint8_t c)
{
    switch (c) {
    case 0x07: vt->bells++; break;                          /* BEL: the app notices (devos_vterm_bells) */
    case 0x08:                                              /* BS */
        if (vt->cx > 0) { vt->cx--; mark(vt, vt->cy); }
        vt->wrap_pending = false;
        break;
    case 0x09: {                                            /* HT */
        int c2 = vt->cx + 1;
        while (c2 < vt->cols - 1 && !vt->tabs[c2]) c2++;
        vt->cx = c2 < vt->cols ? c2 : vt->cols - 1;
        mark(vt, vt->cy);
        break;
    }
    case 0x0A: case 0x0B: case 0x0C: linefeed(vt); break;
    case 0x0D: vt->cx = 0; vt->wrap_pending = false; mark(vt, vt->cy); break;
    case 0x0E: vt->gl = 1; break;                           /* SO */
    case 0x0F: vt->gl = 0; break;                           /* SI */
    case 0x18: case 0x1A: vt->state = ST_GROUND; break;     /* CAN / SUB */
    case 0x1B: vt->state = ST_ESC; break;
    default: break;
    }
}

static void feed_byte(devos_vterm_t *vt, uint8_t b)
{
    /* UTF-8 continuation bytes (only meaningful in GROUND). */
    if (vt->state == ST_GROUND && vt->utf8_need > 0) {
        if ((b & 0xC0) == 0x80) {
            vt->utf8_cp = (vt->utf8_cp << 6) | (b & 0x3F);
            if (--vt->utf8_need == 0) put_char(vt, vt->utf8_cp);
            return;
        }
        vt->utf8_need = 0;
        put_char(vt, 0xFFFD);
    }

    if (b < 0x20 || b == 0x7F) {
        if (b == 0x7F) return;                              /* DEL: ignore */
        /* OSC/DCS end on BEL; ESC inside them starts a string terminator. */
        if (vt->state == ST_OSC && b == 0x07) { osc_dispatch(vt); vt->state = ST_GROUND; return; }
        if ((vt->state == ST_OSC || vt->state == ST_DCS) && b == 0x1B) {
            vt->state = (vt->state == ST_OSC) ? ST_OSC_ESC : ST_DCS_ESC;
            return;
        }
        if (vt->state == ST_OSC || vt->state == ST_DCS) return;
        control(vt, b);
        return;
    }

    switch (vt->state) {
    case ST_GROUND:
        if (b < 0x80) {
            put_char(vt, b);
        } else if ((b & 0xE0) == 0xC0) {
            vt->utf8_cp = b & 0x1F; vt->utf8_need = 1;
        } else if ((b & 0xF0) == 0xE0) {
            vt->utf8_cp = b & 0x0F; vt->utf8_need = 2;
        } else if ((b & 0xF8) == 0xF0) {
            vt->utf8_cp = b & 0x07; vt->utf8_need = 3;
        } else {
            put_char(vt, 0xFFFD);
        }
        break;
    case ST_ESC:
        esc_dispatch(vt, (char)b);
        break;
    case ST_CSI:
        if (b >= '0' && b <= '9') {
            if (vt->nparams == 0) vt->nparams = 1;
            int *p = &vt->params[vt->nparams - 1];
            if (*p < 10000) *p = *p * 10 + (b - '0');
            vt->have_param = true;
        } else if (b == ';' || b == ':') {
            if (vt->nparams == 0) vt->nparams = 1;
            if (vt->nparams < VT_MAX_PARAMS) vt->params[vt->nparams++] = 0;
        } else if (b == '?' || b == '>' || b == '=' || b == '<') {
            vt->priv = (char)b;
        } else if (b >= 0x20 && b <= 0x2F) {
            vt->inter = (char)b;
        } else if (b >= 0x40 && b <= 0x7E) {
            csi_dispatch(vt, (char)b);
            vt->state = ST_GROUND;
        } else {
            vt->state = ST_GROUND;
        }
        break;
    case ST_OSC:
        if (vt->osc_len < (int)sizeof(vt->osc) - 1) vt->osc[vt->osc_len++] = (char)b;
        break;
    case ST_OSC_ESC:
        if (b == '\\') osc_dispatch(vt);
        vt->state = ST_GROUND;
        break;
    case ST_DCS:
        break;
    case ST_DCS_ESC:
        vt->state = (b == '\\') ? ST_GROUND : ST_DCS;
        break;
    case ST_CHARSET_G0:
        vt->g0 = (b == '0') ? 1 : 0;
        vt->state = ST_GROUND;
        break;
    case ST_CHARSET_G1:
        vt->g1 = (b == '0') ? 1 : 0;
        vt->state = ST_GROUND;
        break;
    case ST_ESC_HASH:
        vt->state = ST_GROUND;                              /* DECALN etc.: ignored */
        break;
    default:
        vt->state = ST_GROUND;
        break;
    }
}

/* ------------------------------------------------------------------------ */
static void reset_tabs(devos_vterm_t *vt)
{
    memset(vt->tabs, 0, sizeof(vt->tabs));
    for (int c = 8; c < DEVOS_VT_MAX_COLS; c += 8) vt->tabs[c] = 1;
}

void devos_vterm_reset(devos_vterm_t *vt)
{
    if (!vt) return;
    vt->pen.fg = 7;
    vt->pen.bg = 0;
    vt->pen.attr = VT_ATTR_FG_DEFAULT | VT_ATTR_BG_DEFAULT;
    vt->alt_active = false;
    vt->screen = vt->primary;
    vt->top = 0;
    vt->bottom = vt->rows - 1;
    vt->autowrap = true;
    vt->cursor_visible = true;
    vt->app_cursor = false;
    vt->bracketed_paste = false;
    vt->origin = false;
    vt->insert = false;
    vt->g0 = vt->g1 = vt->gl = 0;
    vt->cx = vt->cy = 0;
    vt->wrap_pending = false;
    vt->state = ST_GROUND;
    vt->utf8_need = 0;
    vt->title[0] = '\0';
    reset_tabs(vt);
    save_cursor(vt);
    for (int r = 0; r < vt->rows; r++) {
        vt->screen = vt->alternate;
        clear_row(vt, r);
        vt->screen = vt->primary;
        clear_row(vt, r);
    }
    mark_all(vt);
}

devos_vterm_t *devos_vterm_create(int cols, int rows, int scrollback_lines)
{
    if (cols < 2) cols = 2;
    if (cols > DEVOS_VT_MAX_COLS) cols = DEVOS_VT_MAX_COLS;
    if (rows < 2) rows = 2;
    devos_vterm_t *vt = vt_alloc(sizeof(*vt));
    if (!vt) return NULL;
    vt->cols = cols;
    vt->rows = rows;
    vt->primary = vt_alloc(sizeof(devos_vt_cell_t) * DEVOS_VT_MAX_COLS * (size_t)rows);
    vt->alternate = vt_alloc(sizeof(devos_vt_cell_t) * DEVOS_VT_MAX_COLS * (size_t)rows);
    vt->dirty = vt_alloc((size_t)rows);
    if (scrollback_lines > 0) {
        vt->sb = vt_alloc(sizeof(devos_vt_cell_t) * DEVOS_VT_MAX_COLS * (size_t)scrollback_lines);
        vt->sb_cap = vt->sb ? scrollback_lines : 0;
    }
    if (!vt->primary || !vt->alternate || !vt->dirty) {
        devos_vterm_destroy(vt);
        return NULL;
    }
    devos_vterm_reset(vt);
    return vt;
}

void devos_vterm_destroy(devos_vterm_t *vt)
{
    if (!vt) return;
    free(vt->primary);
    free(vt->alternate);
    free(vt->sb);
    free(vt->dirty);
    free(vt);
}

void devos_vterm_resize(devos_vterm_t *vt, int cols, int rows)
{
    if (!vt) return;
    if (cols < 2) cols = 2;
    if (cols > DEVOS_VT_MAX_COLS) cols = DEVOS_VT_MAX_COLS;
    if (rows < 2) rows = 2;
    if (cols == vt->cols && rows == vt->rows) return;

    devos_vt_cell_t *np = vt_alloc(sizeof(devos_vt_cell_t) * DEVOS_VT_MAX_COLS * (size_t)rows);
    devos_vt_cell_t *na = vt_alloc(sizeof(devos_vt_cell_t) * DEVOS_VT_MAX_COLS * (size_t)rows);
    uint8_t *nd = vt_alloc((size_t)rows);
    if (!np || !na || !nd) { free(np); free(na); free(nd); return; }

    /* Keep the rows around the cursor: if it would fall off the bottom, drop
     * lines from the top (primary: into history). */
    int shift = 0;
    if (vt->cy >= rows) shift = vt->cy - rows + 1;
    devos_vt_cell_t *save_screen = vt->screen;
    vt->screen = vt->primary;
    if (!vt->alt_active) {
        for (int i = 0; i < shift; i++) push_scrollback(vt, i);
    }
    vt->screen = save_screen;

    devos_vt_cell_t blank = { ' ', 7, 0, VT_ATTR_FG_DEFAULT | VT_ATTR_BG_DEFAULT, 0 };
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < DEVOS_VT_MAX_COLS; c++) {
            np[(size_t)r * DEVOS_VT_MAX_COLS + c] = blank;
            na[(size_t)r * DEVOS_VT_MAX_COLS + c] = blank;
        }
        int src = r + shift;
        if (src < vt->rows) {
            memcpy(&np[(size_t)r * DEVOS_VT_MAX_COLS], &vt->primary[(size_t)src * DEVOS_VT_MAX_COLS],
                   sizeof(devos_vt_cell_t) * DEVOS_VT_MAX_COLS);
            memcpy(&na[(size_t)r * DEVOS_VT_MAX_COLS], &vt->alternate[(size_t)src * DEVOS_VT_MAX_COLS],
                   sizeof(devos_vt_cell_t) * DEVOS_VT_MAX_COLS);
        }
    }
    /* Columns beyond the old width are blank. */
    if (cols > vt->cols) {
        for (int r = 0; r < rows; r++) {
            for (int c = vt->cols; c < cols; c++) {
                np[(size_t)r * DEVOS_VT_MAX_COLS + c] = blank;
                na[(size_t)r * DEVOS_VT_MAX_COLS + c] = blank;
            }
        }
    }
    free(vt->primary);
    free(vt->alternate);
    free(vt->dirty);
    vt->primary = np;
    vt->alternate = na;
    vt->dirty = nd;
    vt->screen = vt->alt_active ? na : np;
    vt->cols = cols;
    vt->rows = rows;
    vt->cy -= shift;
    if (vt->cy < 0) vt->cy = 0;
    if (vt->cx > cols - 1) vt->cx = cols - 1;
    vt->top = 0;
    vt->bottom = rows - 1;
    vt->wrap_pending = false;
    if (vt->saved.cy >= rows) vt->saved.cy = rows - 1;
    if (vt->saved.cx >= cols) vt->saved.cx = cols - 1;
    mark_all(vt);
}

void devos_vterm_feed(devos_vterm_t *vt, const char *data, size_t len)
{
    if (!vt || !data) return;
    for (size_t i = 0; i < len; i++) feed_byte(vt, (uint8_t)data[i]);
}

void devos_vterm_set_output_cb(devos_vterm_t *vt, devos_vterm_output_fn cb, void *user)
{
    if (!vt) return;
    vt->out = cb;
    vt->out_user = user;
}

int devos_vterm_cols(const devos_vterm_t *vt) { return vt ? vt->cols : 0; }
int devos_vterm_rows(const devos_vterm_t *vt) { return vt ? vt->rows : 0; }

int devos_vterm_scrollback_lines(const devos_vterm_t *vt)
{
    return (vt && !vt->alt_active) ? vt->sb_count : 0;
}

uint32_t devos_vterm_scrolled_total(const devos_vterm_t *vt)
{
    return vt ? vt->sb_total : 0;
}

const devos_vt_cell_t *devos_vterm_view_row(const devos_vterm_t *vt, int row, int view_offset)
{
    if (!vt || row < 0 || row >= vt->rows) return NULL;
    int avail = devos_vterm_scrollback_lines(vt);
    if (view_offset > avail) view_offset = avail;
    if (view_offset <= 0) return vt->screen + (size_t)row * DEVOS_VT_MAX_COLS;
    int line = avail - view_offset + row;                   /* 0 = oldest history line */
    if (line >= avail) return vt->screen + (size_t)(line - avail) * DEVOS_VT_MAX_COLS;
    int idx = (vt->sb_head - vt->sb_count + line + vt->sb_cap) % vt->sb_cap;
    return vt->sb + (size_t)idx * DEVOS_VT_MAX_COLS;
}

void devos_vterm_cursor(const devos_vterm_t *vt, int *col, int *row, bool *visible)
{
    if (!vt) return;
    if (col) *col = vt->cx;
    if (row) *row = vt->cy;
    if (visible) *visible = vt->cursor_visible;
}

bool devos_vterm_app_cursor_keys(const devos_vterm_t *vt) { return vt && vt->app_cursor; }
bool devos_vterm_bracketed_paste(const devos_vterm_t *vt) { return vt && vt->bracketed_paste; }
uint32_t devos_vterm_bells(const devos_vterm_t *vt) { return vt ? vt->bells : 0; }
bool devos_vterm_alt_screen(const devos_vterm_t *vt) { return vt && vt->alt_active; }
const char *devos_vterm_title(const devos_vterm_t *vt) { return vt ? vt->title : ""; }

bool devos_vterm_take_dirty(devos_vterm_t *vt, uint8_t *rows_dirty, int max_rows)
{
    if (!vt || !vt->any_dirty) return false;
    int n = vt->rows < max_rows ? vt->rows : max_rows;
    if (rows_dirty) memcpy(rows_dirty, vt->dirty, (size_t)n);
    memset(vt->dirty, 0, (size_t)vt->rows);
    vt->any_dirty = false;
    return true;
}
