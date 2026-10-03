/* jobs_model: AST arena, string pool, diagnostics (see jobs_model.h). */
#include "jobs_model.h"
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
    return ast;
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
