/* jobs_validate: type, schema and scope validation for the Jobs AST
 * (PLAN.md section 6.2/6.3). No side effects; a candidate that fails here is
 * never installed as the active revision. No LVGL/network. */
#include "jobs_model.h"
#include "jobs_internal.h"   /* JOBS_MAX_STEPS, JOBS_MAX_ARGS (runtime limits) */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { TY_NULL = 0, TY_BOOL, TY_INT, TY_NUM, TY_STR, TY_DUR, TY_OBJ, TY_ANY } ty_t;

typedef struct {
    char name[40];
    ty_t type;
    bool is_output;
    bool readonly;      /* loop index: cannot be reassigned */
    const devos_action_descriptor_t *schema;
} sym_t;

typedef struct {
    jobs_ast_t *ast;
    sym_t vars[JOBS_MAX_VARS];
    int nvars;
    int scope[24];
    int nscopes;
    bool in_event;
    bool failed;
} V;

static jobs_topic_check_fn s_topic_check;
static void *s_topic_user;
static char s_topic_advisory[160];       /* non-blocking: an unregistered event topic */
static bool s_topic_advisory_set;

void jobs_validate_set_topic_check(jobs_topic_check_fn fn, void *user)
{
    s_topic_check = fn;
    s_topic_user = user;
}

/* Non-blocking advisory set during the last jobs_validate() pass: a trigger
 * references an event topic no app registers, so it can never fire. The job
 * still loads (it may be from a switched-off provider) but the UI warns. */
const char *jobs_validate_topic_advisory(void)
{
    return s_topic_advisory_set ? s_topic_advisory : NULL;
}

static void verr(V *v, const jobs_node_t *n, const char *fmt, ...)
{
    v->failed = true;
    va_list ap;
    va_start(ap, fmt);
    char msg[120];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    jobs_diag_add(v->ast, n->off, n->len, n->line, n->col, "%s", msg);
}

static void push_scope(V *v) { if (v->nscopes < (int)(sizeof(v->scope) / sizeof(v->scope[0]))) v->scope[v->nscopes++] = v->nvars; }
static void pop_scope(V *v) { if (v->nscopes > 0) v->nvars = v->scope[--v->nscopes]; }

static sym_t *sym_find(V *v, const char *name)
{
    for (int i = 0; i < v->nvars; i++)
        if (strcmp(v->vars[i].name, name) == 0) return &v->vars[i];
    return NULL;
}

static sym_t *sym_add(V *v, const char *name, ty_t type, bool is_output, const devos_action_descriptor_t *schema)
{
    if (sym_find(v, name)) { return NULL; }
    if (v->nvars >= JOBS_MAX_VARS) { return NULL; }
    sym_t *s = &v->vars[v->nvars++];
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->type = type;
    s->is_output = is_output;
    s->readonly = false;
    s->schema = schema;
    return s;
}

static ty_t val_to_ty(devos_val_type_t t)
{
    switch (t) {
    case DEVOS_VAL_BOOL: return TY_BOOL;
    case DEVOS_VAL_INT: return TY_INT;
    case DEVOS_VAL_NUM: return TY_NUM;
    case DEVOS_VAL_STR: return TY_STR;
    case DEVOS_VAL_DURATION: return TY_DUR;
    default: return TY_NULL;
    }
}

static bool is_numeric(ty_t t) { return t == TY_INT || t == TY_NUM || t == TY_DUR; }

static bool compatible(ty_t got, ty_t want)
{
    if (got == TY_ANY || want == TY_ANY) return true;
    if (got == want) return true;
    if (is_numeric(got) && is_numeric(want)) return true;
    return false;
}

static const char *ty_name(ty_t t)
{
    switch (t) {
    case TY_NULL: return "null";
    case TY_BOOL: return "boolean";
    case TY_INT: return "integer";
    case TY_NUM: return "number";
    case TY_STR: return "string";
    case TY_DUR: return "duration";
    case TY_OBJ: return "output";
    default: return "value";
    }
}

/* The literal string of a JN_EXPR_STR that has no ${...}; NULL otherwise. */
static const char *lit_str(const jobs_node_t *n)
{
    if (!n || n->kind != JN_EXPR_STR || !n->a || n->a->next || n->a->sub != JSP_LITERAL) return NULL;
    return n->a->u.str.s;
}

/* True when `n` is a literal of exactly `want` (a string literal must also have
 * no ${...}). The runtime's trigger/policy readers accept only literals here. */
static bool literal_of(const jobs_node_t *n, ty_t want)
{
    if (!n) return false;
    if (want == TY_STR) return lit_str(n) != NULL;
    if (n->kind != JN_EXPR_LIT) return false;
    return val_to_ty(n->u.lit.type) == want;
}

/* The magnitude of a numeric literal however it was written, for bounds: a
 * duration literal is its milliseconds, an int/number literal its value. */
static bool lit_magnitude(const jobs_node_t *n, double *out)
{
    if (!n || n->kind != JN_EXPR_LIT) return false;
    switch (n->u.lit.type) {
    case DEVOS_VAL_DURATION: *out = (double)n->u.lit.v.ms; return true;
    case DEVOS_VAL_INT:      *out = (double)n->u.lit.v.i;  return true;
    case DEVOS_VAL_NUM:      *out = n->u.lit.v.n;          return true;
    default:                 return false;
    }
}

static ty_t expr_type(V *v, const jobs_node_t *n, bool *tainted, bool as_value);

/* Resolve a dotted reference path. Returns false and reports on failure. */
static bool resolve_ref(V *v, const jobs_node_t *n, ty_t *type, bool *is_obj)
{
    const char *path = n->u.str.s;
    const char *dot = strchr(path, '.');
    char base[40];
    size_t bl = dot ? (size_t)(dot - path) : strlen(path);
    if (bl >= sizeof(base)) { verr(v, n, "name is too long"); return false; }
    memcpy(base, path, bl);
    base[bl] = '\0';
    const char *field = dot ? dot + 1 : NULL;

    sym_t *s = sym_find(v, base);
    if (s) {
        if (!field) {
            if (s->is_output) { *is_obj = true; *type = TY_OBJ; return true; }
            *is_obj = false;
            *type = s->type;
            return true;
        }
        if (!s->is_output || !s->schema) { verr(v, n, "'%s' has no field '%s'", base, field); return false; }
        for (int i = 0; i < s->schema->out_count; i++) {
            if (strcmp(s->schema->outs[i].name, field) == 0) {
                *is_obj = false;
                *type = val_to_ty(s->schema->outs[i].type);
                return true;
            }
        }
        verr(v, n, "unknown field '%s.%s'", base, field);
        return false;
    }

    if (strcmp(base, "event") == 0) {
        if (!v->in_event) { verr(v, n, "'event' is only available to event triggers"); return false; }
        if (!field) { *is_obj = true; *type = TY_OBJ; return true; }
        static const struct { const char *n; ty_t t; } ef[] = {
            { "topic", TY_STR }, { "source", TY_STR }, { "payload", TY_STR },
            { "retain", TY_BOOL }, { "truncated", TY_BOOL }, { "seq", TY_INT },
        };
        for (size_t i = 0; i < sizeof(ef) / sizeof(ef[0]); i++)
            if (strcmp(ef[i].n, field) == 0) { *is_obj = false; *type = ef[i].t; return true; }
        verr(v, n, "unknown event field '%s' (available: topic, source, payload, seq, truncated, retain)", field);
        return false;
    }
    if (strcmp(base, "system") == 0) {
        if (!field) { *is_obj = true; *type = TY_OBJ; return true; }
        static const struct { const char *n; ty_t t; } sf[] = {
            { "battery_percent", TY_INT }, { "battery_valid", TY_BOOL }, { "battery_present", TY_BOOL },
            { "charging", TY_BOOL }, { "wifi_connected", TY_BOOL }, { "wifi_ssid", TY_STR },
            { "local_ip", TY_STR }, { "wifi_rssi", TY_INT },
            { "tailscale_online", TY_BOOL }, { "tailscale_ip", TY_STR }, { "tailscale_hostname", TY_STR },
            { "wireguard_online", TY_BOOL }, { "wireguard_name", TY_STR }, { "wireguard_address", TY_STR },
            { "uptime_s", TY_INT }, { "time_valid", TY_BOOL },
            { "psram_free_kb", TY_INT }, { "sram_free_kb", TY_INT }, { "sram_largest_kb", TY_INT },
            { "cpu_core0", TY_INT }, { "cpu_core1", TY_INT },
        };
        for (size_t i = 0; i < sizeof(sf) / sizeof(sf[0]); i++)
            if (strcmp(sf[i].n, field) == 0) { *is_obj = false; *type = sf[i].t; return true; }
        verr(v, n, "unknown system field '%s'", field);
        return false;
    }
    verr(v, n, "unknown variable '%s'", base);
    return false;
}

static ty_t call_type(V *v, const jobs_node_t *n, bool *tainted)
{
    const char *fn = n->u.str.s;
    int argc = 0;
    for (jobs_node_t *a = n->a; a; a = a->next) argc++;

    if (strcmp(fn, "secret") == 0) {
        if (argc != 1 || !n->a || n->a->kind != JN_EXPR_STR || !lit_str(n->a)) {
            verr(v, n, "secret(\"name\") needs one literal name");
            return TY_STR;
        }
        *tainted = true;
        return TY_STR;
    }
    if (strcmp(fn, "contains") == 0) {
        if (argc != 2) { verr(v, n, "contains(text, substring) needs two arguments"); return TY_ANY; }
        bool t1 = false, t2 = false;
        ty_t a = expr_type(v, n->a, &t1, true);
        ty_t b = expr_type(v, n->a->next, &t2, true);
        if (a != TY_STR && a != TY_ANY) verr(v, n, "contains() wants a string, got %s", ty_name(a));
        if (b != TY_STR && b != TY_ANY) verr(v, n, "contains() wants a string, got %s", ty_name(b));
        return TY_BOOL;
    }
    if (strcmp(fn, "json_get") == 0) {
        if (argc != 2) { verr(v, n, "json_get(body, path) needs two arguments"); return TY_ANY; }
        bool t1 = false;
        ty_t a = expr_type(v, n->a, &t1, true);
        if (a != TY_STR && a != TY_ANY) verr(v, n, "json_get() wants a string body, got %s", ty_name(a));
        const char *path = lit_str(n->a->next);
        if (path == NULL) verr(v, n, "json_get() path must be a literal string");
        else if (strlen(path) > JOBS_MAX_JSONPATH) verr(v, n, "json_get() path is too long (max %d)", JOBS_MAX_JSONPATH);
        return TY_ANY;
    }
    verr(v, n, "unknown function '%s'", fn);
    return TY_ANY;
}

/* Type an expression. `as_value` false when the result must be a scalar
 * (not a whole output object). */
static ty_t expr_type(V *v, const jobs_node_t *n, bool *tainted, bool as_value)
{
    if (!n) return TY_NULL;
    switch (n->kind) {
    case JN_EXPR_LIT:
        return val_to_ty(n->u.lit.type);
    case JN_EXPR_STR: {
        for (jobs_node_t *part = n->a; part; part = part->next) {
            if (part->sub == JSP_REF) {
                ty_t t; bool obj = false;
                if (!resolve_ref(v, part, &t, &obj)) return TY_STR;
                if (obj) verr(v, part, "'%s' is an output; use one of its fields", part->u.str.s);
            }
        }
        return TY_STR;
    }
    case JN_EXPR_REF: {
        ty_t t; bool obj = false;
        if (!resolve_ref(v, n, &t, &obj)) return TY_ANY;
        if (as_value && obj) { verr(v, n, "'%s' is an output; use one of its fields", n->u.str.s); return TY_ANY; }
        return t;
    }
    case JN_EXPR_UNARY: {
        bool t = false;
        ty_t a = expr_type(v, n->a, &t, true);
        *tainted = t;
        if (a != TY_BOOL && a != TY_ANY) verr(v, n, "'!' wants a boolean, got %s", ty_name(a));
        return TY_BOOL;
    }
    case JN_EXPR_BINARY: {
        bool ta = false, tb = false;
        ty_t a = expr_type(v, n->a, &ta, true);
        ty_t b = expr_type(v, n->b, &tb, true);
        *tainted = ta || tb;
        if (n->sub == JOP_AND || n->sub == JOP_OR) {
            if ((a != TY_BOOL && a != TY_ANY) || (b != TY_BOOL && b != TY_ANY))
                verr(v, n, "'%s' wants booleans, got %s and %s", jobs_op_name((jobs_op_t)n->sub), ty_name(a), ty_name(b));
            return TY_BOOL;
        }
        bool ordered = n->sub == JOP_LT || n->sub == JOP_LE || n->sub == JOP_GT || n->sub == JOP_GE;
        if (ordered) {
            if (!((is_numeric(a) && is_numeric(b)) || (a == TY_STR && b == TY_STR) || a == TY_ANY || b == TY_ANY))
                verr(v, n, "'%s' cannot compare %s with %s", jobs_op_name((jobs_op_t)n->sub), ty_name(a), ty_name(b));
        } else if (!compatible(a, b) && a != TY_OBJ && b != TY_OBJ) {
            verr(v, n, "'%s' cannot compare %s with %s", jobs_op_name((jobs_op_t)n->sub), ty_name(a), ty_name(b));
        }
        return TY_BOOL;
    }
    case JN_EXPR_CALL:
        return call_type(v, n, tainted);
    default:
        return TY_ANY;
    }
}

static const devos_action_param_t *find_param(const devos_action_descriptor_t *d, const char *name)
{
    for (int i = 0; i < d->param_count; i++)
        if (strcmp(d->params[i].name, name) == 0) return &d->params[i];
    return NULL;
}

static void check_call_args(V *v, const jobs_node_t *action, const devos_action_descriptor_t *d, bool credential_ok)
{
    bool seen[32] = { false };
    for (jobs_node_t *a = action->a; a; a = a->next) {
        const devos_action_param_t *p = find_param(d, a->u.str.s);
        if (!p) { verr(v, a, "unknown argument '%s' for %s", a->u.str.s, d->id); continue; }
        int idx = (int)(p - d->params);
        if (idx >= 0 && idx < 32) {
            if (seen[idx]) { verr(v, a, "duplicate argument '%s'", p->name); continue; }
            seen[idx] = true;
        }
        bool tainted = false;
        ty_t at = expr_type(v, a->a, &tainted, true);
        if (tainted && !p->credential) verr(v, a, "'%s' cannot take a secret", p->name);
        if (p->credential && !tainted) verr(v, a, "'%s' needs secret(\"name\")", p->name);
        if (!p->expression && !p->credential && (a->a->kind == JN_EXPR_REF || a->a->kind == JN_EXPR_CALL))
            verr(v, a, "'%s' must be a literal", p->name);
        ty_t want = val_to_ty(p->type);
        if (p->type == DEVOS_VAL_DURATION) want = TY_DUR;
        if (!compatible(at, want)) verr(v, a, "'%s' wants %s, got %s", p->name, devos_val_type_name(p->type), ty_name(at));
        const char *ls = lit_str(a->a);
        if (ls && p->choices) {
            char choices[128];
            snprintf(choices, sizeof(choices), "%s", p->choices);
            bool ok = false;
            for (char *tok = strtok(choices, "|"); tok; tok = strtok(NULL, "|"))
                if (strcmp(tok, ls) == 0) ok = true;
            if (!ok) verr(v, a, "'%s' must be one of %s", p->name, p->choices);
        }
        if (p->max_len && ls && strlen(ls) > p->max_len)
            verr(v, a, "'%s' is longer than %u bytes", p->name, (unsigned)p->max_len);
        /* Apply declared bounds to any numeric literal the parameter accepts:
         * a DURATION parameter may be given a bare INT/NUM (the runtime reads
         * it as milliseconds, matching the app's parse_dur), and an INT/NUM
         * parameter may be given a duration; the magnitude is the same number
         * either way, so the bounds must not depend on how it was written. */
        if (p->max > p->min) {
            double x;
            if (lit_magnitude(a->a, &x) && (x < p->min || x > p->max)) {
                if (p->type == DEVOS_VAL_DURATION)
                    verr(v, a, "'%s' must be between %g and %g ms", p->name, p->min, p->max);
                else if (p->type == DEVOS_VAL_INT || p->type == DEVOS_VAL_NUM)
                    verr(v, a, "'%s' must be between %g and %g", p->name, p->min, p->max);
            }
        }
    }
    for (int i = 0; i < d->param_count && i < 32; i++) {
        if (d->params[i].required && !seen[i]) verr(v, action, "%s needs '%s'", d->id, d->params[i].name);
    }
    (void)credential_ok;
}

static void check_action(V *v, jobs_node_t *n)
{
    const devos_action_descriptor_t *d = devos_actions_find(n->u.str.s);
    if (!d) {
        /* Reusable job calls use `run "job"(...)`, not an action named call. */
        if (strcmp(n->u.str.s, "call") == 0)
            verr(v, n, "unknown action 'call' (reusable jobs are called with run \"name\"(...))");
        else
            verr(v, n, "unknown action '%s'", n->u.str.s);
        return;
    }
    /* The runtime stack-allocates JOBS_MAX_ARGS values and rejects a wider
     * descriptor as "unknown action" at run time; refuse it up front instead so
     * a definition never validates and then fails every run. */
    if (d->param_count > JOBS_MAX_ARGS) {
        verr(v, n, "%s declares %d arguments, more than the %d the runtime supports",
             d->id, d->param_count, JOBS_MAX_ARGS);
        return;
    }
    check_call_args(v, n, d, true);
    if (n->u.str.s2) {
        if (sym_find(v, n->u.str.s2)) verr(v, n, "'%s' is already defined", n->u.str.s2);
        else if (!sym_add(v, n->u.str.s2, TY_OBJ, true, d))
            verr(v, n, "too many variables (max %d)", JOBS_MAX_VARS);
    }
}

static void check_stmt(V *v, jobs_node_t *n);

static void check_block(V *v, jobs_node_t *b)
{
    push_scope(v);
    for (jobs_node_t *s = b ? b->a : NULL; s && !v->failed; s = s->next) check_stmt(v, s);
    pop_scope(v);
}

static void check_stmt(V *v, jobs_node_t *n)
{
    if (!n) return;
    switch (n->kind) {
    case JN_ACTION:
        check_action(v, n);
        break;
    case JN_SET: {
        bool tainted = false;
        ty_t t = expr_type(v, n->a, &tainted, true);
        if (t == TY_OBJ) verr(v, n, "'set' cannot store a whole output");
        if (tainted) verr(v, n, "'set' cannot store a secret");
        if (is_numeric(t) || t == TY_BOOL || t == TY_STR || t == TY_NULL || t == TY_ANY) {
            sym_t *ex = sym_find(v, n->u.str.s);
            if (ex) {
                if (ex->readonly) verr(v, n, "'%s' is a read-only loop variable", n->u.str.s);
                /* null is the untyped seed: it adopts the first concrete type
                 * assigned (and a concrete variable may be reset to null), like
                 * the runtime's var_set. A change between real types stays an
                 * error. */
                else if (t != TY_NULL && ex->type != TY_NULL && !compatible(t, ex->type))
                    verr(v, n, "'%s' changes type (%s -> %s)", n->u.str.s, ty_name(ex->type), ty_name(t));
                else if (t != TY_NULL && ex->type == TY_NULL)
                    ex->type = t;
            } else if (!sym_add(v, n->u.str.s, t, false, NULL)) {
                verr(v, n, "too many variables (max %d)", JOBS_MAX_VARS);
            }
        }
        break;
    }
    case JN_IF: {
        bool tainted = false;
        ty_t t = expr_type(v, n->a, &tainted, true);
        if (t != TY_BOOL && t != TY_ANY) verr(v, n, "'if' wants a boolean, got %s", ty_name(t));
        check_block(v, n->b);
        if (n->c) check_block(v, n->c);
        break;
    }
    case JN_WAIT:
        if (n->u.i <= 0 || n->u.i > JOBS_MAX_WAIT_MS)
            verr(v, n, "wait must be between 1 ms and %d ms", JOBS_MAX_WAIT_MS);
        break;
    case JN_REPEAT: {
        if (n->count < 1 || n->count > JOBS_MAX_REPEAT)
            verr(v, n, "repeat count must be between 1 and %d", JOBS_MAX_REPEAT);
        if (!n->u.str.s || !n->u.str.s[0]) {
            verr(v, n, "repeat needs a loop variable");
        } else {
            push_scope(v);
            sym_t *s = sym_add(v, n->u.str.s, TY_INT, false, NULL);
            if (!s) verr(v, n, "loop variable '%s' is already defined", n->u.str.s);
            else s->readonly = true;
            for (jobs_node_t *c = n->a ? n->a->a : NULL; c && !v->failed; c = c->next) check_stmt(v, c);
            pop_scope(v);
        }
        break;
    }
    case JN_RUN: {
        if (!n->u.str.s || !n->u.str.s[0]) verr(v, n, "run needs a job name");
        for (jobs_node_t *a = n->a; a; a = a->next) {
            if (a->kind != JN_ARG) continue;
            for (jobs_node_t *b = n->a; b && b != a; b = b->next)
                if (b->kind == JN_ARG && b->u.str.s && a->u.str.s && strcmp(b->u.str.s, a->u.str.s) == 0) {
                    verr(v, a, "duplicate input '%s'", a->u.str.s);
                    break;
                }
            bool tainted = false;
            expr_type(v, a->a, &tainted, true);
        }
        if (n->u.str.s2) {
            if (sym_find(v, n->u.str.s2)) verr(v, n, "'%s' is already defined", n->u.str.s2);
            else if (!sym_add(v, n->u.str.s2, TY_ANY, false, NULL))
                verr(v, n, "too many variables (max %d)", JOBS_MAX_VARS);
        }
        break;
    }
    case JN_RETURN: {
        bool tainted = false;
        ty_t t = expr_type(v, n->a, &tainted, true);
        if (t == TY_OBJ) verr(v, n, "'return' cannot return a whole output");
        if (tainted) verr(v, n, "'return' cannot return a secret");
        break;
    }
    case JN_BLOCK:
        check_block(v, n);
        break;
    default:
        break;
    }
}

static bool valid_hhmm(const char *s)
{
    if (!s || strlen(s) != 5 || s[2] != ':') return false;
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') return false;
    if (s[3] < '0' || s[3] > '9' || s[4] < '0' || s[4] > '9') return false;
    int h = (s[0] - '0') * 10 + (s[1] - '0');
    int m = (s[3] - '0') * 10 + (s[4] - '0');
    return h < 24 && m < 60;
}

static void check_trigger(V *v, jobs_node_t *t)
{
    if (!t) return;
    switch (t->sub) {
    case JTRIG_MANUAL:
        break;
    case JTRIG_EVERY:
        if (t->u.i <= 0) verr(v, t, "interval must be greater than zero");
        break;
    case JTRIG_DAILY:
    case JTRIG_WEEKDAYS:
        if (!valid_hhmm(t->u.str.s)) verr(v, t, "time must be \"HH:MM\" (00:00-23:59)");
        if (t->sub == JTRIG_WEEKDAYS && t->u.str.s2) {
            uint8_t dm = 0;
            if (!jobs_days_parse(t->u.str.s2, &dm))
                verr(v, t, "days must be like \"Mon,Wed,Fri\" (3-letter names, no repeats)");
        }
        break;
    case JTRIG_EVENT: {
        v->in_event = true;
        if (!t->u.str.s || !t->u.str.s[0]) verr(v, t, "an event trigger needs a topic");
        else if (s_topic_check && !s_topic_check(t->u.str.s, s_topic_user)) {
            snprintf(s_topic_advisory, sizeof(s_topic_advisory),
                     "Event topic \"%s\" is not registered, so the trigger never fires.", t->u.str.s);
            s_topic_advisory_set = true;
        }
        for (jobs_node_t *a = t->a; a; a = a->next) {
            static const struct { const char *n; ty_t t; } ea[] = {
                { "topic", TY_STR }, { "include_retained", TY_BOOL }, { "debounce", TY_DUR },
            };
            ty_t want = TY_ANY;
            bool known = false;
            for (size_t i = 0; i < sizeof(ea) / sizeof(ea[0]); i++)
                if (strcmp(ea[i].n, a->u.str.s) == 0) { known = true; want = ea[i].t; }
            if (!known) { verr(v, a, "unknown event argument '%s'", a->u.str.s); continue; }
            bool tainted = false;
            ty_t got = expr_type(v, a->a, &tainted, true);
            if (!compatible(got, want))
                verr(v, a, "'%s' wants %s, got %s", a->u.str.s, ty_name(want), ty_name(got));
            /* jobs_schedule.c read_trigger reads these only from a literal node
             * of the declared type; a variable/expression would silently keep
             * the default, so require a literal here too. */
            else if (!literal_of(a->a, want))
                verr(v, a, "'%s' must be a literal %s", a->u.str.s, ty_name(want));
        }
        if (t->b) {
            bool tainted = false;
            ty_t ty = expr_type(v, t->b, &tainted, true);
            if (ty != TY_BOOL && ty != TY_ANY) verr(v, t, "'where' wants a boolean, got %s", ty_name(ty));
        }
        break;
    }
    default:
        break;
    }
}

static void check_policy(V *v, jobs_node_t *pol)
{
    if (!pol) return;
    for (jobs_node_t *a = pol->a; a; a = a->next) {
        bool tainted = false;
        if (strcmp(a->u.str.s, "timeout") == 0 || strcmp(a->u.str.s, "cooldown") == 0) {
            ty_t t = expr_type(v, a->a, &tainted, true);
            /* The runtime's policy_dur_arg reads only a literal duration, so a
             * bare integer or an expression would silently keep the default. */
            if (t != TY_DUR)
                verr(v, a, "%s wants a duration, got %s", a->u.str.s, ty_name(t));
            else if (!literal_of(a->a, TY_DUR))
                verr(v, a, "%s must be a literal duration", a->u.str.s);
        } else if (strcmp(a->u.str.s, "overlap") == 0) {
            const char *ls = lit_str(a->a);
            if (!ls || (strcmp(ls, "skip") != 0 && strcmp(ls, "queue_one") != 0))
                verr(v, a, "overlap must be \"skip\" or \"queue_one\"");
        } else {
            verr(v, a, "unknown policy '%s'", a->u.str.s);
        }
    }
}

/* ---- worst-case step budget -------------------------------------------- *
 * The interpreter (jobs_runtime.c) charges one step per executed statement and
 * one more when an action completes, against JOBS_MAX_STEPS; when the budget
 * runs out it fails the run *after* any partial side effects. A definition that
 * cannot fit must be refused here instead. This walk mirrors the interpreter's
 * accounting for every construct it can see:
 *   ACTION = 2 (statement + completion); SET/WAIT/RETURN = 1;
 *   IF = 1 + max(then, else); REPEAT = 1 + count x body;
 *   BLOCK = 1 (a bare block is itself an executed statement) + body.
 * A `run` executes a separate definition this validation cannot see, so charge
 * one conservative call chain (JOBS_MAX_CALL_DEPTH frames) - enough to refuse a
 * job whose call structure alone cannot fit, while one ordinary call stays
 * affordable. The total is saturating, so it cannot overflow, and the parser
 * already caps nesting, so the recursion is bounded. */
#define JOBS_RUN_STEP_ALLOWANCE (JOBS_MAX_STEPS / (JOBS_MAX_CALL_DEPTH + 1))
#define JOBS_RUN_STEPS          (1 + JOBS_MAX_CALL_DEPTH * JOBS_RUN_STEP_ALLOWANCE)
#define JOBS_STEP_CAP           (JOBS_MAX_STEPS + 1)   /* any total over budget */

static int64_t steps_add(int64_t a, int64_t b)
{
    if (a >= JOBS_STEP_CAP || b >= JOBS_STEP_CAP) return JOBS_STEP_CAP;
    if (a > JOBS_STEP_CAP - b) return JOBS_STEP_CAP;
    return a + b;
}

static int64_t steps_mul(int64_t a, int64_t b)
{
    if (a <= 0 || b <= 0) return 0;
    if (a >= JOBS_STEP_CAP || b >= JOBS_STEP_CAP) return JOBS_STEP_CAP;
    if (a > JOBS_STEP_CAP / b) return JOBS_STEP_CAP;
    int64_t r = a * b;
    return r > JOBS_STEP_CAP ? JOBS_STEP_CAP : r;
}

static int64_t body_steps(const jobs_node_t *b);

static int64_t stmt_steps(const jobs_node_t *n)
{
    if (!n) return 0;
    switch (n->kind) {
    case JN_ACTION: return 2;
    case JN_SET: case JN_WAIT: case JN_RETURN: return 1;
    case JN_IF: {
        int64_t a = body_steps(n->b), b = body_steps(n->c);
        return steps_add(1, a > b ? a : b);
    }
    case JN_REPEAT: {
        int64_t c = n->count;
        if (c < 0) c = 0;
        if (c > JOBS_MAX_REPEAT) c = JOBS_MAX_REPEAT;
        return steps_add(1, steps_mul(c, body_steps(n->a)));
    }
    case JN_BLOCK: return steps_add(1, body_steps(n));
    case JN_RUN:   return JOBS_RUN_STEPS;
    default:       return 1;
    }
}

static int64_t body_steps(const jobs_node_t *b)
{
    int64_t total = 0;
    for (const jobs_node_t *s = b ? b->a : NULL; s; s = s->next) {
        total = steps_add(total, stmt_steps(s));
        if (total >= JOBS_STEP_CAP) break;   /* already fatal: stop early */
    }
    return total;
}

bool jobs_validate(jobs_ast_t *ast)
{
    if (!ast) return false;
    if (ast->diag_count > 0) return false;
    if (!ast->root) return false;

    s_topic_advisory_set = false;
    V v;
    memset(&v, 0, sizeof(v));
    v.ast = ast;

    jobs_node_t *job = ast->root;
    if (job->kind != JN_JOB) { jobs_diag_add(ast, 0, 0, 0, 0, "no job"); return false; }
    if (!job->u.str.s || !job->u.str.s[0]) jobs_diag_add(ast, job->off, job->len, job->line, job->col, "the job needs a name");
    if (!job->a) jobs_diag_add(ast, job->off, job->len, job->line, job->col, "the job needs a trigger");

    check_trigger(&v, job->a);
    check_policy(&v, job->b);

    /* the job's input parameters are read-only typed locals visible to the body */
    push_scope(&v);
    for (jobs_node_t *p = job->d; p; p = p->next) {
        if (!p->u.str.s || !p->u.str.s[0]) continue;
        if (!sym_add(&v, p->u.str.s, val_to_ty((devos_val_type_t)p->sub), false, NULL))
            verr(&v, p, "duplicate parameter '%s'", p->u.str.s);
        else sym_find(&v, p->u.str.s)->readonly = true;
    }
    check_block(&v, job->c);
    pop_scope(&v);

    /* Fail a body whose worst case cannot fit before it is ever installed: the
     * runtime would otherwise run part of it and only then die "step budget
     * exceeded", after partial side effects. */
    if (body_steps(job->c) > JOBS_MAX_STEPS)
        verr(&v, job->c ? job->c : job,
             "worst case exceeds the %d-step run budget: reduce repeat counts or nesting", JOBS_MAX_STEPS);

    return ast->diag_count == 0;
}
