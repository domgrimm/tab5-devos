#pragma once

/* jobs_build: the Builder's draft AST and edit operations (PLAN.md 6.4/11.2).
 * The Builder edits the same AST the Text view parses; every edit re-serializes
 * through jobs_serialize, so there is one executable definition, not two. No
 * LVGL, so the edit operations are unit-tested on the host.
 *
 * A "custom" node is a valid advanced expression the Builder cannot render as
 * a structured field (a call, a compound expression); the UI shows an opaque
 * card and offers "Edit in Text" rather than rewriting it. */

#include "jobs_model.h"
#include "devos_actions.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JOBS_BUILD_ROWS 128

typedef struct {
    jobs_ast_t *ast;            /* the draft (owned) */
    char diag[160];             /* first diagnostic from the last load */
} jobs_build_t;

typedef struct {
    const jobs_node_t *node;    /* the statement (JN_ACTION/JN_IF/JN_WAIT/JN_SET/...) */
    const jobs_node_t *block;   /* the JN_BLOCK that contains it */
    int index;                  /* position within block */
    int depth;                  /* indentation */
    const char *branch;         /* "if" / "else" for a branch body row, else "" */
} jobs_build_row_t;

/* Parse + validate a draft. false (b->diag) when it isn't valid; the AST is
 * still kept so the UI can show partial structure where possible. */
bool jobs_build_load(jobs_build_t *b, const char *source, size_t len);
void jobs_build_free(jobs_build_t *b);
/* Flatten the job body into rows (branches nested, depth-first). */
int jobs_build_rows(const jobs_build_t *b, jobs_build_row_t *out, int max);
/* Canonical source of the draft. */
size_t jobs_build_source(jobs_build_t *b, char *out, size_t cap);
/* Re-validate the draft in place; updates b->diag. */
bool jobs_build_revalidate(jobs_build_t *b);

/* ---- trigger card ---- */
jobs_node_t *jobs_build_trigger(jobs_build_t *b);
bool jobs_build_set_trigger_kind(jobs_build_t *b, int kind);
bool jobs_build_set_trigger_duration(jobs_build_t *b, int64_t ms);
bool jobs_build_set_trigger_time(jobs_build_t *b, const char *hhmm);   /* daily/weekdays */
/* Event trigger: topic (e.g. "system.boot") and an optional `where` filter. */
bool jobs_build_set_trigger_event(jobs_build_t *b, const char *topic);
bool jobs_build_set_trigger_where(jobs_build_t *b, const char *expr);
const char *jobs_build_trigger_event_topic(const jobs_node_t *t);
/* Canonical text of the trigger's `where` ("" when none). */
const char *jobs_build_trigger_where_text(const jobs_node_t *t);

/* ---- steps ---- */
/* Append an action built from its schema (all params defaulted) to `block`. */
bool jobs_build_add_action(jobs_build_t *b, const jobs_node_t *block, const char *action_id);
bool jobs_build_delete(jobs_build_t *b, const jobs_node_t *block, const jobs_node_t *stmt);
bool jobs_build_move(jobs_build_t *b, const jobs_node_t *block, const jobs_node_t *stmt, int dir);
/* Replace a statement's expression (wait duration / set value) with a literal. */
bool jobs_build_set_wait(jobs_build_t *b, const jobs_node_t *stmt, int64_t ms);
/* Edit a repeat's count and loop-index name; its body stays read-only in the
 * Builder (edited in Text). Count must be 1..JOBS_MAX_REPEAT. */
bool jobs_build_set_repeat(jobs_build_t *b, const jobs_node_t *stmt, int64_t count, const char *index);
/* Number of statements in a block. */
int jobs_build_block_count(const jobs_node_t *block);
/* Move a statement to a target index within its block. */
bool jobs_build_move_to(jobs_build_t *b, const jobs_node_t *block, const jobs_node_t *stmt, int target);

/* ---- conditions and advanced expressions ---- */
const jobs_node_t *jobs_build_if_cond(const jobs_node_t *if_node);
const jobs_node_t *jobs_build_set_value(const jobs_node_t *set_node);
const jobs_node_t *jobs_build_arg_expr(const jobs_node_t *action, const char *param);
bool jobs_build_set_if_cond(jobs_build_t *b, const jobs_node_t *if_node, const char *text);
bool jobs_build_set_set_value(jobs_build_t *b, const jobs_node_t *set_node, const char *text);
bool jobs_build_set_arg_expr(jobs_build_t *b, const jobs_node_t *action, const char *param, const char *text);
/* Canonical text of an expression (static buffer). */
const char *jobs_build_expr_text(const jobs_node_t *e);
/* 1 when the argument is a compound expression (not a literal/plain string). */
bool jobs_build_arg_is_expr(const jobs_node_t *action, const char *param);

/* ---- inspector: action parameters ---- */
/* 1 when the named parameter's value is a simple literal (or absent) the
 * Builder can edit; 0 when it is an advanced expression (custom card). */
bool jobs_build_arg_is_simple(const jobs_node_t *action, const char *param);
/* Set an argument to a literal value (creates it if absent). */
bool jobs_build_set_arg_str(jobs_build_t *b, const jobs_node_t *action, const char *param, const char *text);
bool jobs_build_set_arg_bool(jobs_build_t *b, const jobs_node_t *action, const char *param, bool v);
bool jobs_build_set_arg_int(jobs_build_t *b, const jobs_node_t *action, const char *param, int64_t v);
bool jobs_build_set_arg_duration(jobs_build_t *b, const jobs_node_t *action, const char *param, int64_t ms);
/* Credential parameter: a secret("name") reference. */
bool jobs_build_set_arg_secret(jobs_build_t *b, const jobs_node_t *action, const char *param, const char *name);
const char *jobs_build_arg_secret_name(const jobs_node_t *action, const char *param);
/* Read an argument's literal text ("", "true"/"false", a number), or NULL when
 * the argument is absent or advanced. */
const char *jobs_build_arg_text(const jobs_node_t *action, const char *param);
int64_t jobs_build_arg_duration(const jobs_node_t *action, const char *param);
bool jobs_build_arg_bool(const jobs_node_t *action, const char *param, bool *v);

#ifdef __cplusplus
}
#endif
