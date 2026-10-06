/* jobs_system: the system.log and system.notify action providers. No LVGL:
 * system.log appends a sanitized line to a bounded RAM ring; system.notify
 * queues a small notice the Jobs UI bridge turns into a toast. Completion is
 * "line recorded" / "notice queued", not user acknowledgement (PLAN.md 7.3). */
#include "jobs_providers.h"
#include "devos_actions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_mx;
#define SYS_LOCK()   do { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); } while (0)
#define SYS_UNLOCK() do { if (s_mx) xSemaphoreGive(s_mx); } while (0)
#else
#include <pthread.h>
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
#define SYS_LOCK()   pthread_mutex_lock(&s_mx)
#define SYS_UNLOCK() pthread_mutex_unlock(&s_mx)
#endif

static char s_log[JOBS_SYSTEM_LOG_MAX][JOBS_SYSTEM_LOG_LINE];
static int s_log_head, s_log_n;
static char s_notice[JOBS_SYSTEM_NOTICE_MAX][JOBS_SYSTEM_NOTICE_LEN];
static int s_notice_level[JOBS_SYSTEM_NOTICE_MAX];
static int s_notice_head, s_notice_n;

static void log_push(const char *line)
{
    SYS_LOCK();
    int idx = (s_log_head + s_log_n) % JOBS_SYSTEM_LOG_MAX;
    snprintf(s_log[idx], JOBS_SYSTEM_LOG_LINE, "%s", line);
    if (s_log_n < JOBS_SYSTEM_LOG_MAX) s_log_n++;
    else s_log_head = (s_log_head + 1) % JOBS_SYSTEM_LOG_MAX;
    SYS_UNLOCK();
}

int jobs_system_log_count(void)
{
    SYS_LOCK();
    int n = s_log_n;
    SYS_UNLOCK();
    return n;
}

int jobs_system_log_tail(char *out, int max)
{
    if (!out || max <= 0) return 0;
    if (max > JOBS_SYSTEM_LOG_MAX) max = JOBS_SYSTEM_LOG_MAX;
    SYS_LOCK();
    int n = s_log_n < max ? s_log_n : max;
    int start = (s_log_head + s_log_n - n) % JOBS_SYSTEM_LOG_MAX;
    for (int i = 0; i < n; i++)
        snprintf(out + (size_t)i * JOBS_SYSTEM_LOG_LINE, JOBS_SYSTEM_LOG_LINE, "%s",
                 s_log[(start + i) % JOBS_SYSTEM_LOG_MAX]);
    SYS_UNLOCK();
    return n;
}

static void notice_push(const char *msg, int level)
{
    SYS_LOCK();
    if (s_notice_n >= JOBS_SYSTEM_NOTICE_MAX) {          /* drop the oldest */
        s_notice_head = (s_notice_head + 1) % JOBS_SYSTEM_NOTICE_MAX;
        s_notice_n--;
    }
    int idx = (s_notice_head + s_notice_n) % JOBS_SYSTEM_NOTICE_MAX;
    snprintf(s_notice[idx], JOBS_SYSTEM_NOTICE_LEN, "%s", msg);
    s_notice_level[idx] = level;
    s_notice_n++;
    SYS_UNLOCK();
}

bool jobs_system_take_notice(char *msg, size_t cap, int *level)
{
    bool got = false;
    SYS_LOCK();
    if (s_notice_n > 0) {
        if (msg && cap) snprintf(msg, cap, "%s", s_notice[s_notice_head]);
        if (level) *level = s_notice_level[s_notice_head];
        s_notice_head = (s_notice_head + 1) % JOBS_SYSTEM_NOTICE_MAX;
        s_notice_n--;
        got = true;
    }
    SYS_UNLOCK();
    return got;
}

/* ---- ops ---- */
typedef struct { devos_value_t out[1]; } sys_op_t;

static const char *arg_str(const devos_action_args_t *args, int i, size_t *len)
{
    if (!args || i >= args->arg_count) { if (len) *len = 0; return ""; }
    const devos_value_t *v = &args->args[i];
    if (v->type != DEVOS_VAL_STR || !v->v.str.s) { if (len) *len = 0; return ""; }
    if (len) *len = v->v.str.len;
    return v->v.str.s;
}

static devos_err_t log_start(const devos_action_args_t *args, const devos_action_context_t *ctx, void **op)
{
    (void)ctx;
    size_t n = 0;
    const char *msg = arg_str(args, 0, &n);
    char line[JOBS_SYSTEM_LOG_LINE];
    size_t k = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
    memcpy(line, msg, k);
    line[k] = '\0';
    /* Allocate before the side effect: a failed allocation must not report
     * "could not start" after the line was already recorded. */
    sys_op_t *o = calloc(1, sizeof(*o));
    if (!o) return DEVOS_ERR_NO_MEM;
    log_push(line);
    o->out[0].type = DEVOS_VAL_BOOL;
    o->out[0].v.b = true;
    *op = o;
    return DEVOS_OK;
}

static devos_err_t notify_start(const devos_action_args_t *args, const devos_action_context_t *ctx, void **op)
{
    (void)ctx;
    size_t n = 0;
    const char *msg = arg_str(args, 0, &n);
    const char *lvl = arg_str(args, 1, NULL);
    int level = strcmp(lvl, "error") == 0 ? 2 :
                strcmp(lvl, "warning") == 0 ? 1 :
                strcmp(lvl, "success") == 0 ? 3 : 0;
    char line[JOBS_SYSTEM_NOTICE_LEN];
    size_t k = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
    memcpy(line, msg, k);
    line[k] = '\0';
    /* Allocate before the side effect (see log_start). */
    sys_op_t *o = calloc(1, sizeof(*o));
    if (!o) return DEVOS_ERR_NO_MEM;
    notice_push(line, level);
    o->out[0].type = DEVOS_VAL_BOOL;
    o->out[0].v.b = true;
    *op = o;
    return DEVOS_OK;
}

static devos_err_t sys_poll(void *op, devos_action_state_t *state, devos_action_result_t *result)
{
    sys_op_t *o = op;
    *state = DEVOS_ACT_DONE;
    result->outs = o->out;
    result->out_count = 1;
    return DEVOS_OK;
}
static devos_err_t sys_cancel(void *op) { (void)op; return DEVOS_OK; }
static void sys_release(void *op) { free(op); }
static const devos_action_ops_t SYS_OPS = { log_start, sys_poll, sys_cancel, sys_release, NULL };
static const devos_action_ops_t NOTIFY_OPS = { notify_start, sys_poll, sys_cancel, sys_release, NULL };

static const devos_action_param_t LOG_P[] = {
    { .name = "message", .type = DEVOS_VAL_STR, .required = true, .expression = true, .max_len = 512 },
};
static const devos_action_out_t LOG_O[] = { { .name = "recorded", .type = DEVOS_VAL_BOOL } };
static const devos_action_descriptor_t LOG_D = {
    .id = "system.log", .schema_version = 1, .provider_uid = "jobs", .category = "system",
    .label = "Log a message", .params = LOG_P, .param_count = 1, .outs = LOG_O, .out_count = 1,
    .effect = DEVOS_EFFECT_MUTATE, .retry_safe = true, .local_only = true, .ops = &SYS_OPS,
};

static const devos_action_param_t NOTIFY_P[] = {
    { .name = "message", .type = DEVOS_VAL_STR, .required = true, .expression = true, .max_len = 512 },
    /* `info` stays first so it remains the default: an unset level is a plain
     * notice, not a green tick. */
    { .name = "level", .type = DEVOS_VAL_STR, .choices = "info|success|warning|error",
      .help = "info, success (green tick), warning or error" },
};
static const devos_action_out_t NOTIFY_O[] = { { .name = "queued", .type = DEVOS_VAL_BOOL } };
static const devos_action_descriptor_t NOTIFY_D = {
    .id = "system.notify", .schema_version = 1, .provider_uid = "jobs", .category = "system",
    .label = "Show a notice", .params = NOTIFY_P, .param_count = 2, .outs = NOTIFY_O, .out_count = 1,
    .effect = DEVOS_EFFECT_MUTATE, .local_only = true, .ops = &NOTIFY_OPS,
};

void jobs_system_register(void)
{
#ifdef ESP_PLATFORM
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
#endif
    devos_actions_register(&LOG_D);
    devos_actions_register(&NOTIFY_D);
}
