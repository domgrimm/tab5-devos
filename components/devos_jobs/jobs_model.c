/* jobs_model: AST arena, string pool, diagnostics (see jobs_model.h). */
#include "jobs_model.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static void *jalloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}
#else
static void *jalloc(size_t n) { return malloc(n); }
#endif

void jobs_limits_default(jobs_limits_t *lim)
{
    if (!lim) return;
    lim->max_source = JOBS_MAX_SOURCE;
    lim->max_nodes = JOBS_MAX_NODES;
    lim->max_depth = JOBS_MAX_DEPTH;
    lim->max_vars = JOBS_MAX_VARS;
    lim->max_string = JOBS_MAX_STRING;
}

jobs_ast_t *jobs_ast_new(const jobs_limits_t *lim)
{
    jobs_ast_t *ast = calloc(1, sizeof(*ast));
    if (!ast) return NULL;
    jobs_limits_default(&ast->lim);
    if (lim) ast->lim = *lim;
    if (ast->lim.max_source == 0) ast->lim.max_source = JOBS_MAX_SOURCE;
    if (ast->lim.max_nodes == 0) ast->lim.max_nodes = JOBS_MAX_NODES;
    if (ast->lim.max_string == 0) ast->lim.max_string = JOBS_MAX_STRING;

    ast->source = jalloc(ast->lim.max_source + 1);
    ast->arena = jalloc((size_t)ast->lim.max_nodes * sizeof(jobs_node_t));
    ast->pool = jalloc(ast->lim.max_source + 64);
    if (!ast->source || !ast->arena || !ast->pool) {
        jobs_ast_free(ast);
        return NULL;
    }
    ast->source_len = 0;
    ast->source[0] = '\0';
    ast->arena_cap = (size_t)ast->lim.max_nodes * sizeof(jobs_node_t);
    ast->pool_cap = ast->lim.max_source + 64;
    ast->lang_version = JOBS_LANG_VERSION;
    ast->refs = 1;
    return ast;
}

void jobs_ast_retain(jobs_ast_t *ast) { if (ast) ast->refs++; }

void jobs_ast_release(jobs_ast_t *ast)
{
    if (!ast) return;
    if (--ast->refs <= 0) jobs_ast_free(ast);
}

void jobs_ast_free(jobs_ast_t *ast)
{
    if (!ast) return;
    free(ast->source);
    free(ast->arena);
    free(ast->pool);
    free(ast);
}

jobs_node_t *jobs_node_new(jobs_ast_t *ast, jobs_node_kind_t kind, uint32_t off, uint32_t len, int line, int col)
{
    if (!ast) return NULL;
    if (ast->node_count >= (int)ast->lim.max_nodes ||
        ast->arena_used + sizeof(jobs_node_t) > ast->arena_cap) {
        jobs_diag_add(ast, off, len, line, col, "definition is too large (%d nodes max)", ast->lim.max_nodes);
        return NULL;
    }
    jobs_node_t *n = (jobs_node_t *)(ast->arena + ast->arena_used);
    ast->arena_used += sizeof(jobs_node_t);
    memset(n, 0, sizeof(*n));
    n->kind = (uint8_t)kind;
    n->off = off;
    n->len = len;
    n->line = (uint16_t)line;
    n->col = (uint16_t)col;
    ast->node_count++;
    return n;
}

const char *jobs_pool_str(jobs_ast_t *ast, const char *s, uint32_t len)
{
    if (!ast) return NULL;
    if (len > ast->lim.max_string) {
        jobs_diag_add(ast, 0, 0, 0, 0, "string is too long (%u bytes max)", (unsigned)ast->lim.max_string);
        return NULL;
    }
    if (ast->pool_used + len + 1 > ast->pool_cap) {
        jobs_diag_add(ast, 0, 0, 0, 0, "definition has too much text");
        return NULL;
    }
    char *p = ast->pool + ast->pool_used;
    if (len) memcpy(p, s, len);
    p[len] = '\0';
    ast->pool_used += len + 1;
    return p;
}

void jobs_diag_add(jobs_ast_t *ast, uint32_t off, uint32_t len, int line, int col, const char *fmt, ...)
{
    if (!ast || ast->diag_count >= JOBS_MAX_DIAGS) return;
    jobs_diag_t *d = &ast->diag[ast->diag_count];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->msg, sizeof(d->msg), fmt, ap);
    va_end(ap);
    d->off = off;
    d->len = len;
    d->line = (uint16_t)line;
    d->col = (uint16_t)col;
    ast->diag_count++;
}

bool jobs_failed(const jobs_ast_t *ast) { return !ast || ast->diag_count > 0; }

const char *jobs_trigger_name(jobs_trigger_kind_t k)
{
    switch (k) {
    case JTRIG_MANUAL:   return "manual";
    case JTRIG_EVERY:    return "every";
    case JTRIG_DAILY:    return "daily";
    case JTRIG_WEEKDAYS: return "weekdays";
    case JTRIG_EVENT:    return "event";
    default:             return "?";
    }
}

static const char *const JOBS_DAY_NAMES[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };

int jobs_day_index(const char *name)
{
    if (!name) return -1;
    char c0 = name[0], c1 = name[1], c2 = name[2];
    if (!c0 || !c1 || !c2 || name[3] != '\0') return -1;
    c0 = (char)tolower((unsigned char)c0);
    c1 = (char)tolower((unsigned char)c1);
    c2 = (char)tolower((unsigned char)c2);
    for (int i = 0; i < 7; i++) {
        if (c0 == (char)tolower((unsigned char)JOBS_DAY_NAMES[i][0]) &&
            c1 == (char)tolower((unsigned char)JOBS_DAY_NAMES[i][1]) &&
            c2 == (char)tolower((unsigned char)JOBS_DAY_NAMES[i][2])) return i;
    }
    return -1;
}

bool jobs_days_parse(const char *list, uint8_t *mask_out)
{
    if (mask_out) *mask_out = 0;
    if (!list || !list[0]) return false;
    uint8_t mask = 0;
    const char *p = list;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        char tok[8];
        int k = 0;
        while (*p && *p != ',' && k < 7) tok[k++] = *p++;
        tok[k] = '\0';
        while (k > 0 && (tok[k - 1] == ' ' || tok[k - 1] == '\t')) tok[--k] = '\0';
        int idx = jobs_day_index(tok);
        if (idx < 0) return false;
        int dow = (idx + 1) % 7;
        if (mask & (uint8_t)(1u << dow)) return false;      /* duplicate day */
        mask |= (uint8_t)(1u << dow);
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p != ',') return false;
        p++;
    }
    if (mask == 0) return false;
    if (mask_out) *mask_out = mask;
    return true;
}

int jobs_days_format(uint8_t mask, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    size_t o = 0;
    for (int i = 0; i < 7; i++) {
        int dow = (i + 1) % 7;
        if (!(mask & (uint8_t)(1u << dow))) continue;
        o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? "," : "", JOBS_DAY_NAMES[i]);
        if (o + 1 >= cap) break;
    }
    return (int)o;
}

const char *jobs_op_name(jobs_op_t op)
{
    switch (op) {
    case JOP_NOT: return "!";
    case JOP_AND: return "&&";
    case JOP_OR:  return "||";
    case JOP_EQ:  return "==";
    case JOP_NE:  return "!=";
    case JOP_LT:  return "<";
    case JOP_LE:  return "<=";
    case JOP_GT:  return ">";
    case JOP_GE:  return ">=";
    default:      return "?";
    }
}
