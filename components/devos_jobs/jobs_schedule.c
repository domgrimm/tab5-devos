/* jobs_schedule: the Jobs engine - job registry, triggers, the scheduler tick
 * and the public devos_jobs API (PLAN.md 9.1). Core 0 on target; host tests
 * drive devos_jobs_tick() with a fake clock. No LVGL. */
#include "jobs_internal.h"
#include "jobs_platform.h"
#include "jobs_store.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "devos_config.h"
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

static void start_run(jobs_job_t *j, int64_t now)
{
    char id[JOBS_RUN_ID_MAX];
    snprintf(id, sizeof(id), "r%u", (unsigned)++g_jobs.run_seq);
    jobs_run_begin(j, id, now, j->revision);
}

/* ---- lifecycle ---- */
#ifdef ESP_PLATFORM
static void scheduler_loop(void *arg)
{
    (void)arg;
    for (;;) {
        devos_jobs_tick();
        jobs_platform_sleep_ms(50);
    }
}
#endif

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
    const jobs_node_t *trig = ast->root->a;
    j->trigger_kind = trig ? trig->sub : JTRIG_MANUAL;
    j->interval_ms = (trig && trig->sub == JTRIG_EVERY) ? trig->u.i : 0;
    j->enabled = enabled;
    if (enabled && j->trigger_kind == JTRIG_EVERY && j->interval_ms > 0) {
        j->has_next = true;
        j->next_due_ms = jobs_now_ms() + j->interval_ms;
    }
    return true;
}

bool devos_jobs_init(void)
{
    jobs_platform_init();
    memset(&g_jobs, 0, sizeof(g_jobs));
    g_jobs.state = DEVOS_JOBS_READY;
    g_jobs.ready = true;                 /* durable load is best-effort below */
    if (jobs_store_init(TAB5_SD_MOUNT_POINT)) {
        jobs_store_load(load_cb, NULL);
    }
#ifdef ESP_PLATFORM
    if (!jobs_platform_start_scheduler(scheduler_loop, NULL)) {
        g_jobs.state = DEVOS_JOBS_OFF;
        g_jobs.ready = false;
        return false;
    }
#endif
    return true;
}

void devos_jobs_shutdown(void)
{
    for (int i = 0; i < g_jobs.count; i++) {
        jobs_job_t *j = &g_jobs.jobs[i];
        if (!j->used) continue;
        if (j->run.active) jobs_run_cancel(j);
        if (j->ast) { jobs_ast_release(j->ast); j->ast = NULL; }
    }
    memset(&g_jobs, 0, sizeof(g_jobs));
    g_jobs.state = DEVOS_JOBS_OFF;
}

devos_jobs_state_t devos_jobs_state(void) { return g_jobs.state; }
bool devos_jobs_ready(void) { return g_jobs.ready; }
bool devos_jobs_paused(void) { return g_jobs.paused; }
void devos_jobs_set_paused(bool paused) { g_jobs.paused = paused; }
void devos_jobs_set_system(const devos_jobs_system_t *s)
{
    if (s) g_jobs.sys = *s;
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
        if (j->trigger_kind == JTRIG_EVERY) fmt_interval(j->interval_ms, out->trigger, sizeof(out->trigger));
        else snprintf(out->trigger, sizeof(out->trigger), "manual");
        snprintf(out->last_result, sizeof(out->last_result), "%s", j->last_result);
        out->running = j->run.active;
        out->revision = j->revision;
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
    if (!id || !id[0] || !source) return DEVOS_ERR_INVALID_ARG;
    if (strlen(id) >= DEVOS_JOBS_ID_MAX) return DEVOS_ERR_INVALID_SIZE;

    jobs_ast_t *ast = jobs_parse(source, len, NULL);
    if (!ast) return DEVOS_ERR_NO_MEM;
    if (!jobs_validate(ast)) { jobs_ast_free(ast); return DEVOS_ERR_INVALID_ARG; }

    jobs_job_t *j = jobs_find(id);
    bool existed = j != NULL;
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

    const jobs_node_t *trig = ast->root->a;
    j->trigger_kind = trig ? trig->sub : JTRIG_MANUAL;
    j->interval_ms = (trig && trig->sub == JTRIG_EVERY) ? trig->u.i : 0;
    if (!existed) { j->enabled = false; j->has_next = false; }
    else if (j->enabled && j->trigger_kind == JTRIG_EVERY) {
        j->has_next = true;
        j->next_due_ms = jobs_now_ms() + j->interval_ms;
    }
    if (out_revision) *out_revision = j->revision;
    return DEVOS_OK;
}

devos_err_t devos_jobs_set_enabled(const char *id, bool enabled)
{
    jobs_job_t *j = jobs_find(id);
    if (!j) return DEVOS_ERR_NOT_FOUND;
    j->enabled = enabled;
    if (enabled && j->trigger_kind == JTRIG_EVERY && j->interval_ms > 0) {
        j->has_next = true;
        j->next_due_ms = jobs_now_ms() + j->interval_ms;   /* first run after one interval */
    } else if (!enabled) {
        j->has_next = false;
    }
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
    start_run(j, jobs_now_ms());
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
    if (j->ast) jobs_ast_release(j->ast);
    memset(j, 0, sizeof(*j));
    return DEVOS_OK;
}

void devos_jobs_tick(void)
{
    if (g_jobs.state != DEVOS_JOBS_READY) return;
    int64_t now = jobs_now_ms();
    for (int i = 0; i < g_jobs.count; i++) {
        jobs_job_t *j = &g_jobs.jobs[i];
        if (!j->used) continue;
        /* Automatic dispatch: interval due, not already running, not paused. */
        if (!j->run.active && j->enabled && !g_jobs.paused && g_jobs.ready &&
            j->trigger_kind == JTRIG_EVERY && j->has_next && j->interval_ms > 0 &&
            now >= j->next_due_ms) {
            do { j->next_due_ms += j->interval_ms; } while (j->next_due_ms <= now);   /* skip missed */
            start_run(j, now);
        }
        /* Advance whatever is running (including a run just started above). */
        if (j->run.active) jobs_run_tick(j, now);
    }
}
