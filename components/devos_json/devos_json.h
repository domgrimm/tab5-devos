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

#ifdef __cplusplus
}
#endif
