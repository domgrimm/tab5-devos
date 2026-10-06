/* jobs_proxmox: the Proxmox action providers for Jobs. Wraps the existing
 * devos_proxmox engine (AGENTS.md invariant 7 - never a private HTTP stack).
 * Each command is a correlated ticket on the engine's bounded queue, run by its
 * Core 0 worker whether or not the Proxmox screen is shown, with an immutable
 * config snapshot taken at submit.
 *
 * Honest outcomes: a start/stop/shutdown/reboot HTTP 200 means Proxmox
 * *accepted* the request and returned a task (UPID) - not that the guest
 * reached the requested state (a job should follow it with a wait and a
 * guest_status poll); a transport error on a mutation is outcome-unknown and is
 * never reported as success or retried automatically. guest_status reads the
 * guest directly by vmid, independent of the UI's cached list.
 *
 * A guest is addressed by vmid alone: the engine resolves its node and type
 * from the cluster resources, so a job never has to know them. */
#include "jobs_providers.h"
#include "devos_actions.h"
#include "devos_proxmox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t ticket;
    bool status_read;
    bool done;
    bool cancelled;
    bool started;                   /* Proxmox may have acted on it */
    devos_proxmox_req_result_t res;
    devos_value_t outs[11];
} prox_op_t;

/* Optional gate: the Proxmox app being switched off makes the provider
 * unavailable without Jobs touching app enablement. */
static bool (*s_gate)(char *reason, size_t cap);

void jobs_proxmox_set_gate(bool (*gate)(char *reason, size_t cap))
{
    s_gate = gate;
}

static void put_str(devos_value_t *v, const char *s)
{
    v->type = DEVOS_VAL_STR;
    v->v.str.s = s ? s : "";
    v->v.str.len = (uint32_t)strlen(v->v.str.s);
}

static devos_err_t prox_submit(const devos_action_args_t *args, const char *what, bool status_read, void **op)
{
    if (!args || args->arg_count < 1) return DEVOS_ERR_INVALID_ARG;
    const devos_value_t *v = &args->args[0];
    if (v->type != DEVOS_VAL_INT && v->type != DEVOS_VAL_NUM) return DEVOS_ERR_INVALID_ARG;
    int vmid = (int)(v->type == DEVOS_VAL_INT ? v->v.i : (int64_t)v->v.n);
    if (vmid <= 0) return DEVOS_ERR_INVALID_ARG;
    int timeout = (int)jobs_arg_ms(args, 1, 12000);
    if (timeout < 1000) timeout = 1000;
    if (timeout > 30000) timeout = 30000;
    uint32_t t = devos_proxmox_request(vmid, what, timeout);
    if (!t) return DEVOS_ERR_INVALID_STATE;        /* not configured / queue full */
    prox_op_t *o = calloc(1, sizeof(*o));
    if (!o) { devos_proxmox_request_release(t); return DEVOS_ERR_NO_MEM; }
    o->ticket = t;
    o->status_read = status_read;
    *op = o;
    return DEVOS_OK;
}

static devos_err_t status_start(const devos_action_args_t *a, const devos_action_context_t *c, void **op)
{ (void)c; return prox_submit(a, "status", true, op); }
static devos_err_t start_start(const devos_action_args_t *a, const devos_action_context_t *c, void **op)
{ (void)c; return prox_submit(a, "start", false, op); }
static devos_err_t stop_start(const devos_action_args_t *a, const devos_action_context_t *c, void **op)
{ (void)c; return prox_submit(a, "stop", false, op); }
static devos_err_t shutdown_start(const devos_action_args_t *a, const devos_action_context_t *c, void **op)
{ (void)c; return prox_submit(a, "shutdown", false, op); }
static devos_err_t reboot_start(const devos_action_args_t *a, const devos_action_context_t *c, void **op)
{ (void)c; return prox_submit(a, "reboot", false, op); }

static void fill(prox_op_t *o, devos_action_result_t *result)
{
    devos_proxmox_req_result_t *r = &o->res;
    if (o->status_read) {
        devos_proxmox_inspect_t *g = &r->inspect;
        o->outs[0].type = DEVOS_VAL_BOOL;
        o->outs[0].v.b = !o->cancelled && r->state == DEVOS_PROXMOX_REQ_DONE && r->status == 200;
        o->outs[1].type = DEVOS_VAL_INT;  o->outs[1].v.i = r->status;
        put_str(&o->outs[2], g->status);
        put_str(&o->outs[3], g->node);
        put_str(&o->outs[4], g->name);
        put_str(&o->outs[5], g->kind == DEVOS_PROXMOX_LXC ? "lxc" : "qemu");
        o->outs[6].type = DEVOS_VAL_NUM;  o->outs[6].v.n = g->cpu;
        o->outs[7].type = DEVOS_VAL_INT;  o->outs[7].v.i = (int64_t)g->mem;
        o->outs[8].type = DEVOS_VAL_INT;  o->outs[8].v.i = (int64_t)g->maxmem;
        o->outs[9].type = DEVOS_VAL_INT;  o->outs[9].v.i = g->uptime_s;
        put_str(&o->outs[10], o->cancelled ? "cancelled" : r->error);
        result->out_count = 11;
    } else {
        o->outs[0].type = DEVOS_VAL_INT;  o->outs[0].v.i = r->status;
        /* Proxmox answers 200 with a task id: accepted, not completed. */
        o->outs[1].type = DEVOS_VAL_BOOL;
        o->outs[1].v.b = !o->cancelled && r->status == 200 && r->state == DEVOS_PROXMOX_REQ_DONE;
        put_str(&o->outs[2], r->task);
        o->outs[3].type = DEVOS_VAL_BOOL; o->outs[3].v.b = o->cancelled || r->status == 0;
        put_str(&o->outs[4], o->cancelled ? "cancelled" : r->error);
        result->out_count = 5;
    }
    result->outs = o->outs;
}

static devos_err_t prox_poll(void *op, devos_action_state_t *state, devos_action_result_t *result)
{
    prox_op_t *o = op;
    if (o->done) { fill(o, result); *state = DEVOS_ACT_DONE; return DEVOS_OK; }
    if (o->cancelled) {
        fill(o, result);
        if (!o->status_read && o->started) {
            /* Already handed to Proxmox: the task may be running, so never
             * report a clean local cancellation. */
            o->outs[3].v.b = true;
            put_str(&o->outs[4], "cancel requested after the command was sent; it may have been applied");
            snprintf(result->error, sizeof(result->error), "%s", o->outs[4].v.str.s);
            *state = DEVOS_ACT_UNKNOWN;
        } else {
            if (!o->status_read) put_str(&o->outs[4], "cancelled before the command was sent");
            *state = DEVOS_ACT_CANCELLED;
        }
        o->done = true;
        return DEVOS_OK;
    }
    devos_proxmox_req_result_t r;
    if (!devos_proxmox_request_poll(o->ticket, &r)) {
        if (!o->status_read && o->started) {
            o->outs[0].type = DEVOS_VAL_INT;  o->outs[0].v.i = 0;
            o->outs[1].type = DEVOS_VAL_BOOL; o->outs[1].v.b = false;
            o->outs[2].type = DEVOS_VAL_STR;  put_str(&o->outs[2], "");
            o->outs[3].type = DEVOS_VAL_BOOL; o->outs[3].v.b = true;
            put_str(&o->outs[4], "Proxmox request vanished after it was sent; the outcome is unknown");
            result->outs = o->outs;
            result->out_count = 5;
            snprintf(result->error, sizeof(result->error), "%s", o->outs[4].v.str.s);
            *state = DEVOS_ACT_UNKNOWN;
        } else {
            snprintf(result->error, sizeof(result->error), "Proxmox request vanished");
            *state = DEVOS_ACT_FAILED;
        }
        return DEVOS_OK;
    }
    if (r.state == DEVOS_PROXMOX_REQ_PENDING) { *state = DEVOS_ACT_PENDING; return DEVOS_OK; }
    o->res = r;
    o->done = true;
    if (r.state == DEVOS_PROXMOX_REQ_FAILED) {
        /* Not attempted at all (no such guest, not configured): a real failure,
         * not an unknown outcome. */
        snprintf(result->error, sizeof(result->error), "%s", r.error[0] ? r.error : "Proxmox could not run that");
        *state = DEVOS_ACT_FAILED;
        return DEVOS_OK;
    }
    fill(o, result);
    *state = DEVOS_ACT_DONE;
    return DEVOS_OK;
}

static devos_err_t prox_cancel(void *op)
{
    prox_op_t *o = op;
    o->started = devos_proxmox_request_started(o->ticket);
    o->cancelled = true;
    devos_proxmox_request_cancel(o->ticket);
    return DEVOS_OK;
}
static void prox_release(void *op)
{
    prox_op_t *o = op;
    devos_proxmox_request_release(o->ticket);
    free(o);
}
static bool prox_available(char *reason, size_t cap)
{
    if (s_gate && !s_gate(reason, cap)) return false;
    /* Non-blocking readiness: never call devos_proxmox_init() from here - this
     * runs on the Core 0 scheduler (devos_action_start -> available) and init
     * allocates PSRAM and reads NVS/SD. The app owns initialisation; an
     * uninitialised engine reports not-ready instead. */
    if (!devos_proxmox_ready()) {
        snprintf(reason, cap, "Proxmox is not ready (open the Proxmox app once)");
        return false;
    }
    if (devos_proxmox_configured()) return true;
    snprintf(reason, cap, "Proxmox is not configured (Settings in the Proxmox app)");
    return false;
}

static const devos_action_ops_t STATUS_OPS   = { status_start,   prox_poll, prox_cancel, prox_release, prox_available };
static const devos_action_ops_t START_OPS    = { start_start,    prox_poll, prox_cancel, prox_release, prox_available };
static const devos_action_ops_t STOP_OPS     = { stop_start,     prox_poll, prox_cancel, prox_release, prox_available };
static const devos_action_ops_t SHUTDOWN_OPS = { shutdown_start, prox_poll, prox_cancel, prox_release, prox_available };
static const devos_action_ops_t REBOOT_OPS   = { reboot_start,   prox_poll, prox_cancel, prox_release, prox_available };

static const devos_action_param_t GUEST_P[] = {
    { .name = "vmid", .type = DEVOS_VAL_INT, .required = true, .expression = true, .min = 1, .max = 999999999,
      .help = "the guest's id (its node and type are looked up for you)" },
    { .name = "timeout", .type = DEVOS_VAL_DURATION, .expression = true, .min = 1000, .max = 30000, .def = "12s" },
};
static const devos_action_out_t STATUS_OUT[] = {
    { .name = "ok", .type = DEVOS_VAL_BOOL },
    { .name = "status", .type = DEVOS_VAL_INT },
    { .name = "state", .type = DEVOS_VAL_STR, .help = "running / stopped / paused / suspended" },
    { .name = "node", .type = DEVOS_VAL_STR },
    { .name = "name", .type = DEVOS_VAL_STR },
    { .name = "type", .type = DEVOS_VAL_STR, .help = "qemu or lxc" },
    { .name = "cpu", .type = DEVOS_VAL_NUM, .help = "0..1 of one core" },
    { .name = "mem", .type = DEVOS_VAL_INT, .help = "bytes in use" },
    { .name = "maxmem", .type = DEVOS_VAL_INT, .help = "bytes allocated" },
    { .name = "uptime", .type = DEVOS_VAL_INT, .help = "seconds" },
    { .name = "error", .type = DEVOS_VAL_STR },
};
static const devos_action_out_t MUTATE_OUT[] = {
    { .name = "status", .type = DEVOS_VAL_INT },
    { .name = "accepted", .type = DEVOS_VAL_BOOL, .help = "Proxmox took the task, not that the guest reached the state" },
    { .name = "task", .type = DEVOS_VAL_STR, .help = "the UPID Proxmox returned" },
    { .name = "outcome_unknown", .type = DEVOS_VAL_BOOL },
    { .name = "error", .type = DEVOS_VAL_STR },
};

static const devos_action_descriptor_t STATUS_D = {
    .id = "proxmox.guest_status", .schema_version = 1, .provider_uid = "proxmox", .category = "containers",
    .label = "Guest status", .description = "Read a VM or container's state, by vmid",
    .params = GUEST_P, .param_count = 2, .outs = STATUS_OUT, .out_count = 11,
    .effect = DEVOS_EFFECT_READ, .retry_safe = true, .recommended_timeout_ms = 12000,
    .ops = &STATUS_OPS,
};
static const devos_action_descriptor_t START_D = {
    .id = "proxmox.guest_start", .schema_version = 1, .provider_uid = "proxmox", .category = "containers",
    .label = "Start a guest", .description = "Start a VM or container (accepted is not running)",
    .params = GUEST_P, .param_count = 2, .outs = MUTATE_OUT, .out_count = 5,
    .effect = DEVOS_EFFECT_MUTATE, .retry_safe = false, .recommended_timeout_ms = 15000,
    .ops = &START_OPS,
};
static const devos_action_descriptor_t STOP_D = {
    .id = "proxmox.guest_stop", .schema_version = 1, .provider_uid = "proxmox", .category = "containers",
    .label = "Stop a guest (hard)", .description = "Stop a VM or container immediately (not a clean shutdown)",
    .params = GUEST_P, .param_count = 2, .outs = MUTATE_OUT, .out_count = 5,
    .effect = DEVOS_EFFECT_MUTATE, .retry_safe = false, .recommended_timeout_ms = 15000,
    .ops = &STOP_OPS,
};
static const devos_action_descriptor_t SHUTDOWN_D = {
    .id = "proxmox.guest_shutdown", .schema_version = 1, .provider_uid = "proxmox", .category = "containers",
    .label = "Shut a guest down", .description = "Ask a VM or container to shut down cleanly",
    .params = GUEST_P, .param_count = 2, .outs = MUTATE_OUT, .out_count = 5,
    .effect = DEVOS_EFFECT_MUTATE, .retry_safe = false, .recommended_timeout_ms = 15000,
    .ops = &SHUTDOWN_OPS,
};
static const devos_action_descriptor_t REBOOT_D = {
    .id = "proxmox.guest_reboot", .schema_version = 1, .provider_uid = "proxmox", .category = "containers",
    .label = "Reboot a guest", .description = "Reboot a VM or container (follow with a status poll)",
    .params = GUEST_P, .param_count = 2, .outs = MUTATE_OUT, .out_count = 5,
    .effect = DEVOS_EFFECT_MUTATE, .retry_safe = false, .recommended_timeout_ms = 15000,
    .ops = &REBOOT_OPS,
};

void jobs_proxmox_register(void)
{
    devos_actions_register(&STATUS_D);
    devos_actions_register(&START_D);
    devos_actions_register(&STOP_D);
    devos_actions_register(&SHUTDOWN_D);
    devos_actions_register(&REBOOT_D);
}
