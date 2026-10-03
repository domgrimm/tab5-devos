#pragma once

/* jobs_internal: shared engine state for the Jobs scheduler and interpreter.
 * Not part of the public API; the engine has no LVGL dependency. */

#include "devos_jobs.h"
#include "devos_actions.h"
#include "jobs_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JOBS_MAX_FRAMES   16
#define JOBS_MAX_STEPS    256
#define JOBS_RUN_ID_MAX   20
#define JOBS_VAR_NAME_MAX 40
#define JOBS_RUN_STRPOOL  1024

/* ---- interpreter frames ---- */
typedef enum { FRAME_BLOCK = 0, FRAME_ACTION, FRAME_WAIT } jobs_frame_kind_t;

typedef struct {
    uint8_t kind;
    const jobs_node_t *block;       /* FRAME_BLOCK: the block being walked */
    const jobs_node_t *cursor;      /* next statement in that block */
    const jobs_node_t *action;      /* FRAME_ACTION: the action node */
    devos_action_handle_t op;
    int64_t wake_ms;                /* FRAME_WAIT */
} jobs_frame_t;

typedef struct {
    char name[JOBS_VAR_NAME_MAX];
    devos_value_t v;
} jobs_var_t;

typedef struct {
    bool active;
    bool cancelling;
    bool finished;
    bool ok;
    char run_id[JOBS_RUN_ID_MAX];
    char message[64];
    const jobs_ast_t *ast;          /* retained for the run's lifetime */
    uint32_t revision;
    int64_t started_ms, deadline_ms;
    int steps;
    jobs_frame_t frames[JOBS_MAX_FRAMES];
    int nframes;
    jobs_var_t vars[JOBS_MAX_VARS];
    int nvars;
    char strpool[JOBS_RUN_STRPOOL];   /* interpolated/output strings, run-scoped */
    size_t strpool_used;
    const jobs_node_t *cur;         /* current node, for the UI/trace */
} jobs_run_t;

typedef struct {
    bool used;
    char id[DEVOS_JOBS_ID_MAX];
    char name[DEVOS_JOBS_NAME_MAX];
    bool enabled;
    uint32_t revision;
    jobs_ast_t *ast;                /* engine's reference */
    int trigger_kind;               /* jobs_trigger_kind_t */
    int64_t interval_ms;
    int64_t next_due_ms;
    bool has_next;
    jobs_run_t run;
    char last_result[64];
    int64_t last_run_ms;
    uint32_t skipped;               /* coalesced triggers */
} jobs_job_t;

typedef struct {
    jobs_job_t jobs[DEVOS_JOBS_MAX];
    int count;
    bool paused;
    bool ready;
    devos_jobs_state_t state;
    devos_jobs_system_t sys;
    uint32_t run_seq;
} jobs_engine_t;

extern jobs_engine_t g_jobs;

/* jobs_runtime.c: begin a run, advance it one tick, cancel it. */
void jobs_run_begin(jobs_job_t *j, const char *run_id, int64_t now_ms, uint32_t revision);
void jobs_run_tick(jobs_job_t *j, int64_t now_ms);
void jobs_run_cancel(jobs_job_t *j);

/* jobs_schedule.c */
jobs_job_t *jobs_find(const char *id);

#ifdef __cplusplus
}
#endif
