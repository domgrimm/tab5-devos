/* jobs_build: see jobs_build.h. */
#include "jobs_build.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool set_arg_lit(jobs_build_t *b, const jobs_node_t *action, const char *param, jobs_node_t *lit);

static const jobs_node_t *find_arg(const jobs_node_t *action, const char *param)
{
    for (const jobs_node_t *a = action->a; a; a = a->next)
        if (a->kind == JN_ARG && strcmp(a->u.str.s, param) == 0) return a;
    return NULL;
}

static jobs_node_t *arg_get_or_add(jobs_ast_t *ast, const jobs_node_t *action, const char *param)
{
    jobs_node_t *act = (jobs_node_t *)action;
    for (jobs_node_t *a = act->a; a; a = a->next)
        if (a->kind == JN_ARG && strcmp(a->u.str.s, param) == 0) return a;
    jobs_node_t *arg = jobs_node_new(ast, JN_ARG, 0, 0, 0, 0);
    if (!arg) return NULL;
    arg->u.str.s = jobs_pool_str(ast, param, (uint32_t)strlen(param));
    if (!arg->u.str.s) return NULL;
    if (!act->a) act->a = arg;
    else { jobs_node_t *t = act->a; while (t->next) t = t->next; t->next = arg; }
    return arg;
}

static jobs_node_t *new_lit(jobs_ast_t *ast, devos_val_type_t type)
{
    jobs_node_t *n = jobs_node_new(ast, JN_EXPR_LIT, 0, 0, 0, 0);
    if (n) n->u.lit.type = type;
    return n;
}

/* A plain string literal, as JN_EXPR_STR with one literal part (the same shape
 * the parser builds), so the validator's literal checks see it. */
static jobs_node_t *new_str_lit(jobs_ast_t *ast, const char *text)
{
    jobs_node_t *n = jobs_node_new(ast, JN_EXPR_STR, 0, 0, 0, 0);
    if (!n) return NULL;
    jobs_node_t *part = jobs_node_new(ast, JN_STRPART, 0, 0, 0, 0);
    if (!part) return NULL;
    part->sub = JSP_LITERAL;
    part->u.str.s = jobs_pool_str(ast, text ? text : "", (uint32_t)strlen(text ? text : ""));
    if (!part->u.str.s) return NULL;
    n->a = part;
    return n;
}

/* ---- load / serialize ---- */
bool jobs_build_load(jobs_build_t *b, const char *source, size_t len)
{
    if (!b) return false;
    if (b->ast) { jobs_ast_free(b->ast); b->ast = NULL; }
    b->diag[0] = '\0';
    b->ast = jobs_parse(source, len, NULL);
    if (!b->ast) { snprintf(b->diag, sizeof(b->diag), "out of memory"); return false; }
    if (!jobs_validate(b->ast)) {
        snprintf(b->diag, sizeof(b->diag), "%s", b->ast->diag_count ? b->ast->diag[0].msg : "invalid definition");
        return false;
    }
    return true;
}

void jobs_build_free(jobs_build_t *b)
{
    if (b && b->ast) { jobs_ast_free(b->ast); b->ast = NULL; }
}

bool jobs_build_revalidate(jobs_build_t *b)
{
    if (!b || !b->ast) return false;
    b->ast->diag_count = 0;
    b->diag[0] = '\0';
    if (!jobs_validate(b->ast)) {
        snprintf(b->diag, sizeof(b->diag), "%s", b->ast->diag_count ? b->ast->diag[0].msg : "invalid definition");
        return false;
    }
    return true;
}

size_t jobs_build_source(jobs_build_t *b, char *out, size_t cap)
{
    if (!b || !b->ast) { if (out && cap) out[0] = '\0'; return 0; }
    return jobs_serialize(b->ast, out, cap);
}

/* ---- rows ---- */
static void add_rows(const jobs_node_t *block, int depth, const char *branch,
                     jobs_build_row_t *out, int max, int *n)
{
    if (!block) return;
    int idx = 0;
    for (const jobs_node_t *s = block->a; s && *n < max; s = s->next, idx++) {
        jobs_build_row_t *r = &out[(*n)++];
        r->node = s;
        r->block = block;
        r->index = idx;
        r->depth = depth;
        r->branch = branch ? branch : "";
        if (s->kind == JN_IF) {
            add_rows(s->b, depth + 1, "if", out, max, n);
            if (s->c) add_rows(s->c, depth + 1, "else", out, max, n);
        }
    }
}

int jobs_build_rows(const jobs_build_t *b, jobs_build_row_t *out, int max)
{
    if (!b || !b->ast || !b->ast->root || !out || max <= 0) return 0;
    int n = 0;
    add_rows(b->ast->root->c, 0, "", out, max, &n);
    return n;
}

/* ---- trigger ---- */
jobs_node_t *jobs_build_trigger(jobs_build_t *b)
{
    return (b && b->ast && b->ast->root) ? b->ast->root->a : NULL;
}

bool jobs_build_set_trigger_kind(jobs_build_t *b, int kind)
{
    jobs_node_t *t = jobs_build_trigger(b);
    if (!t) return false;
    if (kind != JTRIG_MANUAL && kind != JTRIG_EVERY && kind != JTRIG_DAILY &&
        kind != JTRIG_WEEKDAYS && kind != JTRIG_EVENT)
        return false;
    int old = t->sub;
    t->sub = (uint8_t)kind;
    if (kind == JTRIG_EVERY) {
        if (old != JTRIG_EVERY || t->u.i <= 0) t->u.i = 60000;
    } else if (kind == JTRIG_DAILY || kind == JTRIG_WEEKDAYS) {
        /* only read the old string when the old kind also stored one */
        const char *keep = (old == JTRIG_DAILY || old == JTRIG_WEEKDAYS) ? t->u.str.s : NULL;
        if (!keep || !keep[0]) t->u.str.s = jobs_pool_str(b->ast, "08:00", 5);
    } else if (kind == JTRIG_EVENT) {
        const char *keep = (old == JTRIG_EVENT) ? t->u.str.s : NULL;
        if (!keep || !keep[0]) t->u.str.s = jobs_pool_str(b->ast, "system.boot", 11);
    }
    return true;
}

bool jobs_build_set_trigger_event(jobs_build_t *b, const char *topic)
{
    jobs_node_t *t = jobs_build_trigger(b);
    if (!t || t->sub != JTRIG_EVENT || !topic || !topic[0]) return false;
    const char *copy = jobs_pool_str(b->ast, topic, (uint32_t)strlen(topic));
    if (!copy) return false;
    t->u.str.s = copy;
    return true;
}

bool jobs_build_set_trigger_where(jobs_build_t *b, const char *expr)
{
    jobs_node_t *t = jobs_build_trigger(b);
    if (!t || t->sub != JTRIG_EVENT) return false;
    if (!expr || !expr[0]) { t->b = NULL; return true; }   /* clear the filter */
    const char *err = NULL;
    jobs_node_t *e = jobs_parse_expr(expr, strlen(expr), b->ast, &err);
    if (!e) { snprintf(b->diag, sizeof(b->diag), "%s", err ? err : "invalid expression"); return false; }
    t->b = e;
    return true;
}

const char *jobs_build_trigger_event_topic(const jobs_node_t *t)
{
    return (t && t->sub == JTRIG_EVENT) ? t->u.str.s : NULL;
}

const char *jobs_build_trigger_where_text(const jobs_node_t *t)
{
    static char buf[192];
    buf[0] = '\0';
    if (t && t->sub == JTRIG_EVENT && t->b) jobs_serialize_expr(t->b, buf, sizeof(buf));
    return buf;
}

bool jobs_build_set_trigger_duration(jobs_build_t *b, int64_t ms)
{
    jobs_node_t *t = jobs_build_trigger(b);
    if (!t || t->sub != JTRIG_EVERY || ms <= 0) return false;
    t->u.i = ms;
    return true;
}

bool jobs_build_set_trigger_time(jobs_build_t *b, const char *hhmm)
{
    jobs_node_t *t = jobs_build_trigger(b);
    if (!t || (t->sub != JTRIG_DAILY && t->sub != JTRIG_WEEKDAYS) || !hhmm) return false;
    const char *copy = jobs_pool_str(b->ast, hhmm, (uint32_t)strlen(hhmm));
    if (!copy) return false;
    t->u.str.s = copy;
    return true;
}

/* ---- steps ---- */
static bool block_append(jobs_ast_t *ast, jobs_node_t *block, jobs_node_t *n)
{
    if (!block || block->kind != JN_BLOCK || !n) return false;
    if (!block->a) block->a = n;
    else { jobs_node_t *t = block->a; while (t->next) t = t->next; t->next = n; }
    (void)ast;
    return true;
}

bool jobs_build_add_action(jobs_build_t *b, const jobs_node_t *block, const char *action_id)
{
    if (!b || !b->ast || !block || !action_id) return false;
    const devos_action_descriptor_t *d = devos_actions_find(action_id);
    if (!d) return false;
    jobs_node_t *n = jobs_node_new(b->ast, JN_ACTION, 0, 0, 0, 0);
    if (!n) return false;
    n->u.str.s = jobs_pool_str(b->ast, action_id, (uint32_t)strlen(action_id));
    if (!n->u.str.s) return false;

    jobs_node_t *tail = NULL;
    for (int i = 0; i < d->param_count; i++) {
        const devos_action_param_t *p = &d->params[i];
        if (p->credential) continue;                 /* omitted: use Text for secrets */
        jobs_node_t *arg = jobs_node_new(b->ast, JN_ARG, 0, 0, 0, 0);
        if (!arg) return false;
        arg->u.str.s = jobs_pool_str(b->ast, p->name, (uint32_t)strlen(p->name));
        if (!arg->u.str.s) return false;
        jobs_node_t *lit;
        if (p->type == DEVOS_VAL_STR) {
            const char *val = "";
            if (p->choices) { static char c[64]; snprintf(c, sizeof(c), "%s", p->choices);
                              char *bar = strchr(c, '|'); if (bar) *bar = '\0'; val = c; }
            else if (p->def) val = p->def;
            lit = new_str_lit(b->ast, val);
        } else {
            lit = new_lit(b->ast, p->type);
            if (lit) {
                switch (p->type) {
                case DEVOS_VAL_BOOL: lit->u.lit.v.b = false; break;
                case DEVOS_VAL_INT:  lit->u.lit.v.i = (p->max > p->min) ? (int64_t)p->min : 0; break;
                case DEVOS_VAL_NUM:  lit->u.lit.v.n = (p->max > p->min) ? p->min : 0; break;
                case DEVOS_VAL_DURATION: lit->u.lit.v.ms = (p->max > p->min) ? (int64_t)p->min : 0; break;
                default: break;
                }
            }
        }
        if (!lit) return false;
        arg->a = lit;
        if (tail) tail->next = arg; else n->a = arg;
        tail = arg;
    }
    return block_append(b->ast, (jobs_node_t *)block, n);
}

bool jobs_build_delete(jobs_build_t *b, const jobs_node_t *block, const jobs_node_t *stmt)
{
    (void)b;
    if (!block || !stmt) return false;
    jobs_node_t *prev = NULL;
    for (jobs_node_t *s = block->a; s; prev = s, s = s->next) {
        if (s == stmt) {
            if (prev) prev->next = s->next;
            else ((jobs_node_t *)block)->a = s->next;
            return true;
        }
    }
    return false;
}

bool jobs_build_move(jobs_build_t *b, const jobs_node_t *block, const jobs_node_t *stmt, int dir)
{
    (void)b;
    if (!block || !stmt) return false;
    jobs_node_t *blk = (jobs_node_t *)block;
    jobs_node_t *prev = NULL, *cur = blk->a;
    while (cur && cur != stmt) { prev = cur; cur = cur->next; }
    if (!cur) return false;
    if (dir < 0) {
        if (!prev) return false;
        jobs_node_t *pp = NULL, *p = blk->a;
        while (p && p != prev) { pp = p; p = p->next; }
        jobs_node_t *after = cur->next;
        if (pp) pp->next = cur; else blk->a = cur;
        cur->next = prev;
        prev->next = after;
    } else {
        jobs_node_t *next = cur->next;
        if (!next) return false;
        jobs_node_t *after = next->next;
        if (prev) prev->next = next; else blk->a = next;
        next->next = cur;
        cur->next = after;
    }
    return true;
}

bool jobs_build_set_wait(jobs_build_t *b, const jobs_node_t *stmt, int64_t ms)
{
    (void)b;
    if (!stmt || stmt->kind != JN_WAIT || ms <= 0) return false;
    ((jobs_node_t *)stmt)->u.i = ms;
    return true;
}

int jobs_build_block_count(const jobs_node_t *block)
{
    int n = 0;
    for (const jobs_node_t *s = block ? block->a : NULL; s; s = s->next) n++;
    return n;
}

bool jobs_build_move_to(jobs_build_t *b, const jobs_node_t *block, const jobs_node_t *stmt, int target)
{
    if (!block || !stmt) return false;
    int count = jobs_build_block_count(block);
    if (target < 0) target = 0;
    if (target >= count) target = count - 1;
    int idx = 0;
    for (const jobs_node_t *s = block->a; s && s != stmt; s = s->next) idx++;
    while (idx < target) { if (!jobs_build_move(b, block, stmt, 1)) return false; idx++; }
    while (idx > target) { if (!jobs_build_move(b, block, stmt, -1)) return false; idx--; }
    return true;
}

/* ---- conditions and advanced expressions ---- */
const jobs_node_t *jobs_build_if_cond(const jobs_node_t *if_node) { return if_node ? if_node->a : NULL; }
const jobs_node_t *jobs_build_set_value(const jobs_node_t *set_node) { return set_node ? set_node->a : NULL; }
const jobs_node_t *jobs_build_arg_expr(const jobs_node_t *action, const char *param)
{
    const jobs_node_t *a = find_arg(action, param);
    return a ? a->a : NULL;
}

static jobs_node_t *parse_expr_into(jobs_build_t *b, const char *text, const char **err)
{
    return jobs_parse_expr(text ? text : "", strlen(text ? text : ""), b->ast, err);
}

bool jobs_build_set_if_cond(jobs_build_t *b, const jobs_node_t *if_node, const char *text)
{
    if (!b || !if_node || if_node->kind != JN_IF) return false;
    const char *err = NULL;
    jobs_node_t *e = parse_expr_into(b, text, &err);
    if (!e) { snprintf(b->diag, sizeof(b->diag), "%s", err ? err : "bad condition"); return false; }
    ((jobs_node_t *)if_node)->a = e;
    return true;
}

bool jobs_build_set_set_value(jobs_build_t *b, const jobs_node_t *set_node, const char *text)
{
    if (!b || !set_node || set_node->kind != JN_SET) return false;
    const char *err = NULL;
    jobs_node_t *e = parse_expr_into(b, text, &err);
    if (!e) { snprintf(b->diag, sizeof(b->diag), "%s", err ? err : "bad value"); return false; }
    ((jobs_node_t *)set_node)->a = e;
    return true;
}

bool jobs_build_set_arg_expr(jobs_build_t *b, const jobs_node_t *action, const char *param, const char *text)
{
    if (!b || !action) return false;
    const char *err = NULL;
    jobs_node_t *e = parse_expr_into(b, text, &err);
    if (!e) { snprintf(b->diag, sizeof(b->diag), "%s", err ? err : "bad expression"); return false; }
    return set_arg_lit(b, action, param, e);
}

const char *jobs_build_expr_text(const jobs_node_t *e)
{
    static char buf[256];
    jobs_serialize_expr(e, buf, sizeof(buf));
    return buf;
}

bool jobs_build_arg_is_expr(const jobs_node_t *action, const char *param)
{
    const jobs_node_t *a = find_arg(action, param);
    if (!a || !a->a) return false;
    const jobs_node_t *e = a->a;
    if (e->kind == JN_EXPR_LIT) return false;
    if (e->kind == JN_EXPR_STR) return false;
    if (e->kind == JN_EXPR_REF) return false;
    return true;
}

/* ---- inspector ---- */
bool jobs_build_arg_is_simple(const jobs_node_t *action, const char *param)
{
    const jobs_node_t *a = find_arg(action, param);
    if (!a || !a->a) return true;
    const jobs_node_t *e = a->a;
    if (e->kind == JN_EXPR_LIT) return true;
    if (e->kind == JN_EXPR_STR) return e->a == NULL || (e->a && !e->a->next && e->a->sub == JSP_LITERAL);
    return false;
}

const char *jobs_build_arg_text(const jobs_node_t *action, const char *param)
{
    static char buf[64];
    buf[0] = '\0';
    const jobs_node_t *a = find_arg(action, param);
    if (!a || !a->a) return buf;
    const jobs_node_t *e = a->a;
    if (e->kind == JN_EXPR_STR && e->a && !e->a->next && e->a->sub == JSP_LITERAL) return e->a->u.str.s;
    if (e->kind == JN_EXPR_LIT) {
        switch (e->u.lit.type) {
        case DEVOS_VAL_BOOL: snprintf(buf, sizeof(buf), "%s", e->u.lit.v.b ? "true" : "false"); break;
        case DEVOS_VAL_INT: snprintf(buf, sizeof(buf), "%lld", (long long)e->u.lit.v.i); break;
        case DEVOS_VAL_NUM: snprintf(buf, sizeof(buf), "%g", e->u.lit.v.n); break;
        case DEVOS_VAL_DURATION: snprintf(buf, sizeof(buf), "%lld", (long long)e->u.lit.v.ms); break;
        case DEVOS_VAL_STR: return e->u.lit.v.str.s ? e->u.lit.v.str.s : "";
        default: break;
        }
    }
    return buf;
}

int64_t jobs_build_arg_duration(const jobs_node_t *action, const char *param)
{
    const jobs_node_t *a = find_arg(action, param);
    if (a && a->a && a->a->kind == JN_EXPR_LIT && a->a->u.lit.type == DEVOS_VAL_DURATION) return a->a->u.lit.v.ms;
    return -1;
}

bool jobs_build_arg_bool(const jobs_node_t *action, const char *param, bool *v)
{
    const jobs_node_t *a = find_arg(action, param);
    if (a && a->a && a->a->kind == JN_EXPR_LIT && a->a->u.lit.type == DEVOS_VAL_BOOL) { if (v) *v = a->a->u.lit.v.b; return true; }
    return false;
}

static bool set_arg_lit(jobs_build_t *b, const jobs_node_t *action, const char *param, jobs_node_t *lit)
{
    if (!b || !b->ast || !action || !lit) return false;
    jobs_node_t *arg = arg_get_or_add(b->ast, action, param);
    if (!arg) return false;
    arg->a = lit;
    return true;
}

bool jobs_build_set_arg_str(jobs_build_t *b, const jobs_node_t *action, const char *param, const char *text)
{
    jobs_node_t *lit = new_str_lit(b->ast, text);
    if (!lit) return false;
    return set_arg_lit(b, action, param, lit);
}

bool jobs_build_set_arg_bool(jobs_build_t *b, const jobs_node_t *action, const char *param, bool v)
{
    jobs_node_t *lit = new_lit(b->ast, DEVOS_VAL_BOOL);
    if (!lit) return false;
    lit->u.lit.v.b = v;
    return set_arg_lit(b, action, param, lit);
}

bool jobs_build_set_arg_int(jobs_build_t *b, const jobs_node_t *action, const char *param, int64_t v)
{
    jobs_node_t *lit = new_lit(b->ast, DEVOS_VAL_INT);
    if (!lit) return false;
    lit->u.lit.v.i = v;
    return set_arg_lit(b, action, param, lit);
}

bool jobs_build_set_arg_duration(jobs_build_t *b, const jobs_node_t *action, const char *param, int64_t ms)
{
    jobs_node_t *lit = new_lit(b->ast, DEVOS_VAL_DURATION);
    if (!lit) return false;
    lit->u.lit.v.ms = ms;
    return set_arg_lit(b, action, param, lit);
}

bool jobs_build_set_arg_secret(jobs_build_t *b, const jobs_node_t *action, const char *param, const char *name)
{
    jobs_node_t *call = jobs_node_new(b->ast, JN_EXPR_CALL, 0, 0, 0, 0);
    if (!call) return false;
    call->u.str.s = jobs_pool_str(b->ast, "secret", 6);
    jobs_node_t *lit = new_str_lit(b->ast, name);
    if (!lit) return false;
    call->a = lit;
    return set_arg_lit(b, action, param, call);
}

const char *jobs_build_arg_secret_name(const jobs_node_t *action, const char *param)
{
    const jobs_node_t *a = find_arg(action, param);
    if (!a || !a->a) return NULL;
    const jobs_node_t *e = a->a;
    if (e->kind != JN_EXPR_CALL || !e->u.str.s || strcmp(e->u.str.s, "secret") != 0) return NULL;
    const jobs_node_t *lit = e->a;
    if (lit && lit->kind == JN_EXPR_STR && lit->a && !lit->a->next && lit->a->sub == JSP_LITERAL)
        return lit->a->u.str.s;
    if (lit && lit->kind == JN_EXPR_LIT && lit->u.lit.type == DEVOS_VAL_STR) return lit->u.lit.v.str.s;
    return NULL;
}
