/* jobs_runtime: the Jobs AST interpreter - explicit frame stack, request-specific
 * action polling, output binding, wait and cancellation (PLAN.md 9.2). No LVGL;
 * the only external work is through devos_actions. */
#include "jobs_internal.h"
#include "jobs_platform.h"

#include <stdio.h>
#include <string.h>

#define JOBS_MAX_ARGS 12
#define JOBS_DEFAULT_RUN_MS 60000

/* ---- variables ---- */
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
    if (strncmp(path, "event.", 6) == 0) { snprintf(err, errcap, "event data is not available here"); return false; }
    snprintf(err, errcap, "unknown variable '%s'", path);
    return false;
}

/* ---- expressions ---- */
static bool eval(jobs_job_t *j, const jobs_node_t *e, devos_value_t *out, char *err, size_t errcap);

static bool eval_str(jobs_job_t *j, const jobs_node_t *e, devos_value_t *out, char *err, size_t errcap)
{
    jobs_run_t *r = &j->run;
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

static bool eval_call(jobs_job_t *j, const jobs_node_t *e, devos_value_t *out, char *err, size_t errcap)
{
    const char *fn = e->u.str.s;
    int argc = 0;
    for (const jobs_node_t *a = e->a; a; a = a->next) argc++;
    if (strcmp(fn, "contains") == 0 && argc == 2) {
        devos_value_t a, b;
        if (!eval(j, e->a, &a, err, errcap) || !eval(j, e->a->next, &b, err, errcap)) return false;
        if (a.type != DEVOS_VAL_STR || b.type != DEVOS_VAL_STR) { snprintf(err, errcap, "contains() wants strings"); return false; }
        char sub[128], hay[512];
        val_to_text(&b, sub, sizeof(sub));
        val_to_text(&a, hay, sizeof(hay));
        out->type = DEVOS_VAL_BOOL;
        out->v.b = strstr(hay, sub) != NULL;
        return true;
    }
    if (strcmp(fn, "json_get") == 0) {
        /* narrow extraction is the advanced-language phase; missing is null */
        out->type = DEVOS_VAL_NULL;
        return true;
    }
    if (strcmp(fn, "secret") == 0) {
        snprintf(err, errcap, "secrets are not configured");
        return false;
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

static bool eval(jobs_job_t *j, const jobs_node_t *e, devos_value_t *out, char *err, size_t errcap)
{
    if (!e) { out->type = DEVOS_VAL_NULL; return true; }
    switch (e->kind) {
    case JN_EXPR_LIT: *out = e->u.lit; return true;
    case JN_EXPR_STR: return eval_str(j, e, out, err, errcap);
    case JN_EXPR_REF: return resolve_path(&j->run, e->u.str.s, out, err, errcap);
    case JN_EXPR_CALL: return eval_call(j, e, out, err, errcap);
    case JN_EXPR_UNARY: {
        devos_value_t a;
        if (!eval(j, e->a, &a, err, errcap)) return false;
        bool b;
        if (!as_bool(&a, &b)) { snprintf(err, errcap, "'!' wants a boolean"); return false; }
        out->type = DEVOS_VAL_BOOL;
        out->v.b = !b;
        return true;
    }
    case JN_EXPR_BINARY: {
        devos_value_t a;
        if (!eval(j, e->a, &a, err, errcap)) return false;
        if (e->sub == JOP_AND || e->sub == JOP_OR) {
            bool ba;
            if (!as_bool(&a, &ba)) { snprintf(err, errcap, "'%s' wants booleans", jobs_op_name((jobs_op_t)e->sub)); return false; }
            if (e->sub == JOP_AND && !ba) { out->type = DEVOS_VAL_BOOL; out->v.b = false; return true; }
            if (e->sub == JOP_OR && ba)  { out->type = DEVOS_VAL_BOOL; out->v.b = true; return true; }
            devos_value_t b;
            if (!eval(j, e->b, &b, err, errcap)) return false;
            bool bb;
            if (!as_bool(&b, &bb)) { snprintf(err, errcap, "'%s' wants booleans", jobs_op_name((jobs_op_t)e->sub)); return false; }
            out->type = DEVOS_VAL_BOOL;
            out->v.b = bb;
            return true;
        }
        devos_value_t b;
        if (!eval(j, e->b, &b, err, errcap)) return false;
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

static bool build_args(jobs_job_t *j, const jobs_node_t *action, const devos_action_descriptor_t *d,
                       devos_value_t *av, char *err, size_t errcap)
{
    for (int i = 0; i < d->param_count; i++) {
        av[i].type = DEVOS_VAL_NULL;
        const jobs_node_t *arg = find_arg(action, d->params[i].name);
        if (!arg) continue;
        if (!eval(j, arg->a, &av[i], err, errcap)) return false;
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
static int64_t policy_timeout_ms(const jobs_ast_t *ast)
{
    const jobs_node_t *pol = ast->root ? ast->root->b : NULL;
    for (const jobs_node_t *a = pol ? pol->a : NULL; a; a = a->next) {
        if (strcmp(a->u.str.s, "timeout") == 0 && a->a && a->a->kind == JN_EXPR_LIT &&
            a->a->u.lit.type == DEVOS_VAL_DURATION)
            return a->a->u.lit.v.ms;
    }
    return JOBS_DEFAULT_RUN_MS;
}

void jobs_run_begin(jobs_job_t *j, const char *run_id, int64_t now_ms, uint32_t revision)
{
    jobs_run_t *r = &j->run;
    memset(r, 0, sizeof(*r));
    r->active = true;
    snprintf(r->run_id, sizeof(r->run_id), "%s", run_id);
    r->ast = j->ast;
    jobs_ast_retain(j->ast);
    r->revision = revision;
    r->started_ms = now_ms;
    int64_t tmo = policy_timeout_ms(j->ast);
    r->deadline_ms = now_ms + (tmo > 0 ? tmo : JOBS_DEFAULT_RUN_MS);
    const jobs_node_t *body = j->ast->root ? j->ast->root->c : NULL;
    r->frames[0].kind = FRAME_BLOCK;
    r->frames[0].block = body;
    r->frames[0].cursor = body ? body->a : NULL;
    r->nframes = 1;
}

static void finish_run(jobs_job_t *j, bool ok, const char *msg, int64_t now)
{
    jobs_run_t *r = &j->run;
    r->active = false;
    r->finished = true;
    r->ok = ok;
    snprintf(r->message, sizeof(r->message), "%.63s", msg ? msg : "");
    snprintf(j->last_result, sizeof(j->last_result), "%.63s", msg && msg[0] ? msg : (ok ? "ok" : "failed"));
    j->last_run_ms = now;
    if (r->ast) { jobs_ast_release((jobs_ast_t *)r->ast); r->ast = NULL; }
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
        char err[64];
        switch (s->kind) {
        case JN_ACTION: {
            const devos_action_descriptor_t *d = devos_actions_find(s->u.str.s);
            if (!d || d->param_count > JOBS_MAX_ARGS) { finish_run(j, false, "unknown action", now_ms); return; }
            devos_value_t av[JOBS_MAX_ARGS];
            if (!build_args(j, s, d, av, err, sizeof(err))) { finish_run(j, false, err, now_ms); return; }
            devos_action_args_t a = { .args = av, .arg_count = d->param_count, .run_id = r->run_id };
            devos_action_handle_t h;
            devos_err_t rc = devos_action_start(s->u.str.s, &a, NULL, &h);
            if (rc != DEVOS_OK) { finish_run(j, false, "action unavailable or busy", now_ms); return; }
            if (r->nframes >= JOBS_MAX_FRAMES) { devos_action_release(h); finish_run(j, false, "too deeply nested", now_ms); return; }
            jobs_frame_t *nf = &r->frames[r->nframes++];
            nf->kind = FRAME_ACTION;
            nf->action = s;
            nf->op = h;
            break;
        }
        case JN_SET: {
            devos_value_t v;
            if (!eval(j, s->a, &v, err, sizeof(err))) { finish_run(j, false, err, now_ms); return; }
            if (!var_set(r, s->u.str.s, &v, err, sizeof(err))) { finish_run(j, false, err, now_ms); return; }
            break;
        }
        case JN_IF: {
            devos_value_t v;
            if (!eval(j, s->a, &v, err, sizeof(err))) { finish_run(j, false, err, now_ms); return; }
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
