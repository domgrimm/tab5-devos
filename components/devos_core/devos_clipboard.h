#pragma once

/* devos_clipboard: the one Universal Clipboard (AGENTS.md #7). Copy puts text
 * here, paste takes it out; nothing keeps a private clipboard. Kept in PSRAM,
 * up to DEVOS_CLIPBOARD_MAX bytes.
 *
 * This header is deliberately free of LVGL (unlike devos_core.h) so engines
 * with no UI - devos_fileshare, whose web page pastes text in - can use the
 * clipboard too. devos_core.h includes it, so an app needs only devos_core.h.
 *
 * set() is safe from any task that isn't the LVGL task touching the clipboard
 * at the same moment; the UI task is the only reader/writer on the device. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_CLIPBOARD_MAX (64 * 1024)

/* Replace the clipboard with `text` (up to DEVOS_CLIPBOARD_MAX bytes; longer
 * is truncated). Returns the length kept: 0 on failure or for "". */
size_t devos_clipboard_set(const char *text, size_t len);
/* The clipboard text (never NULL). *len, if given, is its length. */
const char *devos_clipboard_get(size_t *len);

#ifdef __cplusplus
}
#endif
