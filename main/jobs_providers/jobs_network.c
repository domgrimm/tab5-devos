/* jobs_network: the network.ping action provider for Jobs. Wraps the
 * request-specific devos_probe_* engine so it never disturbs the Network app's
 * ping singleton (PLAN.md 7.3).
 *
 * network.wol sends a magic packet synchronously with a per-call result (the
 * shared last-error is never read). The target must be empty (broadcast) or an
 * IPv4 literal: a hostname would resolve on the scheduler task, which the
 * architecture forbids, so it is refused with a clear diagnostic. */
#include "jobs_providers.h"
#include "devos_actions.h"
#include "devos_netdiag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <lwip/inet.h>
#else
#include <arpa/inet.h>
#endif

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

/* ---- network.wol ---- */
typedef struct {
    devos_value_t outs[3];
    char target[64];
    char error[96];
} wol_op_t;

static bool ip_or_empty(const char *s)
{
    if (!s || !s[0]) return true;
    struct in_addr a;
    return inet_aton(s, &a) != 0;
}

static devos_err_t wol_start(const devos_action_args_t *args, const devos_action_context_t *ctx, void **op)
{
    (void)ctx;
    const char *m = arg_str(args, 0);
    const char *tgt = arg_str(args, 1);
    if (!ip_or_empty(tgt)) return DEVOS_ERR_INVALID_ARG;   /* no scheduler-blocking DNS */
    uint8_t mac[6];
    char perr[96] = "";
    if (devos_wol_parse_mac(m, mac, perr, sizeof(perr)) != 0) return DEVOS_ERR_INVALID_ARG;
    wol_op_t *o = calloc(1, sizeof(*o));
    if (!o) return DEVOS_ERR_NO_MEM;
    char terr[96] = "";
    int rc = devos_wol_send_ex(mac, tgt, o->target, sizeof(o->target), terr, sizeof(terr));
    if (rc != 0) snprintf(o->error, sizeof(o->error), "%s", terr[0] ? terr : "send failed");
    o->outs[0].type = DEVOS_VAL_BOOL; o->outs[0].v.b = rc == 0;
    o->outs[1].type = DEVOS_VAL_STR;  o->outs[1].v.str.s = o->target; o->outs[1].v.str.len = (uint32_t)strlen(o->target);
    o->outs[2].type = DEVOS_VAL_STR;  o->outs[2].v.str.s = o->error;  o->outs[2].v.str.len = (uint32_t)strlen(o->error);
    *op = o;
    return DEVOS_OK;
}
static devos_err_t wol_poll(void *op, devos_action_state_t *state, devos_action_result_t *result)
{
    wol_op_t *o = op;
    result->outs = o->outs;
    result->out_count = 3;
    *state = DEVOS_ACT_DONE;
    return DEVOS_OK;
}
static devos_err_t wol_cancel(void *op) { (void)op; return DEVOS_OK; }
static void wol_release(void *op) { free(op); }
static const devos_action_ops_t WOL_OPS = { wol_start, wol_poll, wol_cancel, wol_release, NULL };

static const devos_action_param_t WOL_P[] = {
    { .name = "mac", .type = DEVOS_VAL_STR, .required = true, .expression = true, .max_len = 32,
      .help = "aa:bb:cc:dd:ee:ff (or a plain hex run)" },
    { .name = "target", .type = DEVOS_VAL_STR, .expression = true, .max_len = 64,
      .help = "empty = broadcast; else an IPv4 address or directed broadcast" },
};
static const devos_action_out_t WOL_O[] = {
    { .name = "sent", .type = DEVOS_VAL_BOOL },
    { .name = "target", .type = DEVOS_VAL_STR },
    { .name = "error", .type = DEVOS_VAL_STR },
};
static const devos_action_descriptor_t WOL_D = {
    .id = "network.wol", .schema_version = 1, .provider_uid = "netdiag", .category = "network",
    .label = "Wake-on-LAN", .description = "Send a magic packet (no promise the target woke)",
    .params = WOL_P, .param_count = 2, .outs = WOL_O, .out_count = 3,
    .effect = DEVOS_EFFECT_NET_SEND, .retry_safe = true, .recommended_timeout_ms = 3000,
    .ops = &WOL_OPS,
};

/* ---- network.dns ---- */
typedef struct {
    int ticket;
    bool done;
    devos_dns_result_t res;
    devos_value_t outs[5];
    char first[256];
    char rcode[24];
} dns_op_t;

static uint16_t dns_type_of(const char *s)
{
    if (!s) return 0;
    if (!strcmp(s, "A")) return DEVOS_DNS_A;
    if (!strcmp(s, "AAAA")) return DEVOS_DNS_AAAA;
    if (!strcmp(s, "CNAME")) return DEVOS_DNS_CNAME;
    if (!strcmp(s, "MX")) return DEVOS_DNS_MX;
    if (!strcmp(s, "TXT")) return DEVOS_DNS_TXT;
    if (!strcmp(s, "NS")) return DEVOS_DNS_NS;
    if (!strcmp(s, "PTR")) return DEVOS_DNS_PTR;
    if (!strcmp(s, "SRV")) return DEVOS_DNS_SRV;
    if (!strcmp(s, "SOA")) return DEVOS_DNS_SOA;
    if (!strcmp(s, "ANY")) return DEVOS_DNS_ANY;
    return 0;
}

static devos_err_t dns_start(const devos_action_args_t *args, const devos_action_context_t *ctx, void **op)
{
    (void)ctx;
    const char *name = arg_str(args, 0);
    if (!name || !name[0]) return DEVOS_ERR_INVALID_ARG;
    const char *server = arg_str(args, 1);
    const char *ts = arg_str(args, 2);
    int ticket = devos_dns_ctx_start(server, name, dns_type_of(ts));
    if (ticket <= 0) return DEVOS_ERR_INVALID_STATE;       /* pool busy / bad name */
    dns_op_t *o = calloc(1, sizeof(*o));
    if (!o) { devos_dns_ctx_release(ticket); return DEVOS_ERR_NO_MEM; }
    o->ticket = ticket;
    *op = o;
    return DEVOS_OK;
}

static devos_err_t dns_poll(void *op, devos_action_state_t *state, devos_action_result_t *result)
{
    dns_op_t *o = op;
    if (!o->done) {
        devos_dns_result_t r;
        int rc = devos_dns_ctx_poll(o->ticket, &r);
        if (rc < 0) { *state = DEVOS_ACT_FAILED; return DEVOS_OK; }
        if (rc == 0) { *state = DEVOS_ACT_PENDING; return DEVOS_OK; }
        o->res = r;
        o->done = true;
    }
    devos_dns_result_t *r = &o->res;
    bool transport_ok = r->error[0] == '\0';
    o->outs[0].type = DEVOS_VAL_BOOL; o->outs[0].v.b = transport_ok && r->rcode == 0 && r->n > 0;
    snprintf(o->rcode, sizeof(o->rcode), "%s", transport_ok ? devos_dns_rcode_name(r->rcode) : "ERROR");
    o->outs[1].type = DEVOS_VAL_STR;  o->outs[1].v.str.s = o->rcode; o->outs[1].v.str.len = (uint32_t)strlen(o->rcode);
    o->outs[2].type = DEVOS_VAL_INT;  o->outs[2].v.i = r->n;
    snprintf(o->first, sizeof(o->first), "%s", r->n > 0 ? r->rr[0].data : "");
    o->outs[3].type = DEVOS_VAL_STR;  o->outs[3].v.str.s = o->first; o->outs[3].v.str.len = (uint32_t)strlen(o->first);
    o->outs[4].type = DEVOS_VAL_STR;  o->outs[4].v.str.s = r->error; o->outs[4].v.str.len = (uint32_t)strlen(r->error);
    result->outs = o->outs;
    result->out_count = 5;
    *state = DEVOS_ACT_DONE;
    return DEVOS_OK;
}
static devos_err_t dns_cancel(void *op)
{
    dns_op_t *o = op;
    devos_dns_ctx_cancel(o->ticket);
    return DEVOS_OK;
}
static void dns_release(void *op)
{
    dns_op_t *o = op;
    devos_dns_ctx_release(o->ticket);
    free(o);
}
static const devos_action_ops_t DNS_OPS = { dns_start, dns_poll, dns_cancel, dns_release, NULL };

static const devos_action_param_t DNS_P[] = {
    { .name = "name", .type = DEVOS_VAL_STR, .required = true, .expression = true, .max_len = 128 },
    { .name = "server", .type = DEVOS_VAL_STR, .expression = true, .max_len = 48,
      .help = "empty = the DHCP server" },
    { .name = "type", .type = DEVOS_VAL_STR, .choices = "A|AAAA|CNAME|MX|TXT|NS|PTR|SRV|SOA|ANY", .def = "A" },
};
static const devos_action_out_t DNS_O[] = {
    { .name = "ok", .type = DEVOS_VAL_BOOL },
    { .name = "rcode", .type = DEVOS_VAL_STR },
    { .name = "count", .type = DEVOS_VAL_INT },
    { .name = "first", .type = DEVOS_VAL_STR },
    { .name = "error", .type = DEVOS_VAL_STR },
};
static const devos_action_descriptor_t DNS_D = {
    .id = "network.dns", .schema_version = 1, .provider_uid = "netdiag", .category = "network",
    .label = "DNS lookup", .description = "One DNS query (A/AAAA/CNAME/MX/TXT/NS/PTR/SRV/SOA)",
    .params = DNS_P, .param_count = 3, .outs = DNS_O, .out_count = 5,
    .effect = DEVOS_EFFECT_READ, .retry_safe = true, .recommended_timeout_ms = 6000,
    .ops = &DNS_OPS,
};

void jobs_network_register(void)
{
    devos_actions_register(&PING_D);
    devos_actions_register(&WOL_D);
    devos_actions_register(&DNS_D);
}
