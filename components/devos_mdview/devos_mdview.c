/* devos_mdview: shared CommonMark-subset renderer. See devos_mdview.h. */
#include "devos_mdview.h"
#include "devos_theme.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Nimbus Mono 14: regular-weight mono matching Montserrat 14 body size.
 * Generated via lv_font_conv from NimbusMonoPS-Regular.otf (see file header
 * in components/devos_ui/lv_font_nimbus_mono_14.c to regenerate). */
LV_FONT_DECLARE(lv_font_nimbus_mono_14);

#define MD_LINE_MAX 2048
#define MD_TABLE_ROWS 18
#define MD_TABLE_COLS 6
#define MD_CELL_MAX 28
#define MD_CELL_PAD 6

/* --------------------------------------------------------------------------
 * Markdown renderer (CommonMark subset, shared by the editor preview,
 * agent artifact viewer, and future rich-text surfaces).
 *
 * Blocks: ATX headings, fenced code, tables, hr, quotes, ul/ol/task lists,
 * paragraphs. Inline: **bold**, *italic*, ~~strike~~, `code`, links, images,
 * autolinks, backslash escapes. Code and tables use Nimbus Mono 14, a
 * regular-weight mono sized to match Montserrat 14 body text.
 *
 * Single regular font in the build, so emphasis falls back honestly:
 *   bold   -> underline (typewriter emphasis; links use color-only, no clash)
 *   italic -> secondary color (no oblique face exists)
 * Out of scope: setext headings, reference links, bare-URL linking,
 * nested-bracket links, indented code blocks, CJK column widths,
 * `\|` escapes in tables.
 * -------------------------------------------------------------------------- */

/* Back a byte count off a split UTF-8 character so truncation never
 * produces tofu. Inspects backward within [0, n) without over-reading. */
size_t devos_md_trunc_ok(const char *s, size_t n)
{
    if (!s || n == 0) return 0;
    if (((unsigned char)s[n - 1] & 0x80) == 0) return n;

    size_t k = n;
    while (k > 0 && ((unsigned char)s[k - 1] & 0xC0) == 0x80) {
        k--;
    }
    if (k == 0) return 0;

    unsigned char lead = (unsigned char)s[k - 1];
    size_t expected_len = 1;
    if ((lead & 0xE0) == 0xC0) expected_len = 2;
    else if ((lead & 0xF0) == 0xE0) expected_len = 3;
    else if ((lead & 0xF8) == 0xF0) expected_len = 4;
    else return k - 1;

    if (k - 1 + expected_len <= n) return n;
    return k - 1;
}

/* Emit one styled run (lv_span_set_text copies, so a shared temp is safe) */
static void md_add_span(lv_obj_t *sg, const char *s, size_t n,
                        const lv_font_t *font, lv_color_t color,
                        lv_text_decor_t decor)
{
    static char seg[501];
    while (n > 0) {
        size_t chunk = n > 500 ? 500 : n;
        if (chunk == 500 && chunk < n) {
            chunk = devos_md_trunc_ok(s, chunk);
            if (chunk == 0) chunk = 500;
        }
        memcpy(seg, s, chunk);
        seg[chunk] = '\0';
        lv_span_t *sp = lv_spangroup_new_span(sg);
        lv_span_set_text(sp, seg);
        lv_style_t *st = lv_span_get_style(sp);
        lv_style_set_text_font(st, font);
        lv_style_set_text_color(st, color);
        lv_style_set_text_decor(st, decor);
        s += chunk;
        n -= chunk;
    }
}

static const char *md_find_marker(const char *s, size_t n, const char *m,
                                  size_t from)
{
    size_t mlen = strlen(m);
    for (size_t i = from; i + mlen <= n; i++) {
        if (memcmp(s + i, m, mlen) == 0) return s + i;
    }
    return NULL;
}

/* Agents link local files as [name](file:///abs/path#L8): the name says it
 * all, and the path is on another computer, so only web URLs are shown. */
static bool md_url_shown(const char *u, size_t ulen)
{
    return !(ulen >= 7 && strncmp(u, "file://", 7) == 0);
}

/* Link destination after "](": sets *url and *ulen, returns closing ')', or NULL.
 * Handles <dest> (spaces allowed), balanced parens, and "title" tails. */
static const char *md_link_end(const char *s, size_t n, const char **url,
                               size_t *ulen)
{
    const char *end = s + n;
    const char *p = s;
    const char *u;
    size_t ul;
    if (p < end && *p == '<') {
        const char *gt = memchr(p + 1, '>', (size_t)(end - (p + 1)));
        if (!gt) return NULL;
        u = p + 1;
        ul = (size_t)(gt - u);
        p = gt + 1;
    } else {
        int depth = 0;
        const char *q = p;
        while (q < end && (depth > 0 ||
               (*q != ' ' && *q != '\t' && *q != ')'))) {
            if (*q == '(') depth++;
            else if (*q == ')') depth--;
            q++;
        }
        u = p;
        ul = (size_t)(q - p);
        p = q;
    }
    while (p < end && *p != ')') p++; /* skip "title" to the closer */
    if (p >= end) return NULL;
    *url = u;
    *ulen = ul;
    return p;
}

/* Closer `*` that is not half of a `**` run nor intraword (2*3*4) */
static const char *md_find_star(const char *s, size_t n, size_t from)
{
    for (size_t i = from; i < n; i++) {
        if (s[i] != '*') continue;
        if ((i > 0 && s[i - 1] == '*') || (i + 1 < n && s[i + 1] == '*')) continue;
        bool prev_a = i > 0 && isalnum((unsigned char)s[i - 1]);
        bool next_a = i + 1 < n && isalnum((unsigned char)s[i + 1]);
        if (prev_a && next_a) continue; /* intraword, not emphasis */
        return s + i;
    }
    return NULL;
}

static bool md_star_opener(const char *s, size_t n, size_t i)
{
    bool prev_a = i > 0 && isalnum((unsigned char)s[i - 1]);
    bool next_a = i + 1 < n && isalnum((unsigned char)s[i + 1]);
    return !(prev_a && next_a);
}

static bool md_uscore_edge(const char *s, size_t n, size_t i, bool opener)
{
    bool prev_alnum = i > 0 && isalnum((unsigned char)s[i - 1]);
    bool next_alnum = i + 1 < n && isalnum((unsigned char)s[i + 1]);
    bool next_space = i + 1 >= n || isspace((unsigned char)s[i + 1]);
    return opener ? (!prev_alnum && !next_space) : (!next_alnum);
}

static void md_render_inline(lv_obj_t *sg, const char *s, size_t n,
                             const lv_font_t *font, lv_color_t color,
                             lv_text_decor_t decor, const devos_palette_t *p)
{
    size_t i = 0, start = 0;
    while (i < n) {
        char c = s[i];

        /* Image: ![alt](src) */
        if (c == '!' && i + 1 < n && s[i + 1] == '[') {
            const char *k = md_find_marker(s, n, "](", i + 2);
            const char *e = NULL;
            const char *u = NULL;
            size_t ul = 0;
            if (k) {
                const char *inner_bracket = memchr(s + i + 2, ']', (size_t)(k - (s + i + 2)));
                if (!inner_bracket) e = md_link_end(k + 2, (s + n) - (k + 2), &u, &ul);
            }
            if (e) {
                md_add_span(sg, s + start, i - start, font, color, decor);
                md_add_span(sg, "[Image: ", 8, font, p->text_muted, decor);
                md_add_span(sg, s + i + 2, k - (s + i + 2), font,
                            p->text_muted, decor);
                md_add_span(sg, "]", 1, font, p->text_muted, decor);
                i = (size_t)(e - s) + 1;
                start = i;
                continue;
            }
        }

        /* Link: [text](url) */
        if (c == '[') {
            const char *k = md_find_marker(s, n, "](", i + 1);
            const char *e = NULL;
            const char *u = NULL;
            size_t ulen = 0;
            if (k) {
                const char *inner_bracket = memchr(s + i + 1, ']', (size_t)(k - (s + i + 1)));
                if (!inner_bracket) e = md_link_end(k + 2, (s + n) - (k + 2), &u, &ulen);
            }
            if (e) {
                md_add_span(sg, s + start, i - start, font, color, decor);
                md_render_inline(sg, s + i + 1, k - (s + i + 1), font,
                                 p->accent_primary, decor, p);
                /* ponytail: device can't tap links, so the URL stays visible */
                if (md_url_shown(u, ulen)) {
                    md_add_span(sg, " (", 2, font, p->text_muted, decor);
                    md_add_span(sg, u, ulen, font, p->text_muted, decor);
                    md_add_span(sg, ")", 1, font, p->text_muted, decor);
                }
                i = (size_t)(e - s) + 1;
                start = i;
                continue;
            }
        }

        /* Autolink: <https://..> / <mail@..> */
        if (c == '<') {
            const char *e = memchr(s + i + 1, '>', n - i - 1);
            if (e && e - (s + i + 1) > 3) {
                bool spaces = false;
                for (const char *t = s + i + 1; t < e; t++) {
                    if (isspace((unsigned char)*t)) { spaces = true; break; }
                }
                if (!spaces) {
                    md_add_span(sg, s + start, i - start, font, color, decor);
                    md_add_span(sg, s + i + 1, (size_t)(e - (s + i + 1)), font,
                                p->accent_primary, decor);
                    i = (size_t)(e - s) + 1;
                    start = i;
                    continue;
                }
            }
        }

        /* Backslash escape */
        if (c == '\\' && i + 1 < n && ispunct((unsigned char)s[i + 1])) {
            md_add_span(sg, s + start, i - start, font, color, decor);
            md_add_span(sg, s + i + 1, 1, font, color, decor);
            i += 2;
            start = i;
            continue;
        }

        /* Inline code (literal, no nesting) */
        if (c == '`') {
            const char *e = md_find_marker(s, n, "`", i + 1);
            if (e && e > s + i + 1) {
                md_add_span(sg, s + start, i - start, font, color, decor);
                md_add_span(sg, s + i + 1, (size_t)(e - (s + i + 1)), font,
                            p->accent_secondary, decor);
                i = (size_t)(e - s) + 1;
                start = i;
                continue;
            }
        }

        /* Bold ** / __ */
        if ((c == '*' && i + 1 < n && s[i + 1] == '*') ||
            (c == '_' && i + 1 < n && s[i + 1] == '_' &&
             md_uscore_edge(s, n, i, true))) {
            char m[3] = {c, c, '\0'};
            const char *e = md_find_marker(s, n, m, i + 2);
            if (e && e > s + i + 2) {
                md_add_span(sg, s + start, i - start, font, color, decor);
                md_render_inline(sg, s + i + 2, (size_t)(e - (s + i + 2)),
                                 font, color,
                                 (lv_text_decor_t)(decor | LV_TEXT_DECOR_UNDERLINE),
                                 p);
                i = (size_t)(e - s) + 2;
                start = i;
                continue;
            }
        }

        /* Strikethrough ~~ */
        if (c == '~' && i + 1 < n && s[i + 1] == '~') {
            const char *e = md_find_marker(s, n, "~~", i + 2);
            if (e && e > s + i + 2) {
                md_add_span(sg, s + start, i - start, font, color, decor);
                md_render_inline(sg, s + i + 2, (size_t)(e - (s + i + 2)),
                                 font, color,
                                 (lv_text_decor_t)(decor | LV_TEXT_DECOR_STRIKETHROUGH),
                                 p);
                i = (size_t)(e - s) + 2;
                start = i;
                continue;
            }
        }

        /* Italic * / _ */
        if (c == '*' && (i + 1 >= n || s[i + 1] != '*') &&
            md_star_opener(s, n, i)) {
            const char *e = md_find_star(s, n, i + 1);
            if (e && e > s + i + 1) {
                md_add_span(sg, s + start, i - start, font, color, decor);
                md_render_inline(sg, s + i + 1, (size_t)(e - (s + i + 1)),
                                 font, p->text_secondary, decor, p);
                i = (size_t)(e - s) + 1;
                start = i;
                continue;
            }
        }
        if (c == '_' && md_uscore_edge(s, n, i, true)) {
            size_t j = i + 1;
            const char *e = NULL;
            for (; j < n; j++) {
                if (s[j] == '_' && (j + 1 >= n || s[j + 1] != '_') &&
                    (j == 0 || s[j - 1] != '_') && md_uscore_edge(s, n, j, false)) {
                    e = s + j;
                    break;
                }
            }
            if (e && e > s + i + 1) {
                md_add_span(sg, s + start, i - start, font, color, decor);
                md_render_inline(sg, s + i + 1, (size_t)(e - (s + i + 1)),
                                 font, p->text_secondary, decor, p);
                i = (size_t)(e - s) + 1;
                start = i;
                continue;
            }
        }

        i++;
    }
    md_add_span(sg, s + start, n - start, font, color, decor);
}

/* Strip inline markers for contexts that can't style (table cells) */
static size_t md_strip_into(const char *s, size_t n, char *out, size_t cap,
                            size_t o)
{
    size_t i = 0;
    while (i < n && o + 1 < cap) {
        char c = s[i];
        /* Doubled markers: drop openers and closers alike */
        if ((c == '*' || c == '_' || c == '~' || c == '`') && i + 1 < n &&
            s[i + 1] == c) {
            i += 2;
            continue;
        }
        /* Singles: strip the pair, recurse into the middle */
        if (c == '*' || c == '_' || c == '`') {
            bool ok = false;
            if (c == '`') ok = true;
            else if (c == '*') ok = md_star_opener(s, n, i);
            else ok = md_uscore_edge(s, n, i, true);
            const char *e = NULL;
            if (ok) {
                for (size_t j = i + 1; j < n; j++) {
                    if (s[j] != c) continue;
                    if (j + 1 < n && s[j + 1] == c) { j++; continue; }
                    if (j > 0 && s[j - 1] == c) continue;
                    e = s + j;
                    break;
                }
            }
            if (e) {
                o = md_strip_into(s + i + 1, (size_t)(e - (s + i + 1)), out,
                                  cap, o);
                i = (size_t)(e - s) + 1;
                continue;
            }
        }
        if (c == '[') {
            const char *k = md_find_marker(s, n, "](", i + 1);
            const char *e = NULL;
            const char *u = NULL;
            size_t ulen = 0;
            if (k) {
                const char *inner_bracket = memchr(s + i + 1, ']', (size_t)(k - (s + i + 1)));
                if (!inner_bracket) e = md_link_end(k + 2, (s + n) - (k + 2), &u, &ulen);
            }
            if (e) {
                o = md_strip_into(s + i + 1, (size_t)(k - (s + i + 1)), out,
                                  cap, o);
                if (md_url_shown(u, ulen)) {
                    if (o + 3 >= cap) break;
                    out[o++] = ' ';
                    out[o++] = '(';
                    if (o + ulen + 1 >= cap) break;
                    memcpy(out + o, u, ulen);
                    o += ulen;
                    out[o++] = ')';
                }
                i = (size_t)(e - s) + 1;
                continue;
            }
        }
        out[o++] = c;
        i++;
    }
    out[o] = '\0';
    return o;
}

static size_t md_strip_inline(const char *s, size_t n, char *out, size_t cap)
{
    size_t o = md_strip_into(s, n, out, cap, 0);
    out[o] = '\0';
    return o;
}

static lv_obj_t *s_parent = NULL;

static lv_obj_t *md_new_block(lv_obj_t *parent, int y)
{
    lv_obj_t *sg = lv_spangroup_create(parent);
    lv_obj_set_width(sg, lv_pct(100));
    lv_obj_set_pos(sg, 0, y);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
    lv_obj_set_style_pad_all(sg, 0, 0);
    lv_obj_set_style_border_width(sg, 0, 0);
    return sg;
}

/* Fence accumulation: one contiguous block, not striped per-line boxes */
static char s_codebuf[8192];
static size_t s_codelen = 0;
static bool s_codedrop = false;

static void md_finish_block(lv_obj_t *sg, int *y, int gap);

static void md_emit_code_block(int *y, const devos_palette_t *p)
{
    if (s_codelen == 0) return;
    if (s_codebuf[s_codelen - 1] == '\n') s_codebuf[--s_codelen] = '\0';
    lv_obj_t *sg = md_new_block(s_parent, *y);
    lv_obj_set_style_bg_color(sg, p->surface_active, 0);
    lv_obj_set_style_bg_opa(sg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sg, 4, 0);
    lv_obj_set_style_pad_all(sg, 8, 0);
    md_add_span(sg, s_codebuf, s_codelen, &lv_font_nimbus_mono_14,
                p->text_primary, LV_TEXT_DECOR_NONE);
    md_finish_block(sg, y, 8);
    s_codelen = 0;
}

static void md_finish_block(lv_obj_t *sg, int *y, int gap)
{
    lv_spangroup_refr_mode(sg);
    lv_obj_update_layout(sg);
    *y += lv_obj_get_height(sg) + gap;
}

/* Split a |table| row into trimmed cells. Returns cell count. */
static int md_split_row(char *row, char cells[MD_TABLE_COLS][MD_CELL_MAX + 1])
{
    while (*row == ' ' || *row == '\t') row++;
    size_t len = strlen(row);
    while (len > 0 && (row[len - 1] == ' ' || row[len - 1] == '\t')) row[--len] = '\0';
    if (len > 0 && row[0] == '|') { row++; len--; }
    while (len > 0 && (row[len - 1] == ' ' || row[len - 1] == '\t')) row[--len] = '\0';
    if (len > 0 && row[len - 1] == '|') row[--len] = '\0';

    int count = 0;
    char *p = row;
    while (*p && count < MD_TABLE_COLS) {
        char *sep = strchr(p, '|');
        if (sep) *sep = '\0';

        char *tok = p;
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t tlen = strlen(tok);
        while (tlen > 0 && (tok[tlen - 1] == ' ' || tok[tlen - 1] == '\t')) tok[--tlen] = '\0';
        if (tlen > MD_CELL_MAX) tlen = devos_md_trunc_ok(tok, MD_CELL_MAX);
        memcpy(cells[count], tok, tlen);
        cells[count][tlen] = '\0';
        count++;

        if (!sep) break;
        p = sep + 1;
    }
    return count;
}

static bool md_is_delim_cell(const char *cell, int *align)
{
    size_t n = strlen(cell);
    if (n == 0) return false;
    bool left = cell[0] == ':';
    bool right = n > 1 && cell[n - 1] == ':';
    size_t a = left ? 1 : 0;
    size_t b = right ? n - 1 : n;
    if (b <= a) return false;
    for (size_t i = a; i < b; i++) {
        if (cell[i] != '-') return false;
    }
    *align = left && right ? 1 : (right ? 2 : 0); /* 0L 1C 2R */
    return true;
}

/* Whole-line delimiter check (for pipe-less GFM table lookahead) */
static bool md_is_delim_line(const char *s)
{
    static char tmp[MD_LINE_MAX];
    static char cells[MD_TABLE_COLS][MD_CELL_MAX + 1];
    size_t n = strlen(s);
    if (n > sizeof(tmp) - 1) n = sizeof(tmp) - 1;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    /* ponytail: md_split_row takes char* (strtok); tmp is expendable */
    int count = md_split_row(tmp, cells);
    if (count == 0) return false;
    for (int c = 0; c < count; c++) {
        int dummy;
        if (!md_is_delim_cell(cells[c], &dummy)) return false;
    }
    return true;
}

/* Padded string for table cells (lv_table_set_cell_value copies) */
static size_t md_pad_string(const char *cell, int width, int align,
                            char *out, size_t cap)
{
    size_t n = strlen(cell);
    if ((int)n > width) n = devos_md_trunc_ok(cell, (size_t)width);
    int fill = width - (int)n;
    int left = align == 2 ? fill : (align == 1 ? fill / 2 : 0);
    int right = fill - left;
    size_t o = 0;
    while (left-- > 0 && o + 1 < cap) out[o++] = ' ';
    if (o + n + 1 > cap) n = cap - o - 1;
    memcpy(out + o, cell, n);
    o += n;
    while (right-- > 0 && o + 1 < cap) out[o++] = ' ';
    out[o] = '\0';
    return o;
}

#define MD_ADVANCE_LINE() { \
    if (raw_len < full_len) { cur += raw_len; continue; } \
    if (!nl) goto md_done; \
    cur = nl + 1; \
    continue; \
}

int devos_md_render(lv_obj_t *parent, const char *text)
{
    if (!parent) return 0;
    lv_obj_update_layout(parent);
    const devos_palette_t *p = devos_theme_get();

    s_parent = parent;
    s_codelen = 0;
    s_codedrop = false;
    if (!text) text = "";

    static char line[MD_LINE_MAX];
    static char tline[MD_LINE_MAX];
    static char rowbuf[MD_LINE_MAX];
    static char cells[MD_TABLE_ROWS][MD_TABLE_COLS][MD_CELL_MAX + 1];

    bool in_code = false;
    bool in_ol = false;
    int ol_num = 1;
    int y = 0;
    const char *cur = text;
    for (;;) {
        const char *nl = strchr(cur, '\n');
        size_t full_len = nl ? (size_t)(nl - cur) : strlen(cur);
        size_t raw_len = full_len;
        if (raw_len > sizeof(line) - 1) raw_len = devos_md_trunc_ok(cur, sizeof(line) - 1);
        memcpy(line, cur, raw_len);
        line[raw_len] = '\0';
        size_t len = raw_len;
        while (len > 0 && line[len - 1] == '\r') line[--len] = '\0';

        const char *t = line;
        while (*t == ' ' || *t == '\t') t++; /* block indent tolerance */

        /* Fence toggle (info string ignored) */
        if (strncmp(t, "```", 3) == 0) {
            if (in_code) {
                md_emit_code_block(&y, p);
            } else {
                s_codelen = 0;
                s_codedrop = false;
            }
            in_code = !in_code;
            in_ol = false;
            MD_ADVANCE_LINE();
        }

        if (in_code) {
            if (!s_codedrop) {
                /* ponytail: truncate absurd fences instead of overflowing */
                if (s_codelen + len + 1 < sizeof(s_codebuf)) {
                    memcpy(s_codebuf + s_codelen, line, len);
                    s_codelen += len;
                    s_codebuf[s_codelen++] = '\n';
                } else {
                    s_codedrop = true;
                }
            }
            MD_ADVANCE_LINE();
        }

        /* Blank */
        if (len == 0) {
            y += 10;
            in_ol = false;
            MD_ADVANCE_LINE();
        }

        /* HR: --- *** ___ (3+ of one marker, spaces tolerated) */
        {
            char m = 0;
            int cnt = 0;
            bool hr = true;
            for (const char *c = t; *c; c++) {
                if (*c == ' ' || *c == '\t') continue;
                if (*c != '-' && *c != '*' && *c != '_') { hr = false; break; }
                if (!m) m = *c;
                if (*c != m) { hr = false; break; }
                cnt++;
            }
            if (hr && cnt >= 3) {
                lv_obj_t *bar = lv_obj_create(s_parent);
                lv_obj_set_size(bar, lv_pct(100), 2);
                lv_obj_set_pos(bar, 0, y + 7);
                lv_obj_set_style_bg_color(bar, p->surface_border, 0);
                lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(bar, 0, 0);
                lv_obj_set_style_radius(bar, 1, 0);
                lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
                y += 18;
                in_ol = false;
                MD_ADVANCE_LINE();
            }
        }

        /* ATX headings (closing #'s stripped) */
        if (*t == '#') {
            int level = 0;
            while (t[level] == '#') level++;
            if (level <= 6 && (t[level] == ' ' || t[level] == '\t' || t[level] == '\0')) {
                const char *c = t + level;
                while (*c == ' ' || *c == '\t') c++;
                static char head[MD_LINE_MAX];
                size_t hlen = strlen(c);
                if (hlen > sizeof(head) - 1) hlen = devos_md_trunc_ok(c, sizeof(head) - 1);
                memcpy(head, c, hlen);
                head[hlen] = '\0';
                while (hlen > 0 && (head[hlen - 1] == ' ' || head[hlen - 1] == '\t')) {
                    head[--hlen] = '\0';
                }
                while (hlen > 0 && head[hlen - 1] == '#') head[--hlen] = '\0';
                while (hlen > 0 && (head[hlen - 1] == ' ' || head[hlen - 1] == '\t')) {
                    head[--hlen] = '\0';
                }
                /* ponytail: H1 16/accent, H2 16/primary, H3 14/accent, H4+ 12/muted */
                const lv_font_t *f = level == 1 ? &lv_font_montserrat_16
                                    : level == 2 ? &lv_font_montserrat_16
                                    : level == 3 ? &lv_font_montserrat_14
                                                 : &lv_font_montserrat_12;
                lv_color_t col = level == 1 ? p->accent_primary
                               : level == 2 ? p->text_primary
                               : level == 3 ? p->accent_primary
                                            : p->text_secondary;
                lv_obj_t *sg = md_new_block(s_parent, y);
                md_render_inline(sg, head, hlen, f, col, LV_TEXT_DECOR_NONE, p);
                md_finish_block(sg, &y, 8);
                in_ol = false;
                MD_ADVANCE_LINE();
            }
        }

        /* Table: header line with | + delimiter row (outer pipes optional) */
        if (strchr(t, '|') != NULL && nl != NULL) {
            /* ponytail: peek the next line; only a delimiter commits to table */
            const char *nstart = nl + 1;
            const char *nnl = strchr(nstart, '\n');
            size_t nlen = nnl ? (size_t)(nnl - nstart) : strlen(nstart);
            bool delim_next = false;
            if (nlen < sizeof(tline)) {
                memcpy(tline, nstart, nlen);
                tline[nlen] = '\0';
                while (nlen > 0 && tline[nlen - 1] == '\r') tline[--nlen] = '\0';
                delim_next = md_is_delim_line(tline);
            }
            if (delim_next) {
                const char *tt = cur;
                int rows = 0;
                int cols = 0;
                int rowcounts[MD_TABLE_ROWS] = {0};
                bool fits = true;
            while (tt && rows < MD_TABLE_ROWS) {
                const char *tnl = strchr(tt, '\n');
                size_t tlen = tnl ? (size_t)(tnl - tt) : strlen(tt);
                if (tlen > sizeof(tline) - 1) tlen = devos_md_trunc_ok(tt, sizeof(tline) - 1);
                    memcpy(tline, tt, tlen);
                    tline[tlen] = '\0';
                    while (tlen > 0 && tline[tlen - 1] == '\r') tline[--tlen] = '\0';
                    const char *tl = tline;
                    while (*tl == ' ' || *tl == '\t') tl++;
                    /* body rows need |; the delim row (rows==1) was pre-validated */
                    if (!strchr(tl, '|') &&
                        !(rows == 1 && md_is_delim_line(tline))) {
                        break;
                    }
                rowcounts[rows] = md_split_row(tline, cells[rows]);
                if (rows == 0) cols = rowcounts[0];
                if (rowcounts[rows] == 0) { fits = false; break; }
                rows++;
                tt = tnl ? tnl + 1 : NULL;
            }
            /* ponytail: ragged rows pad empty; delimiter must cover all cols */
            bool valid = fits && rows >= 2 && cols > 0 && cols <= MD_TABLE_COLS &&
                         rowcounts[1] >= cols;
            int aligns[MD_TABLE_COLS] = {0};
            if (valid) {
                for (int c = 0; c < cols; c++) {
                    if (!md_is_delim_cell(cells[1][c], &aligns[c])) {
                        valid = false;
                        break;
                    }
                }
            }
            if (valid) {
                /* ponytail: LVGL 9.2 tables style every cell from ITEMS with
                 * no per-cell override, so the accent header is its own
                 * 1-row table stacked on the body table. Shared column
                 * widths; alignment is space padding (mono font aligns it). */
                int glyph_adv = lv_text_get_width("0000000000", 10,
                                                  &lv_font_nimbus_mono_14, 0) / 10;
                if (glyph_adv < 1) glyph_adv = 8;
                int widths[MD_TABLE_COLS] = {0};
                for (int r = 0; r < rows; r++) {
                    if (r == 1) continue; /* delimiter */
                    for (int c = 0; c < cols; c++) {
                        const char *cell = c < rowcounts[r] ? cells[r][c] : "";
                        int w = (int)strlen(cell);
                        if (w > widths[c]) widths[c] = w;
                    }
                }
                /* Shrink widest cols (in px, incl. cell padding) to fit.
                 * columns share the content width minus card chrome. */
                int max_px = (int)lv_obj_get_content_width(s_parent) - 18;
                if (max_px < 240) max_px = 500;
                for (;;) {
                    int total = 0;
                    for (int c = 0; c < cols; c++) {
                        total += widths[c] * glyph_adv + 2 * MD_CELL_PAD;
                    }
                    int widest = -1;
                    for (int c = 0; c < cols; c++) {
                        if (widths[c] > 4 &&
                            (widest < 0 || widths[c] > widths[widest])) {
                            widest = c;
                        }
                    }
                    if (total <= max_px || widest < 0) break;
                    widths[widest]--;
                }
                int body_rows = 0;
                for (int r = 2; r < rows; r++) body_rows++;
                for (int pass = 0; pass < 2; pass++) {
                    bool is_head = pass == 0;
                    int nrows = is_head ? 1 : body_rows;
                    if (nrows == 0) continue;
                    lv_obj_t *tbl = lv_table_create(s_parent);
                    int tw = 18; /* MAIN pad 16 + MAIN border 2 */
                    for (int c = 0; c < cols; c++) {
                        tw += widths[c] * glyph_adv + 2 * MD_CELL_PAD;
                    }
                    lv_obj_set_size(tbl, tw, LV_SIZE_CONTENT);
                    lv_obj_set_pos(tbl, 0, y);
                    lv_obj_set_style_bg_color(tbl, p->surface, 0);
                    lv_obj_set_style_bg_opa(tbl, LV_OPA_COVER, 0);
                    lv_obj_set_style_border_color(tbl, p->surface_border, 0);
                    lv_obj_set_style_border_width(tbl, 1, 0);
                    lv_obj_set_style_radius(tbl, 4, 0);
                    lv_obj_set_style_pad_all(tbl, 8, 0);
                    lv_obj_set_style_bg_color(tbl, is_head ? p->surface_active : p->surface,
                                               LV_PART_ITEMS);
                    lv_obj_set_style_bg_opa(tbl, LV_OPA_COVER, LV_PART_ITEMS);
                    lv_obj_set_style_text_font(tbl, &lv_font_nimbus_mono_14,
                                               LV_PART_ITEMS);
                    lv_obj_set_style_text_color(tbl,
                        is_head ? p->accent_primary : p->text_primary,
                        LV_PART_ITEMS);
                    lv_obj_set_style_border_color(tbl, p->surface_border,
                                                  LV_PART_ITEMS);
                    lv_obj_set_style_border_width(tbl, 1, LV_PART_ITEMS);
                    lv_obj_set_style_border_side(tbl, LV_BORDER_SIDE_FULL,
                                                 LV_PART_ITEMS);
                    lv_obj_set_style_pad_left(tbl, MD_CELL_PAD, LV_PART_ITEMS);
                    lv_obj_set_style_pad_right(tbl, MD_CELL_PAD, LV_PART_ITEMS);
                    lv_obj_set_style_pad_top(tbl, 4, LV_PART_ITEMS);
                    lv_obj_set_style_pad_bottom(tbl, 4, LV_PART_ITEMS);
                    lv_obj_clear_flag(tbl, LV_OBJ_FLAG_SCROLLABLE);
                    lv_table_set_column_count(tbl, (uint32_t)cols);
                    lv_table_set_row_count(tbl, (uint32_t)nrows);
                    for (int c = 0; c < cols; c++) {
                        lv_table_set_column_width(tbl, (uint32_t)c,
                            (int32_t)(widths[c] * glyph_adv + 2 * MD_CELL_PAD));
                    }
                    for (int r = 0; r < nrows; r++) {
                        int src = is_head ? 0 : r + 2;
                        for (int c = 0; c < cols; c++) {
                            /* ponytail: LVGL table cells are plain text,
                             * so strip markers (no spans to break) */
                            const char *cell = c < rowcounts[src] ? cells[src][c] : "";
                            md_strip_inline(cell, strlen(cell),
                                            rowbuf, sizeof(rowbuf));
                            char padded[MD_CELL_MAX + 1];
                            md_pad_string(rowbuf, widths[c], aligns[c],
                                          padded, sizeof(padded));
                            lv_table_set_cell_value(tbl, (uint32_t)r,
                                                    (uint32_t)c, padded);
                        }
                    }
                    lv_obj_update_layout(tbl);
                    y += lv_obj_get_height(tbl) + (is_head ? 6 : 8);
                }
                in_ol = false;
                if (!tt) break; /* group ran to end of buffer */
                cur = tt;
                continue;
            }
            /* else fall through: |line| without a table renders as paragraph */
            }
        }

        /* Quote (one level stripped, rest inline) */
        if (*t == '>') {
            const char *c = t + 1;
            if (*c == ' ' || *c == '\t') c++;
            lv_obj_t *sg = md_new_block(s_parent, y);
            md_add_span(sg, "> ", 2, &lv_font_montserrat_14,
                        p->accent_secondary, LV_TEXT_DECOR_NONE);
            md_render_inline(sg, c, strlen(c), &lv_font_montserrat_14,
                             p->accent_secondary, LV_TEXT_DECOR_NONE, p);
            md_finish_block(sg, &y, 6);
            in_ol = false;
            MD_ADVANCE_LINE();
        }

        /* Task list */
        if ((strncmp(t, "- [ ]", 5) == 0 || strncmp(t, "* [ ]", 5) == 0 ||
             strncmp(t, "+ [ ]", 5) == 0) ||
            (strncmp(t, "- [x]", 5) == 0 || strncmp(t, "- [X]", 5) == 0 ||
             strncmp(t, "* [x]", 5) == 0 || strncmp(t, "* [X]", 5) == 0 ||
             strncmp(t, "+ [x]", 5) == 0 || strncmp(t, "+ [X]", 5) == 0)) {
            bool done = t[3] == 'x' || t[3] == 'X';
            lv_obj_t *sg = md_new_block(s_parent, y);
            lv_color_t col = done ? p->accent_secondary : p->text_secondary;
            md_render_inline(sg, t, strlen(t), &lv_font_montserrat_14, col,
                             LV_TEXT_DECOR_NONE, p);
            md_finish_block(sg, &y, 6);
            in_ol = false;
            MD_ADVANCE_LINE();
        }

        /* Unordered list (- * +) */
        if (((*t == '-' || *t == '*' || *t == '+')) &&
            (t[1] == ' ' || t[1] == '\t')) {
            lv_obj_t *sg = md_new_block(s_parent, y);
            md_add_span(sg, "- ", 2, &lv_font_montserrat_14, p->text_primary,
                        LV_TEXT_DECOR_NONE);
            md_render_inline(sg, t + 2, strlen(t + 2), &lv_font_montserrat_14,
                             p->text_primary, LV_TEXT_DECOR_NONE, p);
            md_finish_block(sg, &y, 6);
            in_ol = false;
            MD_ADVANCE_LINE();
        }

        /* Ordered list (renumbered) */
        {
            const char *c = t;
            while (isdigit((unsigned char)*c)) c++;
            if (c > t && (*c == '.' || *c == ')') &&
                (c[1] == ' ' || c[1] == '\t')) {
                if (!in_ol) { in_ol = true; ol_num = 1; }
                const char *body = c + 1;
                while (*body == ' ' || *body == '\t') body++;
                char num[16];
                int numlen = snprintf(num, sizeof(num), "%d. ", ol_num++);
                lv_obj_t *sg = md_new_block(s_parent, y);
                md_add_span(sg, num, (size_t)numlen, &lv_font_montserrat_14,
                            p->text_primary, LV_TEXT_DECOR_NONE);
                md_render_inline(sg, body, strlen(body), &lv_font_montserrat_14,
                                 p->text_primary, LV_TEXT_DECOR_NONE, p);
                md_finish_block(sg, &y, 6);
                MD_ADVANCE_LINE();
            }
            in_ol = false;
        }

        /* Paragraph */
        {
            lv_obj_t *sg = md_new_block(s_parent, y);
            md_render_inline(sg, line, len, &lv_font_montserrat_14,
                             p->text_primary, LV_TEXT_DECOR_NONE, p);
            md_finish_block(sg, &y, 6);
            in_ol = false; /* a paragraph breaks list continuation */
            MD_ADVANCE_LINE();
        }
    }

md_done:
    if (in_code) md_emit_code_block(&y, p); /* unclosed fence still renders */

    return y;
}
