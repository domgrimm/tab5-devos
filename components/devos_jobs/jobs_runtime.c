/* jobs_runtime: the Jobs AST interpreter - explicit frame stack, request-specific
 * action polling, output binding, wait and cancellation (PLAN.md 9.2). No LVGL;
 * the only external work is through devos_actions. */
#include "jobs_internal.h"
#include "jobs_platform.h"
#include "jobs_store.h"
#include "devos_json.h"

#include <stdio.h>
#include <string.h>

#define JOBS_MAX_ARGS 12
#define JOBS_DEFAULT_RUN_MS 60000

/* ---- variables ---- */
const char *jobs_cause_name(jobs_cause_t c)
{
    switch (c) {
    case JOBS_CAUSE_MANUAL:   return "manual";
    case JOBS_CAUSE_SCHEDULE: return "schedule";
    case JOBS_CAUSE_EVENT:    return "event";
    case JOBS_CAUSE_QUEUE:    return "queued";
    default:                  return "-";
    }
}

static jobs_var_t *var_find(jobs_run_t *r, const char *name)
{
    for (int i = 0; i < r->nvars; i++)
        if (strcmp(r->vars[i].name, name) == 0) return &r->vars[i];
    return NULL;
}

/* Copy a string into the run pool; NULL on overflow. */
static const char *pool_dup(jobs_run_t *r, const char *s, size_t n)
{
    if (r->strpool_used + n + 1 > sizeof(r->strpool)) return NULL;
    char *p = r->strpool + r->strpool_used;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    r->strpool_used += n + 1;
    return p;
}

/* Wipe any credential copies the run resolved (called after each action starts,
 * and at run end). Uses the engine's wipe when the bridge installed one. */
static void wipe_run_secrets(jobs_run_t *r)
{
    if (!r->secret_used) return;
    if (g_jobs.secret_wipe) g_jobs.secret_wipe(r->secret_scratch, r->secret_used);
    else memset(r->secret_scratch, 0, r->secret_used);
    r->secret_used = 0;
}

static bool var_set(jobs_run_t *r, const char *name, const devos_value_t *v, char *err, size_t errcap)
{
    jobs_var_t *slot = var_find(r, name);
    if (!slot) {
        if (r->nvars >= JOBS_MAX_VARS) { snprintf(err, errcap, "too many variables"); return false; }
        slot = &r->vars[r->nvars++];
        snprintf(slot->name, sizeof(slot->name), "%s", name);
    }
    slot->v = *v;
    if (v->type == DEVOS_VAL_STR) {
        const char *copy = pool_dup(r, v->v.str.s ? v->v.str.s : "", v->v.str.len);
        if (!copy) { snprintf(err, errcap, "run string pool full"); return false; }
        slot->v.v.str.s = copy;
        slot->v.v.str.len = (uint32_t)strlen(copy);
    }
    return true;
}

static void val_to_text(const devos_value_t *v, char *out, size_t cap)
{
    switch (v->type) {
    case DEVOS_VAL_BOOL:     snprintf(out, cap, "%s", v->v.b ? "true" : "false"); break;
    case DEVOS_VAL_INT:      snprintf(out, cap, "%lld", (long long)v->v.i); break;
    case DEVOS_VAL_NUM:      snprintf(out, cap, "%g", v->v.n); break;
    case DEVOS_VAL_DURATION: snprintf(out, cap, "%lldms", (long long)v->v.ms); break;
    case DEVOS_VAL_STR: {
        size_t n = v->v.str.len;
        if (n >= cap) n = cap - 1;
        if (v->v.str.s) memcpy(out, v->v.str.s, n);
        out[n] = '\0';
        break;
    }
    default: snprintf(out, cap, "null"); break;
    }
}

/* ---- builtin references ---- */
static bool sys_ref(const char *field, devos_value_t *out)
{
    const devos_jobs_system_t *s = &g_jobs.sys;
    if (strcmp(field, "battery_percent") == 0) { out->type = DEVOS_VAL_INT; out->v.i = s->battery_percent; return true; }
    if (strcmp(field, "battery_valid") == 0)   { out->type = DEVOS_VAL_BOOL; out->v.b = s->battery_valid; return true; }
    if (strcmp(field, "battery_present") == 0) { out->type = DEVOS_VAL_BOOL; out->v.b = s->battery_present; return true; }
    if (strcmp(field, "charging") == 0)        { out->type = DEVOS_VAL_BOOL; out->v.b = s->charging; return true; }
    if (strcmp(field, "wifi_connected") == 0)  { out->type = DEVOS_VAL_BOOL; out->v.b = s->wifi_connected; return true; }
    if (strcmp(field, "wifi_ssid") == 0)       { out->type = DEVOS_VAL_STR; out->v.str.s = s->wifi_ssid; out->v.str.len = (uint32_t)strlen(s->wifi_ssid); return true; }
    if (strcmp(field, "local_ip") == 0)        { out->type = DEVOS_VAL_STR; out->v.str.s = s->local_ip; out->v.str.len = (uint32_t)strlen(s->local_ip); return true; }
    if (strcmp(field, "wifi_rssi") == 0)       { out->type = DEVOS_VAL_INT; out->v.i = s->wifi_rssi; return true; }
    if (strcmp(field, "tailscale_online") == 0)   { out->type = DEVOS_VAL_BOOL; out->v.b = s->tailscale_online; return true; }
    if (strcmp(field, "tailscale_ip") == 0)       { out->type = DEVOS_VAL_STR; out->v.str.s = s->tailscale_ip; out->v.str.len = (uint32_t)strlen(s->tailscale_ip); return true; }
    if (strcmp(field, "tailscale_hostname") == 0) { out->type = DEVOS_VAL_STR; out->v.str.s = s->tailscale_hostname; out->v.str.len = (uint32_t)strlen(s->tailscale_hostname); return true; }
    if (strcmp(field, "wireguard_online") == 0)   { out->type = DEVOS_VAL_BOOL; out->v.b = s->wireguard_online; return true; }
    if (strcmp(field, "wireguard_name") == 0)     { out->type = DEVOS_VAL_STR; out->v.str.s = s->wireguard_name; out->v.str.len = (uint32_t)strlen(s->wireguard_name); return true; }
    if (strcmp(field, "wireguard_address") == 0)  { out->type = DEVOS_VAL_STR; out->v.str.s = s->wireguard_address; out->v.str.len = (uint32_t)strlen(s->wireguard_address); return true; }
    if (strcmp(field, "uptime_s") == 0)        { out->type = DEVOS_VAL_INT; out->v.i = s->uptime_s; return true; }
    if (strcmp(field, "time_valid") == 0)      { out->type = DEVOS_VAL_BOOL; out->v.b = s->time_valid; return true; }
    if (strcmp(field, "psram_free_kb") == 0)   { out->type = DEVOS_VAL_INT; out->v.i = s->psram_free_kb; return true; }
    if (strcmp(field, "sram_free_kb") == 0)    { out->type = DEVOS_VAL_INT; out->v.i = s->sram_free_kb; return true; }
    if (strcmp(field, "sram_largest_kb") == 0) { out->type = DEVOS_VAL_INT; out->v.i = s->sram_largest_kb; return true; }
    if (strcmp(field, "cpu_core0") == 0)       { out->type = DEVOS_VAL_INT; out->v.i = s->cpu_core0; return true; }
    if (strcmp(field, "cpu_core1") == 0)       { out->type = DEVOS_VAL_INT; out->v.i = s->cpu_core1; return true; }
    return false;
}

static bool resolve_path(jobs_run_t *r, const char *path, devos_value_t *out, char *err, size_t errcap)
{
    jobs_var_t *v = var_find(r, path);
    if (v) { *out = v->v; return true; }
    if (strncmp(path, "system.", 7) == 0) {
        if (sys_ref(path + 7, out)) return true;
        snprintf(err, errcap, "unknown system field '%s'", path + 7);
        return false;
    }
    if (strncmp(path, "event.", 6) == 0) {
        const jobs_pending_event_t *ev = r->ev_valid ? &r->ev
                                      : g_jobs.cur_event_valid ? &g_jobs.cur_event : NULL;
        if (!ev) { snprintf(err, errcap, "event data is not available here"); return false; }
        const char *f = path + 6;
        if (strcmp(f, "topic") == 0)     { out->type = DEVOS_VAL_STR; out->v.str.s = ev->topic; out->v.str.len = (uint32_t)strlen(ev->topic); return true; }
        if (strcmp(f, "source") == 0)    { out->type = DEVOS_VAL_STR; out->v.str.s = ev->source; out->v.str.len = (uint32_t)strlen(ev->source); return true; }
        if (strcmp(f, "payload") == 0)   { out->type = DEVOS_VAL_STR; out->v.str.s = ev->payload; out->v.str.len = ev->payload_len; return true; }
        if (strcmp(f, "seq") == 0)       { out->type = DEVOS_VAL_INT; out->v.i = ev->seq; return true; }
        if (strcmp(f, "truncated") == 0) { out->type = DEVOS_VAL_BOOL; out->v.b = ev->truncated; return true; }
        if (strcmp(f, "retain") == 0)    { out->type = DEVOS_VAL_BOOL; out->v.b = ev->retain; return true; }
        snprintf(err, errcap, "unknown event field '%s'", f);
        return false;
    }
    snprintf(err, errcap, "unknown variable '%s'", path);
    return false;
}

/* ---- expressions ---- */
static bool eval(jobs_run_t *r, const jobs_node_t *e, devos_value_t *out, char *err, size_t errcap);

static bool eval_str(jobs_run_t *r, const jobs_node_t *e, devos_value_t *out, char *err, size_t errcap)
{
    char buf[JOBS_RUN_STRPOOL];
    size_t o = 0;
    buf[0] = '\0';
    for (const jobs_node_t *part = e->a; part; part = part->next) {
        if (part->sub == JSP_REF) {
            devos_value_t v;
            if (!resolve_path(r, part->u.str.s, &v, err, errcap)) return false;
            char t[128];
            val_to_text(&v, t, sizeof(t));
            size_t n = strlen(t);
            if (o + n + 1 > sizeof(buf)) { snprintf(err, errcap, "interpolated string too long"); return false; }
            memcpy(buf + o, t, n);
            o += n;
        } else {
            size_t n = strlen(part->u.str.s);
            if (o + n + 1 > sizeof(buf)) { snprintf(err, errcap, "interpolated string too long"); return false; }
            memcpy(buf + o, part->u.str.s, n);
            o += n;
        }
    }
    buf[o] = '\0';
    const char *copy = pool_dup(r, buf, o);
    if (!copy) { snprintf(err, errcap, "run string pool full"); return false; }
    out->type = DEVOS_VAL_STR;
    out->v.str.s = copy;
    out->v.str.len = (uint32_t)o;
    return true;
}

static bool eval_call(jobs_run_t *r, const jobs_node_t *e, devos_value_t *out, char *err, size_t errcap)
{
    const char *fn = e->u.str.s;
    int argc = 0;
    for (const jobs_node_t *a = e->a; a; a = a->next) argc++;
    if (strcmp(fn, "contains") == 0 && argc == 2) {
        devos_value_t a, b;
        if (!eval(r, e->a, &a, err, errcap) || !eval(r, e->a->next, &b, err, errcap)) return false;
        if (a.type != DEVOS_VAL_STR || b.type != DEVOS_VAL_STR) { snprintf(err, errcap, "contains() wants strings"); return false; }
        char sub[128], hay[512];
        val_to_text(&b, sub, sizeof(sub));
        val_to_text(&a, hay, sizeof(hay));
        out->type = DEVOS_VAL_BOOL;
        out->v.b = strstr(hay, sub) != NULL;
        return true;
    }
    if (strcmp(fn, "json_get") == 0) {
        /* Narrow dot-path extraction (PLAN.md 6.2): a missing value is null,
         * never an error; a decoded string is copied into the run pool so it
         * lives as long as the variable that holds it. */
        if (argc != 2) { snprintf(err, errcap, "json_get(body, path) needs two arguments"); return false; }
        devos_value_t body;
        if (!eval(r, e->a, &body, err, errcap)) return false;
        if (body.type != DEVOS_VAL_STR || !body.v.str.s) { out->type = DEVOS_VAL_NULL; return true; }
        const jobs_node_t *pn = e->a->next;
        const char *path = (pn && pn->kind == JN_EXPR_STR && pn->a && !pn->a->next && pn->a->sub == JSP_LITERAL)
                               ? pn->a->u.str.s : NULL;
        if (!path || !path[0]) { snprintf(err, errcap, "json_get() path must be a literal string"); return false; }
        devos_json_val_t jv;
        if (!devos_json_path(body.v.str.s, body.v.str.len, path, &jv)) { out->type = DEVOS_VAL_NULL; return true; }
        switch (jv.kind) {
        case DEVOS_JSON_BOOL: out->type = DEVOS_VAL_BOOL; out->v.b = jv.b; return true;
        case DEVOS_JSON_INT:  out->type = DEVOS_VAL_INT;  out->v.i = jv.i; return true;
        case DEVOS_JSON_NUM:  out->type = DEVOS_VAL_NUM;  out->v.n = jv.n; return true;
        case DEVOS_JSON_STR: {
            /* Reject a value whose raw length cannot fit the run pool rather
             * than silently truncating it (escapes only make it shorter). */
            size_t room = sizeof(r->strpool) - r->strpool_used;
            uint32_t raw = jv.len >= 2 ? jv.len - 2 : 0;
            if ((size_t)raw + 1 > room) { snprintf(err, errcap, "json_get string too long"); return false; }
            if (room < 2) { snprintf(err, errcap, "run string pool full"); return false; }
            char *dst = r->strpool + r->strpool_used;
            if (!devos_json_parse_str(jv.s, jv.s + jv.len, dst, room)) {
                out->type = DEVOS_VAL_NULL;
                return true;
            }
            size_t dl = strlen(dst);
            r->strpool_used += dl + 1;
            out->type = DEVOS_VAL_STR;
            out->v.str.s = dst;
            out->v.str.len = (uint32_t)dl;
            return true;
        }
        case DEVOS_JSON_RAW: {
            /* an object/array: its own JSON text, so it can be logged or fed to
             * another json_get without a second parser */
            size_t room = sizeof(r->strpool) - r->strpool_used;
            if ((size_t)jv.len + 1 > room) { snprintf(err, errcap, "json_get value too long"); return false; }
            char *dst = r->strpool + r->strpool_used;
            if (jv.len) memcpy(dst, jv.s, jv.len);
            dst[jv.len] = '\0';
            r->strpool_used += jv.len + 1;
            out->type = DEVOS_VAL_STR;
            out->v.str.s = dst;
            out->v.str.len = jv.len;
            return true;
        }
        default: out->type = DEVOS_VAL_NULL; return true;
        }
    }
    if (strcmp(fn, "secret") == 0) {
        if (argc != 1) { snprintf(err, errcap, "secret(\"name\") needs one argument"); return false; }
        const jobs_node_t *sn = e->a;
        const char *nm = (sn && sn->kind == JN_EXPR_STR && sn->a && !sn->a->next && sn->a->sub == JSP_LITERAL)
                             ? sn->a->u.str.s : NULL;
        if (!nm || !nm[0]) { snprintf(err, errcap, "secret(\"name\") needs a literal name"); return false; }
        if (!g_jobs.secret_resolve) { snprintf(err, errcap, "secrets are not configured"); return false; }
        size_t room = sizeof(r->secret_scratch) - r->secret_used;
        if (room < 2) { snprintf(err, errcap, "too many secrets in one action"); return false; }
        char *dst = r->secret_scratch + r->secret_used;
        int n = g_jobs.secret_resolve(nm, dst, room, g_jobs.secret_user);
        if (n < 0) { snprintf(err, errcap, "secret \"%s\" is not available", nm); return false; }
        r->secret_used += (size_t)n + 1;
        out->type = DEVOS_VAL_STR;
        out->v.str.s = dst;
        out->v.str.len = (uint32_t)n;
        return true;
    }
    snprintf(err, errcap, "unknown function '%s'", fn);
    return false;
}

static bool as_bool(const devos_value_t *v, bool *b)
{
    if (v->type != DEVOS_VAL_BOOL) return false;
    *b = v->v.b;
    return true;
}

static bool eval(jobs_run_t *r, const jobs_node_t *e, devos_value_t *out, char *err, size_t errcap)
{
    if (!e) { out->type = DEVOS_VAL_NULL; return true; }
    switch (e->kind) {
    case JN_EXPR_LIT: *out = e->u.lit; return true;
    case JN_EXPR_STR: return eval_str(r, e, out, err, errcap);
    case JN_EXPR_REF: return resolve_path(r, e->u.str.s, out, err, errcap);
    case JN_EXPR_CALL: return eval_call(r, e, out, err, errcap);
    case JN_EXPR_UNARY: {
        devos_value_t a;
        if (!eval(r, e->a, &a, err, errcap)) return false;
        bool b;
        if (!as_bool(&a, &b)) { snprintf(err, errcap, "'!' wants a boolean"); return false; }
        out->type = DEVOS_VAL_BOOL;
        out->v.b = !b;
        return true;
    }
    case JN_EXPR_BINARY: {
        devos_value_t a;
        if (!eval(r, e->a, &a, err, errcap)) return false;
        if (e->sub == JOP_AND || e->sub == JOP_OR) {
            bool ba;
            if (!as_bool(&a, &ba)) { snprintf(err, errcap, "'%s' wants booleans", jobs_op_name((jobs_op_t)e->sub)); return false; }
            if (e->sub == JOP_AND && !ba) { out->type = DEVOS_VAL_BOOL; out->v.b = false; return true; }
            if (e->sub == JOP_OR && ba)  { out->type = DEVOS_VAL_BOOL; out->v.b = true; return true; }
            devos_value_t b;
            if (!eval(r, e->b, &b, err, errcap)) return false;
            bool bb;
            if (!as_bool(&b, &bb)) { snprintf(err, errcap, "'%s' wants booleans", jobs_op_name((jobs_op_t)e->sub)); return false; }
            out->type = DEVOS_VAL_BOOL;
            out->v.b = bb;
            return true;
        }
        devos_value_t b;
        if (!eval(r, e->b, &b, err, errcap)) return false;
        out->type = DEVOS_VAL_BOOL;
        if (e->sub == JOP_EQ || e->sub == JOP_NE) {
            bool eq;
            if (a.type == DEVOS_VAL_STR && b.type == DEVOS_VAL_STR) {
                eq = a.v.str.len == b.v.str.len && strncmp(a.v.str.s, b.v.str.s, a.v.str.len) == 0;
            } else if (a.type == DEVOS_VAL_BOOL && b.type == DEVOS_VAL_BOOL) {
                eq = a.v.b == b.v.b;
            } else if (a.type == DEVOS_VAL_NULL && b.type == DEVOS_VAL_NULL) {
                eq = true;
            } else {
                double x = a.type == DEVOS_VAL_NUM ? a.v.n : (double)a.v.i;
                double y = b.type == DEVOS_VAL_NUM ? b.v.n : (double)b.v.i;
                eq = x == y;
            }
            out->v.b = (e->sub == JOP_EQ) ? eq : !eq;
            return true;
        }
        double x = a.type == DEVOS_VAL_NUM ? a.v.n : (double)a.v.i;
        double y = b.type == DEVOS_VAL_NUM ? b.v.n : (double)b.v.i;
        switch (e->sub) {
        case JOP_LT: out->v.b = x < y; break;
        case JOP_LE: out->v.b = x <= y; break;
        case JOP_GT: out->v.b = x > y; break;
        default:     out->v.b = x >= y; break;
        }
        return true;
    }
    default:
        snprintf(err, errcap, "cannot evaluate this expression");
        return false;
    }
}

/* ---- action arguments and output binding ---- */
static const jobs_node_t *find_arg(const jobs_node_t *action, const char *name)
{
    for (const jobs_node_t *a = action->a; a; a = a->next)
        if (a->kind == JN_ARG && strcmp(a->u.str.s, name) == 0) return a;
    return NULL;
}

static bool build_args(jobs_run_t *r, const jobs_node_t *action, const devos_action_descriptor_t *d,
                       devos_value_t *av, char *err, size_t errcap)
{
    for (int i = 0; i < d->param_count; i++) {
        av[i].type = DEVOS_VAL_NULL;
        const jobs_node_t *arg = find_arg(action, d->params[i].name);
        if (!arg) continue;
        if (!eval(r, arg->a, &av[i], err, errcap)) return false;
    }
    return true;
}

static bool bind_outputs(jobs_job_t *j, const jobs_node_t *action, const devos_action_result_t *res,
                         char *err, size_t errcap)
{
    const devos_action_descriptor_t *d = devos_actions_find(action->u.str.s);
    if (!d || !action->u.str.s2) return true;          /* no output name: nothing to bind */
    for (int i = 0; i < d->out_count && i < res->out_count; i++) {
        char name[JOBS_VAR_NAME_MAX];
        snprintf(name, sizeof(name), "%s.%s", action->u.str.s2, d->outs[i].name);
        if (!var_set(&j->run, name, &res->outs[i], err, errcap)) return false;
    }
    return true;
}

/* ---- run lifecycle ---- */
static int64_t policy_dur_arg(const jobs_ast_t *ast, const char *name)
{
    const jobs_node_t *pol = ast->root ? ast->root->b : NULL;
    for (const jobs_node_t *a = pol ? pol->a : NULL; a; a = a->next) {
        if (strcmp(a->u.str.s, name) == 0 && a->a && a->a->kind == JN_EXPR_LIT &&
            a->a->u.lit.type == DEVOS_VAL_DURATION)
            return a->a->u.lit.v.ms;
    }
    return -1;
}

void jobs_policy_read(const jobs_ast_t *ast, int64_t *timeout_ms, int64_t *cooldown_ms, bool *queue_one)
{
    if (timeout_ms) *timeout_ms = JOBS_DEFAULT_RUN_MS;
    if (cooldown_ms) *cooldown_ms = 0;
    if (queue_one) *queue_one = false;
    if (!ast || !ast->root) return;
    int64_t t = policy_dur_arg(ast, "timeout");
    if (timeout_ms && t > 0) *timeout_ms = t;
    int64_t c = policy_dur_arg(ast, "cooldown");
    if (cooldown_ms && c >= 0) *cooldown_ms = c;
    if (queue_one) {
        const jobs_node_t *pol = ast->root->b;
        for (const jobs_node_t *a = pol ? pol->a : NULL; a; a = a->next) {
            if (strcmp(a->u.str.s, "overlap") == 0 && a->a) {
                const char *s = a->a->kind == JN_EXPR_LIT && a->a->u.lit.type == DEVOS_VAL_STR
                                    ? a->a->u.lit.v.str.s : NULL;
                if (s && strcmp(s, "queue_one") == 0) *queue_one = true;
            }
        }
    }
}

/* Evaluate a trigger's optional `where` against the copied triggering event.
 * `j` is the job; g_jobs.cur_event must hold the event. Returns true when it
 * matches (or there is no `where`). On evaluation error the trigger is
 * rejected (false) so a broken filter cannot silently run. */
bool jobs_trigger_where_matches(jobs_job_t *j)
{
    const jobs_node_t *where = j->ast && j->ast->root && j->ast->root->a ? j->ast->root->a->b : NULL;
    if (!where) return true;
    jobs_run_t *r = &g_jobs.trig_run;
    memset(r, 0, sizeof(*r));
    char err[64];
    devos_value_t v;
    if (!eval(r, where, &v, err, sizeof(err))) return false;
    bool b;
    if (!as_bool(&v, &b)) return false;
    return b;
}

void jobs_run_begin(jobs_job_t *j, const char *run_id, int64_t now_ms, uint32_t revision,
                    jobs_cause_t cause)
{
    jobs_run_t *r = &j->run;
    memset(r, 0, sizeof(*r));
    r->active = true;
    j->last_cause = (uint8_t)cause;
    snprintf(r->run_id, sizeof(r->run_id), "%s", run_id);
    r->ast = j->ast;
    jobs_ast_retain(j->ast);
    r->revision = revision;
    r->started_ms = now_ms;
    int64_t tmo = JOBS_DEFAULT_RUN_MS;
    jobs_policy_read(j->ast, &tmo, NULL, NULL);
    r->deadline_ms = now_ms + (tmo > 0 ? tmo : JOBS_DEFAULT_RUN_MS);
    const jobs_node_t *body = j->ast->root ? j->ast->root->c : NULL;
    r->frames[0].kind = FRAME_BLOCK;
    r->frames[0].block = body;
    r->frames[0].cursor = body ? body->a : NULL;
    r->nframes = 1;
    /* Seed the call chain with this job so a direct/indirect self-call is a
     * cycle even before any child runs (PLAN.md 9.3). */
    if (j->name[0]) {
        snprintf(r->call_chain[0], sizeof(r->call_chain[0]), "%s", j->name);
        r->call_depth = 1;
    }
    /* Event-triggered run: keep the triggering event so the body can read
     * event.topic/payload/seq/truncated (the trigger `where` already ran). A
     * manual run of an event job gets a synthetic event with just the topic. */
    if (j->trigger_kind == JTRIG_EVENT) {
        if (g_jobs.cur_event_valid) {
            r->ev = g_jobs.cur_event;
        } else {
            memset(&r->ev, 0, sizeof(r->ev));
            snprintf(r->ev.topic, sizeof(r->ev.topic), "%s", j->event_topic);
        }
        r->ev_valid = true;
    }
}

/* Release any callee ASTs a run still holds (job calls). Safe to call twice;
 * finish_run runs it before releasing the parent AST. */
static void release_calls(jobs_run_t *r)
{
    for (int i = 0; i < r->nframes; i++)
        if (r->frames[i].kind == FRAME_CALL && r->frames[i].call_ast) {
            jobs_ast_release((jobs_ast_t *)r->frames[i].call_ast);
            r->frames[i].call_ast = NULL;
        }
}

/* Resolve a run-target by display name (preferred) or id. Returns NULL with
 * two or more matches, so an ambiguous name never silently calls the wrong job. */
static jobs_job_t *find_callable(const char *name)
{
    if (!name || !name[0]) return NULL;
    int names = 0, ids = 0;
    jobs_job_t *by_name = NULL, *by_id = NULL;
    for (int i = 0; i < g_jobs.count; i++) {
        jobs_job_t *j = &g_jobs.jobs[i];
        if (!j->ast || !j->ast->root) continue;
        if (j->name[0] && strcmp(j->name, name) == 0) { by_name = j; names++; }
        if (strcmp(j->id, name) == 0) { by_id = j; ids++; }
    }
    if (names == 1) return by_name;
    if (names > 1) return NULL;
    return ids == 1 ? by_id : NULL;
}

static bool val_type_ok(devos_val_type_t got, uint8_t want)
{
    if (got == want) return true;
    if ((want == DEVOS_VAL_INT || want == DEVOS_VAL_NUM) &&
        (got == DEVOS_VAL_INT || got == DEVOS_VAL_NUM)) return true;
    return false;
}

static void finish_run(jobs_job_t *j, bool ok, const char *msg, int64_t now)
{
    jobs_run_t *r = &j->run;
    wipe_run_secrets(r);
    release_calls(r);
    r->active = false;
    r->finished = true;
    r->ok = ok;
    snprintf(r->message, sizeof(r->message), "%.63s", msg ? msg : "");
    snprintf(j->last_result, sizeof(j->last_result), "%.63s", msg && msg[0] ? msg : (ok ? "ok" : "failed"));
    j->last_run_ms = now;
    j->last_ok = ok;
    j->last_run_wall_s = g_jobs.sys.time_valid ? g_jobs.sys.wall_unix_s : 0;
    if (!ok && r->trace_n > 0) r->trace[r->trace_n - 1].result = 2;   /* the failing step */
    if (r->ast) { jobs_ast_release((jobs_ast_t *)r->ast); r->ast = NULL; }

    /* Bounded history, written through the Core 1 worker so the scheduler
     * (Core 0) never blocks on the SD card. Structured so the UI reads it
     * without parsing JSONL itself (devos_jobs_history_at). */
    char err[96];
    devos_json_escape(msg ? msg : "", err, sizeof(err));
    char line[224];
    snprintf(line, sizeof(line),
             "{\"run\":\"%s\",\"wall\":%lld,\"cause\":\"%s\",\"ok\":%s,\"ms\":%lld,\"steps\":%d,\"err\":\"%s\"}",
             r->run_id, (long long)j->last_run_wall_s, jobs_cause_name((jobs_cause_t)j->last_cause),
             ok ? "true" : "false", (long long)(now - r->started_ms), r->steps, err);
    jobs_store_history_append_async(j->id, line);
}

static void cancel_pending(jobs_job_t *j)
{
    jobs_run_t *r = &j->run;
    for (int i = 0; i < r->nframes; i++) {
        if (r->frames[i].kind == FRAME_ACTION) {
            devos_action_cancel(r->frames[i].op);
            devos_action_release(r->frames[i].op);
            r->frames[i].kind = FRAME_BLOCK;         /* neutralized */
            r->frames[i].cursor = NULL;
        }
    }
}

void jobs_run_cancel(jobs_job_t *j)
{
    if (j->run.active) j->run.cancelling = true;
}

void jobs_run_tick(jobs_job_t *j, int64_t now_ms)
{
    jobs_run_t *r = &j->run;
    if (!r->active) return;
    if (now_ms >= r->deadline_ms) { cancel_pending(j); finish_run(j, false, "timed out", now_ms); return; }
    if (r->cancelling) { cancel_pending(j); finish_run(j, false, "cancelled", now_ms); return; }

    int budget = JOBS_MAX_STEPS - r->steps;
    while (r->active && budget-- > 0) {
        if (r->nframes == 0) { finish_run(j, true, "ok", now_ms); return; }
        jobs_frame_t *f = &r->frames[r->nframes - 1];

        if (f->kind == FRAME_WAIT) {
            if (now_ms >= f->wake_ms) { r->nframes--; continue; }
            return;                                       /* yield */
        }
        if (f->kind == FRAME_REPEAT) {
            /* Re-entered for each iteration: bind the read-only index, then run
             * the body as a nested block. The step budget and run deadline cap
             * the loop even though the count is bounded at validation. */
            const jobs_node_t *rep = f->repeat;
            int64_t count = rep ? rep->count : 0;
            if (f->iter >= count) { r->nframes--; continue; }
            devos_value_t iv;
            iv.type = DEVOS_VAL_INT;
            iv.v.i = f->iter;
            f->iter++;
            char lerr[64];
            if (!var_set(r, rep->u.str.s, &iv, lerr, sizeof(lerr))) {
                finish_run(j, false, lerr, now_ms);
                return;
            }
            if (r->nframes >= JOBS_MAX_FRAMES) { finish_run(j, false, "too deeply nested", now_ms); return; }
            jobs_frame_t *nf = &r->frames[r->nframes++];
            nf->kind = FRAME_BLOCK;
            nf->block = rep->a;
            nf->cursor = rep->a ? rep->a->a : NULL;
            continue;
        }
        if (f->kind == FRAME_CALL) {
            /* The callee body ended (a `return` left its value, or it fell off
             * the end -> null). Restore the caller's scope, bind the output, and
             * drop the callee AST reference. */
            devos_value_t v;
            if (r->call_value_valid) { v = r->call_value; r->call_value_valid = false; }
            else v.type = DEVOS_VAL_NULL;
            r->nvars = f->var_mark;
            if (f->out_name && f->out_name[0]) {
                char cerr[64];
                if (!var_set(r, f->out_name, &v, cerr, sizeof(cerr))) { finish_run(j, false, cerr, now_ms); return; }
            }
            if (f->call_ast) jobs_ast_release((jobs_ast_t *)f->call_ast);
            if (r->call_depth > 0) r->call_depth--;
            r->nframes--;
            continue;
        }
        if (f->kind == FRAME_ACTION) {
            devos_action_state_t st;
            devos_action_result_t res;
            memset(&res, 0, sizeof(res));
            if (devos_action_poll(f->op, &st, &res) != DEVOS_OK) {
                finish_run(j, false, "action lost", now_ms);
                return;
            }
            if (st == DEVOS_ACT_PENDING) return;          /* yield */
            if (st == DEVOS_ACT_DONE) {
                char err[64];
                if (!bind_outputs(j, f->action, &res, err, sizeof(err))) {
                    devos_action_release(f->op);
                    finish_run(j, false, err, now_ms);
                    return;
                }
                devos_action_release(f->op);
                r->nframes--;
                r->steps++;
                continue;
            }
            devos_action_release(f->op);
            finish_run(j, false, res.error[0] ? res.error : "action failed", now_ms);
            return;
        }

        /* FRAME_BLOCK */
        const jobs_node_t *s = f->cursor;
        if (!s) { r->nframes--; continue; }
        f->cursor = s->next;
        r->cur = s;
        r->steps++;
        if (r->trace_n < JOBS_TRACE_MAX) {
            jobs_trace_t *tr = &r->trace[r->trace_n++];
            tr->line = s->line;
            tr->col = s->col;
            tr->kind = s->kind;
            tr->result = 0;
        } else {
            r->trace_over++;
        }
        char err[64];
        switch (s->kind) {
        case JN_ACTION: {
            const devos_action_descriptor_t *d = devos_actions_find(s->u.str.s);
            if (!d || d->param_count > JOBS_MAX_ARGS) { finish_run(j, false, "unknown action", now_ms); return; }
            devos_value_t av[JOBS_MAX_ARGS];
            if (!build_args(r, s, d, av, err, sizeof(err))) { finish_run(j, false, err, now_ms); return; }
            devos_action_args_t a = { .args = av, .arg_count = d->param_count, .run_id = r->run_id };
            devos_action_handle_t h;
            devos_err_t rc = devos_action_start(s->u.str.s, &a, NULL, &h);
            wipe_run_secrets(r);                       /* provider copied what it needs */
            if (rc != DEVOS_OK) {
                char m[80];
                snprintf(m, sizeof(m), "%s %s", s->u.str.s,
                         rc == DEVOS_ERR_INVALID_ARG ? "has invalid arguments" :
                         rc == DEVOS_ERR_INVALID_STATE ? "is unavailable or busy" : "could not start");
                finish_run(j, false, m, now_ms);
                return;
            }
            if (r->nframes >= JOBS_MAX_FRAMES) { devos_action_release(h); finish_run(j, false, "too deeply nested", now_ms); return; }
            jobs_frame_t *nf = &r->frames[r->nframes++];
            nf->kind = FRAME_ACTION;
            nf->action = s;
            nf->op = h;
            break;
        }
        case JN_SET: {
            devos_value_t v;
            if (!eval(r, s->a, &v, err, sizeof(err))) { finish_run(j, false, err, now_ms); return; }
            if (!var_set(r, s->u.str.s, &v, err, sizeof(err))) { finish_run(j, false, err, now_ms); return; }
            break;
        }
        case JN_IF: {
            devos_value_t v;
            if (!eval(r, s->a, &v, err, sizeof(err))) { finish_run(j, false, err, now_ms); return; }
            bool cond;
            if (!as_bool(&v, &cond)) { finish_run(j, false, "'if' wants a boolean", now_ms); return; }
            const jobs_node_t *branch = cond ? s->b : s->c;
            if (branch) {
                if (r->nframes >= JOBS_MAX_FRAMES) { finish_run(j, false, "too deeply nested", now_ms); return; }
                jobs_frame_t *nf = &r->frames[r->nframes++];
                nf->kind = FRAME_BLOCK;
                nf->block = branch;
                nf->cursor = branch->a;
            }
            break;
        }
        case JN_WAIT:
            if (r->nframes >= JOBS_MAX_FRAMES) { finish_run(j, false, "too deeply nested", now_ms); return; }
            {
                jobs_frame_t *nf = &r->frames[r->nframes++];
                nf->kind = FRAME_WAIT;
                nf->wake_ms = now_ms + s->u.i;
            }
            break;
        case JN_REPEAT:
            if (r->nframes >= JOBS_MAX_FRAMES) { finish_run(j, false, "too deeply nested", now_ms); return; }
            {
                jobs_frame_t *nf = &r->frames[r->nframes++];
                nf->kind = FRAME_REPEAT;
                nf->repeat = s;
                nf->iter = 0;
            }
            break;
        case JN_RUN: {
            jobs_job_t *callee = find_callable(s->u.str.s);
            if (!callee) { finish_run(j, false, "run: no unique job by that name", now_ms); return; }
            if (r->call_depth >= JOBS_MAX_CALL_DEPTH) { finish_run(j, false, "run: too deep", now_ms); return; }
            const char *cname = callee->name[0] ? callee->name : callee->id;
            for (int i = 0; i < r->call_depth; i++)
                if (strcmp(r->call_chain[i], cname) == 0) { finish_run(j, false, "run: cycle", now_ms); return; }
            const jobs_node_t *params = callee->ast->root->d;
            int mark = r->nvars;
            devos_value_t vals[JOBS_MAX_PARAMS];
            const jobs_node_t *plist[JOBS_MAX_PARAMS];
            int np = 0;
            for (const jobs_node_t *p = params; p; p = p->next) {
                if (np >= JOBS_MAX_PARAMS) { finish_run(j, false, "run: too many parameters", now_ms); return; }
                const jobs_node_t *arg = NULL;
                for (const jobs_node_t *a = s->a; a; a = a->next)
                    if (a->kind == JN_ARG && a->u.str.s && strcmp(a->u.str.s, p->u.str.s) == 0) { arg = a; break; }
                if (!arg) { finish_run(j, false, "run: missing input", now_ms); return; }
                if (!eval(r, arg->a, &vals[np], err, sizeof(err))) { finish_run(j, false, err, now_ms); return; }
                if (!val_type_ok(vals[np].type, p->sub)) { finish_run(j, false, "run: input type mismatch", now_ms); return; }
                plist[np++] = p;
            }
            for (const jobs_node_t *a = s->a; a; a = a->next) {
                if (a->kind != JN_ARG) continue;
                bool known = false;
                for (const jobs_node_t *p = params; p; p = p->next)
                    if (p->u.str.s && a->u.str.s && strcmp(p->u.str.s, a->u.str.s) == 0) known = true;
                if (!known) { finish_run(j, false, "run: unknown input", now_ms); return; }
            }
            if (r->nframes + 2 > JOBS_MAX_FRAMES) { finish_run(j, false, "run: too deep", now_ms); return; }
            const jobs_ast_t *cast = callee->ast;
            jobs_ast_retain((jobs_ast_t *)cast);
            jobs_frame_t *cf = &r->frames[r->nframes++];
            cf->kind = FRAME_CALL;
            cf->call_ast = cast;
            cf->var_mark = mark;
            cf->out_name = s->u.str.s2;
            snprintf(r->call_chain[r->call_depth], sizeof(r->call_chain[0]), "%s", cname);
            r->call_depth++;
            for (int i = 0; i < np; i++) {
                if (!var_set(r, plist[i]->u.str.s, &vals[i], err, sizeof(err))) { finish_run(j, false, err, now_ms); return; }
            }
            const jobs_node_t *cbody = cast->root->c;
            jobs_frame_t *bf = &r->frames[r->nframes++];
            bf->kind = FRAME_BLOCK;
            bf->block = cbody;
            bf->cursor = cbody ? cbody->a : NULL;
            break;
        }
        case JN_RETURN: {
            devos_value_t v;
            if (!eval(r, s->a, &v, err, sizeof(err))) { finish_run(j, false, err, now_ms); return; }
            /* Unwind the child frames; a `return` at the top level ends the run. */
            while (r->nframes > 1 && r->frames[r->nframes - 1].kind != FRAME_CALL) {
                jobs_frame_t *top = &r->frames[r->nframes - 1];
                if (top->kind == FRAME_ACTION) { devos_action_cancel(top->op); devos_action_release(top->op); }
                r->nframes--;
            }
            if (r->nframes <= 1) { finish_run(j, true, "ok", now_ms); return; }
            r->call_value = v;
            r->call_value_valid = true;
            break;                                  /* the FRAME_CALL is handled next */
        }
        case JN_BLOCK:
            if (r->nframes >= JOBS_MAX_FRAMES) { finish_run(j, false, "too deeply nested", now_ms); return; }
            {
                jobs_frame_t *nf = &r->frames[r->nframes++];
                nf->kind = FRAME_BLOCK;
                nf->block = s;
                nf->cursor = s->a;
            }
            break;
        default:
            break;
        }
    }
    if (r->active) finish_run(j, false, "step budget exceeded", now_ms);
}
