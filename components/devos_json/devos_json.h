#pragma once

/* devos_json: minimal JSON reader for exactly the shapes devOS consumes.
 *
 * No allocations, no cJSON dependency. Unknown fields are ignored, never
 * fatal. All spans are (pointer, length) into the caller's buffer.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parse "..." at p (expects quote); unescapes (incl. BMP \u) into out.
 * Returns position after the closing quote, or NULL. */
const char *devos_json_parse_str(const char *p, const char *end, char *out,
                                 size_t cap);

/* Find "key": in [p,end), string-aware; returns value start or NULL. */
const char *devos_json_find_key(const char *p, const char *end,
                                const char *key);

/* Value of the direct member `key` of the object at p (which points at '{'),
 * or NULL. Unlike find_key, nested objects and arrays are skipped:
 * {"class":{"name":"T20"},"name":"Final"} gives "Final" for "name". */
const char *devos_json_member(const char *p, const char *end, const char *key);

/* A direct member as a string (0 ok, -1 missing / not a string) or a number
 * (a JSON number, or a string holding one, as some APIs send; false if not). */
int devos_json_member_str(const char *obj, const char *end, const char *key, char *out, size_t cap);
bool devos_json_member_num(const char *obj, const char *end, const char *key, double *out);

/* ---- narrow dot-path extraction (Jobs `json_get`) ----
 * A resolved scalar. STR points at the opening quote in the source (still
 * escaped); decode it with devos_json_parse_str. */
typedef enum {
    DEVOS_JSON_NULL = 0, DEVOS_JSON_BOOL, DEVOS_JSON_INT, DEVOS_JSON_NUM, DEVOS_JSON_STR,
    DEVOS_JSON_RAW,         /* an object/array; `s` spans the raw JSON text */
} devos_json_kind_t;

typedef struct {
    devos_json_kind_t kind;
    bool b;
    int64_t i;
    double n;
    const char *s;          /* STR/RAW: into the source */
    uint32_t len;           /* STR: bytes including both quotes; RAW: the span */
} devos_json_val_t;

/* Resolve a dot-separated path with optional array indexes ("a.b[0].c",
 * "[2].name"). An object or array resolves as DEVOS_JSON_RAW (its own JSON
 * text, so it can be logged or fed to another json_get); a missing member or a
 * malformed/over-long path returns false. Bounded path, index and document
 * walk; not full JSONPath. */
bool devos_json_path(const char *js, size_t len, const char *path, devos_json_val_t *out);

/* Balanced span of the {...} or [...] starting at p, or NULL. */
const char *devos_json_span(const char *p, const char *end);

/* First string occurrence of key in [js,js+len); 0 ok, -1 missing. */
int devos_json_get_str(const char *js, size_t len, const char *key, char *out,
                       size_t cap);

/* First integer occurrence of key in [js,js+len); 0 ok, -1 missing. */
int devos_json_get_int(const char *js, size_t len, const char *key,
                       int *out);

/* Iterate top-level elements of the array at p (points at '['). */
void devos_json_array_each(const char *p, size_t len,
                           void (*cb)(const char *, size_t, void *),
                           void *ud);

/* Escape src into dst for embedding in JSON output; returns used length. */
size_t devos_json_escape(const char *src, char *dst, size_t cap);

/* Pretty-print the JSON object/array in [src,src+len) with 2-space indents.
 * Returns the output length, or 0 if it isn't a well-formed object/array or
 * doesn't fit in cap (out is then unusable). */
size_t devos_json_pretty(const char *src, size_t len, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
