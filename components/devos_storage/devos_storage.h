#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize storage subsystem and attempt mount */
bool devos_storage_init(void);

/* Automatic MicroSD Scaffolding & Starter Template Generation */
bool devos_storage_bootstrap(const char *mount_point);

/* Get storage stats */
bool devos_storage_is_mounted(void);
uint32_t devos_storage_get_total_mb(void);
uint32_t devos_storage_get_free_mb(void);
/* Re-read total/free space from the filesystem (slow on first call for large
 * FAT volumes: call from a background task). */
bool devos_storage_refresh_stats(void);

#ifdef __cplusplus
}
#endif
