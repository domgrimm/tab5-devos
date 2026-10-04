/* Host check: devos_codeview_diff (pure line diff, no display).
 *
 * Run after touching the differ:
 *
 *   gcc -o /tmp/codeview_diff_test tools/codeview_diff_test.c -I. -Imain/include \
 *     -Icomponents/devos_ui -Icomponents/lvgl \
 *     -Icomponents/devos_core build_sim/lib/liblvgl.a -lm \
 *     && /tmp/codeview_diff_test
 *
 * (Requires a completed `ninja -C build_sim` for liblvgl.a.)
 */
#include <stdio.h>
#include <string.h>

#include "devos_codeview.c"

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
    char out[2048];

    /* identical texts: all context, no +/- */
    int n = devos_codeview_diff("a\nb\n", "a\nb\n", out, sizeof(out));
    CHECK(n > 0 && strstr(out, "  a\n  b\n") != NULL);
    CHECK(strchr(out, '-') == NULL && strchr(out, '+') == NULL);

    /* one changed line */
    n = devos_codeview_diff("a\nb\nc\n", "a\nB\nc\n", out, sizeof(out));
    CHECK(n > 0 && strstr(out, "- b\n") != NULL && strstr(out, "+ B\n") != NULL);
    CHECK(strstr(out, "  a\n") != NULL && strstr(out, "  c\n") != NULL);

    /* insertion and deletion */
    n = devos_codeview_diff("a\nc\n", "a\nb\nc\n", out, sizeof(out));
    CHECK(strstr(out, "+ b\n") != NULL);
    n = devos_codeview_diff("a\nb\nc\n", "a\nc\n", out, sizeof(out));
    CHECK(strstr(out, "- b\n") != NULL);

    /* empty sides */
    n = devos_codeview_diff("", "x\n", out, sizeof(out));
    CHECK(strstr(out, "+ x\n") != NULL);
    n = devos_codeview_diff("x\n", "", out, sizeof(out));
    CHECK(strstr(out, "- x\n") != NULL);
    n = devos_codeview_diff("", "", out, sizeof(out));
    CHECK(n == 0 && out[0] == '\0');

    /* a job-like edit keeps context around the change */
    const char *oldj = "version 1;\njob \"X\" {\n trigger manual;\n system.log(message: \"x\");\n}\n";
    const char *newj = "version 1;\njob \"X\" {\n trigger every 5m;\n system.log(message: \"x\");\n}\n";
    n = devos_codeview_diff(oldj, newj, out, sizeof(out));
    CHECK(strstr(out, "-  trigger manual;\n") != NULL);
    CHECK(strstr(out, "+  trigger every 5m;\n") != NULL);
    CHECK(strstr(out, "   system.log(message: \"x\");\n") != NULL);

    /* tiny buffer never overflows */
    char tiny[32];
    n = devos_codeview_diff(oldj, newj, tiny, sizeof(tiny));
    CHECK(n < (int)sizeof(tiny) && tiny[sizeof(tiny) - 1] == '\0');

    if (failures == 0) printf("OK\n");
    else printf("FAILED: %d\n", failures);
    return failures ? 1 : 0;
}
