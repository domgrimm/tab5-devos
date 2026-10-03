/* jobs_serialize: canonical source emitter for the Jobs AST. The Builder
 * patches spans; this whole-file emitter is the explicit "Format source"
 * operation and the round-trip oracle (parse(serialize(AST)) is equivalent).
 * No LVGL/network. */
#include "jobs_model.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *out;
    size_t cap, len;
} SB;

static void sb_put(SB *b, const char *s, size_t n)
{
    if (b->len < b->cap) {
        size_t room = b->cap - b->len;
        memcpy(b->out + b->len, s, n < room ? n : room);
    }
    b->len += n;
}

static void sb_fmt(SB *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    sb_put(b, tmp, (size_t)n);
}

static void ind(SB *b, int d) { for (int i = 0; i < d; i++) sb_put(b, "    ", 4); }

static void emit_dur(SB *b, int64_t ms)
{
    if (ms != 0 && ms % 3600000 == 0) sb_fmt(b, "%lldh", (long long)(ms / 3600000));
    else if (ms != 0 && ms % 60000 == 0) sb_fmt(b, "%lldm", (long long)(ms / 60000));
    else if (ms != 0 && ms % 1000 == 0) sb_fmt(b, "%llds", (long long)(ms / 1000));
    else sb_fmt(b, "%lldms", (long long)ms);
}

static void emit_quoted(SB *b, const char *s)
{
    sb_put(b, "\"", 1);
    for (const char *p = s; p && *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  sb_put(b, "\\\"", 2); break;
        case '\\': sb_put(b, "\\\\", 2); break;
        case '\n': sb_put(b, "\\n", 2); break;
        case '\t': sb_put(b, "\\t", 2); break;
        case '\r': sb_put(b, "\\r", 2); break;
        default:   sb_put(b, (const char *)&c, 1); break;
        }
    }
    sb_put(b, "\"", 1);
}

static void emit_expr(SB *b, const jobs_node_t *n);
static void emit_block(SB *b, const jobs_node_t *block, int d);

static void emit_str_node(SB *b, const jobs_node_t *n)
{
    sb_put(b, "\"", 1);
    for (const jobs_node_t *part = n->a; part; part = part->next) {
        if (part->sub == JSP_REF) {
            sb_fmt(b, "${%s}", part->u.str.s);
        } else {
            for (const char *p = part->u.str.s; p && *p; p++) {
                unsigned char c = (unsigned char)*p;
                switch (c) {
                case '"':  sb_put(b, "\\\"", 2); break;
                case '\\': sb_put(b, "\\\\", 2); break;
                case '\n': sb_put(b, "\\n", 2); break;
                case '\t': sb_put(b, "\\t", 2); break;
                case '\r': sb_put(b, "\\r", 2); break;
                default:   sb_put(b, (const char *)&c, 1); break;
                }
            }
        }
    }
    sb_put(b, "\"", 1);
}

static void emit_args(SB *b, const jobs_node_t *arg, bool named)
{
    sb_put(b, "(", 1);
    for (const jobs_node_t *a = arg; a; a = a->next) {
        if (named && a->kind == JN_ARG) {
            sb_fmt(b, "%s: ", a->u.str.s);
            emit_expr(b, a->a);
        } else {
            emit_expr(b, a);
        }
        if (a->next) sb_put(b, ", ", 2);
    }
    sb_put(b, ")", 1);
}

static void emit_expr(SB *b, const jobs_node_t *n)
{
    if (!n) { sb_put(b, "null", 4); return; }
    switch (n->kind) {
    case JN_EXPR_LIT:
        switch (n->u.lit.type) {
        case DEVOS_VAL_NULL: sb_put(b, "null", 4); break;
        case DEVOS_VAL_BOOL: sb_put(b, n->u.lit.v.b ? "true" : "false", n->u.lit.v.b ? 4 : 5); break;
        case DEVOS_VAL_INT:  sb_fmt(b, "%lld", (long long)n->u.lit.v.i); break;
        case DEVOS_VAL_NUM:  sb_fmt(b, "%.17g", n->u.lit.v.n); break;
        case DEVOS_VAL_DURATION: emit_dur(b, n->u.lit.v.ms); break;
        case DEVOS_VAL_STR:  emit_quoted(b, n->u.lit.v.str.s ? n->u.lit.v.str.s : ""); break;
        default: sb_put(b, "null", 4); break;
        }
        break;
    case JN_EXPR_STR: emit_str_node(b, n); break;
    case JN_EXPR_REF: sb_put(b, n->u.str.s, strlen(n->u.str.s)); break;
    case JN_EXPR_UNARY:
        sb_put(b, "!", 1);
        if (n->a && n->a->kind == JN_EXPR_BINARY) { sb_put(b, "(", 1); emit_expr(b, n->a); sb_put(b, ")", 1); }
        else emit_expr(b, n->a);
        break;
    case JN_EXPR_BINARY:
        sb_put(b, "(", 1);
        emit_expr(b, n->a);
        sb_fmt(b, " %s ", jobs_op_name((jobs_op_t)n->sub));
        emit_expr(b, n->b);
        sb_put(b, ")", 1);
        break;
    case JN_EXPR_CALL:
        sb_put(b, n->u.str.s, strlen(n->u.str.s));
        emit_args(b, n->a, false);
        break;
    default:
        sb_put(b, "null", 4);
        break;
    }
}

static void emit_stmt(SB *b, const jobs_node_t *s, int d)
{
    switch (s->kind) {
    case JN_ACTION:
        ind(b, d);
        sb_put(b, s->u.str.s, strlen(s->u.str.s));
        emit_args(b, s->a, true);
        if (s->u.str.s2) sb_fmt(b, " as %s", s->u.str.s2);
        sb_put(b, ";\n", 2);
        break;
    case JN_SET:
        ind(b, d);
        sb_fmt(b, "set %s = ", s->u.str.s);
        emit_expr(b, s->a);
        sb_put(b, ";\n", 2);
        break;
    case JN_WAIT:
        ind(b, d);
        sb_put(b, "wait ", 5);
        emit_dur(b, s->u.i);
        sb_put(b, ";\n", 2);
        break;
    case JN_IF:
        ind(b, d);
        sb_put(b, "if ", 3);
        emit_expr(b, s->a);
        sb_put(b, " ", 1);
        emit_block(b, s->b, d);
        if (s->c) { ind(b, d); sb_put(b, "else ", 5); emit_block(b, s->c, d); }
        break;
    case JN_REPEAT:
        ind(b, d);
        sb_fmt(b, "repeat %lld as %s ", (long long)s->u.i, s->u.str.s);
        emit_block(b, s->a, d);
        break;
    case JN_BLOCK:
        emit_block(b, s, d);
        break;
    default:
        break;
    }
}

static void emit_block(SB *b, const jobs_node_t *block, int d)
{
    sb_put(b, "{\n", 2);
    for (const jobs_node_t *s = block ? block->a : NULL; s; s = s->next) emit_stmt(b, s, d + 1);
    ind(b, d);
    sb_put(b, "}\n", 2);
}

static void emit_trigger(SB *b, const jobs_node_t *t, int d)
{
    ind(b, d);
    sb_fmt(b, "trigger %s", jobs_trigger_name((jobs_trigger_kind_t)t->sub));
    switch (t->sub) {
    case JTRIG_EVERY: sb_put(b, " ", 1); emit_dur(b, t->u.i); break;
    case JTRIG_DAILY:
    case JTRIG_WEEKDAYS: sb_put(b, " ", 1); emit_quoted(b, t->u.str.s); break;
    case JTRIG_EVENT:
        sb_put(b, " ", 1);
        emit_quoted(b, t->u.str.s);
        if (t->a) emit_args(b, t->a, true);
        if (t->b) { sb_put(b, " where ", 7); emit_expr(b, t->b); }
        break;
    default: break;
    }
    sb_put(b, ";\n", 2);
}

static void emit_policy(SB *b, const jobs_node_t *p, int d)
{
    if (!p) return;
    ind(b, d);
    sb_put(b, "policy", 6);
    emit_args(b, p->a, true);
    sb_put(b, ";\n", 2);
}

size_t jobs_serialize(const jobs_ast_t *ast, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    SB b = { out, cap, 0 };
    if (!ast || !ast->root) { out[0] = '\0'; return 0; }
    const jobs_node_t *job = ast->root;
    sb_fmt(&b, "version %d;\n", JOBS_LANG_VERSION);
    sb_put(&b, "job ", 4);
    emit_quoted(&b, job->u.str.s ? job->u.str.s : "");
    sb_put(&b, " {\n", 3);
    emit_trigger(&b, job->a, 1);
    emit_policy(&b, job->b, 1);
    for (const jobs_node_t *s = job->c ? job->c->a : NULL; s; s = s->next) emit_stmt(&b, s, 1);
    sb_put(&b, "}\n", 2);
    if (b.len < b.cap) b.out[b.len] = '\0';
    else b.out[b.cap - 1] = '\0';
    return b.len;
}

size_t jobs_serialize_expr(const jobs_node_t *e, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    SB b = { out, cap, 0 };
    emit_expr(&b, e);
    if (b.len < b.cap) b.out[b.len] = '\0';
    else b.out[b.cap - 1] = '\0';
    return b.len;
}
