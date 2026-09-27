#pragma once

/* devos_qr: scan a QR code with the Tab5 camera (quirc decoder).
 *
 * start() turns the camera on and runs a scan task (network core; a pthread
 * in the simulator): each frame's centre square is decoded at full
 * resolution, mirrored codes included, and a small preview is kept for the
 * UI. It stops by itself once a code is read; take_result() hands it over.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_QR_PREVIEW   360      /* preview is DEVOS_QR_PREVIEW^2 greyscale */
#define DEVOS_QR_TEXT_MAX  3072

typedef enum {
    DEVOS_QR_IDLE = 0,
    DEVOS_QR_SCANNING,
    DEVOS_QR_FOUND,                 /* a code is waiting in take_result() */
    DEVOS_QR_ERROR,                 /* camera problem: see devos_qr_error() */
} devos_qr_state_t;

int devos_qr_start(void);           /* 0 ok, -1 (see devos_qr_error()) */
void devos_qr_stop(void);
devos_qr_state_t devos_qr_state(void);
const char *devos_qr_error(void);
/* The decoded text (NUL-terminated); returns false if none is waiting. */
bool devos_qr_take_result(char *out, size_t cap);
/* Copy the latest preview into out (DEVOS_QR_PREVIEW^2 bytes); returns a
 * counter that changes with every new frame (0 = none yet). */
uint32_t devos_qr_preview(uint8_t *out);

#ifdef __cplusplus
}
#endif
