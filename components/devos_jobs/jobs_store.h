#pragma once

/* jobs_store: durable, transactional storage for Jobs definitions
 * (PLAN.md 5.2/5.3). No LVGL. The authority is the versioned catalog plus
 * immutable revision files; the public `jobs/<id>.job` is a projection that
 * an external editor may change (a candidate, never auto-executed).
 *
 * Layout under the SD root:
 *   jobs/<id>.job                          public projection (editable)
 *   jobs/examples/<name>.job                disabled starters
 *   .devos/jobs/catalog.json               schema, ids, active revs, enabled
 *   .devos/jobs/catalog.json.prev          previous good catalog (recovery)
 *   .devos/jobs/revisions/<id>/<rev>.job   immutable generations
 *   .devos/jobs/history/<id>.jsonl         bounded trace, rotated
 *
 * Writes go to a `.tmp` sibling, are closed, then renamed into place; the
 * catalog is rotated through `.prev` so a torn write recovers to the last
 * complete generation. FAT is not power-loss atomic, so load() selects a
 * complete, valid generation with previous fallback rather than trusting one
 * file. A failed write marks the store degraded and leaves the old catalog
 * intact. */

#include "devos_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JOBS_STORE_ID_MAX 40

typedef struct {
    bool available;         /* the store directories exist / are usable */
    bool degraded;          /* a write failed; running RAM-only */
    char error[96];
    int jobs;               /* catalog entries */
} jobs_store_status_t;

/* Ensure the directory tree and load the catalog into memory. root NULL uses
 * TAB5_SD_MOUNT_POINT. Returns false (status.error) when the card is absent. */
bool jobs_store_init(const char *root);
void jobs_store_status(jobs_store_status_t *out);
bool jobs_store_available(void);

/* One accepted source for a job, newest-first with previous fallback. The
 * callback parses/validates and returns true to accept. */
typedef bool (*jobs_store_load_cb)(const char *id, const char *source, size_t len,
                                   bool enabled, uint32_t revision, void *user);
/* Load every catalog job's active revision (with fallback). Returns how many
 * were accepted. */
int jobs_store_load(jobs_store_load_cb cb, void *user);

/* Commit a validated revision: write the immutable generation, then rotate the
 * catalog. Returns DEVOS_OK, or an error leaving the previous catalog intact. */
devos_err_t jobs_store_commit(const char *id, const char *source, size_t len,
                              uint32_t revision, bool enabled);
devos_err_t jobs_store_set_enabled(const char *id, bool enabled);
devos_err_t jobs_store_remove(const char *id);
/* Append a bounded history line, rotating the segment when it grows too large. */
devos_err_t jobs_store_history_append(const char *id, const char *line);

/* Tests: fail the Nth filesystem step (0 = never). */
void jobs_store_fail_after(int step);
void jobs_store_reset_fail(void);

#ifdef __cplusplus
}
#endif
