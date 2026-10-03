#pragma once

/* jobs_model: the Jobs definition model - one immutable AST shared by the
 * Builder, the Text view and the executor (PLAN.md Jobs, "One definition,
 * several representations"). No LVGL, no devos_core.h, no network, so the
 * parser/validator/serializer are unit-tested on the host.
 *
 * Source is copied into the AST (never a pointer into a live textarea).
 * Nodes and decoded strings live in PSRAM on the target. All spans are byte
 * offsets into the owned source; the UI converts them to line/column. */

#include "devos_actions.h"   /* devos_value_t */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JOBS_LANG_VERSION 1

/* Conservative initial limits (PLAN.md section 10); measured later. */
#define JOBS_MAX_SOURCE   (16 * 1024)
#define JOBS_MAX_NODES    128
#define JOBS_MAX_DEPTH    8
#define JOBS_MAX_VARS     32
#define JOBS_MAX_STRING   4096
#define JOBS_MAX_DIAGS    16
#define JOBS_MAX_WAIT_MS  (5 * 60 * 1000)

typedef struct {
    uint32_t max_source, max_nodes, max_depth, max_vars, max_string;
} jobs_limits_t;

void jobs_limits_default(jobs_limits_t *lim);

typedef enum {
    JN_JOB = 0,
    JN_TRIGGER,
    JN_POLICY,
    JN_BLOCK,
    JN_SET,
    JN_IF,
    JN_WAIT,
    JN_ACTION,
    JN_REPEAT,
    JN_ARG,
    JN_EXPR_LIT,
    JN_EXPR_REF,
    JN_EXPR_UNARY,
    JN_EXPR_BINARY,
    JN_EXPR_CALL,
    JN_EXPR_STR,
    JN_STRPART,
} jobs_node_kind_t;

typedef enum { JTRIG_MANUAL = 0, JTRIG_EVERY, JTRIG_DAILY, JTRIG_WEEKDAYS, JTRIG_EVENT } jobs_trigger_kind_t;
typedef enum { JOP_NOT = 0, JOP_AND, JOP_OR, JOP_EQ, JOP_NE, JOP_LT, JOP_LE, JOP_GT, JOP_GE } jobs_op_t;

/* JN_STRPART: sub == 0 literal, sub == 1 embedded reference */
#define JSP_LITERAL 0
#define JSP_REF     1

/* flags */
#define JNF_UNSUPPORTED 0x0001   /* parsed but not executable in this version (repeat) */
#define JNF_TAINTED     0x0002   /* secret-derived; never log/interpolate */

typedef struct jobs_node {
    uint8_t kind;
    uint8_t sub;
    uint16_t flags;
    uint32_t off, len;           /* source span */
    uint16_t line, col;
    struct jobs_node *next;      /* sibling in a block/list */
    struct jobs_node *a, *b, *c; /* kind-specific children */
    union {
        struct { const char *s; uint32_t slen; const char *s2; uint32_t slen2; } str;
        devos_value_t lit;
        int64_t i;
    } u;
} jobs_node_t;

typedef struct {
    char msg[120];
    uint32_t off, len;
    uint16_t line, col;
} jobs_diag_t;

typedef struct jobs_ast {
    char *source;                /* owned copy, NUL-terminated */
    size_t source_len;
    char *arena;                 /* nodes */
    size_t arena_cap, arena_used;
    char *pool;                  /* decoded strings */
    size_t pool_cap, pool_used;
    jobs_node_t *root;
    jobs_diag_t diag[JOBS_MAX_DIAGS];
    int diag_count;
    int node_count;
    int lang_version;
    int refs;                    /* owners; freed at 0 (engine + active runs) */
    jobs_limits_t lim;
} jobs_ast_t;

/* ---- arena ---- */
jobs_ast_t *jobs_ast_new(const jobs_limits_t *lim);
void jobs_ast_free(jobs_ast_t *ast);
/* Reference counting: an active run retains the revision it started with, so
 * applying a new revision never frees memory a run still reads. */
void jobs_ast_retain(jobs_ast_t *ast);
void jobs_ast_release(jobs_ast_t *ast);
jobs_node_t *jobs_node_new(jobs_ast_t *ast, jobs_node_kind_t kind, uint32_t off, uint32_t len, int line, int col);
/* Copy a string into the AST pool; returns a NUL-terminated pointer or NULL. */
const char *jobs_pool_str(jobs_ast_t *ast, const char *s, uint32_t len);
void jobs_diag_add(jobs_ast_t *ast, uint32_t off, uint32_t len, int line, int col, const char *fmt, ...);
bool jobs_failed(const jobs_ast_t *ast);

/* ---- pipeline (implemented in jobs_parse.c / jobs_validate.c / jobs_serialize.c) ---- */
jobs_ast_t *jobs_parse(const char *src, size_t len, const jobs_limits_t *lim);
/* Parse a single expression into an existing AST (for the Builder's condition
 * and expression fields). Returns the node, or NULL with *err set. */
jobs_node_t *jobs_parse_expr(const char *src, size_t len, jobs_ast_t *ast, const char **err);
/* Type/schema/scope validation; adds diagnostics. true when the AST is executable. */
bool jobs_validate(jobs_ast_t *ast);
/* Canonical source; returns the length (or the needed length if it didn't fit). */
size_t jobs_serialize(const jobs_ast_t *ast, char *out, size_t cap);
/* Canonical text of one expression (for the Builder's fields). */
size_t jobs_serialize_expr(const jobs_node_t *e, char *out, size_t cap);

/* ---- helpers ---- */
const char *jobs_trigger_name(jobs_trigger_kind_t k);
const char *jobs_op_name(jobs_op_t op);

#ifdef __cplusplus
}
#endif
