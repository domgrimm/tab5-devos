/* coder_core: see coder_core.h. No LVGL, no devos_config. */
#include "coder_core.h"
#include "devos_crypto.h"
#include "devos_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Output scratch for the encode/decode paths (input cap lives in the app). */
#define SCRATCH 4096

/* Binary-safe: a decoded payload with non-printable bytes is shown as "0x.."
 * pairs rather than dumped raw into a label. */
static void put_bytes(const uint8_t *b, size_t n, char *out, size_t cap)
{
    int printable = 1;
    for (size_t i = 0; i < n; i++) {
        if (b[i] != '\n' && b[i] != '\r' && b[i] != '\t' && (b[i] < 0x20 || b[i] >= 0x7f)) {
            printable = 0;
            break;
        }
    }
    if (printable) {
        if (n >= cap) n = cap - 1;
        memcpy(out, b, n);
        out[n] = '\0';
        return;
    }
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < cap; i++)
        o += (size_t)snprintf(out + o, cap - o, i ? " %02x" : "0x %02x", b[i]);
    out[o] = '\0';
}

char *coder_transform(coder_tool_t tool, int dec, const char *in, size_t len, char *out, size_t cap)
{
    if (!dec) {
        switch (tool) {
        case CODER_HEX:    devos_hex_encode((const uint8_t *)in, len, out, cap); break;
        case CODER_URL:    devos_url_encode((const uint8_t *)in, len, out, cap); break;
        case CODER_BASE58: devos_base58_encode((const uint8_t *)in, len, out, cap); break;
        case CODER_B64URL: devos_base64url_encode((const uint8_t *)in, len, out, cap); break;
        default:           devos_base64_encode((const uint8_t *)in, len, out, cap); break;
        }
        return out;
    }

    uint8_t *tmp = malloc(len + 1);
    if (!tmp) { snprintf(out, cap, "Out of memory"); return out; }
    int m;
    switch (tool) {
    case CODER_HEX:    m = devos_hex_decode(in, tmp, len); break;
    case CODER_URL:    m = devos_url_decode(in, tmp, len); break;
    case CODER_BASE58: m = devos_base58_decode(in, tmp, len); break;
    case CODER_B64URL: m = devos_base64url_decode(in, tmp, len); break;
    default:           m = devos_base64_decode(in, tmp, len); break;
    }
    if (m < 0) {
        const char *what = tool == CODER_HEX ? "Not valid hex"
                         : tool == CODER_URL ? "Invalid percent-escape"
                         : tool == CODER_BASE58 ? "Not valid base58"
                                                : "Not valid base64";
        snprintf(out, cap, "%s", what);
    } else {
        put_bytes(tmp, (size_t)m, out, cap);
    }
    free(tmp);
    return out;
}

char *coder_crc32(const char *in, size_t len, char *out, size_t cap)
{
    snprintf(out, cap, "%08x", (unsigned)devos_crc32(in, len));
    return out;
}

char *coder_hash(coder_hash_t algo, const char *key, size_t key_len, const char *msg, size_t len,
                 char *out, size_t cap)
{
    uint8_t digest[64];
    devos_hash_t h = (devos_hash_t)algo;
    if (key && key_len) devos_hmac(h, key, key_len, msg, len, digest);
    else                devos_hash(h, msg, len, digest);
    devos_hex_encode(digest, devos_hash_len(h), out, cap);
    return out;
}

char *coder_jwt(const char *in, size_t len, char *out, size_t cap)
{
    const char *d1 = memchr(in, '.', len);
    const char *d2 = d1 ? memchr(d1 + 1, '.', len - (size_t)(d1 + 1 - in)) : NULL;
    if (!d1 || !d2 || memchr(d2 + 1, '.', len - (size_t)(d2 + 1 - in))) {
        snprintf(out, cap, "Not a JWT (expected header.payload.signature)");
        return out;
    }
    uint8_t raw[SCRATCH];
    char pretty[2048];
    size_t off = 0;

    size_t lh = (size_t)(d1 - in);
    if (lh >= sizeof(raw)) { snprintf(out, cap, "Header too long"); return out; }
    memcpy(raw, in, lh);
    int m = devos_base64url_decode((const char *)raw, raw, sizeof(raw) - 1);
    if (m < 0) { snprintf(out, cap, "Header is not valid base64url"); return out; }
    raw[m] = 0;
    size_t pl = devos_json_pretty((const char *)raw, (size_t)m, pretty, sizeof(pretty));
    if (!pl) snprintf(pretty, sizeof(pretty), "(header is not JSON: %.200s)", (const char *)raw);
    off += (size_t)snprintf(out + off, cap - off, "Header\n------\n%s\n\nPayload\n-------\n", pretty);

    size_t lp = (size_t)(d2 - d1 - 1);
    if (lp >= sizeof(raw)) { snprintf(out + off, cap - off, "(payload too long)"); return out; }
    memcpy(raw, d1 + 1, lp);
    m = devos_base64url_decode((const char *)raw, raw, sizeof(raw) - 1);
    if (m < 0) { snprintf(out + off, cap - off, "(payload is not valid base64url)"); return out; }
    raw[m] = 0;
    pl = devos_json_pretty((const char *)raw, (size_t)m, pretty, sizeof(pretty));
    if (!pl) snprintf(pretty, sizeof(pretty), "(payload is not JSON: %.200s)", (const char *)raw);
    off += (size_t)snprintf(out + off, cap - off, "%s\n\nSignature\n---------\n(not verified) ", pretty);

    size_t sl = len - (size_t)(d2 + 1 - in);
    if (sl > 128) sl = 128;
    snprintf(out + off, cap - off, "%.*s\n", (int)sl, d2 + 1);
    return out;
}

char *coder_uuid(char *out, size_t cap)
{
    uint8_t b[16];
    devos_random(b, 16);
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);          /* version 4 */
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);          /* variant 10 */
    snprintf(out, cap, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return out;
}

char *coder_epoch(long long when, int use_now, char *out, size_t cap)
{
    time_t tv = use_now ? time(NULL) : (time_t)when;
    struct tm tm;
    char utc[64], loc[64];
    gmtime_r(&tv, &tm);
    strftime(utc, sizeof(utc), "%Y-%m-%d %H:%M:%S UTC", &tm);
    localtime_r(&tv, &tm);
    strftime(loc, sizeof(loc), "%Y-%m-%d %H:%M:%S %Z", &tm);
    snprintf(out, cap, "%lld\n%s\n%s", (long long)tv, utc, loc);
    return out;
}

int coder_looks_encoded(const char *in, size_t len)
{
    if (!len) return 0;
    int hexish = 1, b64ish = 1;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == ' ' || c == ':' || c == '-') continue;         /* hex separators */
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) hexish = 0;
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '+' || c == '/' || c == '=' || c == '-' || c == '_';
        if (!ok) b64ish = 0;
        if (!hexish && !b64ish) return 0;
    }
    return hexish || b64ish;
}
