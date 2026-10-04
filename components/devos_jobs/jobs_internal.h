#pragma once

/* jobs_internal: shared engine state for the Jobs scheduler and interpreter.
 * Not part of the public API; the engine has no LVGL dependency. */

#include "devos_jobs.h"
#include "devos_actions.h"
#include "devos_events.h"
#include "jobs_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JOBS_MAX_FRAMES   16
#define JOBS_MAX_STEPS    256
#define JOBS_RUN_ID_MAX   20
#define JOBS_VAR_NAME_MAX 40
#define JOBS_RUN_STRPOOL  1024
#define JOBS_SECRET_SCRATCH 1024   /* resolved credential copies, wiped after use */
#define JOBS_TRACE_MAX    128      /* executed steps kept per run, for the trace view */

/* Why a run started (shown in the history and the run detail). */
typedef enum {
    JOBS_CAUSE_NONE = 0,
    JOBS_CAUSE_MANUAL,
    JOBS_CAUSE_SCHEDULE,
    JOBS_CAUSE_EVENT,
    JOBS_CAUSE_QUEUE,          /* coalesced (queue_one) automatic run */
} jobs_cause_t;
const char *jobs_cause_name(jobs_cause_t c);

/* One executed step, snapshotted so it survives the AST release at run end. */
typedef struct {
    uint16_t line, col;
    uint8_t kind;               /* jobs_node_kind_t */
    uint8_t result;             /* 0 ran, 1 condition false, 2 error */
} jobs_trace_t;
#define JOBS_EV_PAYLOAD_MAX 256   /* bounded copy of a triggering event payload */
#define JOBS_SUB_TOPIC_MAX  192   /* MQTT subscription topic (no devos_mqtt dependency) */

/* ---- interpreter frames ---- */
typedef enum { FRAME_BLOCK = 0, FRAME_ACTION, FRAME_WAIT, FRAME_REPEAT, FRAME_CALL } jobs_frame_kind_t;

typedef struct {
    uint8_t kind;
    const jobs_node_t *block;       /* FRAME_BLOCK: the block being walked */
    const jobs_node_t *cursor;      /* next statement in that block */
    const jobs_node_t *action;      /* FRAME_ACTION: the action node */
    const jobs_node_t *repeat;      /* FRAME_REPEAT: the repeat node */
    const jobs_ast_t *call_ast;     /* FRAME_CALL: the callee AST (retained) */
    const char *out_name;           /* FRAME_CALL: the caller's output variable */
    int var_mark;                   /* FRAME_CALL: nvars to restore on return */
    int32_t iter;                   /* FRAME_REPEAT: iterations already started */
    devos_action_handle_t op;
    int64_t wake_ms;                /* FRAME_WAIT */
} jobs_frame_t;

typedef struct {
    char name[JOBS_VAR_NAME_MAX];
    devos_value_t v;
} jobs_var_t;

/* A bounded copy of a triggering event, kept on the job so `where` can read
 * it. Never a pointer into the event queue's reused buffer. */
typedef struct {
    bool pending;
    bool valid;
    char topic[DEVOS_EVENTS_TOPIC_MAX];
    char source[DEVOS_EVENTS_TOPIC_MAX];   /* provider source (MQTT topic) */
    char payload[JOBS_EV_PAYLOAD_MAX];
    uint32_t payload_len;
    uint32_t seq;
    bool truncated;
    bool retain;
    int64_t arrival_ms;
} jobs_pending_event_t;

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
    char secret_scratch[JOBS_SECRET_SCRATCH];  /* resolved credentials; wiped after use */
    size_t secret_used;
    const jobs_node_t *cur;         /* current node, for the UI/trace */
    jobs_pending_event_t ev;        /* triggering event copy, for event.* in the body */
    bool ev_valid;
    jobs_trace_t trace[JOBS_TRACE_MAX];
    int trace_n;
    uint32_t trace_over;            /* steps dropped once the ring filled */
    /* reusable job calls (PLAN.md 9.3) */
    int call_depth;
    char call_chain[JOBS_MAX_CALL_DEPTH][64];
    bool call_value_valid;
    devos_value_t call_value;
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

    /* daily/weekdays (wall clock) */
    int trig_hh, trig_mm;
    int64_t next_wall_s;            /* UTC seconds of the next occurrence */
    bool next_wall_valid;
    int32_t claim_date;             /* local YYYYMMDD already claimed (dedupe) */
    uint32_t seen_tz_gen;           /* tz_generation the deadline was computed for */

    /* event trigger */
    int sub_id;                     /* devos_events subscription, 0 = none */
    char event_topic[DEVOS_EVENTS_TOPIC_MAX];
    char mqtt_topic[JOBS_SUB_TOPIC_MAX];   /* mqtt.message subscription filter, "" = all */
    int mqtt_sub_handle;            /* broker subscription handle, 0 = none */
    bool include_retained;
    int64_t ev_debounce_ms;
    int64_t ev_last_accept_ms;
    jobs_pending_event_t ev;

    /* policy */
    int64_t cooldown_ms;
    int64_t last_start_ms;
    bool overlap_queue_one;
    bool pending_run;               /* queue_one: a trigger arrived while running */

    jobs_run_t run;
    char last_result[64];
    int64_t last_run_ms;
    int64_t last_run_wall_s;        /* unix seconds, 0 when the clock was unset */
    bool last_ok;
    uint8_t last_cause;             /* jobs_cause_t */
    uint32_t skipped;               /* coalesced/skipped triggers */
} jobs_job_t;

typedef struct {
    jobs_job_t jobs[DEVOS_JOBS_MAX];
    int count;
    bool paused;
    bool safe_paused;
    bool stopping;
    bool ready;
    devos_jobs_state_t state;
    devos_jobs_system_t sys;
    uint32_t run_seq;
    /* scratch for evaluating a trigger's `where` before a run exists */
    jobs_run_t trig_run;
    jobs_pending_event_t cur_event; /* immutable copy visible to event.* refs */
    bool cur_event_valid;
    devos_jobs_offset_fn offset_fn; /* DST-correct local offset, optional */
    void *offset_user;
    devos_jobs_mqtt_sub_fn mqtt_sub;      /* installed by the boot bridge */
    devos_jobs_mqtt_unsub_fn mqtt_unsub;
    void *mqtt_user;
    devos_jobs_secret_fn secret_resolve;  /* installed by the boot bridge */
    devos_jobs_secret_wipe_fn secret_wipe;
    void *secret_user;
} jobs_engine_t;

extern jobs_engine_t g_jobs;

/* jobs_runtime.c: begin a run, advance it one tick, cancel it. */
void jobs_run_begin(jobs_job_t *j, const char *run_id, int64_t now_ms, uint32_t revision,
                    jobs_cause_t cause);
void jobs_run_tick(jobs_job_t *j, int64_t now_ms);
void jobs_run_cancel(jobs_job_t *j);
/* Evaluate an event trigger's optional `where` against g_jobs.cur_event.
 * true when it matches (or there is no `where`). */
bool jobs_trigger_where_matches(jobs_job_t *j);
/* Read the portable policy: timeout / cooldown (ms) and overlap==queue_one. */
void jobs_policy_read(const jobs_ast_t *ast, int64_t *timeout_ms, int64_t *cooldown_ms, bool *queue_one);

/* jobs_schedule.c */
jobs_job_t *jobs_find(const char *id);
/* (Re)compute a daily/weekdays job's next wall-clock occurrence. Returns false
 * when the clock is invalid or no occurrence could be found. */
bool jobs_calendar_recompute(jobs_job_t *j);
void jobs_event_subscribe(jobs_job_t *j);
void jobs_event_unsubscribe(jobs_job_t *j);

#ifdef __cplusplus
}
#endif
