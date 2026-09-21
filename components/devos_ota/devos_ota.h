#pragma once

/* devos_ota: firmware update check (and apply on target).
 *
 * Manifest format (JSON over plain HTTP from the feed URL):
 *   {"version":"devOS v0.2.0","url":"http://host/fw/tab5-devos.bin",
 *    "size":1234567}
 *
 * Flow: devos_ota_check() fetches + compares against DEVOS_VERSION_STR.
 * If newer, devos_ota_update_text() describes it and devos_ota_apply()
 * flashes it — esp_https_ota on target (needs ota_0/ota_1 partitions),
 * dry-run report in simulation. Feed URL persists (NVS on target,
 * JSON file in sim); default points at the dev LAN update shelf.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_OTA_FEED_MAX 128

void devos_ota_init(void);
/* Fetch the feed manifest and compare. 0 = checked (see update_text),
 * <0 = fetch failed. */
int devos_ota_check(void);
bool devos_ota_has_update(void);
const char *devos_ota_update_text(void);
/* Flash the pending update (target) or report the dry run (sim). */
int devos_ota_apply(void);
void devos_ota_get_feed(char *out, size_t len);
int devos_ota_set_feed(const char *url);

#ifdef __cplusplus
}
#endif
