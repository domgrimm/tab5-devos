/* Host test for the Coder's Toolkit core logic (main/apps/app_coder/coder_core.c).
 * No LVGL: coder_core only needs devos_crypto and devos_json.
 *
 *   gcc -O2 -Icomponents/devos_crypto -Icomponents/devos_json \
 *       -Imain/apps/app_coder \
 *       tools/coder_test.c main/apps/app_coder/coder_core.c \
 *       components/devos_crypto/devos_crypto.c components/devos_json/devos_json.c \
 *       -o /tmp/coder_test && /tmp/coder_test
 */
#include "coder_core.h"
#include "devos_crypto.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void eq(const char *name, const char *got, const char *want)
{
    if (strcmp(got, want)) {
        printf("FAIL %s\n  got  %s\n  want %s\n", name, got, want);
        fails++;
    } else {
        printf("ok   %s\n", name);
    }
}

static void has(const char *name, const char *hay, const char *needle)
{
    if (!strstr(hay, needle)) {
        printf("FAIL %s: %s not found in output\n", name, needle);
        fails++;
    } else {
        printf("ok   %s\n", name);
    }
}

int main(void)
{
    char out[4096];

    /* transform: encode / decode round trips */
    coder_transform(CODER_B64, 0, "Hello, world!", 13, out, sizeof(out));
    eq("b64 encode", out, "SGVsbG8sIHdvcmxkIQ==");
    coder_transform(CODER_B64, 1, "SGVsbG8sIHdvcmxkIQ==", 20, out, sizeof(out));
    eq("b64 decode", out, "Hello, world!");

    coder_transform(CODER_B64URL, 0, "sub/ject?", 9, out, sizeof(out));
    eq("b64url encode", out, "c3ViL2plY3Q_");
    coder_transform(CODER_B64URL, 1, "c3ViL2plY3Q_", 11, out, sizeof(out));
    eq("b64url decode", out, "sub/ject?");
    coder_transform(CODER_HEX, 0, "ABC", 3, out, sizeof(out));
    eq("hex encode", out, "414243");
    coder_transform(CODER_HEX, 1, "41:42-43", 8, out, sizeof(out));
    eq("hex decode", out, "ABC");

    coder_transform(CODER_URL, 0, "a b/c?d=e&f", 11, out, sizeof(out));
    eq("url encode", out, "a%20b%2Fc%3Fd%3De%26f");
    coder_transform(CODER_URL, 1, "a%20b%2Fc", 8, out, sizeof(out));
    eq("url decode", out, "a b/c");

    /* base58 round-trip + a known vector ("hello world" -> StV1DL6CwTryKyV) */
    coder_transform(CODER_BASE58, 0, "hello world", 11, out, sizeof(out));
    eq("base58 encode", out, "StV1DL6CwTryKyV");
    coder_transform(CODER_BASE58, 1, "StV1DL6CwTryKyV", 15, out, sizeof(out));
    eq("base58 decode", out, "hello world");
    coder_transform(CODER_BASE58, 0, "\0\0abc", 5, out, sizeof(out));
    eq("base58 leading zeros", out, "11ZiCa");
    coder_transform(CODER_BASE58, 1, "0OIl", 4, out, sizeof(out));
    eq("base58 bad input", out, "Not valid base58");

    /* crc-32 */
    coder_crc32("123456789", 9, out, sizeof(out));
    eq("crc32", out, "cbf43926");
    coder_crc32("", 0, out, sizeof(out));
    eq("crc32 empty", out, "00000000");

    /* bad input -> a short message, never a crash */
    coder_transform(CODER_B64, 1, "***", 3, out, sizeof(out));
    eq("b64 bad input", out, "Not valid base64");
    coder_transform(CODER_HEX, 1, "xyz", 3, out, sizeof(out));
    eq("hex bad input", out, "Not valid hex");

    /* binary decode is shown as hex, not raw bytes */
    coder_transform(CODER_HEX, 1, "00ff10", 6, out, sizeof(out));
    has("hex binary shown", out, "0x");

    /* hash + HMAC */
    coder_hash(CODER_SHA256, NULL, 0, "abc", 3, out, sizeof(out));
    eq("sha256 abc", out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    coder_hash(CODER_SHA256, "key", 3, "The quick brown fox jumps over the lazy dog", 43, out, sizeof(out));
    eq("hmac-sha256", out, "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");

    /* JWT: header.payload.signature -> pretty header + payload */
    const char *jwt = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
                      "eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIn0."
                      "SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c";
    coder_jwt(jwt, strlen(jwt), out, sizeof(out));
    has("jwt header shown", out, "\"alg\"");
    has("jwt payload shown", out, "\"name\"");
    has("jwt not verified", out, "(not verified)");
    coder_jwt("not.a.jwt.extra", 15, out, sizeof(out));
    eq("jwt malformed", out, "Not a JWT (expected header.payload.signature)");

    /* UUID v4 shape: version nibble 4, variant 8/9/a/b */
    coder_uuid(out, sizeof(out));
    if (strlen(out) != 36 || out[8] != '-' || out[13] != '-' || out[18] != '-' || out[23] != '-' ||
        out[14] != '4' || strchr("89ab", out[19]) == NULL) {
        printf("FAIL uuid shape: %s\n", out);
        fails++;
    } else {
        printf("ok   uuid shape\n");
    }
    char u2[64];
    coder_uuid(u2, sizeof(u2));
    if (strcmp(out, u2) == 0) { printf("FAIL uuid not random\n"); fails++; }
    else printf("ok   uuid random\n");

    /* epoch: fixed + "now" */
    coder_epoch(0, 1, out, sizeof(out));
    has("epoch now utc", out, "UTC");
    coder_epoch(1000000000, 0, out, sizeof(out));
    has("epoch fixed", out, "2001-09-09 01:46:40 UTC");
    has("epoch seconds line", out, "1000000000");

    /* encoded detection */
    if (!coder_looks_encoded("deadbeef", 8)) { printf("FAIL looks_encoded hex\n"); fails++; }
    else printf("ok   looks_encoded hex\n");
    if (coder_looks_encoded("not encoded!", 12)) { printf("FAIL looks_encoded plain\n"); fails++; }
    else printf("ok   looks_encoded plain\n");

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
