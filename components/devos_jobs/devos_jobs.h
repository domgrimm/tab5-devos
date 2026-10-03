#pragma once

/* devos_jobs: the Jobs engine - a persistent, keyboard-first automation
 * system (PLAN.md Jobs section). It parses one definition per job, validates
 * it against the action/event schemas, schedules triggers on a monotonic
 * clock, and executes the AST with request-specific provider operations.
 *
 * Engine rules (AGENTS.md):
 *   - no LVGL and no devos_core.h here: the parser/validator/serializer are
 *     host-tested; the scheduler talks to the UI through bounded snapshots.
 *   - scheduler runs on Core 0; source/history I/O on a Core 1 storage worker.
 *   - the engine is off (getters safe, nothing scheduled) when the app is
 *     switched off in Settings > Apps.
 *
 * CONTRACT STATUS (Phase 0/1): the model/parser/validator/serializer exist and
 * are tested. The lifecycle, scheduler and command surface below are declared
 * here and implemented in Phases 3-5; callers must treat DEVOS_JOBS_OFF as the
 * safe answer until then. */

#include "devos_err.h"
#include "jobs_model.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_JOBS_MAX      32
#define DEVOS_JOBS_ID_MAX   40
#define DEVOS_JOBS_NAME_MAX 64

typedef enum {
    DEVOS_JOBS_OFF = 0,      /* disabled or not initialised */
    DEVOS_JOBS_LOADING,      /* reading definitions, not dispatching */
    DEVOS_JOBS_READY,        /* loaded; dispatching per enabled jobs */
} devos_jobs_state_t;

typedef enum {
    DEVOS_JOB_DISABLED = 0,
    DEVOS_JOB_ENABLED,
    DEVOS_JOB_BLOCKED,       /* enabled but a provider/secret is unavailable */
} devos_job_state_t;

typedef struct {
    char id[DEVOS_JOBS_ID_MAX];
    char name[DEVOS_JOBS_NAME_MAX];
    devos_job_state_t state;
    char trigger[48];        /* "every 5m", "daily 08:00", ... */
    char last_result[48];    /* "ok 3s ago", "failed: ..." */
    bool running;
    uint32_t revision;       /* active definition revision */
} devos_job_summary_t;

typedef struct {
    bool active;
    char job_id[DEVOS_JOBS_ID_MAX];
    char run_id[24];
    int64_t elapsed_ms;
    int node_id;             /* current node, maps to a source span */
    bool cancelling;
} devos_jobs_run_t;

/* ---- lifecycle ---- */
/* Called by main only when the Jobs app is enabled; false means the engine
 * stayed off (getters then report OFF). */
bool devos_jobs_init(void);
void devos_jobs_shutdown(void);
devos_jobs_state_t devos_jobs_state(void);
/* Global automatic pause (persisted); Run now still works. */
bool devos_jobs_paused(void);
void devos_jobs_set_paused(bool paused);

/* ---- read-only snapshots (safe when OFF) ---- */
int devos_jobs_count(void);
bool devos_jobs_summary_at(int index, devos_job_summary_t *out);
bool devos_jobs_run(devos_jobs_run_t *out);

/* ---- commands (bounded; return an admission result) ----
 * The UI enqueues these and gets completion through the snapshot API. They do
 * not return pointers into worker-owned memory. */
devos_err_t devos_jobs_apply(const char *id, const char *source, size_t len, uint32_t *out_revision);
devos_err_t devos_jobs_set_enabled(const char *id, bool enabled);
devos_err_t devos_jobs_run_now(const char *id);
devos_err_t devos_jobs_cancel(const char *id);
devos_err_t devos_jobs_delete(const char *id);

#ifdef __cplusplus
}
#endif
