#pragma once

/* devos_hashfile: hash a file on the SD card (or anywhere on the VFS) without
 * holding it in memory. Streams it in chunks on a Core 0 worker and reports
 * progress, so the UI task never blocks on a multi-MB read (AGENTS.md #1).
 *
 * One job at a time. Start with devos_hashfile_start(), poll devos_hashfile_state()
 * while it runs, and read the digest with devos_hashfile_result() when it
 * reports DONE. The worker is created on first use and kept for later jobs. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEVOS_HASHFILE_IDLE = 0,
    DEVOS_HASHFILE_RUNNING,
    DEVOS_HASHFILE_DONE,
    DEVOS_HASHFILE_FAILED,
} devos_hashfile_state_t;

/* Start hashing `path`. algo is a devos_hash_t value (1 SHA-1, 2 SHA-256,
 * 3 SHA-512, 4 SHA-384). Returns 0 if the job started, <0 if one is already
 * running or the worker could not start. */
int devos_hashfile_start(const char *path, int algo);
devos_hashfile_state_t devos_hashfile_state(void);
/* 0..100 while running, -1 otherwise. */
int devos_hashfile_progress(void);
/* Hex digest, "" until DEVOS_HASHFILE_DONE. */
const char *devos_hashfile_result(void);
/* Why it failed, "" otherwise. */
const char *devos_hashfile_error(void);
/* Bytes hashed / the file size, for the status line. */
uint64_t devos_hashfile_bytes(void);
uint64_t devos_hashfile_total(void);

#ifdef __cplusplus
}
#endif
