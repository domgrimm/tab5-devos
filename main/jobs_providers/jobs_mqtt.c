/* jobs_mqtt: the mqtt.publish action provider for Jobs. Wraps the existing
 * devos_mqtt engine (AGENTS.md invariant 7 - never a private MQTT stack).
 *
 * Completion is request-specific (PLAN.md 7.3): QUEUED (accepted locally) ->
 * SENT (bytes on the wire) -> ACKED (matching PUBACK, QoS 1). A lost session
 * or a missing acknowledgement never reports delivery; QoS 2 is rejected. The
 * engine's single broker and plain-TCP limit are documented in README/PLAN. */
#include "jobs_providers.h"
#include "devos_actions.h"
#include "devos_mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t ticket;
    bool done;
    uint8_t qos;
    devos_value_t outs[2];
} mqtt_op_t;

static const char *arg_str(const devos_action_args_t *args, int i)
{
    if (!args || i >= args->arg_count) return NULL;
    const devos_value_t *v = &args->args[i];
    return v->type == DEVOS_VAL_STR && v->v.str.s ? v->v.str.s : NULL;
}
static int64_t arg_int(const devos_action_args_t *args, int i, int64_t def)
{
    if (!args || i >= args->arg_count) return def;
    const devos_value_t *v = &args->args[i];
    if (v->type == DEVOS_VAL_INT) return v->v.i;
    if (v->type == DEVOS_VAL_NUM) return (int64_t)v->v.n;
    return def;
}
static int64_t arg_ms(const devos_action_args_t *args, int i, int64_t def)
{
    if (!args || i >= args->arg_count) return def;
    const devos_value_t *v = &args->args[i];
    return v->type == DEVOS_VAL_DURATION ? v->v.ms : def;
}
static bool arg_bool(const devos_action_args_t *args, int i, bool def)
{
    if (!args || i >= args->arg_count) return def;
    const devos_value_t *v = &args->args[i];
    return v->type == DEVOS_VAL_BOOL ? v->v.b : def;
}

static devos_err_t mqtt_start(const devos_action_args_t *args, const devos_action_context_t *ctx, void **op)
{
    (void)ctx;
    const char *topic = arg_str(args, 0);
    if (!topic || !topic[0]) return DEVOS_ERR_INVALID_ARG;
    int64_t qos = arg_int(args, 3, 0);
    if (qos < 0 || qos > 1) return DEVOS_ERR_INVALID_ARG;      /* reject QoS 2 */
    const char *payload = arg_str(args, 1);
    uint32_t id = devos_mqtt_publish_ticket(topic, payload ? payload : "",
                                            payload ? strlen(payload) : 0,
                                            (int)qos, arg_bool(args, 2, false),
                                            (int)arg_ms(args, 4, 10000));
    if (!id) return DEVOS_ERR_INVALID_STATE;                   /* not connected / no ticket */
    mqtt_op_t *o = calloc(1, sizeof(*o));
    if (!o) { devos_mqtt_ticket_release(id); return DEVOS_ERR_NO_MEM; }
    o->ticket = id;
    o->qos = (uint8_t)qos;
    *op = o;
    return DEVOS_OK;
}

static devos_err_t mqtt_poll(void *op, devos_action_state_t *state, devos_action_result_t *result)
{
    mqtt_op_t *o = op;
    if (o->done) { *state = DEVOS_ACT_DONE; return DEVOS_OK; }
    devos_mqtt_ticket_state_t st;
    if (!devos_mqtt_ticket_poll(o->ticket, &st)) { *state = DEVOS_ACT_FAILED; return DEVOS_OK; }
    switch (st) {
    case DEVOS_MQTT_TICKET_QUEUED:
        *state = DEVOS_ACT_PENDING;
        return DEVOS_OK;
    case DEVOS_MQTT_TICKET_SENT:
        if (o->qos == 1) { *state = DEVOS_ACT_PENDING; return DEVOS_OK; }   /* wait for PUBACK */
        o->outs[0].type = DEVOS_VAL_BOOL; o->outs[0].v.b = true;           /* QoS 0: bytes sent */
        o->outs[1].type = DEVOS_VAL_BOOL; o->outs[1].v.b = false;
        break;
    case DEVOS_MQTT_TICKET_ACKED:
        o->outs[0].type = DEVOS_VAL_BOOL; o->outs[0].v.b = true;
        o->outs[1].type = DEVOS_VAL_BOOL; o->outs[1].v.b = true;
        break;
    case DEVOS_MQTT_TICKET_TIMEOUT:
        o->outs[0].type = DEVOS_VAL_BOOL; o->outs[0].v.b = true;           /* sent, not acknowledged */
        o->outs[1].type = DEVOS_VAL_BOOL; o->outs[1].v.b = false;
        snprintf(result->error, sizeof(result->error), "no acknowledgement");
        break;
    case DEVOS_MQTT_TICKET_LOST:
        o->outs[0].type = DEVOS_VAL_BOOL; o->outs[0].v.b = false;          /* never claim delivery */
        o->outs[1].type = DEVOS_VAL_BOOL; o->outs[1].v.b = false;
        snprintf(result->error, sizeof(result->error), "session lost before completion");
        break;
    default: /* FAILED */
        *state = DEVOS_ACT_FAILED;
        snprintf(result->error, sizeof(result->error), "publish failed");
        return DEVOS_OK;
    }
    o->done = true;
    result->outs = o->outs;
    result->out_count = 2;
    *state = DEVOS_ACT_DONE;
    return DEVOS_OK;
}

static devos_err_t mqtt_cancel(void *op)
{
    (void)op;                       /* a sent mutation is not undone; release frees the ticket */
    return DEVOS_OK;
}
static void mqtt_release(void *op)
{
    mqtt_op_t *o = op;
    devos_mqtt_ticket_release(o->ticket);
    free(o);
}
static bool mqtt_available(char *reason, size_t cap)
{
    if (devos_mqtt_state() == DEVOS_MQTT_UP) return true;
    snprintf(reason, cap, "MQTT is not connected");
    return false;
}
static const devos_action_ops_t MQTT_OPS = { mqtt_start, mqtt_poll, mqtt_cancel, mqtt_release, mqtt_available };

static const devos_action_param_t MQTT_P[] = {
    { .name = "topic", .type = DEVOS_VAL_STR, .required = true, .expression = true, .max_len = 192 },
    { .name = "payload", .type = DEVOS_VAL_STR, .expression = true, .max_len = 4096 },
    { .name = "retain", .type = DEVOS_VAL_BOOL },
    { .name = "qos", .type = DEVOS_VAL_INT, .min = 0, .max = 1, .def = "0" },
    { .name = "timeout", .type = DEVOS_VAL_DURATION, .expression = true, .min = 500, .max = 60000, .def = "10s" },
};
static const devos_action_out_t MQTT_O[] = {
    { .name = "sent", .type = DEVOS_VAL_BOOL },
    { .name = "acknowledged", .type = DEVOS_VAL_BOOL },
};
static const devos_action_descriptor_t MQTT_D = {
    .id = "mqtt.publish", .schema_version = 1, .provider_uid = "mqtt", .category = "network",
    .label = "MQTT publish", .description = "Publish to the configured broker (QoS 0 or 1)",
    .params = MQTT_P, .param_count = 5, .outs = MQTT_O, .out_count = 2,
    .effect = DEVOS_EFFECT_NET_SEND, .retry_safe = false, .recommended_timeout_ms = 15000,
    .ops = &MQTT_OPS,
};

void jobs_mqtt_register(void)
{
    devos_actions_register(&MQTT_D);
}
