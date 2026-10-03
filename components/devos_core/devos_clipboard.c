/* devos_clipboard: see devos_clipboard.h. LVGL-free, so non-UI engines
 * (devos_fileshare) can build it too. */
#include "devos_clipboard.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

static char *s_text;
static size_t s_len;

size_t devos_clipboard_set(const char *text, size_t len)
{
    if (!text) len = 0;
    if (len > DEVOS_CLIPBOARD_MAX) len = DEVOS_CLIPBOARD_MAX;
#ifdef ESP_PLATFORM
    char *n = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   /* keep internal RAM free */
    if (!n) n = malloc(len + 1);
#else
    char *n = malloc(len + 1);
#endif
    if (!n) return 0;
    if (len) memcpy(n, text, len);
    n[len] = '\0';
    free(s_text);
    s_text = n;
    s_len = strlen(n);                  /* stops at an embedded NUL */
    return s_len;
}

const char *devos_clipboard_get(size_t *len)
{
    if (len) *len = s_text ? s_len : 0;
    return s_text ? s_text : "";
}
