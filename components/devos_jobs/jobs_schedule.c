/* jobs_schedule: the Jobs engine - job registry, triggers, the scheduler tick
 * and the public devos_jobs API (PLAN.md 9.1). Core 0 on target; host tests
 * drive devos_jobs_tick() with a fake clock and a fake wall clock. No LVGL.
 *
 * Triggers: manual (Run now only), every <dur> (monotonic, phase-anchored),
 * daily/weekdays "HH:MM" (device-local wall clock, DST-safe, one occurrence
 * per local date) and event "topic" [where expr] (system events through
 * devos_events). Automatic admission honours the portable policy: overlap
 * skip/queue_one and cooldown. */
#include "jobs_internal.h"
#include "jobs_platform.h"
#include "jobs_store.h"
#include "devos_json.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "devos_config.h"
#include "esp_heap_caps.h"
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif
#ifndef TAB5_SD_MOUNT_POINT
#define TAB5_SD_MOUNT_POINT "./sim_sdcard"
#endif

jobs_engine_t g_jobs EXT_RAM_BSS_ATTR;

jobs_job_t *jobs_find(const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < g_jobs.count; i++)
        if (g_jobs.jobs[i].used && strcmp(g_jobs.jobs[i].id, id) == 0) return &g_jobs.jobs[i];
    return NULL;
}

static jobs_job_t *job_alloc(void)
{
    for (int i = 0; i < DEVOS_JOBS_MAX; i++)
        if (!g_jobs.jobs[i].used) {
            if (i >= g_jobs.count) g_jobs.count = i + 1;
            return &g_jobs.jobs[i];
        }
    return NULL;
}

static void fmt_interval(int64_t ms, char *out, size_t cap)
{
    if (ms % 3600000 == 0) snprintf(out, cap, "every %lldh", (long long)(ms / 3600000));
    else if (ms % 60000 == 0) snprintf(out, cap, "every %lldm", (long long)(ms / 60000));
    else if (ms % 1000 == 0) snprintf(out, cap, "every %llds", (long long)(ms / 1000));
    else snprintf(out, cap, "every %lldms", (long long)ms);
}

static void fmt_trigger(const jobs_job_t *j, char *out, size_t cap)
{
    switch (j->trigger_kind) {
    case JTRIG_EVERY:    fmt_interval(j->interval_ms, out, cap); break;
    case JTRIG_DAILY:    snprintf(out, cap, "daily %02d:%02d", j->trig_hh, j->trig_mm); break;
    case JTRIG_WEEKDAYS: {
        char days[32] = "";
        if (j->trig_days && j->trig_days != JOBS_DAYS_DEFAULT) {
            char d[28];
            jobs_days_format(j->trig_days, d, sizeof(d));
            snprintf(days, sizeof(days), " %s", d);
        }
        snprintf(out, cap, "weekdays %02d:%02d%s", j->trig_hh, j->trig_mm, days);
        break;
    }
    case JTRIG_EVENT:    snprintf(out, cap, "on %s", j->event_topic[0] ? j->event_topic : "?"); break;
    default:             snprintf(out, cap, "manual"); break;
    }
}

static void start_run(jobs_job_t *j, int64_t now, jobs_cause_t cause)
{
    char id[JOBS_RUN_ID_MAX];
    snprintf(id, sizeof(id), "r%u", (unsigned)++g_jobs.run_seq);
    jobs_run_begin(j, id, now, j->revision, cause);
}

/* ---- calendar (daily / weekdays) ---------------------------------------- */

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b, r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) q--;
    return q;
}

/* Days since 1970-01-01 -> civil date (Howard Hinnant). */
static void civil_from_days(int64_t z, int *yy, unsigned *mm, unsigned *dd)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)yoe + (int)(era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp + (mp < 10 ? 3 : -9);
    *yy = y + (m <= 2);
    *mm = m;
    *dd = d;
}

/* Local UTC offset at a UTC instant: the injected DST-correct function, else
 * the snapshot's current offset. */
static int32_t jobs_offset_at(int64_t utc_s)
{
    if (g_jobs.offset_fn) return g_jobs.offset_fn(utc_s, g_jobs.offset_user);
    return g_jobs.sys.tz_offset_s;
}

/* Local YYYYMMDD of a UTC instant. */
static int32_t local_date_of(int64_t utc_s)
{
    int64_t local = utc_s + jobs_offset_at(utc_s);
    int y; unsigned m, d;
    civil_from_days(floor_div(local, 86400), &y, &m, &d);
    return y * 10000 + (int)m * 100 + (int)d;
}

/* Weekday of a day number: 0 = Sunday .. 6 = Saturday (1970-01-01 = Thursday). */
static int weekday_of(int64_t day)
{
    return (int)(((((day % 7) + 7) % 7) + 4) % 7);
}

/* Next occurrence strictly after `now`, in local time, converted to UTC. Skips
 * a local date already claimed and a nonexistent (DST-gap) local time. */
bool jobs_calendar_recompute(jobs_job_t *j)
{
    j->next_wall_valid = false;
    if (!g_jobs.sys.time_valid) return false;
    int64_t now_utc = g_jobs.sys.wall_unix_s;
    int64_t now_local = now_utc + jobs_offset_at(now_utc);
    int64_t day = floor_div(now_local, 86400);
    int64_t sod = now_local - day * 86400;
    int64_t target = (int64_t)j->trig_hh * 3600 + (int64_t)j->trig_mm * 60;
    if (target <= sod) day++;                 /* next future occurrence */
    for (int i = 0; i < 400; i++) {           /* bounded: at most ~1 year of skips */
        if (j->trigger_kind == JTRIG_WEEKDAYS) {
            uint8_t mask = j->trig_days ? j->trig_days : JOBS_DAYS_DEFAULT;
            if (!(mask & (uint8_t)(1u << weekday_of(day)))) { day++; continue; }
        }
        int64_t cand_local = day * 86400 + target;
        int64_t cand_utc = cand_local - jobs_offset_at(cand_local);
        cand_utc = cand_local - jobs_offset_at(cand_utc);
        if (cand_utc + jobs_offset_at(cand_utc) != cand_local) { day++; continue; }  /* DST gap: skip date */
        if (local_date_of(cand_utc) == j->claim_date) { day++; continue; }           /* already claimed */
        j->next_wall_s = cand_utc;
        j->next_wall_valid = true;
        j->seen_tz_gen = g_jobs.sys.tz_generation;
        return true;
    }
    return false;
}

/* ---- trigger configuration ---------------------------------------------- */

/* A plain string argument: a bare literal or a single-literal JN_EXPR_STR. */
static bool lit_str_of(const jobs_node_t *e, const char **out)
{
    if (!e) return false;
    if (e->kind == JN_EXPR_LIT && e->u.lit.type == DEVOS_VAL_STR) { *out = e->u.lit.v.str.s; return true; }
    if (e->kind == JN_EXPR_STR && e->a && e->a->sub == JSP_LITERAL && !e->a->next) {
        *out = e->a->u.str.s;
        return true;
    }
    return false;
}

static void read_trigger(jobs_job_t *j, const jobs_ast_t *ast)
{
    j->trigger_kind = JTRIG_MANUAL;
    j->interval_ms = 0;
    j->trig_hh = 8;
    j->trig_mm = 0;
    j->trig_days = 0;
    j->event_topic[0] = '\0';
    j->mqtt_topic[0] = '\0';
    j->mqtt_sub_handle = 0;
    j->ev_debounce_ms = 0;
    j->include_retained = false;
    j->cooldown_ms = 0;
    j->overlap_queue_one = false;

    const jobs_node_t *t = ast->root ? ast->root->a : NULL;
    if (t) {
        j->trigger_kind = t->sub;
        if (t->sub == JTRIG_EVERY) {
            j->interval_ms = t->u.i;
        } else if (t->sub == JTRIG_DAILY || t->sub == JTRIG_WEEKDAYS) {
            const char *s = (t->u.str.s && strlen(t->u.str.s) == 5) ? t->u.str.s : "08:00";
            j->trig_hh = (s[0] - '0') * 10 + (s[1] - '0');
            j->trig_mm = (s[3] - '0') * 10 + (s[4] - '0');
            j->trig_days = 0;
            if (t->sub == JTRIG_WEEKDAYS && t->u.str.s2)
                jobs_days_parse(t->u.str.s2, &j->trig_days);   /* validated; 0 = default */
        } else if (t->sub == JTRIG_EVENT) {
            snprintf(j->event_topic, sizeof(j->event_topic), "%s", t->u.str.s ? t->u.str.s : "");
            for (const jobs_node_t *a = t->a; a; a = a->next) {
                const jobs_node_t *v = a->a;
                const char *sv = NULL;
                if (strcmp(a->u.str.s, "debounce") == 0 && v && v->kind == JN_EXPR_LIT &&
                    v->u.lit.type == DEVOS_VAL_DURATION)
                    j->ev_debounce_ms = v->u.lit.v.ms;
                else if (strcmp(a->u.str.s, "include_retained") == 0 && v && v->kind == JN_EXPR_LIT &&
                         v->u.lit.type == DEVOS_VAL_BOOL)
                    j->include_retained = v->u.lit.v.b;
                else if (strcmp(a->u.str.s, "topic") == 0 && lit_str_of(v, &sv))
                    snprintf(j->mqtt_topic, sizeof(j->mqtt_topic), "%s", sv);
            }
        }
    }
    jobs_policy_read(ast, NULL, &j->cooldown_ms, &j->overlap_queue_one);
}

/* ---- event ingress ------------------------------------------------------- */

static void event_cb(const devos_event_t *ev, void *user)
{
    jobs_job_t *j = user;
    if (!j || !j->used || !ev) return;
    int64_t now = jobs_now_ms();
    if (j->ev_debounce_ms > 0 && j->ev_last_accept_ms &&
        now - j->ev_last_accept_ms < j->ev_debounce_ms) {
        j->skipped++;                          /* debounced */
        return;
    }
    j->ev_last_accept_ms = now;
    j->ev.pending = true;
    j->ev.valid = true;
    j->ev.seq = ev->seq;
    j->ev.truncated = ev->truncated;
    j->ev.retain = ev->retain;
    snprintf(j->ev.topic, sizeof(j->ev.topic), "%s", ev->topic);
    snprintf(j->ev.source, sizeof(j->ev.source), "%s", ev->source);
    uint32_t n = ev->payload_len;
    if (n > JOBS_EV_PAYLOAD_MAX) n = JOBS_EV_PAYLOAD_MAX;
    if (ev->payload && n) memcpy(j->ev.payload, ev->payload, n);
    j->ev.payload_len = n;
    j->ev.payload[n < sizeof(j->ev.payload) ? n : sizeof(j->ev.payload) - 1] = '\0';
    j->ev.arrival_ms = now;
}

void jobs_event_unsubscribe(jobs_job_t *j)
{
    if (!j) return;
    if (j->sub_id > 0) { devos_events_unsubscribe(j->sub_id); j->sub_id = 0; }
    if (j->mqtt_sub_handle > 0 && g_jobs.mqtt_unsub) {
        g_jobs.mqtt_unsub(j->mqtt_sub_handle, g_jobs.mqtt_user);
        j->mqtt_sub_handle = 0;
    }
    j->ev.pending = false;
}

void jobs_event_subscribe(jobs_job_t *j)
{
    if (!j) return;
    jobs_event_unsubscribe(j);
    if (j->trigger_kind != JTRIG_EVENT || !j->event_topic[0]) return;
    j->sub_id = devos_events_subscribe(j->event_topic, event_cb, j);
    /* An mqtt.message trigger also owns a broker subscription so the broker
     * sends the topics it needs, without touching the user's own four. */
    if (strcmp(j->event_topic, "mqtt.message") == 0 && g_jobs.mqtt_sub) {
        const char *filter = j->mqtt_topic[0] ? j->mqtt_topic : "#";
        j->mqtt_sub_handle = g_jobs.mqtt_sub(filter, g_jobs.mqtt_user);
    }
}

/* ---- lifecycle ---- */
static void scheduler_loop(void *arg)
{
    (void)arg;
    while (!g_jobs.stopping) {
        devos_jobs_tick();
        jobs_platform_sleep_ms(50);
    }
}

/* Recompute the automatic schedule for a job (called on load/apply/enable). */
static void job_arm(jobs_job_t *j)
{
    j->has_next = false;
    j->next_wall_valid = false;
    j->pending_run = false;
    j->ev.pending = false;               /* never fire a stale event on re-enable */
    if (!j->enabled) return;
    if (j->trigger_kind == JTRIG_EVERY && j->interval_ms > 0) {
        j->has_next = true;
        j->next_due_ms = jobs_now_ms() + j->interval_ms;   /* first run after one interval */
    } else if (j->trigger_kind == JTRIG_DAILY || j->trigger_kind == JTRIG_WEEKDAYS) {
        jobs_calendar_recompute(j);
    }
}

/* Load callback: parse + validate a stored revision, then install it. */
static bool load_cb(const char *id, const char *source, size_t len, bool enabled,
                    uint32_t revision, void *user)
{
    (void)user;
    jobs_ast_t *ast = jobs_parse(source, len, NULL);
    if (!ast) return false;
    if (!jobs_validate(ast)) { jobs_ast_free(ast); return false; }
    jobs_job_t *j = jobs_find(id);
    if (!j) {
        j = job_alloc();
        if (!j) { jobs_ast_free(ast); return false; }
        memset(j, 0, sizeof(*j));
        j->used = true;
        snprintf(j->id, sizeof(j->id), "%s", id);
    }
    if (j->run.active) { jobs_ast_free(ast); return false; }
    if (j->ast) jobs_ast_release(j->ast);
    j->ast = ast;
    j->revision = revision;
    snprintf(j->name, sizeof(j->name), "%s", ast->root->u.str.s ? ast->root->u.str.s : id);
    j->enabled = enabled;
    read_trigger(j, ast);
    jobs_event_subscribe(j);
    job_arm(j);
    return true;
}

static bool jobs_topic_known(const char *topic, void *user)
{
    (void)user;
    if (devos_events_topic_count() == 0) return true;   /* registry not populated (host tests) */
    return devos_events_topic_schema(topic) != NULL;
}

bool devos_jobs_init(void)
{
    jobs_platform_init();
    /* main installs the offset / mqtt / secret hooks just before starting the
     * engine, so preserve them across the reset (otherwise secret() and MQTT
     * triggers would silently lose their bridges). */
    devos_jobs_offset_fn off = g_jobs.offset_fn;
    void *off_user = g_jobs.offset_user;
    devos_jobs_mqtt_sub_fn msub = g_jobs.mqtt_sub;
    devos_jobs_mqtt_unsub_fn munsub = g_jobs.mqtt_unsub;
    void *muser = g_jobs.mqtt_user;
    devos_jobs_secret_fn sres = g_jobs.secret_resolve;
    devos_jobs_secret_wipe_fn swipe = g_jobs.secret_wipe;
    void *suser = g_jobs.secret_user;
    memset(&g_jobs, 0, sizeof(g_jobs));
    g_jobs.offset_fn = off;
    g_jobs.offset_user = off_user;
    g_jobs.mqtt_sub = msub;
    g_jobs.mqtt_unsub = munsub;
    g_jobs.mqtt_user = muser;
    g_jobs.secret_resolve = sres;
    g_jobs.secret_wipe = swipe;
    g_jobs.secret_user = suser;
    jobs_validate_set_topic_check(jobs_topic_known, NULL);
    g_jobs.state = DEVOS_JOBS_READY;
    g_jobs.ready = true;                 /* durable load is best-effort below */
    if (jobs_store_init(TAB5_SD_MOUNT_POINT)) {
        jobs_store_worker_start();           /* Core 1 history writes */
        jobs_store_load(load_cb, NULL);
    }
    /* Run the scheduler unless a test has injected a fake clock (then the test
     * drives devos_jobs_tick() itself). Works on target and in the simulator. */
    if (!jobs_platform_clock_overridden() && !jobs_platform_start_scheduler(scheduler_loop, NULL)) {
        g_jobs.state = DEVOS_JOBS_OFF;
        g_jobs.ready = false;
        return false;
    }
    return true;
}

void devos_jobs_shutdown(void)
{
    jobs_store_worker_stop();
    for (int i = 0; i < g_jobs.count; i++) {
        jobs_job_t *j = &g_jobs.jobs[i];
        if (!j->used) continue;
        if (j->run.active) jobs_run_cancel(j);
        jobs_event_unsubscribe(j);
        if (j->ast) { jobs_ast_release(j->ast); j->ast = NULL; }
    }
    memset(&g_jobs, 0, sizeof(g_jobs));
    g_jobs.state = DEVOS_JOBS_OFF;
}

devos_jobs_state_t devos_jobs_state(void) { return g_jobs.state; }
bool devos_jobs_ready(void) { return g_jobs.ready; }
bool devos_jobs_paused(void) { return g_jobs.paused; }
void devos_jobs_set_paused(bool paused) { g_jobs.paused = paused; }
bool devos_jobs_safe_paused(void) { return g_jobs.safe_paused; }
void devos_jobs_set_safe_pause(bool pause) { g_jobs.safe_paused = pause; }
void devos_jobs_set_system(const devos_jobs_system_t *s)
{
    if (s) g_jobs.sys = *s;
}
void devos_jobs_set_offset_fn(devos_jobs_offset_fn fn, void *user)
{
    g_jobs.offset_fn = fn;
    g_jobs.offset_user = user;
}
void devos_jobs_set_mqtt_hooks(devos_jobs_mqtt_sub_fn sub, devos_jobs_mqtt_unsub_fn unsub, void *user)
{
    g_jobs.mqtt_sub = sub;
    g_jobs.mqtt_unsub = unsub;
    g_jobs.mqtt_user = user;
}

void devos_jobs_set_secret_hooks(devos_jobs_secret_fn resolve, devos_jobs_secret_wipe_fn wipe, void *user)
{
    g_jobs.secret_resolve = resolve;
    g_jobs.secret_wipe = wipe;
    g_jobs.secret_user = user;
}

void devos_jobs_memory(devos_jobs_memory_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->sched_stack_free = (uint32_t)jobs_platform_sched_stack_free();
    out->store_stack_free = (uint32_t)jobs_store_stack_free();
#ifdef ESP_PLATFORM
    out->internal_free_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    out->internal_largest_kb = (uint32_t)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
#endif
}

/* ---- snapshots ---- */
int devos_jobs_count(void)
{
    int n = 0;
    for (int i = 0; i < g_jobs.count; i++) n += g_jobs.jobs[i].used;
    return n;
}

bool devos_jobs_summary_at(int index, devos_job_summary_t *out)
{
    if (!out || index < 0) return false;
    int k = 0;
    for (int i = 0; i < g_jobs.count; i++) {
        jobs_job_t *j = &g_jobs.jobs[i];
        if (!j->used) continue;
        if (k++ != index) continue;
        memset(out, 0, sizeof(*out));
        snprintf(out->id, sizeof(out->id), "%s", j->id);
        snprintf(out->name, sizeof(out->name), "%s", j->name);
        out->state = !j->enabled ? DEVOS_JOB_DISABLED : DEVOS_JOB_ENABLED;
        fmt_trigger(j, out->trigger, sizeof(out->trigger));
        snprintf(out->last_result, sizeof(out->last_result), "%s", j->last_result);
        out->running = j->run.active;
        out->revision = j->revision;
        out->last_run_wall_s = j->last_run_wall_s;
        out->last_ok = j->last_ok;
        snprintf(out->last_cause, sizeof(out->last_cause), "%s",
                 (j->last_result[0] || j->last_run_ms) ? jobs_cause_name((jobs_cause_t)j->last_cause) : "-");
        if (j->trigger_kind == JTRIG_EVERY && j->has_next) {
            int64_t now = jobs_now_ms();
            out->next_run_in_ms = j->next_due_ms > now ? j->next_due_ms - now : 0;
        } else if ((j->trigger_kind == JTRIG_DAILY || j->trigger_kind == JTRIG_WEEKDAYS) && j->next_wall_valid) {
            out->next_run_wall_s = j->next_wall_s;
            if (g_jobs.sys.time_valid && j->next_wall_s > g_jobs.sys.wall_unix_s)
                out->next_run_in_ms = (j->next_wall_s - g_jobs.sys.wall_unix_s) * 1000;
        }
        return true;
    }
    return false;
}

bool devos_jobs_run(devos_jobs_run_t *out)
{
    if (!out) return false;
    for (int i = 0; i < g_jobs.count; i++) {
        jobs_job_t *j = &g_jobs.jobs[i];
        if (!j->used || !j->run.active) continue;
        memset(out, 0, sizeof(*out));
        out->active = true;
        snprintf(out->job_id, sizeof(out->job_id), "%s", j->id);
        snprintf(out->run_id, sizeof(out->run_id), "%s", j->run.run_id);
        out->elapsed_ms = jobs_now_ms() - j->run.started_ms;
        out->node_id = j->run.cur ? (int)j->run.cur->off : -1;
        out->cancelling = j->run.cancelling;
        return true;
    }
    return false;
}

/* ---- commands ---- */
devos_err_t devos_jobs_apply(const char *id, const char *source, size_t len, uint32_t *out_revision)
{
    return devos_jobs_apply_base(id, source, len, 0, false, out_revision);
}

devos_err_t devos_jobs_apply_base(const char *id, const char *source, size_t len,
                                  uint32_t base_revision, bool base_known, uint32_t *out_revision)
{
    if (!id || !id[0] || !source) return DEVOS_ERR_INVALID_ARG;
    if (strlen(id) >= DEVOS_JOBS_ID_MAX) return DEVOS_ERR_INVALID_SIZE;

    jobs_ast_t *ast = jobs_parse(source, len, NULL);
    if (!ast) return DEVOS_ERR_NO_MEM;
    if (!jobs_validate(ast)) { jobs_ast_free(ast); return DEVOS_ERR_INVALID_ARG; }

    jobs_job_t *j = jobs_find(id);
    bool existed = j != NULL;
    /* Stale editor: the active revision moved on. Preserve its draft elsewhere
     * (the UI offers Reload / Save as new); here the apply is refused. */
    if (base_known && (existed ? j->revision : 0) != base_revision) {
        jobs_ast_free(ast);
        return DEVOS_ERR_INVALID_STATE;
    }
    if (!j) {
        j = job_alloc();
        if (!j) { jobs_ast_free(ast); return DEVOS_ERR_NO_MEM; }
        memset(j, 0, sizeof(*j));
        j->used = true;
        snprintf(j->id, sizeof(j->id), "%s", id);
    }

    /* An active run retains its own reference to the revision it started with,
     * so replacing j->ast here is safe (PLAN.md 5.3). Commit to durable storage
     * first: if that fails, the previous revision stays authoritative. */
    uint32_t new_rev = j->revision + 1;
    bool new_enabled = existed ? j->enabled : false;
    if (jobs_store_available()) {
        devos_err_t rc = jobs_store_commit(id, source, len, new_rev, new_enabled);
        if (rc != DEVOS_OK) {
            jobs_ast_free(ast);
            if (!existed) j->used = false;
            return rc;
        }
    }

    if (j->ast) jobs_ast_release(j->ast);
    j->ast = ast;
    j->revision = new_rev;
    snprintf(j->name, sizeof(j->name), "%s", ast->root->u.str.s ? ast->root->u.str.s : id);
    read_trigger(j, ast);
    jobs_event_subscribe(j);
    if (!existed) j->enabled = false;
    job_arm(j);
    if (out_revision) *out_revision = j->revision;
    return DEVOS_OK;
}

devos_err_t devos_jobs_set_enabled(const char *id, bool enabled)
{
    jobs_job_t *j = jobs_find(id);
    if (!j) return DEVOS_ERR_NOT_FOUND;
    j->enabled = enabled;
    job_arm(j);
    if (jobs_store_available()) {
        devos_err_t rc = jobs_store_set_enabled(id, enabled);
        if (rc != DEVOS_OK && rc != DEVOS_ERR_INVALID_STATE) return rc;
    }
    return DEVOS_OK;
}

devos_err_t devos_jobs_run_now(const char *id)
{
    jobs_job_t *j = jobs_find(id);
    if (!j) return DEVOS_ERR_NOT_FOUND;
    if (!j->ast) return DEVOS_ERR_INVALID_STATE;
    if (j->run.active) return DEVOS_ERR_INVALID_STATE;      /* overlap: skip */
    start_run(j, jobs_now_ms(), JOBS_CAUSE_MANUAL);
    return DEVOS_OK;
}

devos_err_t devos_jobs_cancel(const char *id)
{
    jobs_job_t *j = jobs_find(id);
    if (!j) return DEVOS_ERR_NOT_FOUND;
    if (!j->run.active) return DEVOS_ERR_INVALID_STATE;
    jobs_run_cancel(j);
    return DEVOS_OK;
}

devos_err_t devos_jobs_delete(const char *id)
{
    jobs_job_t *j = jobs_find(id);
    if (!j) return DEVOS_ERR_NOT_FOUND;
    if (j->run.active) return DEVOS_ERR_INVALID_STATE;      /* cancel/finish first */
    if (jobs_store_available()) jobs_store_remove(id);
    jobs_event_unsubscribe(j);
    if (j->ast) jobs_ast_release(j->ast);
    memset(j, 0, sizeof(*j));
    return DEVOS_OK;
}

devos_err_t devos_jobs_save_draft(const char *id, const char *source, size_t len)
{
    if (!id || !id[0] || !source) return DEVOS_ERR_INVALID_ARG;
    if (!jobs_store_available()) return DEVOS_ERR_INVALID_STATE;
    return jobs_store_draft_save(id, source, len);
}

devos_err_t devos_jobs_load_draft(const char *id, char *out, size_t cap, size_t *out_len)
{
    if (!jobs_store_available()) return DEVOS_ERR_INVALID_STATE;
    return jobs_store_draft_load(id, out, cap, out_len);
}

void devos_jobs_prepare_shutdown(void)
{
    g_jobs.stopping = true;
    g_jobs.ready = false;                   /* no new automatic starts */
    for (int i = 0; i < g_jobs.count; i++) {
        jobs_job_t *j = &g_jobs.jobs[i];
        if (j->used && j->run.active) jobs_run_cancel(j);
    }
}

const char *devos_jobs_restart_check(void)
{
    for (int i = 0; i < g_jobs.count; i++) {
        jobs_job_t *j = &g_jobs.jobs[i];
        if (j->used && j->run.active && !j->run.cancelling) return "a job is running";
    }
    return NULL;
}

devos_err_t devos_jobs_check(const char *source, size_t len, char *diag, size_t cap)
{
    if (diag && cap) diag[0] = '\0';
    jobs_ast_t *ast = jobs_parse(source, len, NULL);
    if (!ast) { if (diag && cap) snprintf(diag, cap, "out of memory"); return DEVOS_ERR_NO_MEM; }
    if (!jobs_validate(ast)) {
        if (diag && cap)
            snprintf(diag, cap, "%s", ast->diag_count ? ast->diag[0].msg : "invalid definition");
        jobs_ast_free(ast);
        return DEVOS_ERR_INVALID_ARG;
    }
    jobs_ast_free(ast);
    return DEVOS_OK;
}

devos_err_t devos_jobs_source(const char *id, char *out, size_t cap, size_t *out_len)
{
    jobs_job_t *j = jobs_find(id);
    if (!j || !j->ast) return DEVOS_ERR_NOT_FOUND;
    size_t n = jobs_serialize(j->ast, out, cap);
    if (out_len) *out_len = n;
    return DEVOS_OK;
}

int devos_jobs_history(const char *id, char *out, size_t cap)
{
    return jobs_store_history_read(id, out, cap);
}

const char *devos_jobs_topic_advisory(void)
{
    return jobs_validate_topic_advisory();
}

int devos_jobs_revisions(const char *id, uint32_t *out, int max)
{
    return jobs_store_revisions(id, out, max);
}

int devos_jobs_revision_source(const char *id, uint32_t rev, char *out, size_t cap)
{
    return jobs_store_revision_read(id, rev, out, cap);
}

#define DRY_ID "~dry"               /* in-memory only: never in the catalog */

static int64_t s_dry_t0;
static uint32_t s_dry_timeout_ms;

devos_err_t devos_jobs_dry_start(const char *source, size_t len, char *diag, size_t cap)
{
    if (diag && cap) diag[0] = '\0';
    if (!source) return DEVOS_ERR_INVALID_ARG;
    jobs_ast_t *ast = jobs_parse(source, len, NULL);
    if (!ast) return DEVOS_ERR_NO_MEM;
    if (!jobs_validate(ast)) {
        if (diag && cap)
            snprintf(diag, cap, "%.95s", ast->diag_count ? ast->diag[0].msg : "invalid definition");
        jobs_ast_free(ast);
        return DEVOS_ERR_INVALID_ARG;
    }
    if (jobs_find(DRY_ID)) { jobs_ast_free(ast); return DEVOS_ERR_INVALID_STATE; }
    jobs_job_t *j = job_alloc();
    if (!j) { jobs_ast_free(ast); return DEVOS_ERR_NO_MEM; }
    memset(j, 0, sizeof(*j));
    j->used = true;
    snprintf(j->id, sizeof(j->id), "%s", DRY_ID);
    j->ast = ast;
    j->revision = 1;
    snprintf(j->name, sizeof(j->name), "%s", ast->root->u.str.s ? ast->root->u.str.s : DRY_ID);
    j->enabled = false;
    read_trigger(j, ast);
    s_dry_t0 = jobs_now_ms();
    s_dry_timeout_ms = 0;                       /* set by the first poll */
    start_run(j, s_dry_t0, JOBS_CAUSE_MANUAL);
    return DEVOS_OK;
}

/* Harvest a finished (or timed-out) dry run and free its slot. */
static devos_err_t dry_harvest(jobs_job_t *j, devos_dryrun_t *out, bool timed_out)
{
    if (timed_out) {
        jobs_run_cancel(j);
        out->timed_out = true;
        snprintf(out->message, sizeof(out->message), "timed out after %u ms", (unsigned)s_dry_timeout_ms);
    } else {
        out->ok = j->last_ok;
        snprintf(out->message, sizeof(out->message), "%.95s", j->last_result);
    }
    out->duration_ms = (int32_t)(jobs_now_ms() - s_dry_t0);
    out->steps = j->run.steps;
    out->trace_n = j->run.trace_n < DEVOS_DRYRUN_TRACE ? j->run.trace_n : DEVOS_DRYRUN_TRACE;
    for (int i = 0; i < out->trace_n; i++) {
        out->trace[i].line = j->run.trace[i].line;
        out->trace[i].col = j->run.trace[i].col;
        out->trace[i].kind = j->run.trace[i].kind;
        out->trace[i].result = j->run.trace[i].result;
    }
    devos_err_t rc = out->timed_out ? DEVOS_ERR_TIMEOUT : DEVOS_OK;
    jobs_event_unsubscribe(j);
    jobs_ast_release(j->ast);
    memset(j, 0, sizeof(*j));
    return rc;
}

/* Non-blocking poll for the UI tick: true when the run finished (or the
 * timeout elapsed) - the result is in *out and the slot is freed. While it
 * returns false the scheduler is still advancing the run. */
bool devos_jobs_dry_poll(devos_dryrun_t *out, uint32_t timeout_ms)
{
    if (!out) return true;
    memset(out, 0, sizeof(*out));
    jobs_job_t *j = jobs_find(DRY_ID);
    if (!j) return true;                         /* none active */
    if (timeout_ms == 0) timeout_ms = 30000;
    if (timeout_ms < 1000) timeout_ms = 1000;
    if (timeout_ms > 120000) timeout_ms = 120000;
    s_dry_timeout_ms = timeout_ms;
    if (!j->run.active) { dry_harvest(j, out, false); return true; }
    if (jobs_now_ms() - s_dry_t0 >= (int64_t)timeout_ms) { dry_harvest(j, out, true); return true; }
    return false;
}

void devos_jobs_dry_cancel(void)
{
    jobs_job_t *j = jobs_find(DRY_ID);
    if (!j) return;
    if (j->run.active) jobs_run_cancel(j);
    jobs_event_unsubscribe(j);
    if (j->ast) jobs_ast_release(j->ast);
    memset(j, 0, sizeof(*j));
}

devos_err_t devos_jobs_dry_run(const char *source, size_t len, uint32_t timeout_ms,
                               devos_dryrun_t *out)
{
    if (!source || !out) return DEVOS_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (timeout_ms == 0) timeout_ms = 30000;
    if (timeout_ms < 1000) timeout_ms = 1000;
    if (timeout_ms > 120000) timeout_ms = 120000;
    char diag[160];
    devos_err_t rc = devos_jobs_dry_start(source, len, diag, sizeof(diag));
    if (rc != DEVOS_OK) {
        snprintf(out->message, sizeof(out->message), "%.95s", diag);
        return rc;
    }
    /* The scheduler advances the run (same as Run now). Under a fake clock
     * (host tests) there is no scheduler task, so pump the run directly. */
    bool fake = jobs_platform_clock_overridden();
    int max_iter = (int)(timeout_ms / 20);
    for (int i = 0; i < max_iter; i++) {
        jobs_job_t *j = jobs_find(DRY_ID);
        if (!j || !j->run.active) break;
        if (fake) jobs_run_tick(j, jobs_now_ms());
        else jobs_platform_sleep_ms(20);
    }
    if (devos_jobs_dry_poll(out, timeout_ms)) return out->timed_out ? DEVOS_ERR_TIMEOUT : DEVOS_OK;
    devos_jobs_dry_cancel();
    out->timed_out = true;
    return DEVOS_ERR_TIMEOUT;
}

int devos_jobs_history_recent(const char *id, devos_run_record_t *out, int max)
{
    if (!id || !out || max <= 0) return 0;
    static EXT_RAM_BSS_ATTR char buf[4096];
    int n = jobs_store_history_read(id, buf, sizeof(buf));
    if (n <= 0) return 0;
    const char *end_buf = buf + n;
    const char *starts[128];
    int count = 0;
    const char *p = buf;
    while (p < end_buf && count < 128) {
        const char *e = memchr(p, '\n', (size_t)(end_buf - p));
        if (!e) e = end_buf;
        if (e > p) starts[count++] = p;
        p = e + 1;
    }
    int written = 0;
    for (int i = count - 1; i >= 0 && written < max; i--) {   /* newest first */
        const char *line = starts[i];
        const char *end = memchr(line, '\n', (size_t)(end_buf - line));
        if (!end) end = end_buf;
        devos_run_record_t *r = &out[written];
        memset(r, 0, sizeof(*r));
        double d = 0;
        if (devos_json_member_num(line, end, "wall", &d)) r->wall = (int64_t)d;
        if (devos_json_member_num(line, end, "ms", &d)) r->duration_ms = (int32_t)d;
        if (devos_json_member_num(line, end, "steps", &d)) r->steps = (int)d;
        const char *okp = devos_json_member(line, end, "ok");
        r->ok = okp && *okp == 't';
        devos_json_member_str(line, end, "cause", r->cause, sizeof(r->cause));
        devos_json_member_str(line, end, "err", r->error, sizeof(r->error));
        written++;
    }
    return written;
}

int devos_jobs_trace(const char *id, devos_jobs_step_t *out, int max)
{
    jobs_job_t *j = jobs_find(id);
    if (!j || !out || max <= 0) return 0;
    int n = j->run.trace_n < max ? j->run.trace_n : max;
    for (int i = 0; i < n; i++) {
        out[i].line = j->run.trace[i].line;
        out[i].col = j->run.trace[i].col;
        out[i].kind = j->run.trace[i].kind;
        out[i].result = j->run.trace[i].result;
    }
    return n;
}

/* ---- scheduler tick ------------------------------------------------------ */

/* Consume one due automatic trigger for `j`; returns true when a trigger fired.
 * Advances the schedule (skip missed intervals / claim the local date). */
static bool job_consume_trigger(jobs_job_t *j, int64_t now)
{
    switch (j->trigger_kind) {
    case JTRIG_EVERY:
        if (j->has_next && j->interval_ms > 0 && now >= j->next_due_ms) {
            do { j->next_due_ms += j->interval_ms; } while (j->next_due_ms <= now);   /* skip missed */
            return true;
        }
        return false;

    case JTRIG_DAILY:
    case JTRIG_WEEKDAYS: {
        if (!g_jobs.sys.time_valid) return false;               /* block while time is invalid */
        if (!j->next_wall_valid || j->seen_tz_gen != g_jobs.sys.tz_generation) {
            if (!jobs_calendar_recompute(j)) return false;      /* clock/timezone changed */
        }
        if (g_jobs.sys.wall_unix_s >= j->next_wall_s) {
            j->claim_date = local_date_of(j->next_wall_s);      /* claim before dispatch */
            jobs_calendar_recompute(j);                         /* arm the next occurrence */
            return true;
        }
        return false;
    }

    case JTRIG_EVENT:
        if (j->ev.pending) {
            j->ev.pending = false;
            /* MQTT: the trigger's declared subscription filters by the message
             * source topic; retained messages are ignored unless opted in. */
            if (strcmp(j->event_topic, "mqtt.message") == 0) {
                if (j->mqtt_topic[0] && !devos_events_topic_match(j->mqtt_topic, j->ev.source))
                    return false;
                if (j->ev.retain && !j->include_retained)
                    return false;
            }
            g_jobs.cur_event = j->ev;
            g_jobs.cur_event_valid = true;
            bool match = jobs_trigger_where_matches(j);
            if (!match) g_jobs.cur_event_valid = false;   /* keep it for the run body */
            return match;                                 /* no match: consumed, not fired */
        }
        return false;

    default:
        return false;
    }
}

/* Automatic admission: cooldown after the last automatic start. */
static bool admit_auto(const jobs_job_t *j, int64_t now)
{
    if (j->cooldown_ms > 0 && j->last_start_ms && now - j->last_start_ms < j->cooldown_ms)
        return false;
    return true;
}

void devos_jobs_tick(void)
{
    if (g_jobs.state != DEVOS_JOBS_READY) return;
    int64_t now = jobs_now_ms();

    /* Fill per-job pending slots from the bounded event queue (callbacks run
     * on this task, outside the events lock). */
    devos_events_drain(0);

    for (int i = 0; i < g_jobs.count; i++) {
        jobs_job_t *j = &g_jobs.jobs[i];
        if (!j->used) continue;

        bool auto_ok = j->enabled && !g_jobs.paused && !g_jobs.safe_paused &&
                       !g_jobs.stopping && g_jobs.ready;
        bool due = auto_ok ? job_consume_trigger(j, now) : false;

        if (j->run.active) {
            /* overlap: skip, or retain only the latest pending trigger */
            if (due) {
                if (j->overlap_queue_one) j->pending_run = true;
                else j->skipped++;
            }
            jobs_run_tick(j, now);
        }

        if (!j->run.active) {
            bool fire = due || j->pending_run;
            if (fire) {
                bool queued = j->pending_run;
                j->pending_run = false;
                if (admit_auto(j, now)) {
                    j->last_start_ms = now;
                    start_run(j, now, queued ? JOBS_CAUSE_QUEUE :
                              (j->trigger_kind == JTRIG_EVENT ? JOBS_CAUSE_EVENT : JOBS_CAUSE_SCHEDULE));
                    jobs_run_tick(j, now);   /* advance the fresh run one step */
                } else {
                    j->skipped++;            /* cooldown */
                }
            }
        }
        g_jobs.cur_event_valid = false;      /* the run (if any) holds its own copy */
    }
}
