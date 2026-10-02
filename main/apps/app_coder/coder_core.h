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

typedef enum { CODER_SHA1 = 1, CODER_SHA256 = 2, CODER_SHA512 = 3 } coder_hash_t;

/* Hex digest. key/key_len > 0 makes it an HMAC. */
char *coder_hash(coder_hash_t algo, const char *key, size_t key_len, const char *msg, size_t len,
                 char *out, size_t cap);

/* Split a JWT and pretty-print its header and payload; signature shown, not
 * verified. */
char *coder_jwt(const char *in, size_t len, char *out, size_t cap);

/* A random UUID v4. */
char *coder_uuid(char *out, size_t cap);

/* Unix seconds -> "seconds\n<utc>\n<local>". in empty/0 means "now". */
char *coder_epoch(long long when, int use_now, char *out, size_t cap);

/* 1 if `in` looks like a decode input (hex or base64/base64url), so the UI can
 * offer the opposite direction. */
int coder_looks_encoded(const char *in, size_t len);

#ifdef __cplusplus
}
#endif
