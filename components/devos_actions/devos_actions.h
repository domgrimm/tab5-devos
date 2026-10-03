#pragma once

/* devos_actions: the typed action contract shared by the Jobs executor, the
 * Jobs GUI builder and every app that exposes automatable operations
 * (AGENTS.md "Jobs-compatible actions and events").
 *
 * This header is deliberately free of LVGL and devos_core.h: the Jobs engine
 * compiles and is unit-tested on the host. Schemas are static and immutable;
 * registration is done once at boot from a provider hook, not from an app's
 * LVGL init(). The registry rejects duplicates and overflow.
 *
 * Phase 1 ships the schema registry and the value model. The asynchronous
 * operation contract (start/poll/cancel/release) is declared here and
 * implemented by the providers in Phase 2. */

#include "devos_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_ACTIONS_MAX 64

/* ---- typed values (also the Jobs expression value model) ---- */
typedef enum {
    DEVOS_VAL_NULL = 0,
    DEVOS_VAL_BOOL,
    DEVOS_VAL_INT,          /* signed 64-bit */
    DEVOS_VAL_NUM,          /* finite double */
    DEVOS_VAL_STR,          /* UTF-8, not necessarily NUL-terminated */
    DEVOS_VAL_DURATION,     /* milliseconds, distinct from INT/NUM */
} devos_val_type_t;

typedef struct {
    devos_val_type_t type;
    union {
        bool b;
        int64_t i;
        double n;
        struct { const char *s; uint32_t len; } str;
        int64_t ms;
    } v;
} devos_value_t;

const char *devos_val_type_name(devos_val_type_t t);

/* ---- action schema ---- */
typedef enum {
    DEVOS_EFFECT_READ = 0,  /* observes, no side effects */
    DEVOS_EFFECT_NET_SEND,  /* sends bytes to the network */
    DEVOS_EFFECT_MUTATE,    /* changes remote/local state */
} devos_effect_t;

typedef struct {
    const char *name;
    devos_val_type_t type;
    bool required;
    bool credential;        /* value must be secret("name"), never a literal */
    bool expression;        /* may hold a variable/expression, not only a literal */
    const char *def;        /* default literal as text, or NULL */
    const char *choices;    /* enum choices "a|b|c", or NULL */
    double min, max;        /* numeric/duration bounds (used when max > min) */
    uint32_t max_len;       /* string cap (0 = default) */
    const char *help;
} devos_action_param_t;

typedef struct {
    const char *name;
    devos_val_type_t type;
    bool sensitive;         /* never log/interpolate */
    const char *help;
} devos_action_out_t;

typedef struct {
    const char *id;             /* "http.request" */
    uint16_t schema_version;
    const char *provider_uid;   /* owning app/engine uid, e.g. "rest" */
    const char *category;
    const char *label;
    const char *description;
    const devos_action_param_t *params;
    int param_count;
    const devos_action_out_t *outs;
    int out_count;
    devos_effect_t effect;
    bool retry_safe;
    int recommended_timeout_ms;
} devos_action_descriptor_t;

/* ---- registry (frozen once Jobs definitions are validated) ---- */
devos_err_t devos_actions_register(const devos_action_descriptor_t *d);
const devos_action_descriptor_t *devos_actions_find(const char *id);
int devos_actions_count(void);
const devos_action_descriptor_t *devos_actions_at(int index);
/* Tests only: forget every registration. */
void devos_actions_reset(void);

/* ---- asynchronous operation contract (implemented by providers, Phase 2) ----
 * A handle carries a slot generation so a stale reference is rejected rather
 * than acting on a reused slot. start() copies/owns the arguments until the
 * operation completes; poll() snapshots state; release() frees exactly once. */
typedef struct { uint32_t slot; uint32_t gen; } devos_action_handle_t;
#define DEVOS_ACTION_HANDLE_NONE ((devos_action_handle_t){ 0, 0 })

typedef struct {
    devos_value_t *args;        /* named values, see the descriptor's params */
    int arg_count;
    const char *run_id;         /* correlation, for traces */
} devos_action_args_t;

typedef struct {
    void *user;                 /* provider context */
} devos_action_context_t;

typedef enum {
    DEVOS_ACT_PENDING = 0,      /* submitted, no final result yet */
    DEVOS_ACT_DONE,             /* provider completed; typed output available */
    DEVOS_ACT_FAILED,           /* could not execute (invalid/unavailable/...) */
    DEVOS_ACT_CANCELLED,        /* confirmed local cancellation */
    DEVOS_ACT_UNKNOWN,          /* cancelled/timed out after a mutating send */
} devos_action_state_t;

typedef struct {
    devos_action_state_t state;
    devos_value_t *outs;        /* descriptor-ordered outputs */
    int out_count;
    char error[96];             /* diagnostic when FAILED/CANCELLED */
    int64_t duration_ms;
} devos_action_result_t;

devos_err_t devos_action_start(const char *action_id, const devos_action_args_t *args,
                               const devos_action_context_t *ctx, devos_action_handle_t *out);
devos_err_t devos_action_poll(devos_action_handle_t handle, devos_action_state_t *state,
                              devos_action_result_t *result);
devos_err_t devos_action_cancel(devos_action_handle_t handle);
void devos_action_release(devos_action_handle_t handle);

#ifdef __cplusplus
}
#endif
