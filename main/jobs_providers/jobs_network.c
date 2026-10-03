/* jobs_network: the network.ping action provider for Jobs. Wraps the
 * request-specific devos_probe_* engine so it never disturbs the Network app's
 * ping singleton (PLAN.md 7.3). */
#include "jobs_providers.h"
#include "devos_actions.h"
#include "devos_netdiag.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    int handle;
    devos_value_t outs[4];
} ping_op_t;

static const char *arg_str(const devos_action_args_t *args, int i)
{
    if (!args || i >= args->arg_count) return "";
    const devos_value_t *v = &args->args[i];
    return v->type == DEVOS_VAL_STR && v->v.str.s ? v->v.str.s : "";
}
static int64_t arg_ms(const devos_action_args_t *args, int i, int64_t def)
{
    if (!args || i >= args->arg_count) return def;
    const devos_value_t *v = &args->args[i];
    return v->type == DEVOS_VAL_DURATION ? v->v.ms : def;
}

static devos_err_t ping_start(const devos_action_args_t *args, const devos_action_context_t *ctx, void **op)
{
    (void)ctx;
    int timeout = (int)arg_ms(args, 1, 3000);
    int h = devos_probe_submit(arg_str(args, 0), timeout);
    if (h == -2) return DEVOS_ERR_INVALID_STATE;          /* one probe at a time */
    if (h < 0) return DEVOS_ERR_FAIL;
    ping_op_t *o = calloc(1, sizeof(*o));
    if (!o) { devos_probe_release(h); return DEVOS_ERR_NO_MEM; }
    o->handle = h;
    *op = o;
    return DEVOS_OK;
}

static devos_err_t ping_poll(void *op, devos_action_state_t *state, devos_action_result_t *result)
{
    ping_op_t *o = op;
    int st = 0;
    devos_probe_result_t r;
    if (devos_probe_poll(o->handle, &st, &r) != 0) {
        *state = DEVOS_ACT_FAILED;
        result->error[0] = '\0';
        return DEVOS_OK;
    }
    if (st == 0) { *state = DEVOS_ACT_PENDING; return DEVOS_OK; }
    o->outs[0].type = DEVOS_VAL_BOOL;  o->outs[0].v.b = r.ok;
    o->outs[1].type = DEVOS_VAL_STR;   o->outs[1].v.str.s = r.ip;  o->outs[1].v.str.len = (uint32_t)strlen(r.ip);
    o->outs[2].type = DEVOS_VAL_INT;   o->outs[2].v.i = r.latency_ms;
    o->outs[3].type = DEVOS_VAL_STR;   o->outs[3].v.str.s = r.error; o->outs[3].v.str.len = (uint32_t)strlen(r.error);
    result->outs = o->outs;
    result->out_count = 4;
    *state = DEVOS_ACT_DONE;
    return DEVOS_OK;
}

static devos_err_t ping_cancel(void *op)
{
    ping_op_t *o = op;
    devos_probe_cancel(o->handle);
    return DEVOS_OK;
}
static void ping_release(void *op)
{
    ping_op_t *o = op;
    devos_probe_release(o->handle);
    free(o);
}
static const devos_action_ops_t PING_OPS = { ping_start, ping_poll, ping_cancel, ping_release, NULL };

static const devos_action_param_t PING_P[] = {
    { .name = "host", .type = DEVOS_VAL_STR, .required = true, .expression = true, .max_len = 253 },
    { .name = "timeout", .type = DEVOS_VAL_DURATION, .expression = true, .min = 200, .max = 30000 },
};
static const devos_action_out_t PING_O[] = {
    { .name = "ok", .type = DEVOS_VAL_BOOL },
    { .name = "ip", .type = DEVOS_VAL_STR },
    { .name = "latency_ms", .type = DEVOS_VAL_INT },
    { .name = "error_code", .type = DEVOS_VAL_STR },
};
static const devos_action_descriptor_t PING_D = {
    .id = "network.ping", .schema_version = 1, .provider_uid = "netdiag", .category = "network",
    .label = "Ping a host", .params = PING_P, .param_count = 2, .outs = PING_O, .out_count = 4,
    .effect = DEVOS_EFFECT_NET_SEND, .retry_safe = true, .recommended_timeout_ms = 5000,
    .ops = &PING_OPS,
};

void jobs_network_register(void)
{
    devos_actions_register(&PING_D);
}
