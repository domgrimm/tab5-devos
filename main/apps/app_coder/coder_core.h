#pragma once

/* coder_core: the pure logic behind the Coder's Toolkit - no LVGL, no config,
 * so it builds on the host and is unit-tested in tools/coder_test.c. The app
 * (app_coder.c) only lays out the widgets and calls these.
 *
 * Every function writes a NUL-terminated result into out (cap bytes) and
 * returns out. Inputs are byte strings; `len` is explicit so the JWT / hash
 * paths never assume a NUL. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CODER_B64 = 0,
    CODER_B64URL,
    CODER_HEX,
    CODER_URL,
    CODER_BASE58,
    CODER_HASH,
    CODER_JWT,
    CODER_UUID,
    CODER_EPOCH,
} coder_tool_t;

/* Base64 / Base64 URL / Hex / URL / Base58: encode (dec=false) or decode
 * (dec=true). Binary results that aren't printable come back as "0x.." hex
 * pairs; a bad input yields a short human message ("Not valid base64"). */
char *coder_transform(coder_tool_t tool, int dec, const char *in, size_t len, char *out, size_t cap);

/* CRC-32 (IEEE) as 8 lowercase hex digits. */
char *coder_crc32(const char *in, size_t len, char *out, size_t cap);

typedef enum { CODER_SHA1 = 1, CODER_SHA256 = 2, CODER_SHA512 = 3, CODER_SHA384 = 4 } coder_hash_t;

/* Hex digest. key/key_len > 0 makes it an HMAC. */
char *coder_hash(coder_hash_t algo, const char *key, size_t key_len, const char *msg, size_t len,
                 char *out, size_t cap);

/* Split a JWT, pretty-print its header and payload, and (when key/key_len is
 * non-empty) verify an HS256 / HS384 / HS512 signature. iat / nbf / exp claims
 * are shown as dates with their status. The signature is never trusted unless
 * a key is given. */
char *coder_jwt(const char *in, size_t len, const char *key, size_t key_len, char *out, size_t cap);

/* A random UUID v4. */
char *coder_uuid(char *out, size_t cap);

/* Unix seconds -> "seconds\n<utc>\n<local>". in empty/0 means "now". */
char *coder_epoch(long long when, int use_now, char *out, size_t cap);

/* Bidirectional Unix-time converter. `in` is either signed epoch seconds
 * ("1700000000") or a UTC date/time ("2026-10-03", "2026-10-03 12:34:56",
 * "2026-10-03T12:34:56Z"); an empty input means "now". Writes the same shape
 * as coder_epoch(), or a short "Unrecognised input" message. */
char *coder_epoch_convert(const char *in, size_t len, int use_now, char *out, size_t cap);

/* IPv4 subnet report from "a.b.c.d" or "a.b.c.d/prefix" (prefix 0..32; a bare
 * address is a /32). Lists network, netmask, wildcard, broadcast, host range,
 * usable host count and the address type (private / loopback / ...). */
char *coder_subnet(const char *in, size_t len, char *out, size_t cap);

/* Explain a cron expression (5 fields, or "@daily" and friends) and list the
 * next `next` runs after `from` (use_now takes time(NULL) instead). `next` is
 * clamped to 0..10. Times are shown in local time; the same expression is
 * therefore easy to check against a machine's crontab. */
char *coder_cron(const char *in, size_t len, long long from, int use_now, int next, char *out, size_t cap);

/* Regex tester: run `pattern` over `subject` and list every match with its
 * offsets and capture groups. flags are CODER_RX_*. max_matches is clamped to
 * 1..100. Supported syntax: literals, '.', '^' '$', character classes
 * '[abc]' '[^a-z]' with '\d \D \w \W \s \S', the quantifiers '* + ? {n} {n,}
 * {n,m}' (lazy with a trailing '?'), groups '(...)' and '(?:...)', and
 * alternation '|'. No backreferences. */
#define CODER_RX_ICASE     1u
#define CODER_RX_MULTILINE 2u
#define CODER_RX_DOTALL    4u
char *coder_regex(const char *pattern, size_t plen, const char *subject, size_t slen,
                  unsigned flags, int max_matches, char *out, size_t cap);

/* 1 if `in` looks like a decode input (hex or base64/base64url), so the UI can
 * offer the opposite direction. */
int coder_looks_encoded(const char *in, size_t len);

#ifdef __cplusplus
}
#endif
