/* Host test for the Jobs scheduler and interpreter (components/devos_jobs
 * jobs_runtime.c + jobs_schedule.c) with an injected fake clock and fake
 * providers: branches on a negative result, wait yielding so another job
 * advances, interval scheduling (first run after one interval, skip missed),
 * run-now overlap refusal, cancel, and revision retain/release.
 *
 *   gcc -O2 -Icomponents/devos_jobs -Icomponents/devos_actions -Icomponents/devos_err \
 *       -Imain/jobs_providers tools/jobs_runtime_test.c \
 *       components/devos_jobs/jobs_model.c components/devos_jobs/jobs_parse.c \
 *       components/devos_jobs/jobs_validate.c components/devos_jobs/jobs_serialize.c \
 *       components/devos_jobs/jobs_platform.c components/devos_jobs/jobs_runtime.c \
 *       components/devos_jobs/jobs_schedule.c components/devos_actions/devos_actions.c \
 *       main/jobs_providers/jobs_system.c -lpthread -o /tmp/jobs_runtime_test && /tmp/jobs_runtime_test
 */
#include "devos_jobs.h"
#include "jobs_internal.h"
#include "jobs_platform.h"
#include "devos_actions.h"
#include "jobs_providers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- fake clock ---- */
static int64_t s_now;
static int64_t fake_now(void *u) { (void)u; return s_now; }

/* ---- fake providers ---- */
static int s_check_ok = 1;
typedef struct { int polls, need; devos_value_t outs[1]; } t_op_t;

static devos_err_t check_start(const devos_action_args_t *a, const devos_action_context_t *c, void **op)
{
    (void)a; (void)c;
    t_op_t *o = calloc(1, sizeof(*o));
    if (!o) return DEVOS_ERR_NO_MEM;
    o->need = 1;
    o->outs[0].type = DEVOS_VAL_BOOL;
    o->outs[0].v.b = s_check_ok;
    *op = o;
    return DEVOS_OK;
}
static devos_err_t slow_start(const devos_action_args_t *a, const devos_action_context_t *c, void **op)
{
    (void)a; (void)c;
    t_op_t *o = calloc(1, sizeof(*o));
    if (!o) return DEVOS_ERR_NO_MEM;
    o->need = 3;                                   /* complete after 3 polls */
    o->outs[0].type = DEVOS_VAL_BOOL;
    o->outs[0].v.b = true;
    *op = o;
    return DEVOS_OK;
}
static devos_err_t t_poll(void *op, devos_action_state_t *st, devos_action_result_t *res)
{
    t_op_t *o = op;
    if (o->polls++ < o->need - 1) { *st = DEVOS_ACT_PENDING; return DEVOS_OK; }
    *st = DEVOS_ACT_DONE;
    res->outs = o->outs;
    res->out_count = 1;
    return DEVOS_OK;
}
static devos_err_t t_cancel(void *op) { (void)op; return DEVOS_OK; }
static void t_release(void *op) { free(op); }
static const devos_action_ops_t CHECK_OPS = { check_start, t_poll, t_cancel, t_release, NULL };
static const devos_action_ops_t SLOW_OPS = { slow_start, t_poll, t_cancel, t_release, NULL };
static const devos_action_out_t BOOL_O[] = { { .name = "ok", .type = DEVOS_VAL_BOOL } };
static const devos_action_descriptor_t CHECK_D = {
    .id = "test.check", .schema_version = 1, .outs = BOOL_O, .out_count = 1, .ops = &CHECK_OPS,
};
static const devos_action_descriptor_t SLOW_D = {
    .id = "test.slow", .schema_version = 1, .outs = BOOL_O, .out_count = 1, .ops = &SLOW_OPS,
};

static const char *last_log(void)
{
    static char lines[1 * JOBS_SYSTEM_LOG_LINE];
    int n = jobs_system_log_tail(lines, 1);
    return n == 1 ? lines : "";
}

static void apply_ok(const char *id, const char *src)
{
    uint32_t rev = 0;
    devos_err_t rc = devos_jobs_apply(id, src, strlen(src), &rev);
    if (rc != DEVOS_OK) { printf("FAIL apply %s: %d\n", id, rc); fails++; }
    checks++;
}

int main(void)
{
    jobs_platform_set_clock(fake_now, NULL);
    s_now = 1000;
    CHECK(devos_jobs_init());
    jobs_system_register();
    CHECK(devos_actions_register(&CHECK_D) == DEVOS_OK);
    CHECK(devos_actions_register(&SLOW_D) == DEVOS_OK);

    /* 1. branch on a negative transport result */
    s_check_ok = 0;
    apply_ok("branch", "version 1;\njob \"branch\" {\n trigger manual;\n"
                       " test.check() as c;\n"
                       " if !c.ok { system.log(message: \"fail\"); } else { system.log(message: \"ok\"); }\n}\n");
    CHECK(devos_jobs_run_now("branch") == DEVOS_OK);
    devos_jobs_tick();
    CHECK(strcmp(last_log(), "fail") == 0);
    s_check_ok = 1;
    CHECK(devos_jobs_run_now("branch") == DEVOS_OK);
    devos_jobs_tick();
    CHECK(strcmp(last_log(), "ok") == 0);

    /* 2. wait yields; another job advances meanwhile */
    apply_ok("waiter", "version 1;\njob \"waiter\" {\n trigger manual;\n wait 100ms;\n"
                       " system.log(message: \"A done\");\n}\n");
    apply_ok("quick", "version 1;\njob \"quick\" {\n trigger manual;\n system.log(message: \"B done\");\n}\n");
    CHECK(devos_jobs_run_now("waiter") == DEVOS_OK);
    CHECK(devos_jobs_run_now("quick") == DEVOS_OK);
    devos_jobs_tick();                              /* B completes; A parks on the wait */
    CHECK(strcmp(last_log(), "B done") == 0);
    devos_jobs_run_t snap;
    CHECK(devos_jobs_run(&snap) && strcmp(snap.job_id, "waiter") == 0);
    s_now += 50; devos_jobs_tick();                 /* still waiting */
    CHECK(strcmp(last_log(), "B done") == 0);
    s_now += 50; devos_jobs_tick();                 /* 100 ms elapsed: A finishes */
    CHECK(strcmp(last_log(), "A done") == 0);

    /* 3. interval: first run one interval after enabling, then each interval */
    s_now = 2000;
    apply_ok("every", "version 1;\njob \"every\" {\n trigger every 1s;\n system.log(message: \"tick\");\n}\n");
    CHECK(devos_jobs_set_enabled("every", true) == DEVOS_OK);
    int base = jobs_system_log_count();
    devos_jobs_tick();                              /* t=2000, not due (due at 3000) */
    CHECK(jobs_system_log_count() == base);
    s_now = 3000; devos_jobs_tick();                /* due */
    CHECK(jobs_system_log_count() == base + 1);
    s_now = 3500; devos_jobs_tick();
    CHECK(jobs_system_log_count() == base + 1);
    s_now = 4000; devos_jobs_tick();
    CHECK(jobs_system_log_count() == base + 2);

    /* 4. overlap: run now is refused while the job is running */
    apply_ok("slow", "version 1;\njob \"slow\" {\n trigger manual;\n test.slow() as s;\n}\n");
    CHECK(devos_jobs_run_now("slow") == DEVOS_OK);
    CHECK(devos_jobs_run_now("slow") == DEVOS_ERR_INVALID_STATE);
    CHECK(devos_jobs_cancel("slow") == DEVOS_OK);
    devos_jobs_tick();
    CHECK(devos_jobs_count() >= 5);

    /* 5. cancel finishes the run */
    devos_job_summary_t sum;
    CHECK(devos_jobs_summary_at(4, &sum) && strcmp(sum.id, "slow") == 0);
    CHECK(strstr(sum.last_result, "cancelled") != NULL);

    /* 6. revision retain: a running run keeps its own AST when a new revision is applied */
    apply_ok("rev", "version 1;\njob \"rev\" {\n trigger manual;\n system.log(message: \"rev1\");\n}\n");
    CHECK(devos_jobs_run_now("rev") == DEVOS_OK);
    apply_ok("rev", "version 1;\njob \"rev\" {\n trigger manual;\n system.log(message: \"rev2\");\n}\n");
    devos_jobs_tick();                              /* the run uses rev1 */
    CHECK(strcmp(last_log(), "rev1") == 0);
    CHECK(devos_jobs_run_now("rev") == DEVOS_OK);   /* a new run uses rev2 */
    devos_jobs_tick();
    CHECK(strcmp(last_log(), "rev2") == 0);

    /* 7. global pause stops automatic runs but not Run now */
    apply_ok("paused", "version 1;\njob \"paused\" {\n trigger every 1s;\n system.log(message: \"auto\");\n}\n");
    devos_jobs_set_enabled("paused", true);
    devos_jobs_set_paused(true);
    base = jobs_system_log_count();
    s_now += 5000; devos_jobs_tick();
    CHECK(jobs_system_log_count() == base);         /* no automatic run while paused */
    CHECK(devos_jobs_run_now("paused") == DEVOS_OK);
    devos_jobs_tick();
    CHECK(strcmp(last_log(), "auto") == 0);
    devos_jobs_set_paused(false);

    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}
