#pragma once

/* jobs_providers: registers the first-slice Jobs action providers at boot,
 * after devos_actions is ready and before any definition is validated.
 * Kept in main/ so orchestration can depend on several engines without
 * reversing component dependencies (PLAN.md section 4.1). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "devos_actions.h"   /* devos_value_t / devos_action_args_t */
#include "devos_jobs.h"      /* devos_jobs_system_t for the event bridge */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- shared argument coercion (invariant 7: one copy, never per provider) --
 * A job source writes a bare integer for a duration ("5000" = 5000 ms) and the
 * app's parse_dur() treats it that way, so the runtime must too; a NUM is
 * rounded to the nearest millisecond. An integer accepts INT or NUM (and a
 * DURATION, its ms). These live here as static inline so every provider links
 * without a shared .c. */
static inline int64_t jobs_arg_ms(const devos_action_args_t *args, int i, int64_t def)
{
    if (!args || i < 0 || i >= args->arg_count) return def;
    const devos_value_t *v = &args->args[i];
    switch (v->type) {
    case DEVOS_VAL_DURATION: return v->v.ms;
    case DEVOS_VAL_INT:      return v->v.i;
    case DEVOS_VAL_NUM:      return (int64_t)(v->v.n < 0.0 ? v->v.n - 0.5 : v->v.n + 0.5);
    default:                 return def;
    }
}

static inline int64_t jobs_arg_int(const devos_action_args_t *args, int i, int64_t def)
{
    if (!args || i < 0 || i >= args->arg_count) return def;
    const devos_value_t *v = &args->args[i];
    switch (v->type) {
    case DEVOS_VAL_INT:      return v->v.i;
    case DEVOS_VAL_NUM:      return (int64_t)v->v.n;
    case DEVOS_VAL_DURATION: return v->v.ms;
    default:                 return def;
    }
}

static inline bool jobs_arg_bool(const devos_action_args_t *args, int i, bool def)
{
    if (!args || i < 0 || i >= args->arg_count) return def;
    const devos_value_t *v = &args->args[i];
    return v->type == DEVOS_VAL_BOOL ? v->v.b : def;
}

/* Register every provider's schemas and runtime handlers (idempotent). */
void jobs_providers_register_all(void);

/* Individual providers (also registered through register_all). */
void jobs_system_register(void);
void jobs_http_register(void);
void jobs_network_register(void);
void jobs_mqtt_register(void);
void jobs_docker_register(void);
void jobs_proxmox_register(void);
void jobs_events_register(void);

/* Optional availability gate for the Docker provider (e.g. the Docker app is
 * switched off in Settings > Apps). Jobs never re-enables an app; a false
 * result makes every docker.* action unavailable with `reason`. NULL clears
 * the gate. Not needed by host tests, so it lives in the provider. */
void jobs_docker_set_gate(bool (*gate)(char *reason, size_t cap));
/* The Proxmox app being switched off makes every proxmox.* action unavailable
 * with a reason (Jobs never re-enables an app). */
void jobs_proxmox_set_gate(bool (*gate)(char *reason, size_t cap));

/* ---- system-event producers (Phase 6) -----------------------------------
 * Called from main's boot path / 1 Hz loop, never from an engine. The bridge
 * publishes typed events (system.boot, Wi-Fi connect/disconnect, battery
 * below) through devos_events; a system-event job's trigger consumes them. */
void jobs_events_publish_boot(bool recovery);
void jobs_events_poll(const devos_jobs_system_t *s);

/* The persisted, monotonically-increasing boot counter reported in
 * system.boot's boot_id (0 until jobs_events_publish_boot() runs). Lets a job
 * tell two boots apart. */
unsigned jobs_events_boot_id(void);
/* Events this bridge could not queue because devos_events was full (a drop is
 * never silently swallowed; devos_events_stats() counts the core's own). */
unsigned devos_jobs_events_dropped(void);

/* ---- system.log / system.notify sinks (consumed by the Jobs UI bridge) ----
 * `system.log` appends a sanitized line; `system.notify` queues a notice for
 * the UI to toast. Both are bounded and safe to call when the UI is absent. */
#define JOBS_SYSTEM_LOG_MAX     32
#define JOBS_SYSTEM_LOG_LINE    128
#define JOBS_SYSTEM_NOTICE_MAX  16
#define JOBS_SYSTEM_NOTICE_LEN  160

/* Copy up to `max` most-recent log lines into out (each JOBS_SYSTEM_LOG_LINE
 * bytes), newest last. Returns the number copied. */
int jobs_system_log_tail(char *out, int max);
int jobs_system_log_count(void);
/* Pop the oldest queued notice. level: 0 info, 1 warning, 2 error, 3 success. */
bool jobs_system_take_notice(char *msg, size_t cap, int *level);

#ifdef __cplusplus
}
#endif
