/* Compatibility fixture for a hypothetical future app that wants to be
 * Jobs-compatible (AGENTS.md invariant 10). It registers a headless action
 * schema without constructing any LVGL object, proves a switched-off provider
 * reports unavailable safely, that two simultaneous calls have distinct
 * results, that cancel/release is exact, and that an event payload survives the
 * producer reusing its buffer.
 *
 *   gcc -O2 -Icomponents/devos_actions -Icomponents/devos_events -Icomponents/devos_err \
 *       tools/jobs_compat_test.c components/devos_actions/devos_actions.c \
 *       components/devos_events/devos_events.c -lpthread -o /tmp/jobs_compat_test && /tmp/jobs_compat_test
 */
#include "devos_actions.h"
#include "devos_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- the "future app" provider: weather.get ---- */
static bool s_app_enabled = true;
static int s_started, s_released;

typedef struct { int id; devos_value_t outs[2]; } weather_op_t;

static devos_err_t w_start(const devos_action_args_t *a, const devos_action_context_t *c, void **op)
{
    (void)a; (void)c;
    weather_op_t *o = calloc(1, sizeof(*o));
    if (!o) return DEVOS_ERR_NO_MEM;
    o->id = ++s_started;
    o->outs[0].type = DEVOS_VAL_INT; o->outs[0].v.i = 10 + o->id;   /* distinct per call */
    o->outs[1].type = DEVOS_VAL_STR; o->outs[1].v.str.s = "sunny"; o->outs[1].v.str.len = 5;
    *op = o;
    return DEVOS_OK;
}
static devos_err_t w_poll(void *op, devos_action_state_t *st, devos_action_result_t *res)
{
    weather_op_t *o = op;
    *st = DEVOS_ACT_DONE;
    res->outs = o->outs;
    res->out_count = 2;
    return DEVOS_OK;
}
static devos_err_t w_cancel(void *op) { (void)op; return DEVOS_OK; }
static void w_release(void *op) { s_released++; free(op); }
static bool w_available(char *reason, size_t cap)
{
    if (s_app_enabled) return true;
    if (reason && cap) snprintf(reason, cap, "the Weather app is switched off");
    return false;
}
static const devos_action_ops_t W_OPS = { w_start, w_poll, w_cancel, w_release, w_available };
static const devos_action_param_t W_P[] = { { .name = "city", .type = DEVOS_VAL_STR, .required = true } };
static const devos_action_out_t W_O[] = { { .name = "temp_c", .type = DEVOS_VAL_INT },
                                          { .name = "sky", .type = DEVOS_VAL_STR } };
static const devos_action_descriptor_t W_D = {
    .id = "weather.get", .schema_version = 1, .provider_uid = "weather", .category = "tools",
    .params = W_P, .param_count = 1, .outs = W_O, .out_count = 2,
    .effect = DEVOS_EFFECT_READ, .retry_safe = true, .ops = &W_OPS,
};

/* ---- event payload reuse ---- */
static char s_seen[64];
static void on_topic(const devos_event_t *ev, void *user)
{
    (void)user;
    snprintf(s_seen, sizeof(s_seen), "%.*s", (int)ev->payload_len, (const char *)ev->payload);
}

int main(void)
{
    /* registration needs no LVGL objects; a schema is discoverable while its
     * provider is unavailable */
    CHECK(devos_actions_register(&W_D) == DEVOS_OK);
    char why[80];
    s_app_enabled = false;
    CHECK(!devos_actions_available("weather.get", why, sizeof(why)));
    CHECK(strstr(why, "switched off") != NULL);
    devos_action_handle_t h;
    devos_action_args_t args = { 0 };
    CHECK(devos_action_start("weather.get", &args, NULL, &h) == DEVOS_ERR_INVALID_STATE);
    s_app_enabled = true;
    CHECK(devos_actions_available("weather.get", why, sizeof(why)));

    /* two simultaneous calls have distinct results */
    devos_action_handle_t h1, h2;
    CHECK(devos_action_start("weather.get", &args, NULL, &h1) == DEVOS_OK);
    CHECK(devos_action_start("weather.get", &args, NULL, &h2) == DEVOS_OK);
    CHECK(h1.slot != h2.slot);
    devos_action_state_t st;
    devos_action_result_t r1, r2;
    CHECK(devos_action_poll(h1, &st, &r1) == DEVOS_OK && st == DEVOS_ACT_DONE);
    CHECK(devos_action_poll(h2, &st, &r2) == DEVOS_OK && st == DEVOS_ACT_DONE);
    CHECK(r1.outs[0].v.i != r2.outs[0].v.i);
    CHECK(devos_actions_outstanding() == 2);

    /* cancel/release is exact */
    CHECK(devos_action_cancel(h1) == DEVOS_OK);
    devos_action_release(h1);
    devos_action_release(h2);
    devos_action_release(h2);                        /* stale: no-op */
    CHECK(s_released == 2 && devos_actions_outstanding() == 0);

    /* an event payload is copied: the producer may reuse its buffer */
    CHECK(devos_events_init() == DEVOS_OK);
    CHECK(devos_events_subscribe("app/+/state", on_topic, NULL) > 0);
    char buf[32];
    snprintf(buf, sizeof(buf), "on");
    devos_event_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.topic, sizeof(ev.topic), "app/lamp/state");
    CHECK(devos_events_publish(&ev, buf, strlen(buf)) == DEVOS_OK);
    snprintf(buf, sizeof(buf), "overwritten");       /* reuse the producer buffer */
    devos_events_drain(0);
    CHECK(strcmp(s_seen, "on") == 0);

    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}
