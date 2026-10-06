/* Host test for the system action providers (main/jobs_providers/jobs_system.c)
 * driven through the real devos_actions runtime: system.log records a bounded
 * line, system.notify queues a bounded notice.
 *
 *   gcc -O2 -Imain/jobs_providers -Icomponents/devos_actions -Icomponents/devos_err \
 *       tools/providers_test.c main/jobs_providers/jobs_system.c \
 *       components/devos_actions/devos_actions.c -lpthread -o /tmp/providers_test && /tmp/providers_test
 */
#include "jobs_providers.h"
#include "devos_actions.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static devos_value_t vs(const char *s) { devos_value_t v; v.type = DEVOS_VAL_STR; v.v.str.s = s; v.v.str.len = (uint32_t)strlen(s); return v; }

/* Run an action to completion and return its first output as a bool. */
static bool run_bool(const char *id, devos_value_t *args, int n)
{
    devos_action_args_t a = { .args = args, .arg_count = n };
    devos_action_handle_t h;
    if (devos_action_start(id, &a, NULL, &h) != DEVOS_OK) return false;
    devos_action_state_t st;
    devos_action_result_t res;
    memset(&res, 0, sizeof(res));
    for (int i = 0; i < 8 && devos_action_poll(h, &st, &res) == DEVOS_OK && st == DEVOS_ACT_PENDING; i++) {}
    bool got = (st == DEVOS_ACT_DONE) && res.out_count >= 1 && res.outs[0].v.b;
    devos_action_release(h);
    return got;
}

int main(void)
{
    jobs_system_register();                      /* only the system provider is linked here */
    CHECK(devos_actions_find("system.log") != NULL);
    CHECK(devos_actions_find("system.notify") != NULL);
    /* system.log mutates the local log ring, so it is declared MUTATE like
     * system.notify (a dry run is still allowed; only the class changes). Both
     * are declared local_only: a dry run must not warn about a local sink. */
    CHECK(devos_actions_find("system.log")->effect == DEVOS_EFFECT_MUTATE);
    CHECK(devos_actions_find("system.notify")->effect == DEVOS_EFFECT_MUTATE);
    CHECK(devos_actions_find("system.log")->local_only);
    CHECK(devos_actions_find("system.notify")->local_only);

    /* system.log records a line and reports recorded=true */
    devos_value_t a[2] = { vs("hello from a job") };
    CHECK(run_bool("system.log", a, 1));
    CHECK(jobs_system_log_count() == 1);
    char lines[4 * JOBS_SYSTEM_LOG_LINE];
    int n = jobs_system_log_tail(lines, 4);
    CHECK(n == 1 && strcmp(lines, "hello from a job") == 0);

    /* the log ring is bounded (wraps, keeps the newest) */
    for (int i = 0; i < JOBS_SYSTEM_LOG_MAX + 5; i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "line %d", i);
        devos_value_t m = vs(msg);
        CHECK(run_bool("system.log", &m, 1));
    }
    CHECK(jobs_system_log_count() == JOBS_SYSTEM_LOG_MAX);
    n = jobs_system_log_tail(lines, 1);
    CHECK(n == 1 && strcmp(lines, "line 36") == 0);       /* newest kept */

    /* system.notify queues a notice with its level */
    devos_value_t nargs[2] = { vs("NAS offline"), vs("warning") };
    CHECK(run_bool("system.notify", nargs, 2));
    char msg[JOBS_SYSTEM_NOTICE_LEN];
    int level = -1;
    CHECK(jobs_system_take_notice(msg, sizeof(msg), &level));
    CHECK(strcmp(msg, "NAS offline") == 0 && level == 1);
    CHECK(!jobs_system_take_notice(msg, sizeof(msg), &level));   /* queue drained */

    /* the success level is its own value (the UI maps it to the green tick) */
    devos_value_t sargs[2] = { vs("Backup done"), vs("success") };
    CHECK(run_bool("system.notify", sargs, 2));
    CHECK(jobs_system_take_notice(msg, sizeof(msg), &level));
    CHECK(strcmp(msg, "Backup done") == 0 && level == 3);
    CHECK(!jobs_system_take_notice(msg, sizeof(msg), &level));

    /* an unknown level falls back to info rather than failing the run */
    devos_value_t uargs[2] = { vs("odd"), vs("banana") };
    CHECK(run_bool("system.notify", uargs, 2));
    CHECK(jobs_system_take_notice(msg, sizeof(msg), &level) && level == 0);

    /* availability is reported for a real provider */
    char why[64];
    CHECK(devos_actions_available("system.log", why, sizeof(why)));

    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}
