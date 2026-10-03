/* Host test for the devos_actions operation runtime (components/devos_actions).
 * Pure logic: a fake provider exercises lifecycle, admission, cancellation,
 * stale handles and exactly-once release.
 *
 *   gcc -O2 -Icomponents/devos_actions -Icomponents/devos_err tools/actions_test.c \
 *       components/devos_actions/devos_actions.c -o /tmp/actions_test && /tmp/actions_test
 */
#include "devos_actions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- fake provider ---- */
typedef struct { int step; } fake_op_t;
static int s_started, s_released, s_cancelled;
static bool s_available = true;
static devos_value_t s_out[2];

static devos_err_t f_start(const devos_action_args_t *args, const devos_action_context_t *ctx, void **op)
{
    (void)args; (void)ctx;
    *op = calloc(1, sizeof(fake_op_t));
    if (!*op) return DEVOS_ERR_NO_MEM;
    s_started++;
    return DEVOS_OK;
}
static devos_err_t f_poll(void *op, devos_action_state_t *state, devos_action_result_t *result)
{
    fake_op_t *f = op;
    if (f->step++ < 2) { *state = DEVOS_ACT_PENDING; return DEVOS_OK; }
    *state = DEVOS_ACT_DONE;
    s_out[0].type = DEVOS_VAL_BOOL; s_out[0].v.b = true;
    s_out[1].type = DEVOS_VAL_INT; s_out[1].v.i = 42;
    result->outs = s_out;
    result->out_count = 2;
    return DEVOS_OK;
}
static devos_err_t f_cancel(void *op) { (void)op; s_cancelled++; return DEVOS_OK; }
static void f_release(void *op) { s_released++; free(op); }
static bool f_available(char *reason, size_t cap)
{
    if (s_available) return true;
    if (reason && cap) snprintf(reason, cap, "provider offline");
    return false;
}

static const devos_action_param_t P[] = { { .name = "host", .type = DEVOS_VAL_STR, .required = true } };
static const devos_action_out_t O[] = { { .name = "ok", .type = DEVOS_VAL_BOOL }, { .name = "n", .type = DEVOS_VAL_INT } };
static const devos_action_ops_t OPS = { f_start, f_poll, f_cancel, f_release, f_available };
static const devos_action_descriptor_t D = {
    .id = "test.run", .schema_version = 1, .params = P, .param_count = 1,
    .outs = O, .out_count = 2, .ops = &OPS,
};
static const devos_action_descriptor_t NOOPS = { .id = "test.schema_only", .schema_version = 1 };

static void test_lifecycle(void)
{
    devos_actions_reset();
    CHECK(devos_actions_register(&D) == DEVOS_OK);
    CHECK(devos_actions_register(&NOOPS) == DEVOS_OK);
    CHECK(devos_actions_register(NULL) == DEVOS_ERR_INVALID_ARG);

    s_started = s_released = s_cancelled = 0;
    s_available = true;
    char why[64];
    CHECK(devos_actions_available("test.run", why, sizeof(why)));
    CHECK(!devos_actions_available("nope", why, sizeof(why)) && strstr(why, "unknown"));
    CHECK(!devos_actions_available("test.schema_only", why, sizeof(why)) && strstr(why, "provider"));

    devos_action_handle_t h;
    devos_action_args_t args = { 0 };
    CHECK(devos_action_start("test.run", &args, NULL, &h) == DEVOS_OK);
    CHECK(h.slot != 0 && devos_actions_outstanding() == 1);
    CHECK(s_started == 1);

    devos_action_state_t st;
    devos_action_result_t res;
    memset(&res, 0, sizeof(res));
    CHECK(devos_action_poll(h, &st, &res) == DEVOS_OK && st == DEVOS_ACT_PENDING);
    CHECK(devos_action_poll(h, &st, &res) == DEVOS_OK && st == DEVOS_ACT_PENDING);
    CHECK(devos_action_poll(h, &st, &res) == DEVOS_OK && st == DEVOS_ACT_DONE);
    CHECK(res.out_count == 2 && res.outs[1].v.i == 42);

    devos_action_release(h);
    CHECK(s_released == 1 && devos_actions_outstanding() == 0);
    devos_action_release(h);                      /* stale: no-op */
    CHECK(s_released == 1);
    CHECK(devos_action_poll(h, &st, &res) == DEVOS_ERR_NOT_FOUND);
}

static void test_cancel_and_admission(void)
{
    devos_actions_reset();
    CHECK(devos_actions_register(&D) == DEVOS_OK);
    devos_action_args_t args = { 0 };
    devos_action_handle_t h;

    /* unavailable provider: start is refused, not silently queued */
    s_available = false;
    char why[64];
    CHECK(!devos_actions_available("test.run", why, sizeof(why)) && strstr(why, "offline"));
    CHECK(devos_action_start("test.run", &args, NULL, &h) == DEVOS_ERR_INVALID_STATE);
    s_available = true;

    /* schema-only action has no runtime */
    CHECK(devos_action_start("test.schema_only", &args, NULL, &h) == DEVOS_ERR_INVALID_ARG);
    CHECK(devos_action_start("missing", &args, NULL, &h) == DEVOS_ERR_INVALID_ARG);

    /* cancel then release */
    s_cancelled = s_released = 0;
    CHECK(devos_action_start("test.run", &args, NULL, &h) == DEVOS_OK);
    CHECK(devos_action_cancel(h) == DEVOS_OK && s_cancelled == 1);
    devos_action_release(h);
    CHECK(s_released == 1);

    /* capacity: the slot table is bounded */
    devos_action_handle_t hs[DEVOS_ACTION_OPS_MAX];
    int got = 0;
    for (int i = 0; i < DEVOS_ACTION_OPS_MAX; i++)
        if (devos_action_start("test.run", &args, NULL, &hs[i]) == DEVOS_OK) got++;
    CHECK(got == DEVOS_ACTION_OPS_MAX);
    CHECK(devos_action_start("test.run", &args, NULL, &h) == DEVOS_ERR_NO_MEM);
    for (int i = 0; i < got; i++) devos_action_release(hs[i]);
    CHECK(devos_actions_outstanding() == 0);
}

int main(void)
{
    test_lifecycle();
    test_cancel_and_admission();
    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}
