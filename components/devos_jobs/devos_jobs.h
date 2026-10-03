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

/* Engine-facing system snapshot (a bridge copies it from devos_sysmon so the
 * engine never touches the GUI telemetry or I2C). */
typedef struct {
    bool     battery_valid, battery_present, charging;
    uint8_t  battery_percent;
    bool     wifi_connected;
    char     wifi_ssid[33];
    char     local_ip[20];
    int8_t   wifi_rssi;
    /* VPN status for network.tailscale_* / network.wireguard_* events and the
     * matching system.* expression fields. Filled by the boot bridge from the
     * owning engine's thread-safe getter (never GUI telemetry). */
    bool     tailscale_online;
    char     tailscale_ip[16];
    char     tailscale_hostname[40];
    bool     wireguard_online;
    char     wireguard_name[32];
    char     wireguard_address[32];
    bool     time_valid;
    /* Wall clock for daily/weekdays schedules. `wall_unix_s` is UTC seconds;
     * local time = wall_unix_s + tz_offset_s. `tz_generation` bumps on a
     * timezone or wall-clock change so the scheduler recomputes deadlines. */
    int64_t  wall_unix_s;
    int32_t  tz_offset_s;
    uint32_t tz_generation;
    uint32_t uptime_s;
    uint8_t  cpu_core0, cpu_core1;
    uint32_t psram_free_kb, sram_free_kb, sram_largest_kb;
} devos_jobs_system_t;
void devos_jobs_set_system(const devos_jobs_system_t *s);

/* Optional: the local UTC offset at a given UTC instant, for DST-correct
 * daily/weekdays occurrences. The boot bridge installs one built on the
 * device's POSIX timezone; host tests inject a fake. NULL means "use the
 * snapshot's tz_offset_s for every instant". */
typedef int32_t (*devos_jobs_offset_fn)(int64_t utc_s, void *user);
void devos_jobs_set_offset_fn(devos_jobs_offset_fn fn, void *user);

/* MQTT subscription hooks: the boot bridge installs these so an
 * `event "mqtt.message"` trigger acquires/releases a broker subscription
 * without the Jobs engine depending on devos_mqtt. `sub` returns a handle > 0
 * or -1; `unsub` releases it. Both may be NULL (MQTT triggers then run with
 * whatever the user's own subscriptions deliver). */
typedef int  (*devos_jobs_mqtt_sub_fn)(const char *topic, void *user);
typedef void (*devos_jobs_mqtt_unsub_fn)(int handle, void *user);
void devos_jobs_set_mqtt_hooks(devos_jobs_mqtt_sub_fn sub, devos_jobs_mqtt_unsub_fn unsub, void *user);

/* Advance the scheduler once (interval deadlines, running jobs, waits). The
 * target calls this from its Core 0 task; host tests drive it with a fake
 * clock. Safe when the engine is off. */
void devos_jobs_tick(void);
/* True once load/validation finished and automatic dispatch may start. */
bool devos_jobs_ready(void);

/* ---- read-only snapshots (safe when OFF) ---- */
int devos_jobs_count(void);
bool devos_jobs_summary_at(int index, devos_job_summary_t *out);
bool devos_jobs_run(devos_jobs_run_t *out);
/* Canonical source of a job's active revision (serialized AST). */
devos_err_t devos_jobs_source(const char *id, char *out, size_t cap, size_t *out_len);
/* Parse + validate without applying; the first diagnostic goes to diag. */
devos_err_t devos_jobs_check(const char *source, size_t len, char *diag, size_t cap);
/* Recent run-history lines for a job (newest last), bounded. Returns bytes. */
int devos_jobs_history(const char *id, char *out, size_t cap);

/* ---- commands (bounded; return an admission result) ----
 * The UI enqueues these and gets completion through the snapshot API. They do
 * not return pointers into worker-owned memory. */
devos_err_t devos_jobs_apply(const char *id, const char *source, size_t len, uint32_t *out_revision);
/* Apply with a base revision for conflict detection (the revision the editor
 * loaded). base_known=false skips the check. A stale base returns
 * DEVOS_ERR_INVALID_STATE (conflict) and changes nothing. */
devos_err_t devos_jobs_apply_base(const char *id, const char *source, size_t len,
                                  uint32_t base_revision, bool base_known, uint32_t *out_revision);
/* Draft checkpoint: writes the draft file only, never activates a revision. */
devos_err_t devos_jobs_save_draft(const char *id, const char *source, size_t len);
devos_err_t devos_jobs_load_draft(const char *id, char *out, size_t cap, size_t *out_len);
devos_err_t devos_jobs_set_enabled(const char *id, bool enabled);
devos_err_t devos_jobs_run_now(const char *id);
devos_err_t devos_jobs_cancel(const char *id);
devos_err_t devos_jobs_delete(const char *id);

/* Safe start / reverted boot keeps automatic execution paused until the user
 * resumes; Run now still works. */
void devos_jobs_set_safe_pause(bool pause);
bool devos_jobs_safe_paused(void);
/* Cancel active runs and stop new automatic starts (before restart/shutdown). */
void devos_jobs_prepare_shutdown(void);
/* A reason when a restart would lose work, else NULL (for devos_core's restart
 * check). */
const char *devos_jobs_restart_check(void);

#ifdef __cplusplus
}
#endif
