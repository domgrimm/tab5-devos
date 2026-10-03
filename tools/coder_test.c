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
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    setenv("TZ", "UTC", 1);                 /* cron next-runs are local time */
    tzset();
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

    /* SHA-384 also feeds the JWT HS384 path */
    coder_hash(CODER_SHA384, NULL, 0, "abc", 3, out, sizeof(out));
    eq("sha384 abc", out, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");

    /* JWT: header.payload.signature -> pretty header + payload */
    const char *jwt = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
                      "eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4gRG9lIiwiaWF0IjoxNTE2MjM5MDIyfQ."
                      "SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c";
    coder_jwt(jwt, strlen(jwt), NULL, 0, out, sizeof(out));
    has("jwt header shown", out, "\"alg\"");
    has("jwt payload shown", out, "\"name\"");
    has("jwt not verified", out, "(not verified");
    has("jwt iat dated", out, "2018-01-18");
    coder_jwt("not.a.jwt.extra", 15, NULL, 0, out, sizeof(out));
    eq("jwt malformed", out, "Not a JWT (expected header.payload.signature)");

    /* JWT signature verify with the jwt.io HS256 example */
    coder_jwt(jwt, strlen(jwt), "your-256-bit-secret", 19, out, sizeof(out));
    has("jwt hmac valid", out, "signature valid");
    has("jwt hmac alg", out, "HMAC-256");
    coder_jwt(jwt, strlen(jwt), "wrong-secret", 12, out, sizeof(out));
    has("jwt hmac invalid", out, "signature INVALID");

    /* epoch -> date and date -> epoch */
    coder_epoch_convert("1000000000", 10, 0, out, sizeof(out));
    has("epoch numeric", out, "2001-09-09 01:46:40 UTC");
    coder_epoch_convert("2001-09-09 01:46:40", 19, 0, out, sizeof(out));
    has("date to epoch", out, "1000000000");
    coder_epoch_convert("1970-01-01", 10, 0, out, sizeof(out));
    has("date 1970", out, "1970-01-01 00:00:00 UTC");
    coder_epoch_convert("nonsense", 8, 0, out, sizeof(out));
    has("date bad", out, "Unrecognised");

    /* subnet calculator */
    coder_subnet("192.168.1.10/24", 15, out, sizeof(out));
    has("subnet network", out, "Network      192.168.1.0/24");
    has("subnet broadcast", out, "Broadcast    192.168.1.255");
    has("subnet hosts", out, "254 usable");
    has("subnet private", out, "Private");
    coder_subnet("10.1.2.3", 8, out, sizeof(out));
    has("subnet bare is /32", out, "10.1.2.3/32");
    coder_subnet("10.0.0.0/31", 11, out, sizeof(out));
    has("subnet /31 hosts", out, "2 usable of 2 addresses");
    coder_subnet("192.168.1.10/255.255.255.0", 26, out, sizeof(out));
    has("subnet netmask form", out, "192.168.1.0/24");
    coder_subnet("172.16.0.0/12", 13, out, sizeof(out));
    has("subnet 172 private", out, "Private");
    coder_subnet("999.1.1.1/24", 12, out, sizeof(out));
    has("subnet bad", out, "Not an IPv4");
    coder_subnet("10.0.0.0/33", 11, out, sizeof(out));
    has("subnet prefix range", out, "Prefix must be 0..32");

    /* cron explainer (TZ pinned to UTC at the top of main) */
    coder_cron("*/15 * * * *", 12, 1000000000, 0, 3, out, sizeof(out));
    has("cron minute list", out, "0,15,30,45");
    has("cron next run", out, "2001-09-09 02:00");
    coder_cron("@daily", 6, 1000000000, 0, 1, out, sizeof(out));
    has("cron macro time", out, "at 00:00");
    has("cron macro day", out, "every day");
    coder_cron("0 3 * * 1", 9, 1000000000, 0, 1, out, sizeof(out));
    has("cron weekday", out, "MON");
    coder_cron("0 3 * * 1", 9, 1000000000, 0, 1, out, sizeof(out));
    has("cron at time", out, "at 03:00");
    coder_cron("0 0 1 * 1", 9, 1000000000, 0, 1, out, sizeof(out));
    has("cron dom or dow", out, "or on MON");
    coder_cron("60 * * * *", 10, 1000000000, 0, 0, out, sizeof(out));
    has("cron bad minute", out, "Bad minute field");
    coder_cron("@reboot", 7, 0, 0, 0, out, sizeof(out));
    has("cron reboot", out, "boot");

    /* regex tester */
    coder_regex("\\d+", 3, "a12 b345", 8, 0, 10, out, sizeof(out));
    has("regex count", out, "2 matches");
    has("regex first", out, "\"12\"");
    has("regex second", out, "\"345\"");
    coder_regex("(\\w+)@(\\w+)", 11, "me@here", 7, 0, 10, out, sizeof(out));
    has("regex group1", out, "$1 0-2");
    has("regex group2", out, "$2 3-7");
    coder_regex("abc", 3, "ABC", 3, CODER_RX_ICASE, 10, out, sizeof(out));
    has("regex icase", out, "1 match");
    coder_regex("^b", 2, "a\nb", 3, CODER_RX_MULTILINE, 10, out, sizeof(out));
    has("regex multiline", out, "1 match");
    coder_regex("a*", 2, "bbb", 3, 0, 10, out, sizeof(out));
    has("regex empty ok", out, "match");
    /* a quantifier must not swallow the atoms that follow it */
    coder_regex("colou?r", 7, "color and colour", 16, 0, 10, out, sizeof(out));
    has("regex optional literal", out, "\"color\"");
    has("regex optional literal 2", out, "\"colour\"");
    coder_regex("^\\w+$", 5, "hello\nworld", 11, 0, 10, out, sizeof(out));
    has("regex anchors need multiline", out, "No match");
    coder_regex("(", 1, "x", 1, 0, 10, out, sizeof(out));
    has("regex error", out, "Regex error");

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
