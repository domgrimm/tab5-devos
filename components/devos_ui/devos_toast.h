#pragma once

/* devos_toast: short notices just below the top bar ("Copied the code",
 * "Wi-Fi connected to HomeWiFi"), over whatever is on screen, gone after a
 * couple of seconds. Taps go through them. A newer toast replaces the one
 * showing.
 *
 * devos_toast_show() can be called from any task: engines on core 0 too. It
 * only queues the text; the UI task shows it within ~50 ms.
 *
 * devos_toast_watch() (main's 1 Hz loop) raises the system ones from the
 * telemetry: Wi-Fi connected / lost, Tailscale and WireGuard up / down, a
 * low battery. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEVOS_TOAST_INFO = 0,
    DEVOS_TOAST_OK,         /* done: copied, saved, connected */
    DEVOS_TOAST_WARN,       /* didn't happen, or needs attention */
    DEVOS_TOAST_ERROR,
} devos_toast_type_t;

#define DEVOS_TOAST_DEFAULT_MS 2000

/* Start the UI side (after the top bar exists). Toasts sent before are kept. */
void devos_toast_init(void);
/* duration_ms 0 = DEVOS_TOAST_DEFAULT_MS. */
void devos_toast_show(const char *msg, devos_toast_type_t type, uint32_t duration_ms);
void devos_toast_watch(void);

#ifdef __cplusplus
}
#endif
