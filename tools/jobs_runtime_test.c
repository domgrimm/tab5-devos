/* Host test for the Jobs scheduler and interpreter (components/devos_jobs
 * jobs_runtime.c + jobs_schedule.c) with an injected fake clock and fake
 * providers: branches on a negative result, wait yielding so another job
 * advances, interval scheduling (first run after one interval, skip missed),
 * run-now overlap refusal, cancel, and revision retain/release.
 *
 *   gcc -O2 -Icomponents/devos_jobs -Icomponents/devos_actions -Icomponents/devos_err \
 *       -Icomponents/devos_json -Icomponents/devos_events -Imain/jobs_providers tools/jobs_runtime_test.c \
 *       components/devos_jobs/jobs_model.c components/devos_jobs/jobs_parse.c \
 *       components/devos_jobs/jobs_validate.c components/devos_jobs/jobs_serialize.c \
 *       components/devos_jobs/jobs_platform.c components/devos_jobs/jobs_runtime.c \
 *       components/devos_jobs/jobs_schedule.c components/devos_jobs/jobs_store.c \
 *       components/devos_actions/devos_actions.c components/devos_json/devos_json.c \
 *       components/devos_events/devos_events.c \
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
#include <unistd.h>

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

/* A credential-capable provider: records the token it was handed. */
static char s_got_token[64];
static int s_secret_wipes;
static devos_err_t auth_start(const devos_action_args_t *a, const devos_action_context_t *c, void **op)
{
    (void)c;
    const char *t = (a && a->arg_count > 0 && a->args[0].type == DEVOS_VAL_STR) ? a->args[0].v.str.s : "";
    snprintf(s_got_token, sizeof(s_got_token), "%s", t);
    t_op_t *o = calloc(1, sizeof(*o));
    if (!o) return DEVOS_ERR_NO_MEM;
    o->need = 1;
    o->outs[0].type = DEVOS_VAL_BOOL;
    o->outs[0].v.b = true;
    *op = o;
    return DEVOS_OK;
}
static const devos_action_ops_t AUTH_OPS = { auth_start, t_poll, t_cancel, t_release, NULL };
static const devos_action_param_t AUTH_P[] = {
    { .name = "token", .type = DEVOS_VAL_STR, .credential = true, .max_len = 256 },
};
static const devos_action_descriptor_t AUTH_D = {
    .id = "test.auth", .schema_version = 1, .params = AUTH_P, .param_count = 1,
    .outs = BOOL_O, .out_count = 1, .ops = &AUTH_OPS,
};

/* Fake secret store: "tok" -> "s3cr3t"; records wipes. */
static int fake_secret_resolve(const char *name, char *out, size_t cap, void *user)
{
    (void)user;
    if (strcmp(name, "tok") != 0) return -1;
    snprintf(out, cap, "s3cr3t");
    return 6;
}
static void fake_secret_wipe(void *p, size_t len)
{
    (void)p;
    (void)len;
    s_secret_wipes++;
}

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
    CHECK(devos_actions_register(&AUTH_D) == DEVOS_OK);
    devos_jobs_set_secret_hooks(fake_secret_resolve, fake_secret_wipe, NULL);

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

    /* stop the interval jobs so the later checks see only their own logs */
    devos_jobs_set_enabled("every", false);
    devos_jobs_set_enabled("paused", false);

    /* 8. bounded repeat: the body runs once per index; the index is read-only */
    s_now += 1000;
    apply_ok("loop", "version 1;\njob \"loop\" {\n trigger manual;\n"
                     " repeat 3 as i {\n system.log(message: \"iter ${i}\");\n }\n}\n");
    base = jobs_system_log_count();
    CHECK(devos_jobs_run_now("loop") == DEVOS_OK);
    devos_jobs_tick();
    CHECK(jobs_system_log_count() == base + 3);
    CHECK(strcmp(last_log(), "iter 2") == 0);

    /* 9. json_get: dot paths, array indexes, missing -> null */
    apply_ok("json", "version 1;\njob \"json\" {\n trigger manual;\n"
                     " set body = \"{\\\"temp\\\": 21, \\\"list\\\": [10, 20], \\\"nested\\\": {\\\"ok\\\": true}}\";\n"
                     " set t = json_get(body, \"temp\");\n"
                     " system.log(message: \"temp=${t}\");\n"
                     " set a = json_get(body, \"list[1]\");\n"
                     " system.log(message: \"arr=${a}\");\n"
                     " if json_get(body, \"nested.ok\") { system.log(message: \"nested true\"); }\n"
                     " set missing = json_get(body, \"nope\");\n"
                     " if missing == null { system.log(message: \"missing null\"); }\n"
                     "}\n");
    base = jobs_system_log_count();
    CHECK(devos_jobs_run_now("json") == DEVOS_OK);
    devos_jobs_tick();
    CHECK(jobs_system_log_count() == base + 4);
    {
        static char tail[4 * JOBS_SYSTEM_LOG_LINE];
        int tn = jobs_system_log_tail(tail, 4);
        CHECK(tn == 4);
        CHECK(strstr(tail + 0 * JOBS_SYSTEM_LOG_LINE, "temp=21") != NULL);
        CHECK(strstr(tail + 1 * JOBS_SYSTEM_LOG_LINE, "arr=20") != NULL);
        CHECK(strstr(tail + 2 * JOBS_SYSTEM_LOG_LINE, "nested true") != NULL);
        CHECK(strstr(tail + 3 * JOBS_SYSTEM_LOG_LINE, "missing null") != NULL);
    }

    /* json_get of an object returns its raw JSON, composable with another get */
    apply_ok("rawjson", "version 1;\njob \"RawJson\" {\n trigger manual;\n"
                        " set body = \"{\\\"nested\\\": {\\\"ok\\\": true, \\\"n\\\": 7}}\";\n"
                        " set sub = json_get(body, \"nested\");\n"
                        " set ok = json_get(sub, \"ok\");\n"
                        " set nn = json_get(sub, \"n\");\n"
                        " if ok { system.log(message: \"composed ${nn}\"); }\n"
                        "}\n");
    base = jobs_system_log_count();
    CHECK(devos_jobs_run_now("rawjson") == DEVOS_OK);
    devos_jobs_tick();
    CHECK(jobs_system_log_count() == base + 1);
    CHECK(strcmp(last_log(), "composed 7") == 0);

    /* 10. reusable job calls: typed inputs, a return value, cycle and errors */
    apply_ok("echo", "version 1;\njob \"Echo\"(msg: string) {\n trigger manual;\n"
                     " system.log(message: msg);\n return msg;\n}\n");
    apply_ok("caller", "version 1;\njob \"Caller\" {\n trigger manual;\n"
                       " run \"Echo\"(msg: \"hello\") as out;\n"
                       " system.log(message: \"got ${out}\");\n}\n");
    base = jobs_system_log_count();
    CHECK(devos_jobs_run_now("caller") == DEVOS_OK);
    devos_jobs_tick();
    CHECK(jobs_system_log_count() == base + 2);
    CHECK(strcmp(last_log(), "got hello") == 0);

    /* a child that branches; the parent consumes the returned boolean */
    s_check_ok = 1;
    apply_ok("isup", "version 1;\njob \"IsUp\" {\n trigger manual;\n test.check() as c;\n return c.ok;\n}\n");
    apply_ok("usesup", "version 1;\njob \"UsesUp\" {\n trigger manual;\n"
                       " run \"IsUp\"() as up;\n if up { system.log(message: \"child up\"); }\n"
                       " else { system.log(message: \"child down\"); }\n}\n");
    CHECK(devos_jobs_run_now("usesup") == DEVOS_OK);
    devos_jobs_tick();
    CHECK(strcmp(last_log(), "child up") == 0);

    /* a missing input is a runtime error (the callee's params are not known at
     * the caller's validation time) */
    apply_ok("needarg", "version 1;\njob \"NeedArg\"(x: int) {\n trigger manual;\n return x;\n}\n");
    apply_ok("missarg", "version 1;\njob \"MissArg\" {\n trigger manual;\n run \"NeedArg\"() as v;\n}\n");
    CHECK(devos_jobs_run_now("missarg") == DEVOS_OK);
    devos_jobs_tick();
    {
        devos_job_summary_t ms;
        bool found = false;
        for (int i = 0; i < devos_jobs_count(); i++)
            if (devos_jobs_summary_at(i, &ms) && strcmp(ms.id, "missarg") == 0) { found = true; break; }
        CHECK(found && strstr(ms.last_result, "missing input") != NULL);
    }

    /* cycles (direct or indirect) are refused, not run forever */
    apply_ok("cyca", "version 1;\njob \"CycA\" {\n trigger manual;\n run \"CycB\"() as x;\n}\n");
    apply_ok("cycb", "version 1;\njob \"CycB\" {\n trigger manual;\n run \"CycA\"() as y;\n}\n");
    CHECK(devos_jobs_run_now("cyca") == DEVOS_OK);
    devos_jobs_tick();
    {
        devos_job_summary_t cs;
        bool found = false;
        for (int i = 0; i < devos_jobs_count(); i++)
            if (devos_jobs_summary_at(i, &cs) && strcmp(cs.id, "cyca") == 0) { found = true; break; }
        CHECK(found && strstr(cs.last_result, "cycle") != NULL);
    }

    /* the call depth is bounded (root + JOBS_MAX_CALL_DEPTH nested calls) */
    for (int i = 1; i <= 6; i++) {
        char id[16], nm[16], src[256];
        snprintf(id, sizeof(id), "deep%d", i);
        snprintf(nm, sizeof(nm), "Deep%d", i);
        if (i < 6)
            snprintf(src, sizeof(src), "version 1;\njob \"%s\" {\n trigger manual;\n run \"Deep%d\"() as x;\n}\n", nm, i + 1);
        else
            snprintf(src, sizeof(src), "version 1;\njob \"%s\" {\n trigger manual;\n system.log(message: \"deep end\");\n}\n", nm);
        uint32_t dr = 0;
        CHECK(devos_jobs_apply(id, src, strlen(src), &dr) == DEVOS_OK);
    }
    CHECK(devos_jobs_run_now("deep1") == DEVOS_OK);
    devos_jobs_tick();
    {
        devos_job_summary_t ds;
        bool found = false;
        for (int i = 0; i < devos_jobs_count(); i++)
            if (devos_jobs_summary_at(i, &ds) && strcmp(ds.id, "deep1") == 0) { found = true; break; }
        CHECK(found && strstr(ds.last_result, "too deep") != NULL);
    }

    /* a credential field written as secret("name") resolves through the hook
     * and the run's copy is wiped after the action starts */
    apply_ok("auth", "version 1;\njob \"Auth\" {\n trigger manual;\n"
                     " test.auth(token: secret(\"tok\")) as r;\n"
                     " if r.ok { system.log(message: \"auth ok\"); }\n}\n");
    s_got_token[0] = '\0';
    s_secret_wipes = 0;
    CHECK(devos_jobs_run_now("auth") == DEVOS_OK);
    devos_jobs_tick();
    CHECK(strcmp(s_got_token, "s3cr3t") == 0);
    CHECK(s_secret_wipes >= 1);                    /* the run wiped its copy */
    CHECK(strcmp(last_log(), "auth ok") == 0);

    /* a missing secret fails the run with a clear diagnostic, never a blank
     * credential */
    apply_ok("authmiss", "version 1;\njob \"AuthMiss\" {\n trigger manual;\n"
                         " test.auth(token: secret(\"nope\")) as r;\n}\n");
    CHECK(devos_jobs_run_now("authmiss") == DEVOS_OK);
    devos_jobs_tick();
    {
        devos_job_summary_t as;
        bool found = false;
        for (int i = 0; i < devos_jobs_count(); i++)
            if (devos_jobs_summary_at(i, &as) && strcmp(as.id, "authmiss") == 0) { found = true; break; }
        CHECK(found && strstr(as.last_result, "not available") != NULL);
    }

    /* the step budget caps an oversized repeat body instead of hanging */
    static char big[2048];
    size_t bo = (size_t)snprintf(big, sizeof(big),
                                 "version 1;\njob \"big\" {\n trigger manual;\n repeat 32 as i {\n");
    for (int i = 0; i < 10 && bo < sizeof(big) - 80; i++)
        bo += (size_t)snprintf(big + bo, sizeof(big) - bo, "  system.log(message: \"x\");\n");
    bo += (size_t)snprintf(big + bo, sizeof(big) - bo, " }\n}\n");
    uint32_t brev = 0;
    CHECK(devos_jobs_apply("big", big, bo, &brev) == DEVOS_OK);
    CHECK(devos_jobs_run_now("big") == DEVOS_OK);
    devos_jobs_tick();
    {
        bool found = false;
        devos_job_summary_t bs;
        for (int i = 0; i < devos_jobs_count(); i++)
            if (devos_jobs_summary_at(i, &bs) && strcmp(bs.id, "big") == 0) { found = true; break; }
        CHECK(found && strstr(bs.last_result, "step budget") != NULL);
    }

    /* dashboard fields + structured history + trace (P1 engine additions) */
    apply_ok("dash", "version 1;\njob \"dash\" {\n trigger every 1s;\n test.check() as c;\n"
                     " if c.ok { system.log(message: \"dash ok\"); }\n}\n");
    CHECK(devos_jobs_run_now("dash") == DEVOS_OK);
    devos_jobs_tick();
    {
        devos_job_summary_t ds;
        bool found = false;
        for (int i = 0; i < devos_jobs_count(); i++)
            if (devos_jobs_summary_at(i, &ds) && strcmp(ds.id, "dash") == 0) { found = true; break; }
        CHECK(found);
        CHECK(ds.last_ok && strcmp(ds.last_cause, "manual") == 0);
    }
    CHECK(devos_jobs_set_enabled("dash", true) == DEVOS_OK);
    {
        devos_job_summary_t ds;
        for (int i = 0; i < devos_jobs_count(); i++)
            if (devos_jobs_summary_at(i, &ds) && strcmp(ds.id, "dash") == 0) break;
        CHECK(ds.next_run_in_ms > 0 && ds.next_run_in_ms <= 1000);   /* first run after one interval */
    }
    {
        devos_jobs_step_t steps[8];
        int tn = devos_jobs_trace("dash", steps, 8);
        CHECK(tn >= 2);                              /* the action + the if */
    }
    usleep(60 * 1000);                               /* the history worker flushes */
    {
        devos_run_record_t rec[4];
        int hr = devos_jobs_history_recent("dash", rec, 4);
        CHECK(hr >= 1);
        CHECK(rec[0].ok && strcmp(rec[0].cause, "manual") == 0 && rec[0].steps >= 2);
    }

    /* dry run: validate + run a draft without applying anything */
    {
        devos_dryrun_t dr;
        const char *dsrc = "version 1;\njob \"Dry\" {\n trigger manual;\n test.check() as c;\n"
                           " if c.ok { system.log(message: \"dry ok\"); }\n}\n";
        CHECK(devos_jobs_dry_run(dsrc, strlen(dsrc), 5000, &dr) == DEVOS_OK);
        CHECK(dr.ok && !dr.timed_out && dr.steps >= 2 && dr.trace_n >= 2);
        CHECK(strcmp(last_log(), "dry ok") == 0);
        CHECK(devos_jobs_dry_run("version 1;\njob {\n trigger manual;\n}\n", 34, 1000, &dr) ==
              DEVOS_ERR_INVALID_ARG);
        CHECK(dr.message[0] != '\0');
        bool ghost = false;
        for (int i = 0; i < devos_jobs_count(); i++) {
            devos_job_summary_t s;
            if (devos_jobs_summary_at(i, &s) && s.id[0] == '~') ghost = true;
        }
        CHECK(!ghost);                           /* no catalog entry, no trace left */
    }

    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}
