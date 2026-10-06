/* jobs_http: the http.request action provider for Jobs. Wraps the existing
 * devos_http submit/poll/cancel worker (AGENTS.md invariant 7 - never
 * esp_http_client). Transport success can include 4xx/5xx: `ok` means a
 * response arrived, `status` carries the code (PLAN.md 7.2/7.3).
 *
 * Admission is capped at two outstanding Jobs HTTP tickets so an interactive
 * REST request keeps headroom. Credential headers are copied by devos_http on
 * submit, so the provider's own buffer is wiped immediately after. */
#include "jobs_providers.h"
#include "devos_actions.h"
#include "devos_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"          /* EXT_RAM_BSS_ATTR */
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

#define JOBS_HTTP_MAX_ACTIVE 2

static int s_active;

typedef struct {
    int job;
    bool done;
    bool have_resp;
    devos_http_resp_t resp;
    devos_value_t outs[6];
} http_op_t;

static const char *arg_str(const devos_action_args_t *args, int i)
{
    if (!args || i >= args->arg_count) return NULL;
    const devos_value_t *v = &args->args[i];
    return v->type == DEVOS_VAL_STR && v->v.str.s ? v->v.str.s : NULL;
}
/* "Authorization: Bearer " (22) + token + CRLF (2) + NUL. A token longer than
 * the room here would be silently truncated into a wrong header (a 401 that
 * looks like bad credentials), so it is refused instead. */
#define JOBS_HTTP_AUTH_CAP 300
#define JOBS_HTTP_TOKEN_MAX (JOBS_HTTP_AUTH_CAP - 25)

/* The bearer token plus the caller's extra header lines, combined for
 * devos_http (which copies them on submit, so this is wiped right after).
 * File-scope and in PSRAM: this runs on the Jobs scheduler task, whose stack
 * is 8 KiB, and the header text can be a couple of KiB. */
#define JOBS_HTTP_HDR_CAP (JOBS_HTTP_AUTH_CAP + 2048 + 8)
static EXT_RAM_BSS_ATTR char s_hdrs[JOBS_HTTP_HDR_CAP];

static devos_err_t http_start(const devos_action_args_t *args, const devos_action_context_t *ctx, void **op)
{
    (void)ctx;
    const char *url = arg_str(args, 1);
    if (!url || !url[0]) return DEVOS_ERR_INVALID_ARG;
    if (s_active >= JOBS_HTTP_MAX_ACTIVE) return DEVOS_ERR_INVALID_STATE;

    s_hdrs[0] = '\0';
    size_t ho = 0;
    const char *tok = arg_str(args, 4);          /* bearer_token (credential) */
    if (tok && tok[0]) {
        if (strlen(tok) > JOBS_HTTP_TOKEN_MAX) return DEVOS_ERR_INVALID_ARG;
        int n = snprintf(s_hdrs, sizeof(s_hdrs), "Authorization: Bearer %s\r\n", tok);
        if (n < 0 || n >= (int)sizeof(s_hdrs)) { memset(s_hdrs, 0, sizeof(s_hdrs)); return DEVOS_ERR_INVALID_ARG; }
        ho = (size_t)n;
    }
    const char *extra = arg_str(args, 6);        /* headers: "Name: value" lines */
    if (extra && extra[0]) {
        size_t el = strlen(extra);
        if (ho + el + 2 > sizeof(s_hdrs)) { memset(s_hdrs, 0, sizeof(s_hdrs)); return DEVOS_ERR_INVALID_ARG; }
        memcpy(s_hdrs + ho, extra, el);
        ho += el;
        /* devos_http takes LF- or CRLF-separated lines; terminate the block so
         * a header appended after the token cannot run into it. */
        if (s_hdrs[ho - 1] != '\n') s_hdrs[ho++] = '\n';
        s_hdrs[ho] = '\0';
    }

    devos_http_req_t req;
    memset(&req, 0, sizeof(req));
    req.method = arg_str(args, 0) ? arg_str(args, 0) : "GET";
    req.url = url;
    req.headers = s_hdrs[0] ? s_hdrs : NULL;
    req.body = arg_str(args, 5);
    req.body_len = req.body ? strlen(req.body) : 0;
    req.timeout_ms = (int)jobs_arg_ms(args, 2, 10000);
    req.max_body = (size_t)jobs_arg_int(args, 3, 16384);
    req.insecure = jobs_arg_bool(args, 7, false);   /* https: accept any certificate */
    req.max_redirects = 5;                       /* devos_http returns 3xx with 0 */

    int job = devos_http_submit(&req);
    memset(s_hdrs, 0, sizeof(s_hdrs));           /* the secret copy is devos_http's now */
    if (job < 0) return DEVOS_ERR_NO_MEM;

    http_op_t *o = calloc(1, sizeof(*o));
    if (!o) { devos_http_cancel(job); return DEVOS_ERR_NO_MEM; }
    o->job = job;
    s_active++;
    *op = o;
    return DEVOS_OK;
}

static void http_fill(http_op_t *o, devos_action_result_t *result)
{
    devos_http_resp_t *r = &o->resp;
    o->outs[0].type = DEVOS_VAL_BOOL; o->outs[0].v.b = r->status != 0;
    o->outs[1].type = DEVOS_VAL_INT;  o->outs[1].v.i = r->status;
    o->outs[2].type = DEVOS_VAL_STR;
    o->outs[2].v.str.s = r->body ? r->body : "";
    o->outs[2].v.str.len = (uint32_t)(r->body ? r->body_len : 0);
    o->outs[3].type = DEVOS_VAL_BOOL; o->outs[3].v.b = r->truncated;
    o->outs[4].type = DEVOS_VAL_INT;  o->outs[4].v.i = r->ms_total;
    o->outs[5].type = DEVOS_VAL_STR;
    o->outs[5].v.str.s = r->error;
    o->outs[5].v.str.len = (uint32_t)strlen(r->error);
    result->outs = o->outs;
    result->out_count = 6;
}

static devos_err_t http_poll(void *op, devos_action_state_t *state, devos_action_result_t *result)
{
    http_op_t *o = op;
    if (o->done) { http_fill(o, result); *state = DEVOS_ACT_DONE; return DEVOS_OK; }
    int rc = devos_http_poll(o->job, &o->resp);
    if (rc == 0) { *state = DEVOS_ACT_PENDING; return DEVOS_OK; }
    if (rc < 0) {
        /* The job slot is gone (e.g. the worker reclaimed a cancelled/aborted
         * request). Bind the declared outputs anyway so the job reads a real
         * error instead of the runtime's bare "action failed". */
        if (!o->resp.error[0])
            snprintf(o->resp.error, sizeof(o->resp.error), "HTTP request did not complete");
        http_fill(o, result);
        snprintf(result->error, sizeof(result->error), "%s", o->resp.error);
        *state = DEVOS_ACT_FAILED;
        return DEVOS_OK;
    }
    o->done = true;
    o->have_resp = true;
    http_fill(o, result);
    *state = DEVOS_ACT_DONE;
    return DEVOS_OK;
}

static devos_err_t http_cancel(void *op)
{
    http_op_t *o = op;
    /* HTTP is retry_safe = true: a request already sent is harmless to repeat,
     * so the provider never claims a mutating CANCELLED state for it. The
     * worker abandons the job; a subsequent poll reports it FAILED. */
    devos_http_cancel(o->job);
    return DEVOS_OK;
}
static void http_release(void *op)
{
    http_op_t *o = op;
    if (o->have_resp) devos_http_resp_free(&o->resp);
    if (s_active > 0) s_active--;
    free(o);
}
static const devos_action_ops_t HTTP_OPS = { http_start, http_poll, http_cancel, http_release, NULL };

static const devos_action_param_t HTTP_P[] = {
    { .name = "method", .type = DEVOS_VAL_STR, .expression = true, .choices = "GET|POST|PUT|DELETE|HEAD|PATCH" },
    { .name = "url", .type = DEVOS_VAL_STR, .required = true, .expression = true, .max_len = 1024 },
    { .name = "timeout", .type = DEVOS_VAL_DURATION, .expression = true, .min = 200, .max = 120000 },
    { .name = "max_body", .type = DEVOS_VAL_INT, .expression = true, .min = 1, .max = 65536 },
    { .name = "bearer_token", .type = DEVOS_VAL_STR, .credential = true, .max_len = 256 },
    { .name = "body", .type = DEVOS_VAL_STR, .expression = true, .max_len = 8192 },
    { .name = "headers", .type = DEVOS_VAL_STR, .expression = true, .max_len = 2048,
      .help = "extra header lines, one per line: Name: value" },
    { .name = "insecure", .type = DEVOS_VAL_BOOL,
      .help = "https: accept any certificate (self-signed LAN services)" },
};
static const devos_action_out_t HTTP_O[] = {
    { .name = "ok", .type = DEVOS_VAL_BOOL },
    { .name = "status", .type = DEVOS_VAL_INT },
    { .name = "body", .type = DEVOS_VAL_STR },
    { .name = "truncated", .type = DEVOS_VAL_BOOL },
    { .name = "duration_ms", .type = DEVOS_VAL_INT },
    { .name = "error", .type = DEVOS_VAL_STR },
};
static const devos_action_descriptor_t HTTP_D = {
    .id = "http.request", .schema_version = 1, .provider_uid = "rest", .category = "network",
    .label = "HTTP request", .params = HTTP_P, .param_count = 8, .outs = HTTP_O, .out_count = 6,
    .effect = DEVOS_EFFECT_NET_SEND, .retry_safe = true, .recommended_timeout_ms = 15000,
    .ops = &HTTP_OPS,
};

void jobs_http_register(void)
{
    devos_actions_register(&HTTP_D);
}
