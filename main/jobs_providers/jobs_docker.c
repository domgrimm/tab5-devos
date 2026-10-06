/* jobs_docker: the Docker action providers for Jobs. Wraps the existing
 * devos_docker engine (AGENTS.md invariant 7 - never a private HTTP/Docker
 * stack). Each command is a correlated ticket on the engine's bounded queue,
 * run by its Core 0 worker whether or not the Docker screen is shown, with an
 * immutable config/endpoint snapshot taken at submit (PLAN.md 7.3, Phase 8).
 *
 * Honest outcomes: a start/stop/restart HTTP 204/304 means the request was
 * accepted, not that the service recovered (a job should follow it with a
 * wait and an HTTP/health probe); a transport error on a mutation is
 * outcome-unknown and is never reported as success or retried automatically.
 * inspect reads the daemon directly by id/name, independent of the UI cache. */
#include "jobs_providers.h"
#include "devos_actions.h"
#include "devos_docker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t ticket;
    bool inspect;
    bool done;
    bool cancelled;
    bool started;                   /* the daemon may have acted on it */
    devos_docker_req_result_t res;
    devos_value_t outs[8];
} docker_op_t;

/* Optional gate: the Docker app being switched off makes the provider
 * unavailable without Jobs touching app enablement. */
static bool (*s_gate)(char *reason, size_t cap);

void jobs_docker_set_gate(bool (*gate)(char *reason, size_t cap))
{
    s_gate = gate;
}

static const char *arg_str(const devos_action_args_t *args, int i)
{
    if (!args || i >= args->arg_count) return NULL;
    const devos_value_t *v = &args->args[i];
    return v->type == DEVOS_VAL_STR && v->v.str.s ? v->v.str.s : NULL;
}

static devos_err_t docker_submit(const devos_action_args_t *args, const char *what, bool inspect, void **op)
{
    const char *c = arg_str(args, 0);
    if (!c || !c[0]) return DEVOS_ERR_INVALID_ARG;
    int timeout = (int)jobs_arg_ms(args, 1, 12000);
    if (timeout < 1000) timeout = 1000;
    if (timeout > 30000) timeout = 30000;
    uint32_t t = inspect ? devos_docker_inspect(c, timeout) : devos_docker_request(c, what, timeout);
    if (!t) return DEVOS_ERR_INVALID_STATE;        /* not configured / queue full */
    docker_op_t *o = calloc(1, sizeof(*o));
    if (!o) { devos_docker_request_release(t); return DEVOS_ERR_NO_MEM; }
    o->ticket = t;
    o->inspect = inspect;
    *op = o;
    return DEVOS_OK;
}

static devos_err_t inspect_start(const devos_action_args_t *a, const devos_action_context_t *c, void **op)
{ (void)c; return docker_submit(a, "inspect", true, op); }
static devos_err_t start_start(const devos_action_args_t *a, const devos_action_context_t *c, void **op)
{ (void)c; return docker_submit(a, "start", false, op); }
static devos_err_t stop_start(const devos_action_args_t *a, const devos_action_context_t *c, void **op)
{ (void)c; return docker_submit(a, "stop", false, op); }
static devos_err_t restart_start(const devos_action_args_t *a, const devos_action_context_t *c, void **op)
{ (void)c; return docker_submit(a, "restart", false, op); }

static void put_str(devos_value_t *v, const char *s)
{
    v->type = DEVOS_VAL_STR;
    v->v.str.s = s ? s : "";
    v->v.str.len = (uint32_t)strlen(v->v.str.s);
}

static void fill(docker_op_t *o, devos_action_result_t *result)
{
    devos_docker_req_result_t *r = &o->res;
    if (o->inspect) {
        o->outs[0].type = DEVOS_VAL_BOOL;
        o->outs[0].v.b = !o->cancelled && r->state == DEVOS_DOCKER_REQ_DONE && r->status == 200;
        o->outs[1].type = DEVOS_VAL_INT;  o->outs[1].v.i = r->status;
        put_str(&o->outs[2], r->inspect.state);
        put_str(&o->outs[3], r->inspect.health);
        put_str(&o->outs[4], r->inspect.id);
        put_str(&o->outs[5], r->inspect.name);
        o->outs[6].type = DEVOS_VAL_INT;  o->outs[6].v.i = r->inspect.observed;
        put_str(&o->outs[7], o->cancelled ? "cancelled" : r->error);
        result->out_count = 8;
    } else {
        o->outs[0].type = DEVOS_VAL_INT;  o->outs[0].v.i = r->status;
        o->outs[1].type = DEVOS_VAL_BOOL; o->outs[1].v.b = !o->cancelled && (r->status == 204 || r->status == 304);
        o->outs[2].type = DEVOS_VAL_BOOL; o->outs[2].v.b = o->cancelled || r->status == 0;
        put_str(&o->outs[3], o->cancelled ? "cancelled" : r->error);
        result->out_count = 4;
    }
    result->outs = o->outs;
}

static devos_err_t docker_poll(void *op, devos_action_state_t *state, devos_action_result_t *result)
{
    docker_op_t *o = op;
    if (o->done) { fill(o, result); *state = DEVOS_ACT_DONE; return DEVOS_OK; }
    if (o->cancelled) {
        fill(o, result);
        if (!o->inspect && o->started) {
            /* The command was already handed to the daemon: the effect may have
             * landed, so never report a clean local cancellation. */
            o->outs[2].v.b = true;
            put_str(&o->outs[3], "cancel requested after the command was sent; it may have been applied");
            snprintf(result->error, sizeof(result->error), "%s", o->outs[3].v.str.s);
            *state = DEVOS_ACT_UNKNOWN;
        } else {
            if (!o->inspect) put_str(&o->outs[3], "cancelled before the command was sent");
            *state = DEVOS_ACT_CANCELLED;
        }
        o->done = true;
        return DEVOS_OK;
    }
    devos_docker_req_result_t r;
    if (!devos_docker_request_poll(o->ticket, &r)) {
        if (!o->inspect && o->started) {
            o->outs[0].type = DEVOS_VAL_INT;  o->outs[0].v.i = 0;
            o->outs[1].type = DEVOS_VAL_BOOL; o->outs[1].v.b = false;
            o->outs[2].type = DEVOS_VAL_BOOL; o->outs[2].v.b = true;
            put_str(&o->outs[3], "Docker request vanished after it was sent; the outcome is unknown");
            result->outs = o->outs;
            result->out_count = 4;
            snprintf(result->error, sizeof(result->error), "%s", o->outs[3].v.str.s);
            *state = DEVOS_ACT_UNKNOWN;
        } else {
            snprintf(result->error, sizeof(result->error), "Docker request vanished");
            *state = DEVOS_ACT_FAILED;
        }
        return DEVOS_OK;
    }
    if (r.state == DEVOS_DOCKER_REQ_PENDING) { *state = DEVOS_ACT_PENDING; return DEVOS_OK; }
    o->res = r;
    o->done = true;
    fill(o, result);
    *state = DEVOS_ACT_DONE;
    return DEVOS_OK;
}

static devos_err_t docker_cancel(void *op)
{
    docker_op_t *o = op;
    o->started = devos_docker_request_started(o->ticket);
    o->cancelled = true;
    devos_docker_request_cancel(o->ticket);
    return DEVOS_OK;
}
static void docker_release(void *op)
{
    docker_op_t *o = op;
    devos_docker_request_release(o->ticket);
    free(o);
}
static bool docker_available(char *reason, size_t cap)
{
    if (s_gate && !s_gate(reason, cap)) return false;
    /* Non-blocking readiness: never call devos_docker_init() from here - this
     * runs on the Core 0 scheduler (devos_action_start -> available) and init
     * busy-spins while allocating PSRAM and reading NVS/SD. The app owns
     * initialisation; an uninitialised engine reports not-ready instead. */
    if (!devos_docker_ready()) {
        snprintf(reason, cap, "Docker is not ready (open the Docker app once)");
        return false;
    }
    if (devos_docker_configured()) return true;
    snprintf(reason, cap, "Docker is not configured (Settings in the Docker app)");
    return false;
}

static const devos_action_ops_t INSPECT_OPS = { inspect_start, docker_poll, docker_cancel, docker_release, docker_available };
static const devos_action_ops_t START_OPS   = { start_start,   docker_poll, docker_cancel, docker_release, docker_available };
static const devos_action_ops_t STOP_OPS    = { stop_start,    docker_poll, docker_cancel, docker_release, docker_available };
static const devos_action_ops_t RESTART_OPS = { restart_start, docker_poll, docker_cancel, docker_release, docker_available };

static const devos_action_param_t CONTAINER_P[] = {
    { .name = "container", .type = DEVOS_VAL_STR, .required = true, .expression = true, .max_len = 128,
      .help = "container id or name" },
    { .name = "timeout", .type = DEVOS_VAL_DURATION, .expression = true, .min = 1000, .max = 30000, .def = "12s" },
};
static const devos_action_out_t INSPECT_OUT[] = {
    { .name = "ok", .type = DEVOS_VAL_BOOL },
    { .name = "status", .type = DEVOS_VAL_INT },
    { .name = "state", .type = DEVOS_VAL_STR },
    { .name = "health", .type = DEVOS_VAL_STR },
    { .name = "id", .type = DEVOS_VAL_STR },
    { .name = "name", .type = DEVOS_VAL_STR },
    { .name = "updated", .type = DEVOS_VAL_INT, .help = "unix seconds the inspect was read" },
    { .name = "error", .type = DEVOS_VAL_STR },
};
static const devos_action_out_t MUTATE_OUT[] = {
    { .name = "status", .type = DEVOS_VAL_INT },
    { .name = "accepted", .type = DEVOS_VAL_BOOL, .help = "the daemon accepted the request, not that it recovered" },
    { .name = "outcome_unknown", .type = DEVOS_VAL_BOOL },
    { .name = "error", .type = DEVOS_VAL_STR },
};

static const devos_action_descriptor_t INSPECT_D = {
    .id = "docker.inspect", .schema_version = 1, .provider_uid = "docker", .category = "containers",
    .label = "Inspect a container", .description = "Read a container's current state, by id or name",
    .params = CONTAINER_P, .param_count = 2, .outs = INSPECT_OUT, .out_count = 8,
    .effect = DEVOS_EFFECT_READ, .retry_safe = true, .recommended_timeout_ms = 12000,
    .ops = &INSPECT_OPS,
};
static const devos_action_descriptor_t START_D = {
    .id = "docker.start", .schema_version = 1, .provider_uid = "docker", .category = "containers",
    .label = "Start a container", .description = "Start a container (request accepted is not service recovery)",
    .params = CONTAINER_P, .param_count = 2, .outs = MUTATE_OUT, .out_count = 4,
    .effect = DEVOS_EFFECT_MUTATE, .retry_safe = false, .recommended_timeout_ms = 15000,
    .ops = &START_OPS,
};
static const devos_action_descriptor_t STOP_D = {
    .id = "docker.stop", .schema_version = 1, .provider_uid = "docker", .category = "containers",
    .label = "Stop a container", .description = "Stop a container (request accepted is not service stop)",
    .params = CONTAINER_P, .param_count = 2, .outs = MUTATE_OUT, .out_count = 4,
    .effect = DEVOS_EFFECT_MUTATE, .retry_safe = false, .recommended_timeout_ms = 15000,
    .ops = &STOP_OPS,
};
static const devos_action_descriptor_t RESTART_D = {
    .id = "docker.restart", .schema_version = 1, .provider_uid = "docker", .category = "containers",
    .label = "Restart a container", .description = "Restart a container (follow with a health probe)",
    .params = CONTAINER_P, .param_count = 2, .outs = MUTATE_OUT, .out_count = 4,
    .effect = DEVOS_EFFECT_MUTATE, .retry_safe = false, .recommended_timeout_ms = 15000,
    .ops = &RESTART_OPS,
};

void jobs_docker_register(void)
{
    devos_actions_register(&INSPECT_D);
    devos_actions_register(&START_D);
    devos_actions_register(&STOP_D);
    devos_actions_register(&RESTART_D);
}
