/* jobs_network: the network.ping / network.wol / network.dns action providers
 * for Jobs. Wraps the request-specific devos_probe_* / devos_wol_* /
 * devos_dns_ctx_* engines so they never disturb the Network app's singletons
 * (PLAN.md 7.3). All three rely on that app's engine initialisation; with the
 * Network app switched off they report unavailable and never touch a
 * half-built engine (AGENTS.md invariant 10).
 *
 * network.wol sends a magic packet through its per-call ticket (the shared
 * last-error is never read). The target may be empty (broadcast), an IPv4
 * literal or a hostname: the hostname is resolved on the engine's worker, not
 * on the scheduler task. */
#include "jobs_providers.h"
#include "devos_actions.h"
#include "devos_netdiag.h"

#include <stdio.h>
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

/* The request-specific engines live in devos_netdiag, which only the Network
 * app initialises. Reporting unavailable (with a reason) is safe and never
 * re-enables the app. */
static bool netdiag_available(char *reason, size_t cap)
{
    if (devos_netdiag_ready()) return true;
    snprintf(reason, cap, "The Network app is switched off in Settings > Apps");
    return false;
}

static devos_err_t ping_start(const devos_action_args_t *args, const devos_action_context_t *ctx, void **op)
{
    (void)ctx;
    const char *host = arg_str(args, 0);
    if (!host[0] || strlen(host) > 253) return DEVOS_ERR_INVALID_ARG;
    int timeout = (int)jobs_arg_ms(args, 1, 3000);
    int h = devos_probe_submit(host, timeout);
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
        /* The probe handle vanished. Bind the declared outputs so the job sees
         * a real error, not the runtime's bare "action failed". */
        snprintf(result->error, sizeof(result->error), "the ping probe vanished");
        o->outs[0].type = DEVOS_VAL_BOOL; o->outs[0].v.b = false;
        o->outs[1].type = DEVOS_VAL_STR;  o->outs[1].v.str.s = "";    o->outs[1].v.str.len = 0;
        o->outs[2].type = DEVOS_VAL_INT;  o->outs[2].v.i = -1;
        o->outs[3].type = DEVOS_VAL_STR;  o->outs[3].v.str.s = result->error;
        o->outs[3].v.str.len = (uint32_t)strlen(result->error);
        result->outs = o->outs;
        result->out_count = 4;
        *state = DEVOS_ACT_FAILED;
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
static const devos_action_ops_t PING_OPS = { ping_start, ping_poll, ping_cancel, ping_release, netdiag_available };

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
    int ticket;
    bool done;
    bool ok;
    devos_value_t outs[3];
    char target[64];
    char error[96];
} wol_op_t;

static devos_err_t wol_start(const devos_action_args_t *args, const devos_action_context_t *ctx, void **op)
{
    (void)ctx;
    const char *m = arg_str(args, 0);
    const char *tgt = arg_str(args, 1);
    if (strlen(tgt) >= sizeof(((wol_op_t *)0)->target)) return DEVOS_ERR_INVALID_ARG;  /* would truncate */
    uint8_t mac[6];
    char perr[96] = "";
    if (devos_wol_parse_mac(m, mac, perr, sizeof(perr)) != 0) return DEVOS_ERR_INVALID_ARG;
    int t = devos_wol_submit(mac, tgt);
    if (t <= 0) return DEVOS_ERR_INVALID_STATE;           /* no free slot */
    wol_op_t *o = calloc(1, sizeof(*o));
    if (!o) { devos_wol_release(t); return DEVOS_ERR_NO_MEM; }
    o->ticket = t;
    *op = o;
    return DEVOS_OK;
}
static devos_err_t wol_poll(void *op, devos_action_state_t *state, devos_action_result_t *result)
{
    wol_op_t *o = op;
    if (!o->done) {
        bool ok = false;
        char target[64], err[96];
        int rc = devos_wol_poll(o->ticket, &ok, target, sizeof(target), err, sizeof(err));
        if (rc < 0) {
            /* The ticket vanished: bind the declared outputs with a real error. */
            snprintf(result->error, sizeof(result->error), "the Wake-on-LAN send vanished");
            o->outs[0].type = DEVOS_VAL_BOOL; o->outs[0].v.b = false;
            o->outs[1].type = DEVOS_VAL_STR;  o->outs[1].v.str.s = ""; o->outs[1].v.str.len = 0;
            o->outs[2].type = DEVOS_VAL_STR;  o->outs[2].v.str.s = result->error;
            o->outs[2].v.str.len = (uint32_t)strlen(result->error);
            result->outs = o->outs;
            result->out_count = 3;
            *state = DEVOS_ACT_FAILED;
            return DEVOS_OK;
        }
        if (rc == 0) { *state = DEVOS_ACT_PENDING; return DEVOS_OK; }
        o->ok = ok;
        snprintf(o->target, sizeof(o->target), "%s", target);
        snprintf(o->error, sizeof(o->error), "%s", err);
        o->done = true;
    }
    o->outs[0].type = DEVOS_VAL_BOOL; o->outs[0].v.b = o->ok;
    o->outs[1].type = DEVOS_VAL_STR;  o->outs[1].v.str.s = o->target; o->outs[1].v.str.len = (uint32_t)strlen(o->target);
    o->outs[2].type = DEVOS_VAL_STR;  o->outs[2].v.str.s = o->error;  o->outs[2].v.str.len = (uint32_t)strlen(o->error);
    result->outs = o->outs;
    result->out_count = 3;
    *state = DEVOS_ACT_DONE;
    return DEVOS_OK;
}
static devos_err_t wol_cancel(void *op)
{
    wol_op_t *o = op;
    devos_wol_cancel(o->ticket);
    return DEVOS_OK;
}
static void wol_release(void *op)
{
    wol_op_t *o = op;
    devos_wol_release(o->ticket);
    free(o);
}
static const devos_action_ops_t WOL_OPS = { wol_start, wol_poll, wol_cancel, wol_release, netdiag_available };

static const devos_action_param_t WOL_P[] = {
    { .name = "mac", .type = DEVOS_VAL_STR, .required = true, .expression = true, .max_len = 32,
      .help = "aa:bb:cc:dd:ee:ff (or a plain hex run)" },
    { .name = "target", .type = DEVOS_VAL_STR, .expression = true, .max_len = 64,
      .help = "empty = broadcast; else a host, IPv4 address or directed broadcast" },
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
#define JOBS_DNS_MAX_ACTIVE 1   /* one Jobs lookup at a time; the engine pool is 2 */
static int s_dns_active;

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
    if (s_dns_active >= JOBS_DNS_MAX_ACTIVE) return DEVOS_ERR_INVALID_STATE;
    const char *name = arg_str(args, 0);
    if (!name || !name[0]) return DEVOS_ERR_INVALID_ARG;
    const char *server = arg_str(args, 1);
    const char *ts = arg_str(args, 2);
    /* The engine copies into 128/48-byte buffers; refuse rather than truncate
     * to the wrong name (a confidently wrong answer). */
    if (strlen(name) >= 128 || strlen(server) >= 48) return DEVOS_ERR_INVALID_ARG;
    int ticket = devos_dns_ctx_start(server, name, dns_type_of(ts));
    if (ticket <= 0) return DEVOS_ERR_INVALID_STATE;       /* pool busy / bad name */
    dns_op_t *o = calloc(1, sizeof(*o));
    if (!o) { devos_dns_ctx_release(ticket); return DEVOS_ERR_NO_MEM; }
    o->ticket = ticket;
    s_dns_active++;
    *op = o;
    return DEVOS_OK;
}

static devos_err_t dns_poll(void *op, devos_action_state_t *state, devos_action_result_t *result)
{
    dns_op_t *o = op;
    if (!o->done) {
        devos_dns_result_t r;
        int rc = devos_dns_ctx_poll(o->ticket, &r);
        if (rc < 0) {
            /* The ticket vanished: bind the declared outputs with a real error. */
            snprintf(result->error, sizeof(result->error), "the DNS lookup vanished");
            o->outs[0].type = DEVOS_VAL_BOOL; o->outs[0].v.b = false;
            snprintf(o->rcode, sizeof(o->rcode), "%s", "ERROR");
            o->outs[1].type = DEVOS_VAL_STR;  o->outs[1].v.str.s = o->rcode; o->outs[1].v.str.len = (uint32_t)strlen(o->rcode);
            o->outs[2].type = DEVOS_VAL_INT;  o->outs[2].v.i = 0;
            o->outs[3].type = DEVOS_VAL_STR;  o->outs[3].v.str.s = ""; o->outs[3].v.str.len = 0;
            o->outs[4].type = DEVOS_VAL_STR;  o->outs[4].v.str.s = result->error;
            o->outs[4].v.str.len = (uint32_t)strlen(result->error);
            result->outs = o->outs;
            result->out_count = 5;
            *state = DEVOS_ACT_FAILED;
            return DEVOS_OK;
        }
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
    if (s_dns_active > 0) s_dns_active--;
    free(o);
}
static const devos_action_ops_t DNS_OPS = { dns_start, dns_poll, dns_cancel, dns_release, netdiag_available };

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
