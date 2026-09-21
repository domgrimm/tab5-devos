/* Ponytail check: pure markdown helpers from devos_mdview.c (no display).
 *
 * Covers table row splitting, delimiter detection, marker stripping,
 * and emphasis flanking rules. Run after touching the renderer:
 *
 *   gcc -o /tmp/md_preview_test tools/md_preview_test.c -I. -Imain/include \
 *     -Icomponents/devos_mdview -Icomponents/lvgl \
 *     -Icomponents/devos_core -Icomponents/devos_ui build_sim/lib/liblvgl.a -lm \
 *     && /tmp/md_preview_test
 *
 * (Requires a completed `ninja -C build_sim` for liblvgl.a.)
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "devos_mdview.c"

/* Link stubs for devOS fns referenced by non-test code in the unit */
const lv_font_t lv_font_nimbus_mono_14;
static devos_palette_t stub_palette;
const devos_palette_t *devos_theme_get(void) { return &stub_palette; }
int devos_theme_add_listener(devos_theme_change_cb_t cb, void *ud)
{
    (void)cb; (void)ud; return 0;
}
devos_theme_type_t devos_theme_get_type(void) { return DEVOS_THEME_DARK; }
bool devos_theme_is_dark(void) { return true; }

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } \
} while (0)

int main(void)
{
    /* md_find_marker */
    const char *s = "a **b** c";
    CHECK(md_find_marker(s, strlen(s), "**", 0) == s + 2);
    CHECK(md_find_marker(s, strlen(s), "**", 3) == s + 5);
    CHECK(md_find_marker(s, strlen(s), "~~", 0) == NULL);

    /* md_find_star: skips ** halves and intraword stars */
    const char *s2 = "2*3*4 and *it* end";
    const char *f = md_find_star(s2, strlen(s2), 0);
    CHECK(f != NULL && (size_t)(f - s2) == 10);
    const char *s3 = "**bold**";
    CHECK(md_find_star(s3, strlen(s3), 0) == NULL);

    /* md_uscore_edge */
    CHECK(md_uscore_edge("_a_", 3, 0, true) == true);
    CHECK(md_uscore_edge("a_b", 3, 1, true) == false);
    CHECK(md_uscore_edge("_a_", 3, 2, false) == true);
    CHECK(md_uscore_edge("_a_b", 4, 2, false) == false);

    /* md_split_row */
    char cells[MD_TABLE_COLS][MD_CELL_MAX + 1];
    char row1[] = "| Name | Age | City |";
    CHECK(md_split_row(row1, cells) == 3);
    CHECK(strcmp(cells[0], "Name") == 0);
    CHECK(strcmp(cells[1], "Age") == 0);
    CHECK(strcmp(cells[2], "City") == 0);
    char row1_empty[] = "| Name | | City |";
    CHECK(md_split_row(row1_empty, cells) == 3);
    CHECK(strcmp(cells[0], "Name") == 0);
    CHECK(strcmp(cells[1], "") == 0);
    CHECK(strcmp(cells[2], "City") == 0);
    char row2[] = "|---|---|---|";
    CHECK(md_split_row(row2, cells) == 3);
    int al = -1;
    CHECK(md_is_delim_cell(cells[0], &al) && al == 0);
    char row3[] = "| :--- | :---: | ---: |";
    CHECK(md_split_row(row3, cells) == 3);
    CHECK(md_is_delim_cell(cells[0], &al) && al == 0);
    CHECK(md_is_delim_cell(cells[1], &al) && al == 1);
    CHECK(md_is_delim_cell(cells[2], &al) && al == 2);
    CHECK(!md_is_delim_cell("Name", &al));
    CHECK(!md_is_delim_cell("", &al));
    CHECK(!md_is_delim_cell(":-x", &al));

    /* md_strip_inline */
    char out[256];
    const char *in1 = "**bold** and *it* and ~~s~~ and `c` done";
    md_strip_inline(in1, strlen(in1), out, sizeof(out));
    CHECK(strcmp(out, "bold and it and s and c done") == 0);
    const char *in2 = "[text](http://x.y \"t\") tail";
    md_strip_inline(in2, strlen(in2), out, sizeof(out));
    CHECK(strcmp(out, "text (http://x.y) tail") == 0);
    const char *in2b = "- [ ] see [link](http://x.y) tail";
    md_strip_inline(in2b, strlen(in2b), out, sizeof(out));
    CHECK(strcmp(out, "- [ ] see link (http://x.y) tail") == 0);
    const char *in3 = "2*3*4 stays";
    md_strip_inline(in3, strlen(in3), out, sizeof(out));
    CHECK(strcmp(out, "2*3*4 stays") == 0);

    /* md_link_end: balanced parens, <dest>, titles */
    const char *u = NULL;
    size_t ul = 0;
    const char *lu = "http://x/wiki/A_(B) \"t\") tail";
    const char *le = md_link_end(lu, strlen(lu), &u, &ul);
    CHECK(le != NULL && ul == 19 && memcmp(u, "http://x/wiki/A_(B)", 19) == 0);
    const char *lu2 = "<http://x.y/a b>\") tail";
    le = md_link_end(lu2, strlen(lu2), &u, &ul);
    CHECK(le != NULL && ul == 14 && memcmp(u, "http://x.y/a b", 14) == 0);
    const char *lu3 = "http://x.y) tail";
    le = md_link_end(lu3, strlen(lu3), &u, &ul);
    CHECK(le != NULL && ul == 10);
    CHECK(md_link_end("http://x.y tail", 14, &u, &ul) == NULL);

    /* md_is_delim_line with and without outer pipes */
    CHECK(md_is_delim_line("| --- | :---: | ---: |") == true);
    CHECK(md_is_delim_line("--- | :---: | ---:") == true);
    CHECK(md_is_delim_line("---") == true);
    CHECK(md_is_delim_line("| a | b |") == false);
    CHECK(md_is_delim_line("") == false);

    /* devos_md_trunc_ok never splits a codepoint: "é" = C3 A9 */
    const char e2[] = {'a', (char)0xC3, (char)0xA9, 'b', '\0'};
    CHECK(devos_md_trunc_ok(e2, 3) == 3);
    CHECK(devos_md_trunc_ok(e2, 2) == 1);
    CHECK(devos_md_trunc_ok(e2, 1) == 1);
    CHECK(devos_md_trunc_ok(e2, 0) == 0);

    /* 3-byte UTF-8: Euro sign "\xE2\x82\xAC" */
    const char e3[] = {'a', (char)0xE2, (char)0x82, (char)0xAC, 'b', '\0'};
    CHECK(devos_md_trunc_ok(e3, 4) == 4);
    CHECK(devos_md_trunc_ok(e3, 3) == 1);
    CHECK(devos_md_trunc_ok(e3, 2) == 1);
    CHECK(devos_md_trunc_ok(e3, 1) == 1);
    CHECK(devos_md_trunc_ok(e3, 0) == 0);
    const char e3_sub[] = {'a', (char)0xE2, (char)0x82, '\0'};
    CHECK(devos_md_trunc_ok(e3_sub, 3) == 1);

    /* 4-byte UTF-8: Emoji "\xF0\x9F\x9A\x80" */
    const char e4[] = {'x', (char)0xF0, (char)0x9F, (char)0x9A, (char)0x80, 'y', '\0'};
    CHECK(devos_md_trunc_ok(e4, 5) == 5);
    CHECK(devos_md_trunc_ok(e4, 4) == 1);
    CHECK(devos_md_trunc_ok(e4, 3) == 1);
    CHECK(devos_md_trunc_ok(e4, 2) == 1);
    CHECK(devos_md_trunc_ok(e4, 1) == 1);

    if (failures == 0) printf("md unit tests: ALL PASS\n");
    return failures != 0;
}
