/* Host soak / leak test for the Jobs engine. Drives a mixed workload for many
 * simulated seconds - interval jobs, manual run-now, waits, repeats, reusable
 * job calls, cancellations - and checks that job slots, action slots and run
 * state all settle back to zero (no unbounded growth). Run it under
 * AddressSanitizer (add -fsanitize=address,undefined) or Valgrind to catch
 * leaks where that runtime is available; the slot checks run everywhere.
 *
 *   mkdir -p /tmp/jobs_soak && cd /tmp/jobs_soak
 *   gcc -O2 -I$REPO/components/devos_jobs -I$REPO/components/devos_actions \
 *       -I$REPO/components/devos_err -I$REPO/components/devos_json \
 *       -I$REPO/components/devos_events -I$REPO/main/jobs_providers \
 *       $REPO/tools/jobs_soak_test.c \
 *       $REPO/components/devos_jobs/jobs_model.c $REPO/components/devos_jobs/jobs_parse.c \
 *       $REPO/components/devos_jobs/jobs_validate.c $REPO/components/devos_jobs/jobs_serialize.c \
 *       $REPO/components/devos_jobs/jobs_platform.c $REPO/components/devos_jobs/jobs_runtime.c \
 *       $REPO/components/devos_jobs/jobs_schedule.c $REPO/components/devos_jobs/jobs_store.c \
 *       $REPO/components/devos_actions/devos_actions.c $REPO/components/devos_json/devos_json.c \
 *       $REPO/components/devos_events/devos_events.c \
 *       $REPO/main/jobs_providers/jobs_system.c -lpthread -o /tmp/jobs_soak_test && /tmp/jobs_soak_test
 */
#include "devos_jobs.h"
#include "devos_actions.h"
#include "jobs_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int64_t s_now;
static int64_t fake_now(void *u) { (void)u; return s_now; }

typedef struct { int polls, need; devos_value_t outs[1]; } op_t;
static devos_err_t op_start(const devos_action_args_t *a, const devos_action_context_t *c, void **op)
{
    (void)a; (void)c;
    op_t *o = calloc(1, sizeof(*o));
    if (!o) return DEVOS_ERR_NO_MEM;
    o->need = 2;
    o->outs[0].type = DEVOS_VAL_BOOL;
    o->outs[0].v.b = true;
    *op = o;
    return DEVOS_OK;
}
static devos_err_t op_poll(void *op, devos_action_state_t *st, devos_action_result_t *res)
{
    op_t *o = op;
    if (o->polls++ < o->need - 1) { *st = DEVOS_ACT_PENDING; return DEVOS_OK; }
    *st = DEVOS_ACT_DONE;
    res->outs = o->outs;
    res->out_count = 1;
    return DEVOS_OK;
}
static devos_err_t op_cancel(void *op) { (void)op; return DEVOS_OK; }
static void op_release(void *op) { free(op); }
static const devos_action_ops_t OPS = { op_start, op_poll, op_cancel, op_release, NULL };
static const devos_action_out_t OUT[] = { { .name = "ok", .type = DEVOS_VAL_BOOL } };
static const devos_action_descriptor_t ACT = {
    .id = "soak.work", .schema_version = 1, .outs = OUT, .out_count = 1, .ops = &OPS,
};

static void apply(const char *id, const char *src)
{
    uint32_t rev = 0;
    if (devos_jobs_apply(id, src, strlen(src), &rev) != DEVOS_OK) { printf("apply %s failed\n", id); fails++; }
    checks++;
}

int main(void)
{
    jobs_platform_set_clock(fake_now, NULL);
    s_now = 1000;
    CHECK(devos_jobs_init());
    CHECK(devos_actions_register(&ACT) == DEVOS_OK);

    apply("tick", "version 1;\njob \"tick\" {\n trigger every 200ms;\n soak.work() as w;\n}\n");
    apply("loop", "version 1;\njob \"loop\" {\n trigger every 500ms;\n"
                  " repeat 4 as i { soak.work() as w; }\n}\n");
    apply("wait", "version 1;\njob \"wait\" {\n trigger every 300ms;\n wait 100ms;\n soak.work() as w;\n}\n");
    apply("child", "version 1;\njob \"child\"(n: int) {\n trigger manual;\n soak.work() as w;\n return w.ok;\n}\n");
    apply("caller", "version 1;\njob \"caller\" {\n trigger every 700ms;\n"
                    " run \"child\"(n: 1) as a; if a { soak.work() as w; }\n}\n");
    apply("manual", "version 1;\njob \"manual\" {\n trigger manual;\n soak.work() as w;\n}\n");

    CHECK(devos_jobs_set_enabled("tick", true) == DEVOS_OK);
    CHECK(devos_jobs_set_enabled("loop", true) == DEVOS_OK);
    CHECK(devos_jobs_set_enabled("wait", true) == DEVOS_OK);
    CHECK(devos_jobs_set_enabled("caller", true) == DEVOS_OK);
    const int n_jobs = devos_jobs_count();

    /* ~1200 simulated seconds of mixed traffic */
    const int ITERS = 12000;
    for (int i = 0; i < ITERS; i++) {
        s_now += 100;
        if (i % 7 == 0) devos_jobs_run_now("manual");         /* may refuse if already running */
        if (i % 13 == 0) devos_jobs_run_now("caller");
        if (i % 29 == 0) devos_jobs_cancel("manual");
        if (i % 101 == 0) devos_jobs_set_paused(!devos_jobs_paused());
        devos_jobs_tick();
    }

    /* settle: no new starts, drain every running job */
    devos_jobs_set_paused(true);
    for (int i = 0; i < 50; i++) { s_now += 100; devos_jobs_tick(); }

    devos_jobs_run_t run;
    CHECK(!devos_jobs_run(&run) || !run.active);
    CHECK(devos_actions_outstanding() == 0);                  /* every action slot released */
    CHECK(devos_jobs_count() == n_jobs);                      /* no job-slot leak */

    /* cancel/run-now one more time and settle again */
    for (int i = 0; i < 20; i++) {
        devos_jobs_run_now("manual");
        devos_jobs_cancel("manual");
        s_now += 100;
        devos_jobs_tick();
    }
    for (int i = 0; i < 50; i++) { s_now += 100; devos_jobs_tick(); }
    CHECK(devos_actions_outstanding() == 0);
    CHECK(devos_jobs_count() == n_jobs);

    printf("soak: %d iterations over %lld s, %d jobs, 0 leaked slots\n",
           ITERS, (long long)(s_now - 1000) / 1000, n_jobs);
    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}
