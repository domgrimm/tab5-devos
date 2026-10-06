/* jobs_parse: lexer and recursive-descent/Pratt parser for the Jobs text
 * language v1 (PLAN.md section 6). No LVGL, no network, no devos_core.h.
 *
 * Every diagnostic carries a byte span and line/column. A candidate with any
 * diagnostic never becomes executable: jobs_parse always returns an AST (so
 * the UI can show errors), but jobs_validate()/jobs_failed() gate execution. */
#include "jobs_model.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    T_EOF = 0, T_IDENT, T_NUMBER, T_DURATION, T_STRING,
    T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN, T_SEMI, T_COMMA, T_COLON, T_ASSIGN, T_DOT,
    T_NOT, T_AND, T_OR, T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
};

typedef struct {
    int kind;
    const char *s; uint32_t slen;   /* identifier text (into source) */
    int64_t i;                       /* integer literal or duration ms */
    double n;                        /* numeric literal */
    bool is_float;
    uint32_t off, len;
    int line, col;
} tok_t;

typedef struct {
    const char *src;
    size_t len, pos;
    int line, col;
    tok_t cur;
    jobs_ast_t *ast;
    int depth;
    bool err;
} P;

/* ---------------------------------------------------------------- lexer */
static int pk(P *p) { return p->pos < p->len ? (unsigned char)p->src[p->pos] : -1; }
static int pk2(P *p) { return p->pos + 1 < p->len ? (unsigned char)p->src[p->pos + 1] : -1; }
static int gch(P *p)
{
    if (p->pos >= p->len) return -1;
    int c = (unsigned char)p->src[p->pos++];
    if (c == '\n') { p->line++; p->col = 1; } else { p->col++; }
    return c;
}

static bool ident_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool ident_cont(int c) { return ident_start(c) || (c >= '0' && c <= '9'); }

static void perr(P *p, const tok_t *t, const char *fmt, ...)
{
    if (p->err) return;
    p->err = true;
    va_list ap;
    va_start(ap, fmt);
    char msg[120];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    jobs_diag_add(p->ast, t->off, t->len, t->line, t->col, "%s", msg);
}

static void next(P *p)
{
    /* skip whitespace and // comments */
    for (;;) {
        int c = pk(p);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { gch(p); continue; }
        if (c == '/' && pk2(p) == '/') {
            while (pk(p) != -1 && pk(p) != '\n') gch(p);
            continue;
        }
        break;
    }
    tok_t *t = &p->cur;
    memset(t, 0, sizeof(*t));
    t->line = p->line;
    t->col = p->col;
    t->off = (uint32_t)p->pos;

    int c = pk(p);
    if (c == -1) { t->kind = T_EOF; t->len = 0; return; }

    if (ident_start(c)) {
        uint32_t start = (uint32_t)p->pos;
        while (ident_cont(pk(p))) gch(p);
        t->kind = T_IDENT;
        t->s = p->src + start;
        t->slen = (uint32_t)p->pos - start;
        t->len = t->slen;
        return;
    }

    if (c >= '0' && c <= '9') {
        uint32_t start = (uint32_t)p->pos;
        while (pk(p) >= '0' && pk(p) <= '9') gch(p);
        bool isf = false;
        if (pk(p) == '.' && pk2(p) >= '0' && pk2(p) <= '9') {
            isf = true;
            gch(p);
            while (pk(p) >= '0' && pk(p) <= '9') gch(p);
        }
        /* duration unit, only when it ends the token (no trailing ident char) */
        int u = pk(p);
        if (!isf && (u == 'm' || u == 's' || u == 'h')) {
            int64_t unit_ms = 0;
            size_t adv = 1;
            if (u == 'm' && pk2(p) == 's') { unit_ms = 1; adv = 2; }
            else if (u == 'm') unit_ms = 60 * 1000;
            else if (u == 's') unit_ms = 1000;
            else if (u == 'h') unit_ms = 3600 * 1000;
            size_t after = p->pos + adv;
            int nc = after < p->len ? (unsigned char)p->src[after] : -1;
            if (unit_ms && !ident_cont(nc)) {
                char numbuf[24];
                uint32_t nl = (uint32_t)p->pos - start;
                if (nl < sizeof(numbuf)) {
                    memcpy(numbuf, p->src + start, nl);
                    numbuf[nl] = '\0';
                    long long v = strtoll(numbuf, NULL, 10);
                    /* Saturated values overflow the run deadline (now + tmo) into
                     * the past, so the job would fail instantly as "timed out". */
                    if (v < 0 || v > INT64_MAX / unit_ms) {
                        perr(p, t, "duration is too large");
                        return;
                    }
                    for (size_t k = 0; k < adv; k++) gch(p);
                    t->kind = T_DURATION;
                    t->i = v * unit_ms;
                    t->len = (uint32_t)p->pos - start;
                    return;
                }
            }
        }
        char numbuf[32];
        uint32_t nl = (uint32_t)p->pos - start;
        if (nl >= sizeof(numbuf)) nl = sizeof(numbuf) - 1;
        memcpy(numbuf, p->src + start, nl);
        numbuf[nl] = '\0';
        t->len = (uint32_t)p->pos - start;
        if (isf) { t->kind = T_NUMBER; t->n = strtod(numbuf, NULL); t->is_float = true; }
        else { t->kind = T_NUMBER; t->i = strtoll(numbuf, NULL, 10); }
        return;
    }

    if (c == '"') {
        uint32_t start = (uint32_t)p->pos;
        gch(p);                                   /* opening quote */
        for (;;) {
            int d = pk(p);
            if (d == -1) { perr(p, t, "unterminated string"); t->kind = T_STRING; t->len = (uint32_t)p->pos - start; return; }
            if (d == '\\') { gch(p); if (pk(p) != -1) gch(p); continue; }
            if (d == '$' && pk2(p) == '{') {       /* interpolation: skip to } */
                gch(p); gch(p);
                while (pk(p) != -1 && pk(p) != '}') gch(p);
                if (pk(p) == '}') gch(p);
                continue;
            }
            if (d == '"') { gch(p); break; }
            gch(p);
        }
        t->kind = T_STRING;
        t->len = (uint32_t)p->pos - start;
        return;
    }

    switch (c) {
    case '{': gch(p); t->kind = T_LBRACE; break;
    case '}': gch(p); t->kind = T_RBRACE; break;
    case '(': gch(p); t->kind = T_LPAREN; break;
    case ')': gch(p); t->kind = T_RPAREN; break;
    case ';': gch(p); t->kind = T_SEMI; break;
    case ',': gch(p); t->kind = T_COMMA; break;
    case ':': gch(p); t->kind = T_COLON; break;
    case '.': gch(p); t->kind = T_DOT; break;
    case '=': gch(p); if (pk(p) == '=') { gch(p); t->kind = T_EQ; } else t->kind = T_ASSIGN; break;
    case '!': gch(p); if (pk(p) == '=') { gch(p); t->kind = T_NE; } else t->kind = T_NOT; break;
    case '<': gch(p); if (pk(p) == '=') { gch(p); t->kind = T_LE; } else t->kind = T_LT; break;
    case '>': gch(p); if (pk(p) == '=') { gch(p); t->kind = T_GE; } else t->kind = T_GT; break;
    case '&': gch(p); if (pk(p) == '&') { gch(p); t->kind = T_AND; } else { perr(p, t, "expected &&"); t->kind = T_AND; } break;
    case '|': gch(p); if (pk(p) == '|') { gch(p); t->kind = T_OR; } else { perr(p, t, "expected ||"); t->kind = T_OR; } break;
    default:
        gch(p);
        perr(p, t, "unexpected character '%c'", c);
        t->kind = T_EOF;
        break;
    }
    t->len = (uint32_t)p->pos - t->off;
}

/* ------------------------------------------------------------- helpers */
static bool at(P *p, int kind) { return p->cur.kind == kind; }

static bool ident_is(P *p, const char *kw)
{
    if (p->cur.kind != T_IDENT) return false;
    size_t n = strlen(kw);
    return p->cur.slen == n && strncmp(p->cur.s, kw, n) == 0;
}

static bool accept(P *p, int kind)
{
    if (p->cur.kind == kind) { next(p); return true; }
    return false;
}

static bool expect(P *p, int kind, const char *what)
{
    if (p->cur.kind == kind) { next(p); return true; }
    perr(p, &p->cur, "expected %s", what);
    return false;
}

static bool expect_ident(P *p, const char *kw)
{
    if (ident_is(p, kw)) { next(p); return true; }
    perr(p, &p->cur, "expected '%s'", kw);
    return false;
}

/* A job input parameter type keyword -> devos_val_type_t. */
static bool param_type(P *p, uint8_t *out)
{
    static const struct { const char *name; uint8_t ty; } T[] = {
        { "int", DEVOS_VAL_INT }, { "number", DEVOS_VAL_NUM }, { "string", DEVOS_VAL_STR },
        { "boolean", DEVOS_VAL_BOOL }, { "duration", DEVOS_VAL_DURATION },
    };
    if (p->cur.kind != T_IDENT) { perr(p, &p->cur, "expected a parameter type"); return false; }
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        if (p->cur.slen == strlen(T[i].name) && strncmp(p->cur.s, T[i].name, p->cur.slen) == 0) {
            *out = T[i].ty;
            next(p);
            return true;
        }
    }
    perr(p, &p->cur, "unknown parameter type '%.20s' (int / number / string / boolean / duration)", p->cur.s);
    return false;
}

static bool is_keyword(const tok_t *t, const char *kw)
{
    size_t n = strlen(kw);
    return t->kind == T_IDENT && t->slen == n && strncmp(t->s, kw, n) == 0;
}

static void depth_enter(P *p)
{
    if (++p->depth > (int)p->ast->lim.max_depth) perr(p, &p->cur, "nested too deeply (max %d)", p->ast->lim.max_depth);
}
static void depth_leave(P *p) { if (p->depth > 0) p->depth--; }

/* Read IDENT ('.' IDENT)* into buf; returns a pooled string or NULL. */
static const char *read_dotted(P *p, char *buf, size_t cap)
{
    if (!at(p, T_IDENT)) { perr(p, &p->cur, "expected a name"); return NULL; }
    size_t o = 0;
    for (;;) {
        if (p->cur.slen >= cap - o) { perr(p, &p->cur, "name is too long"); return NULL; }
        memcpy(buf + o, p->cur.s, p->cur.slen);
        o += p->cur.slen;
        next(p);
        if (!at(p, T_DOT)) break;
        next(p);
        if (!at(p, T_IDENT)) { perr(p, &p->cur, "expected a name after '.'"); return NULL; }
        buf[o++] = '.';
    }
    buf[o] = '\0';
    return jobs_pool_str(p->ast, buf, (uint32_t)o);
}

/* Decode a string literal span (including quotes) into a JN_EXPR_STR. */
static jobs_node_t *parse_string(P *p, const tok_t *t)
{
    jobs_node_t *node = jobs_node_new(p->ast, JN_EXPR_STR, t->off, t->len, t->line, t->col);
    if (!node) return NULL;
    const char *s = p->src + t->off + 1;
    uint32_t n = t->len >= 2 ? t->len - 2 : 0;      /* inside the quotes */
    char lit[512];
    size_t lo = 0;
    jobs_node_t *head = NULL, *tail = NULL;

    #define FLUSH_LIT() do {                                              \
        if (lo) {                                                         \
            jobs_node_t *sp = jobs_node_new(p->ast, JN_STRPART, t->off, t->len, t->line, t->col); \
            if (!sp) return NULL;                                         \
            sp->sub = JSP_LITERAL;                                        \
            sp->u.str.s = jobs_pool_str(p->ast, lit, (uint32_t)lo);       \
            if (!sp->u.str.s) return NULL;                                \
            if (tail) tail->next = sp; else head = sp;                    \
            tail = sp; lo = 0;                                            \
        }                                                                 \
    } while (0)

    for (uint32_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) {
            char e = s[++i];
            char out = e;
            switch (e) {
            case 'n': out = '\n'; break;
            case 't': out = '\t'; break;
            case 'r': out = '\r'; break;
            case 'b': out = '\b'; break;
            case 'f': out = '\f'; break;
            case '"': out = '"'; break;
            case '\\': out = '\\'; break;
            case '/': out = '/'; break;
            default: jobs_diag_add(p->ast, t->off, t->len, t->line, t->col, "unknown escape \\%c", e); p->err = true; return NULL;
            }
            if (lo + 1 < sizeof(lit)) lit[lo++] = out;
            continue;
        }
        if (c == '$' && i + 1 < n && s[i + 1] == '{') {
            FLUSH_LIT();
            uint32_t j = i + 2;
            while (j < n && s[j] != '}') j++;
            if (j >= n) { jobs_diag_add(p->ast, t->off, t->len, t->line, t->col, "unterminated ${...}"); p->err = true; return NULL; }
            uint32_t k = i + 2, end = j;
            while (k < end && (s[k] == ' ' || s[k] == '\t')) k++;
            while (end > k && (s[end - 1] == ' ' || s[end - 1] == '\t')) end--;
            jobs_node_t *sp = jobs_node_new(p->ast, JN_STRPART, t->off, t->len, t->line, t->col);
            if (!sp) return NULL;
            sp->sub = JSP_REF;
            sp->u.str.s = jobs_pool_str(p->ast, s + k, end - k);
            if (!sp->u.str.s) return NULL;
            if (tail) tail->next = sp; else head = sp;
            tail = sp;
            i = j;
            continue;
        }
        if (lo + 1 < sizeof(lit)) lit[lo++] = c;
    }
    FLUSH_LIT();
    #undef FLUSH_LIT
    node->a = head;
    return node;
}

/* ------------------------------------------------------------ parser */
static jobs_node_t *parse_expr(P *p);
static jobs_node_t *parse_block(P *p);
static jobs_node_t *parse_stmt(P *p);

static jobs_node_t *parse_args(P *p, bool named)
{
    if (!expect(p, T_LPAREN, "'('")) return NULL;
    if (accept(p, T_RPAREN)) return NULL;            /* empty list */
    jobs_node_t *head = NULL, *tail = NULL;
    for (;;) {
        jobs_node_t *arg;
        if (named) {
            if (!at(p, T_IDENT)) { perr(p, &p->cur, "expected an argument name"); return NULL; }
            tok_t nm = p->cur;
            next(p);
            if (!expect(p, T_COLON, "':'")) return NULL;
            arg = jobs_node_new(p->ast, JN_ARG, nm.off, nm.len, nm.line, nm.col);
            if (!arg) return NULL;
            arg->u.str.s = jobs_pool_str(p->ast, nm.s, nm.slen);
            if (!arg->u.str.s) return NULL;
            arg->a = parse_expr(p);
        } else {
            arg = parse_expr(p);
        }
        if (p->err || !arg) return NULL;
        if (tail) tail->next = arg; else head = arg;
        tail = arg;
        if (!accept(p, T_COMMA)) break;
    }
    if (!expect(p, T_RPAREN, "')'")) return NULL;
    return head;
}

static jobs_node_t *parse_primary(P *p)
{
    tok_t t = p->cur;
    if (at(p, T_NUMBER)) {
        next(p);
        jobs_node_t *n = jobs_node_new(p->ast, JN_EXPR_LIT, t.off, t.len, t.line, t.col);
        if (!n) return NULL;
        if (t.is_float) { n->u.lit.type = DEVOS_VAL_NUM; n->u.lit.v.n = t.n; }
        else { n->u.lit.type = DEVOS_VAL_INT; n->u.lit.v.i = t.i; }
        return n;
    }
    if (at(p, T_DURATION)) {
        next(p);
        jobs_node_t *n = jobs_node_new(p->ast, JN_EXPR_LIT, t.off, t.len, t.line, t.col);
        if (!n) return NULL;
        n->u.lit.type = DEVOS_VAL_DURATION;
        n->u.lit.v.ms = t.i;
        return n;
    }
    if (at(p, T_STRING)) {
        next(p);
        return parse_string(p, &t);
    }
    if (at(p, T_LPAREN)) {
        next(p);
        jobs_node_t *e = parse_expr(p);
        if (!expect(p, T_RPAREN, "')'")) return NULL;
        return e;
    }
    if (at(p, T_NOT)) {
        next(p);
        jobs_node_t *n = jobs_node_new(p->ast, JN_EXPR_UNARY, t.off, t.len, t.line, t.col);
        if (!n) return NULL;
        n->sub = JOP_NOT;
        n->a = parse_primary(p);
        return p->err ? NULL : n;
    }
    if (at(p, T_IDENT)) {
        if (is_keyword(&t, "true") || is_keyword(&t, "false")) {
            next(p);
            jobs_node_t *n = jobs_node_new(p->ast, JN_EXPR_LIT, t.off, t.len, t.line, t.col);
            if (!n) return NULL;
            n->u.lit.type = DEVOS_VAL_BOOL;
            n->u.lit.v.b = is_keyword(&t, "true");
            return n;
        }
        if (is_keyword(&t, "null")) {
            next(p);
            jobs_node_t *n = jobs_node_new(p->ast, JN_EXPR_LIT, t.off, t.len, t.line, t.col);
            if (!n) return NULL;
            n->u.lit.type = DEVOS_VAL_NULL;
            return n;
        }
        char buf[96];
        uint32_t off = t.off, len = t.len;
        int line = t.line, col = t.col;
        const char *name = read_dotted(p, buf, sizeof(buf));
        if (!name) return NULL;
        if (at(p, T_LPAREN)) {                       /* builtin call */
            jobs_node_t *n = jobs_node_new(p->ast, JN_EXPR_CALL, off, len, line, col);
            if (!n) return NULL;
            n->u.str.s = name;
            n->a = parse_args(p, false);
            return p->err ? NULL : n;
        }
        jobs_node_t *n = jobs_node_new(p->ast, JN_EXPR_REF, off, len, line, col);
        if (!n) return NULL;
        n->u.str.s = name;
        return n;
    }
    perr(p, &t, "expected a value or expression");
    return NULL;
}

static int op_prec(int kind)
{
    switch (kind) {
    case T_OR: return 1;
    case T_AND: return 2;
    case T_EQ: case T_NE: case T_LT: case T_LE: case T_GT: case T_GE: return 3;
    default: return 0;
    }
}
static jobs_op_t op_of(int kind)
{
    switch (kind) {
    case T_OR: return JOP_OR;
    case T_AND: return JOP_AND;
    case T_EQ: return JOP_EQ;
    case T_NE: return JOP_NE;
    case T_LT: return JOP_LT;
    case T_LE: return JOP_LE;
    case T_GT: return JOP_GT;
    default: return JOP_GE;
    }
}

static jobs_node_t *parse_expr_prec(P *p, int min_prec)
{
    jobs_node_t *lhs = parse_primary(p);
    if (p->err || !lhs) return NULL;
    for (;;) {
        int prec = op_prec(p->cur.kind);
        if (prec == 0 || prec < min_prec) break;
        tok_t op = p->cur;
        int kind = p->cur.kind;
        next(p);
        jobs_node_t *rhs = parse_expr_prec(p, prec + 1);
        if (p->err || !rhs) return NULL;
        jobs_node_t *n = jobs_node_new(p->ast, JN_EXPR_BINARY, op.off, op.len, op.line, op.col);
        if (!n) return NULL;
        n->sub = (uint8_t)op_of(kind);
        n->a = lhs;
        n->b = rhs;
        lhs = n;
    }
    return lhs;
}

static jobs_node_t *parse_expr(P *p)
{
    depth_enter(p);
    jobs_node_t *e = parse_expr_prec(p, 1);
    depth_leave(p);
    return e;
}

static jobs_node_t *parse_trigger(P *p)
{
    tok_t t = p->cur;
    if (!expect_ident(p, "trigger")) return NULL;
    jobs_node_t *n = jobs_node_new(p->ast, JN_TRIGGER, t.off, t.len, t.line, t.col);
    if (!n) return NULL;

    if (ident_is(p, "manual")) { n->sub = JTRIG_MANUAL; next(p); }
    else if (ident_is(p, "every")) {
        n->sub = JTRIG_EVERY; next(p);
        if (!at(p, T_DURATION)) { perr(p, &p->cur, "expected a duration (e.g. 5m)"); return NULL; }
        n->u.i = p->cur.i;
        next(p);
    } else if (ident_is(p, "daily") || ident_is(p, "weekdays")) {
        bool weekly = ident_is(p, "weekdays");
        n->sub = weekly ? JTRIG_WEEKDAYS : JTRIG_DAILY;
        next(p);
        if (!at(p, T_STRING)) { perr(p, &p->cur, "expected a time \"HH:MM\""); return NULL; }
        tok_t ts = p->cur;
        next(p);
        jobs_node_t *str = parse_string(p, &ts);
        if (!str) return NULL;
        if (str->a && str->a->next) { perr(p, &ts, "a time cannot contain ${...}"); return NULL; }
        n->u.str.s = str->a ? str->a->u.str.s : "";
        /* optional day selector: weekdays "08:00" days "Mon,Wed" */
        if (weekly && ident_is(p, "days")) {
            next(p);
            if (!at(p, T_STRING)) { perr(p, &p->cur, "expected days like \"Mon,Wed,Fri\""); return NULL; }
            tok_t ds = p->cur;
            next(p);
            jobs_node_t *dstr = parse_string(p, &ds);
            if (!dstr) return NULL;
            if (dstr->a && dstr->a->next) { perr(p, &ds, "days cannot contain ${...}"); return NULL; }
            n->u.str.s2 = dstr->a ? dstr->a->u.str.s : "";
        }
    } else if (ident_is(p, "event")) {        n->sub = JTRIG_EVENT; next(p);
        if (!at(p, T_STRING)) { perr(p, &p->cur, "expected an event topic"); return NULL; }
        tok_t ts = p->cur;
        next(p);
        jobs_node_t *str = parse_string(p, &ts);
        if (!str) return NULL;
        n->u.str.s = str->a ? str->a->u.str.s : "";
        if (at(p, T_LPAREN)) n->a = parse_args(p, true);
        if (ident_is(p, "where")) { next(p); n->b = parse_expr(p); }
    } else {
        perr(p, &p->cur, "expected manual / every / daily / weekdays / event");
        return NULL;
    }
    if (!expect(p, T_SEMI, "';'")) return NULL;
    return n;
}

static jobs_node_t *parse_policy(P *p)
{
    tok_t t = p->cur;
    if (!expect_ident(p, "policy")) return NULL;
    jobs_node_t *n = jobs_node_new(p->ast, JN_POLICY, t.off, t.len, t.line, t.col);
    if (!n) return NULL;
    n->a = parse_args(p, true);
    if (p->err) return NULL;
    if (!expect(p, T_SEMI, "';'")) return NULL;
    return n;
}

static jobs_node_t *parse_action(P *p)
{
    tok_t t = p->cur;
    char buf[96];
    const char *id = read_dotted(p, buf, sizeof(buf));
    if (!id) return NULL;
    jobs_node_t *n = jobs_node_new(p->ast, JN_ACTION, t.off, t.len, t.line, t.col);
    if (!n) return NULL;
    n->u.str.s = id;
    n->a = parse_args(p, true);
    if (p->err) return NULL;
    if (ident_is(p, "as")) {
        next(p);
        if (!at(p, T_IDENT)) { perr(p, &p->cur, "expected an output name"); return NULL; }
        n->u.str.s2 = jobs_pool_str(p->ast, p->cur.s, p->cur.slen);
        if (!n->u.str.s2) return NULL;
        next(p);
    }
    if (!expect(p, T_SEMI, "';'")) return NULL;
    return n;
}

static jobs_node_t *parse_stmt(P *p)
{
    if (at(p, T_LBRACE)) return parse_block(p);
    if (!at(p, T_IDENT)) { perr(p, &p->cur, "expected a statement"); return NULL; }

    if (ident_is(p, "set")) {
        tok_t t = p->cur;
        next(p);
        if (!at(p, T_IDENT)) { perr(p, &p->cur, "expected a variable name"); return NULL; }
        tok_t nm = p->cur;
        next(p);
        if (!expect(p, T_ASSIGN, "'='")) return NULL;
        jobs_node_t *n = jobs_node_new(p->ast, JN_SET, t.off, t.len, t.line, t.col);
        if (!n) return NULL;
        n->u.str.s = jobs_pool_str(p->ast, nm.s, nm.slen);
        if (!n->u.str.s) return NULL;
        n->a = parse_expr(p);
        if (p->err) return NULL;
        if (!expect(p, T_SEMI, "';'")) return NULL;
        return n;
    }
    if (ident_is(p, "if")) {
        tok_t t = p->cur;
        next(p);
        jobs_node_t *n = jobs_node_new(p->ast, JN_IF, t.off, t.len, t.line, t.col);
        if (!n) return NULL;
        n->a = parse_expr(p);
        if (p->err) return NULL;
        n->b = parse_block(p);
        if (p->err) return NULL;
        if (ident_is(p, "else")) { next(p); n->c = parse_block(p); }
        return n;
    }
    if (ident_is(p, "wait")) {
        tok_t t = p->cur;
        next(p);
        if (!at(p, T_DURATION)) { perr(p, &p->cur, "expected a duration (e.g. 2s)"); return NULL; }
        jobs_node_t *n = jobs_node_new(p->ast, JN_WAIT, t.off, t.len, t.line, t.col);
        if (!n) return NULL;
        n->u.i = p->cur.i;
        next(p);
        if (!expect(p, T_SEMI, "';'")) return NULL;
        return n;
    }
    if (ident_is(p, "repeat")) {
        tok_t t = p->cur;
        next(p);
        if (!at(p, T_NUMBER)) { perr(p, &p->cur, "expected a repeat count"); return NULL; }
        int64_t count = p->cur.i;
        next(p);
        if (!expect_ident(p, "as")) return NULL;
        if (!at(p, T_IDENT)) { perr(p, &p->cur, "expected a loop variable"); return NULL; }
        tok_t nm = p->cur;
        next(p);
        jobs_node_t *n = jobs_node_new(p->ast, JN_REPEAT, t.off, t.len, t.line, t.col);
        if (!n) return NULL;
        n->count = count;
        n->u.str.s = jobs_pool_str(p->ast, nm.s, nm.slen);
        if (!n->u.str.s) return NULL;
        n->a = parse_block(p);
        return p->err ? NULL : n;
    }
    if (ident_is(p, "return")) {
        tok_t t = p->cur;
        next(p);
        jobs_node_t *n = jobs_node_new(p->ast, JN_RETURN, t.off, t.len, t.line, t.col);
        if (!n) return NULL;
        n->a = parse_expr(p);
        if (p->err) return NULL;
        if (!expect(p, T_SEMI, "';'")) return NULL;
        return n;
    }
    if (ident_is(p, "run")) {
        tok_t t = p->cur;
        next(p);
        if (!at(p, T_STRING)) { perr(p, &p->cur, "expected a job name in quotes"); return NULL; }
        tok_t nt = p->cur;
        next(p);
        jobs_node_t *nm = parse_string(p, &nt);
        if (!nm) return NULL;
        if (nm->a && nm->a->next) { perr(p, &nt, "a job name cannot contain ${...}"); return NULL; }
        jobs_node_t *n = jobs_node_new(p->ast, JN_RUN, t.off, t.len, t.line, t.col);
        if (!n) return NULL;
        n->u.str.s = nm->a ? nm->a->u.str.s : "";
        if (at(p, T_LPAREN)) {
            n->a = parse_args(p, true);
            if (p->err) return NULL;
        }
        if (ident_is(p, "as")) {
            next(p);
            if (!at(p, T_IDENT)) { perr(p, &p->cur, "expected an output name"); return NULL; }
            n->u.str.s2 = jobs_pool_str(p->ast, p->cur.s, p->cur.slen);
            if (!n->u.str.s2) return NULL;
            next(p);
        }
        if (!expect(p, T_SEMI, "';'")) return NULL;
        return n;
    }
    return parse_action(p);
}

static jobs_node_t *parse_block(P *p)
{
    if (!expect(p, T_LBRACE, "'{'")) return NULL;
    depth_enter(p);
    jobs_node_t *block = jobs_node_new(p->ast, JN_BLOCK, p->cur.off, 0, p->cur.line, p->cur.col);
    if (!block) { depth_leave(p); return NULL; }
    jobs_node_t *head = NULL, *tail = NULL;
    while (!p->err && !at(p, T_RBRACE) && !at(p, T_EOF)) {
        jobs_node_t *s = parse_stmt(p);
        if (p->err || !s) break;
        if (tail) tail->next = s; else head = s;
        tail = s;
    }
    block->a = head;
    if (!expect(p, T_RBRACE, "'}'")) { depth_leave(p); return NULL; }
    depth_leave(p);
    return block;
}

jobs_ast_t *jobs_parse(const char *src, size_t len, const jobs_limits_t *lim)
{
    jobs_ast_t *ast = jobs_ast_new(lim);
    if (!ast) return NULL;
    if (!src) { jobs_diag_add(ast, 0, 0, 0, 0, "no source"); return ast; }
    if (len > ast->lim.max_source) {
        jobs_diag_add(ast, 0, 0, 0, 0, "source is too large (%u bytes max)", (unsigned)ast->lim.max_source);
        return ast;
    }
    memcpy(ast->source, src, len);
    ast->source[len] = '\0';
    ast->source_len = len;

    P p;
    memset(&p, 0, sizeof(p));
    p.src = ast->source;
    p.len = len;
    p.line = 1;
    p.col = 1;
    p.ast = ast;
    next(&p);

    if (!ident_is(&p, "version")) {
        perr(&p, &p.cur, "expected 'version %d;'", JOBS_LANG_VERSION);
        return ast;
    }
    next(&p);
    if (!at(&p, T_NUMBER) || p.cur.is_float) { perr(&p, &p.cur, "expected a language version number"); return ast; }
    int ver = (int)p.cur.i;
    next(&p);
    if (!expect(&p, T_SEMI, "';'")) return ast;
    if (ver != JOBS_LANG_VERSION) {
        perr(&p, &p.cur, "unsupported language version %d (this build is %d)", ver, JOBS_LANG_VERSION);
        return ast;
    }
    ast->lang_version = ver;

    if (!expect_ident(&p, "job")) return ast;
    if (!at(&p, T_STRING)) { perr(&p, &p.cur, "expected a job name"); return ast; }
    tok_t name_tok = p.cur;
    next(&p);
    jobs_node_t *root = jobs_node_new(ast, JN_JOB, name_tok.off, name_tok.len, name_tok.line, name_tok.col);
    if (!root) return ast;
    jobs_node_t *name = parse_string(&p, &name_tok);
    if (!name) return ast;
    if (name->a && name->a->next) { perr(&p, &name_tok, "a job name cannot contain ${...}"); return ast; }
    root->u.str.s = name->a ? name->a->u.str.s : "";
    if (at(&p, T_LPAREN)) {
        next(&p);
        if (!at(&p, T_RPAREN)) {
            for (;;) {
                if (!at(&p, T_IDENT)) { perr(&p, &p.cur, "expected a parameter name"); return ast; }
                tok_t pn = p.cur;
                next(&p);
                if (!expect(&p, T_COLON, "':'")) return ast;
                uint8_t ty;
                if (!param_type(&p, &ty)) return ast;
                jobs_node_t *par = jobs_node_new(ast, JN_PARAM, pn.off, pn.len, pn.line, pn.col);
                if (!par) return ast;
                par->sub = ty;
                par->u.str.s = jobs_pool_str(ast, pn.s, pn.slen);
                if (!par->u.str.s) return ast;
                if (!root->d) root->d = par;
                else { jobs_node_t *t = root->d; while (t->next) t = t->next; t->next = par; }
                if (!accept(&p, T_COMMA)) break;
            }
        }
        if (!expect(&p, T_RPAREN, "')'")) return ast;
    }
    if (!expect(&p, T_LBRACE, "'{'")) return ast;

    root->a = parse_trigger(&p);
    if (p.err) return ast;
    if (ident_is(&p, "policy")) {
        root->b = parse_policy(&p);
        if (p.err) return ast;
    }
    jobs_node_t *body = jobs_node_new(ast, JN_BLOCK, p.cur.off, 0, p.cur.line, p.cur.col);
    if (!body) return ast;
    jobs_node_t *head = NULL, *tail = NULL;
    while (!p.err && !at(&p, T_RBRACE) && !at(&p, T_EOF)) {
        jobs_node_t *s = parse_stmt(&p);
        if (p.err || !s) break;
        if (tail) tail->next = s; else head = s;
        tail = s;
    }
    body->a = head;
    root->c = body;
    if (!expect(&p, T_RBRACE, "'}'")) return ast;
    if (!p.err && !at(&p, T_EOF)) perr(&p, &p.cur, "unexpected text after the job");
    if (!p.err) ast->root = root;
    return ast;
}

jobs_node_t *jobs_parse_expr(const char *src, size_t len, jobs_ast_t *ast, const char **err)
{
    if (err) *err = NULL;
    if (!ast || !src) { if (err) *err = "no expression"; return NULL; }
    P p;
    memset(&p, 0, sizeof(p));
    p.src = src;
    p.len = len;
    p.line = 1;
    p.col = 1;
    p.ast = ast;
    next(&p);
    jobs_node_t *e = parse_expr(&p);
    if (p.err || !e) { if (err) *err = ast->diag_count ? ast->diag[0].msg : "bad expression"; return NULL; }
    if (p.cur.kind != T_EOF) { if (err) *err = "unexpected text after the expression"; return NULL; }
    return e;
}
