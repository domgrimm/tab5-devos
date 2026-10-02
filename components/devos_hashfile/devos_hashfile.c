/* devos_hashfile: see devos_hashfile.h. No LVGL. */
#include "devos_hashfile.h"
#include "devos_crypto.h"
#include "devos_config.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK (16 * 1024)               /* read block: small enough for internal RAM */

static char s_path[256];
static char s_hex[65];
static char s_err[96];
static volatile devos_hashfile_state_t s_state = DEVOS_HASHFILE_IDLE;
static volatile int s_pct = -1;
static volatile uint64_t s_bytes, s_total;
static int s_algo;
static bool s_task_up;

#ifdef ESP_PLATFORM

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

static TaskHandle_t s_task;

/* The read buffer: a file on the SD card is streamed, so 16 KB is plenty and
 * it can live in PSRAM (AGENTS.md #2 - only the worker's small stack needs
 * internal RAM). */
static void worker(void *arg)
{
    (void)arg;
    FILE *f = fopen(s_path, "rb");
    if (!f) {
        snprintf(s_err, sizeof(s_err), "Could not open the file");
        s_state = DEVOS_HASHFILE_FAILED;
        s_task_up = false;
        vTaskDelete(NULL);
        return;
    }
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, 0, SEEK_SET);
    s_total = end > 0 ? (uint64_t)end : 0;

    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(CHUNK);
    union { max_align_t a; char b[512]; } ctx_store;
    devos_hash_ctx_t *ctx = devos_hash_begin((devos_hash_t)s_algo, (devos_hash_ctx_t *)ctx_store.b);

    size_t got;
    s_bytes = 0;
    while (buf && (got = fread(buf, 1, CHUNK, f)) > 0) {
        devos_hash_update(ctx, buf, got);
        s_bytes += got;
        s_pct = s_total ? (int)(s_bytes * 100 / s_total) : 0;
    }
    uint8_t digest[64];
    if (buf) devos_hash_end(ctx, digest);
    free(buf);
    bool rerr = ferror(f);
    fclose(f);

    if (!buf) { snprintf(s_err, sizeof(s_err), "Out of memory"); s_state = DEVOS_HASHFILE_FAILED; }
    else if (rerr) { snprintf(s_err, sizeof(s_err), "Read error"); s_state = DEVOS_HASHFILE_FAILED; }
    else {
        devos_hex_encode(digest, devos_hash_len((devos_hash_t)s_algo), s_hex, sizeof(s_hex));
        s_pct = 100;
        s_state = DEVOS_HASHFILE_DONE;
    }
    s_task_up = false;
    vTaskDelete(NULL);
}

int devos_hashfile_start(const char *path, int algo)
{
    if (s_state == DEVOS_HASHFILE_RUNNING) return -1;
    if (!path || !*path) return -1;
    snprintf(s_path, sizeof(s_path), "%s", path);
    s_algo = algo;
    s_hex[0] = s_err[0] = '\0';
    s_pct = 0;
    s_bytes = s_total = 0;
    s_state = DEVOS_HASHFILE_RUNNING;
    s_task_up = true;
    if (xTaskCreatePinnedToCore(worker, "hashfile", 4096, NULL, 3, &s_task, DEVOS_CORE_NET_CRYPTO) != pdPASS) {
        s_task_up = false;
        snprintf(s_err, sizeof(s_err), "Could not start the worker");
        s_state = DEVOS_HASHFILE_FAILED;
        return -1;
    }
    return 0;
}

#else /* simulator: synchronous, no threads needed for a host test */

int devos_hashfile_start(const char *path, int algo)
{
    if (s_state == DEVOS_HASHFILE_RUNNING) return -1;
    snprintf(s_path, sizeof(s_path), "%s", path);
    s_algo = algo;
    s_hex[0] = s_err[0] = '\0';
    s_bytes = s_total = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(s_err, sizeof(s_err), "Could not open the file"); s_state = DEVOS_HASHFILE_FAILED; return -1; }
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, 0, SEEK_SET);
    s_total = end > 0 ? (uint64_t)end : 0;
    union { max_align_t a; char b[512]; } ctx_store;
    devos_hash_ctx_t *ctx = devos_hash_begin((devos_hash_t)algo, (devos_hash_ctx_t *)ctx_store.b);
    uint8_t *buf = malloc(CHUNK);
    size_t got;
    while (buf && (got = fread(buf, 1, CHUNK, f)) > 0) { devos_hash_update(ctx, buf, got); s_bytes += got; }
    uint8_t digest[64];
    if (buf) devos_hash_end(ctx, digest);
    free(buf);
    fclose(f);
    if (!buf) { snprintf(s_err, sizeof(s_err), "Out of memory"); s_state = DEVOS_HASHFILE_FAILED; return -1; }
    devos_hex_encode(digest, devos_hash_len((devos_hash_t)algo), s_hex, sizeof(s_hex));
    s_pct = 100;
    s_state = DEVOS_HASHFILE_DONE;
    return 0;
}

#endif

devos_hashfile_state_t devos_hashfile_state(void) { return s_state; }
int devos_hashfile_progress(void) { return s_state == DEVOS_HASHFILE_RUNNING ? s_pct : -1; }
const char *devos_hashfile_result(void) { return s_state == DEVOS_HASHFILE_DONE ? s_hex : ""; }
const char *devos_hashfile_error(void) { return s_err; }
uint64_t devos_hashfile_bytes(void) { return s_bytes; }
uint64_t devos_hashfile_total(void) { return s_total; }
