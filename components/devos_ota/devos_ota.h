#pragma once

/* devos_ota: over-the-air firmware updates.
 *
 * Manifest (JSON, http:// or https:// feed URL):
 *   {"version": "0.2.0", "url": "http://host:8090/tab5-devos.bin",
 *    "size": 1854432, "sha256": "<64 hex chars>", "notes": "What's new"}
 * Relative "url" values are resolved against the feed URL. Generate one with
 * tools/make_ota_manifest.py.
 *
 * Checking and installing run on a background task (never blocks the UI):
 * the image is streamed into the spare OTA slot, SHA-256 checked against the
 * manifest, validated by ESP-IDF, then booted. Only newer versions install.
 * With rollback enabled in the bootloader, a new image must call
 * devos_ota_mark_boot_ok() (main does, once the UI is up) or the next reset
 * returns to the previous firmware. The simulator only checks (dry run).
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_OTA_FEED_MAX 160

typedef enum {
    DEVOS_OTA_IDLE = 0,         /* never checked */
    DEVOS_OTA_CHECKING,
    DEVOS_OTA_UP_TO_DATE,
    DEVOS_OTA_AVAILABLE,        /* newer version found: devos_ota_apply() */
    DEVOS_OTA_DOWNLOADING,      /* see devos_ota_progress() */
    DEVOS_OTA_VERIFYING,
    DEVOS_OTA_REBOOTING,
    DEVOS_OTA_FAILED,           /* see devos_ota_update_text() */
} devos_ota_state_t;

void devos_ota_init(void);
/* Start a feed check in the background. 0 = started, <0 = busy / bad URL. */
int devos_ota_check(void);
/* Start installing the available update. 0 = started. */
int devos_ota_apply(void);
bool devos_ota_busy(void);
bool devos_ota_has_update(void);
devos_ota_state_t devos_ota_state(void);
/* Download progress 0..100 (-1 when not downloading). */
int devos_ota_progress(void);
/* One-line human status ("Up to date (v0.1.0)", "Downloading 42%"...). */
const char *devos_ota_update_text(void);
/* Available version and release notes ("" if none). */
const char *devos_ota_available_version(void);
const char *devos_ota_notes(void);
/* Confirm this firmware booted fine (cancels a pending rollback). */
void devos_ota_mark_boot_ok(void);

void devos_ota_get_feed(char *out, size_t len);
int devos_ota_set_feed(const char *url);

/* "devOS v0.1.0" / "0.1.0" -> 0x000100; -1 if unparseable. Exposed for tests. */
long devos_ota_parse_version(const char *s);
/* Which build is running: "build 8dc91853, Sep 27 2026 08:46" (the ELF hash
 * esptool stamps into the image, and its build time). A manifest "build"
 * matching it means this exact image is already installed; a different
 * build of the same version is offered as an update. */
const char *devos_ota_build_text(void);

#ifdef __cplusplus
}
#endif
