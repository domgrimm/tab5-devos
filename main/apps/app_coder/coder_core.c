/* coder_core: see coder_core.h. No LVGL, no devos_config. */
#include "coder_core.h"
#include "devos_crypto.h"
#include "devos_json.h"

#include <ctype.h>
#include <limits.h>
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

/* One UTC timestamp line, for the JWT claim dates. */
static void fmt_utc(long long secs, char *out, size_t cap)
{
    time_t tv = (time_t)secs;
    struct tm tm;
    gmtime_r(&tv, &tm);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S UTC", &tm);
}

/* "3d ago" / "in 3d" style, for a claim relative to now. */
static void human_age(long long now, long long when, char *out, size_t cap)
{
    long long d = now - when, a = d < 0 ? -d : d;
    const char *dir = d >= 0 ? "ago" : "from now";
    if (a < 60)          snprintf(out, cap, "%llds %s", a, dir);
    else if (a < 3600)   snprintf(out, cap, "%lldm %s", a / 60, dir);
    else if (a < 86400)  snprintf(out, cap, "%lldh %s", a / 3600, dir);
    else                 snprintf(out, cap, "%lldd %s", a / 86400, dir);
}

char *coder_jwt(const char *in, size_t len, const char *key, size_t key_len, char *out, size_t cap)
{
    const char *d1 = memchr(in, '.', len);
    const char *d2 = d1 ? memchr(d1 + 1, '.', len - (size_t)(d1 + 1 - in)) : NULL;
    if (!d1 || !d2 || memchr(d2 + 1, '.', len - (size_t)(d2 + 1 - in))) {
        snprintf(out, cap, "Not a JWT (expected header.payload.signature)");
        return out;
    }
    uint8_t raw[SCRATCH];
    char pretty[2048];
    char hdr_json[SCRATCH];
    char payload_json[SCRATCH];
    size_t off = 0;

    size_t lh = (size_t)(d1 - in);
    if (lh >= sizeof(raw)) { snprintf(out, cap, "Header too long"); return out; }
    memcpy(raw, in, lh);
    raw[lh] = 0;                                   /* the decoder scans to NUL */
    int m = devos_base64url_decode((const char *)raw, raw, sizeof(raw) - 1);
    if (m < 0) { snprintf(out, cap, "Header is not valid base64url"); return out; }
    raw[m] = 0;
    memcpy(hdr_json, raw, (size_t)m + 1);
    size_t pl = devos_json_pretty((const char *)raw, (size_t)m, pretty, sizeof(pretty));
    if (!pl) snprintf(pretty, sizeof(pretty), "(header is not JSON: %.200s)", (const char *)raw);
    off += (size_t)snprintf(out + off, cap - off, "Header\n------\n%s\n\nPayload\n-------\n", pretty);

    size_t lp = (size_t)(d2 - d1 - 1);
    if (lp >= sizeof(raw)) { snprintf(out + off, cap - off, "(payload too long)"); return out; }
    memcpy(raw, d1 + 1, lp);
    raw[lp] = 0;                                   /* the decoder scans to NUL */
    m = devos_base64url_decode((const char *)raw, raw, sizeof(raw) - 1);
    if (m < 0) { snprintf(out + off, cap - off, "(payload is not valid base64url)"); return out; }
    raw[m] = 0;
    memcpy(payload_json, raw, (size_t)m + 1);
    pl = devos_json_pretty((const char *)raw, (size_t)m, pretty, sizeof(pretty));
    if (!pl) snprintf(pretty, sizeof(pretty), "(payload is not JSON: %.200s)", (const char *)raw);
    off += (size_t)snprintf(out + off, cap - off, "%s\n", pretty);

    /* Claim timestamps, so exp / nbf / iat read as real dates. */
    long long now = (long long)time(NULL);
    const char *pjs = payload_json, *pje = payload_json + strlen(payload_json);
    static const char *ck[3] = { "iat", "nbf", "exp" };
    static const char *cl[3] = { "Issued", "Not before", "Expires" };
    char cline[3][160];
    int nclaims = 0;
    for (int i = 0; i < 3; i++) {
        double v;
        if (!devos_json_member_num(pjs, pje, ck[i], &v)) continue;
        long long t = (long long)v;
        char ts[48], age[32];
        fmt_utc(t, ts, sizeof(ts));
        human_age(now, t, age, sizeof(age));
        const char *state = "";
        if (i == 2) state = now >= t ? "  EXPIRED " : "  valid ";
        else if (i == 1) state = now < t ? "  NOT YET VALID " : "  valid ";
        else state = "  ";
        snprintf(cline[nclaims], sizeof(cline[0]), "%-10s %s%s(%s)\n", cl[i], ts, state, age);
        nclaims++;
    }
    if (nclaims) {
        off += (size_t)snprintf(out + off, cap - off, "\nClaims\n------\n");
        for (int i = 0; i < nclaims; i++) off += (size_t)snprintf(out + off, cap - off, "%s", cline[i]);
    }

    /* Signature: only checked when a secret is supplied. */
    const char *alg = devos_json_member(hdr_json, hdr_json + strlen(hdr_json), "alg");
    char algname[16] = "";
    if (alg && *alg == '"') devos_json_parse_str(alg, hdr_json + strlen(hdr_json), algname, sizeof(algname));

    off += (size_t)snprintf(out + off, cap - off, "\nSignature\n---------\n");
    if (!key || !key_len) {
        size_t sl = len - (size_t)(d2 + 1 - in);
        if (sl > 96) sl = 96;
        off += (size_t)snprintf(out + off, cap - off, "%.*s\n(not verified - type a secret to check)\n", (int)sl, d2 + 1);
    } else {
        devos_hash_t h;
        if      (!strcmp(algname, "HS256")) h = DEVOS_HASH_SHA256;
        else if (!strcmp(algname, "HS384")) h = DEVOS_HASH_SHA384;
        else if (!strcmp(algname, "HS512")) h = DEVOS_HASH_SHA512;
        else {
            snprintf(out + off, cap - off,
                     "%s\nCan't verify %s (only HS256 / HS384 / HS512 have a shared secret)\n",
                     algname[0] ? algname : "(no alg)", algname[0] ? algname : "this token");
            return out;
        }
        uint8_t mac[64];
        size_t hmlen = devos_hash_len(h);
        devos_hmac(h, key, key_len, in, (size_t)(d2 - in), mac);
        char sigb64[256];
        size_t sn = len - (size_t)(d2 + 1 - in);
        if (sn >= sizeof(sigb64)) sn = sizeof(sigb64) - 1;
        memcpy(sigb64, d2 + 1, sn);
        sigb64[sn] = 0;
        while (sn && (sigb64[sn - 1] == '=' || sigb64[sn - 1] == '\n' || sigb64[sn - 1] == '\r' || sigb64[sn - 1] == ' '))
            sigb64[--sn] = 0;
        uint8_t sig[128];
        int sbn = devos_base64url_decode(sigb64, sig, sizeof(sig));
        int valid = (sbn == (int)hmlen) && devos_ct_equal(sig, mac, hmlen);
        off += (size_t)snprintf(out + off, cap - off, "%s\nHMAC-%s over header.payload matches: %s\n",
                                algname, algname + 2, valid ? "YES - signature valid" : "NO - signature INVALID");
    }
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

/* =========================================================================
 * Unix time: date -> epoch (the other direction of coder_epoch)
 * ========================================================================= */

static long long days_from_civil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097LL + (long long)doe - 719468LL;
}

static int take_digits(const char **pp, const char *end, int min, int max, int *out)
{
    const char *p = *pp;
    int n = 0, v = 0;
    while (p < end && *p >= '0' && *p <= '9' && n < max) { v = v * 10 + (*p - '0'); p++; n++; }
    if (n < min) return 0;
    *pp = p;
    *out = v;
    return 1;
}

char *coder_epoch_convert(const char *in, size_t len, int use_now, char *out, size_t cap)
{
    if (use_now || !len) return coder_epoch(0, 1, out, cap);

    const char *p = in, *end = in + len;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;

    /* All digits (optionally signed): epoch seconds. */
    {
        const char *q = p;
        if (q < end && (*q == '-' || *q == '+')) q++;
        int digits = 0, allnum = 1;
        for (const char *r = q; r < end; r++) {
            if (*r < '0' || *r > '9') { allnum = 0; break; }
            digits++;
        }
        if (allnum && digits) {
            char buf[32];
            size_t n = (size_t)(end - p);
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            memcpy(buf, p, n);
            buf[n] = 0;
            return coder_epoch(strtoll(buf, NULL, 10), 0, out, cap);
        }
    }

    /* Date/time: YYYY-MM-DD[ T]HH:MM[:SS][Z], understood as UTC. */
    int y, mo, d, h = 0, mi = 0, s = 0;
    if (!take_digits(&p, end, 4, 4, &y) || p >= end || *p != '-') goto bad;
    p++;
    if (!take_digits(&p, end, 1, 2, &mo) || p >= end || *p != '-') goto bad;
    p++;
    if (!take_digits(&p, end, 1, 2, &d)) goto bad;
    if (p < end && (*p == 'T' || *p == 't' || *p == ' ')) {
        p++;
        if (!take_digits(&p, end, 1, 2, &h) || p >= end || *p != ':') goto bad;
        p++;
        if (!take_digits(&p, end, 1, 2, &mi)) goto bad;
        if (p < end && *p == ':') {
            p++;
            if (!take_digits(&p, end, 1, 2, &s)) goto bad;
        }
    }
    while (p < end && (*p == 'Z' || *p == 'z' || *p == ' ')) p++;
    if (p != end) goto bad;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) goto bad;

    long long secs = days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400LL + h * 3600 + mi * 60 + s;
    return coder_epoch(secs, 0, out, cap);

bad:
    snprintf(out, cap, "Unrecognised input - use epoch seconds or YYYY-MM-DD HH:MM:SS");
    return out;
}

/* =========================================================================
 * IPv4 subnet calculator
 * ========================================================================= */

static int parse_ipv4(const char *s, size_t len, uint32_t *out)
{
    uint32_t v = 0;
    int part = 0, val = 0, digits = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            if (++digits > 3 || val > 255) return 0;
        } else if (c == '.') {
            if (!digits || part > 2) return 0;
            v = (v << 8) | (uint32_t)val;
            part++;
            val = 0;
            digits = 0;
        } else {
            return 0;
        }
    }
    if (!digits || part != 3) return 0;
    *out = (v << 8) | (uint32_t)val;
    return 1;
}

static void fmt_ipv4(uint32_t v, char *out, size_t cap)
{
    snprintf(out, cap, "%u.%u.%u.%u", (unsigned)((v >> 24) & 0xff), (unsigned)((v >> 16) & 0xff),
             (unsigned)((v >> 8) & 0xff), (unsigned)(v & 0xff));
}

static const char *ipv4_class(uint32_t ip)
{
    unsigned a = ip >> 24;
    if (a < 128) return "A";
    if (a < 192) return "B";
    if (a < 224) return "C";
    if (a < 240) return "D (multicast)";
    return "E (reserved)";
}

static const char *ipv4_type(uint32_t ip)
{
    if ((ip >> 24) == 10) return "Private (RFC 1918)";
    if ((ip >> 20) == 0xac1) return "Private (RFC 1918)";
    if ((ip >> 16) == 0xc0a8) return "Private (RFC 1918)";
    if ((ip >> 24) == 127) return "Loopback (RFC 1122)";
    if ((ip >> 16) == 0xa9fe) return "Link-local / APIPA";
    if ((ip >> 22) == 0x191) return "Carrier-grade NAT (RFC 6598)";
    if ((ip >> 24) == 0) return "This network (RFC 1122)";
    if (ip == 0xffffffffu) return "Limited broadcast";
    if ((ip >> 28) == 0xe) return "Multicast (RFC 5771)";
    if ((ip >> 28) == 0xf) return "Reserved (RFC 1112)";
    return "Public";
}

char *coder_subnet(const char *in, size_t len, char *out, size_t cap)
{
    const char *p = in, *end = in + len;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;

    const char *slash = memchr(p, '/', (size_t)(end - p));
    uint32_t ip;
    if (!parse_ipv4(p, slash ? (size_t)(slash - p) : (size_t)(end - p), &ip)) {
        snprintf(out, cap, "Not an IPv4 address - use a.b.c.d or a.b.c.d/prefix");
        return out;
    }

    int prefix = 32;
    if (slash) {
        const char *ps = slash + 1;
        size_t pn = (size_t)(end - ps);
        if (memchr(ps, '.', pn)) {
            uint32_t nm;
            if (!parse_ipv4(ps, pn, &nm)) { snprintf(out, cap, "Bad netmask"); return out; }
            int bits = 0;
            uint32_t m = 0x80000000u;
            while (bits < 32 && (nm & m)) { bits++; m >>= 1; }
            if (nm != (bits == 0 ? 0u : (~0u << (32 - bits)))) { snprintf(out, cap, "Non-contiguous netmask"); return out; }
            prefix = bits;
        } else {
            if (!pn) { snprintf(out, cap, "Missing prefix length"); return out; }
            int v = 0;
            for (size_t i = 0; i < pn; i++) {
                if (ps[i] < '0' || ps[i] > '9') { snprintf(out, cap, "Bad prefix length"); return out; }
                v = v * 10 + (ps[i] - '0');
            }
            if (v > 32) { snprintf(out, cap, "Prefix must be 0..32"); return out; }
            prefix = v;
        }
    }

    uint32_t mask = prefix == 0 ? 0u : (prefix == 32 ? 0xffffffffu : (~0u << (32 - prefix)));
    uint32_t net = ip & mask, bcast = net | ~mask;
    unsigned long long total = prefix == 32 ? 1ull : (1ull << (32 - prefix));
    unsigned long long usable = prefix == 32 ? 1ull : (prefix == 31 ? 2ull : total - 2);
    uint32_t first = prefix >= 31 ? net : net + 1;
    uint32_t last = prefix >= 31 ? bcast : bcast - 1;
    char s_ip[20], s_net[20], s_mask[20], s_wc[20], s_bc[20], s_first[20], s_last[20];
    fmt_ipv4(ip, s_ip, sizeof(s_ip));
    fmt_ipv4(net, s_net, sizeof(s_net));
    fmt_ipv4(mask, s_mask, sizeof(s_mask));
    fmt_ipv4(~mask, s_wc, sizeof(s_wc));
    fmt_ipv4(bcast, s_bc, sizeof(s_bc));
    fmt_ipv4(first, s_first, sizeof(s_first));
    fmt_ipv4(last, s_last, sizeof(s_last));
    snprintf(out, cap,
             "Address      %s/%d\n"
             "Network      %s/%d\n"
             "Netmask      %s\n"
             "Wildcard     %s\n"
             "Broadcast    %s\n"
             "Host range   %s - %s\n"
             "Hosts        %llu usable of %llu addresses\n"
             "Type         %s\n"
             "Class        %s\n",
             s_ip, prefix, s_net, prefix, s_mask, s_wc, s_bc, s_first, s_last,
             usable, total, ipv4_type(ip), ipv4_class(ip));
    return out;
}

/* =========================================================================
 * Cron explainer
 * ========================================================================= */

typedef struct { uint64_t bits; int star; } cfield_t;

#define CBIT(f, v) (((f).bits >> (v)) & 1ULL)

static const char *const CRON_MON[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
static const char *const CRON_DOW[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char *const CRON_FIELD_NAME[5] = { "minute", "hour", "day", "month", "weekday" };

static int ci_eq3(const char *a, const char *b)
{
    for (int i = 0; i < 3; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

static int cron_atom(const char **pp, const char *end, int lo, int hi,
                     const char *const *names, int nnames, int *out)
{
    const char *p = *pp;
    if (p >= end) return 0;
    if (isalpha((unsigned char)*p) && names) {
        if (end - p < 3) return 0;
        for (int i = 0; i < nnames; i++)
            if (ci_eq3(p, names[i])) { *out = lo + i; *pp = p + 3; return 1; }
        return 0;
    }
    int v = 0, n = 0;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; n++; }
    if (!n || v < lo || v > hi) return 0;
    *pp = p;
    *out = v;
    return 1;
}

static int cron_field(const char *s, size_t len, int lo, int hi,
                      const char *const *names, int nnames, cfield_t *f, const char **err)
{
    f->bits = 0;
    f->star = (len == 1 && s[0] == '*');
    const char *p = s, *end = s + len;
    if (p >= end) { *err = "empty"; return 0; }
    while (p < end) {
        const char *comma = memchr(p, ',', (size_t)(end - p));
        const char *item_end = comma ? comma : end;
        const char *slash = memchr(p, '/', (size_t)(item_end - p));
        const char *range_end = slash ? slash : item_end;
        int a, b, step = 1;

        if (range_end - p == 1 && *p == '*') {
            a = lo; b = hi;
        } else {
            const char *q = p;
            if (!cron_atom(&q, range_end, lo, hi, names, nnames, &a)) { *err = "bad value"; return 0; }
            if (q < range_end && *q == '-') {
                q++;
                if (!cron_atom(&q, range_end, lo, hi, names, nnames, &b)) { *err = "bad range"; return 0; }
            } else {
                b = a;
            }
            if (q != range_end) { *err = "unexpected text"; return 0; }
        }
        if (a > b) { *err = "reversed range"; return 0; }
        if (slash) {
            const char *q = slash + 1;
            if (q >= item_end) { *err = "missing step"; return 0; }
            int v = 0;
            while (q < item_end) {
                if (*q < '0' || *q > '9') { *err = "bad step"; return 0; }
                v = v * 10 + (*q - '0');
                q++;
            }
            if (v <= 0) { *err = "step must be > 0"; return 0; }
            step = v;
        }
        for (int v = a; v <= b; v += step) f->bits |= 1ULL << v;
        p = comma ? comma + 1 : end;
    }
    if (!f->bits) { *err = "no values"; return 0; }
    return 1;
}

static int cron_count(const cfield_t *f, int lo, int hi)
{
    int n = 0;
    for (int v = lo; v <= hi; v++) n += (int)CBIT(*f, v);
    return n;
}

static void cron_list(const cfield_t *f, int lo, int hi, const char *const *names, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = 0;
    for (int v = lo; v <= hi && o + 1 < cap;) {
        if (!CBIT(*f, v)) { v++; continue; }
        int run = v;
        while (run + 1 <= hi && CBIT(*f, run + 1)) run++;
        char a[16], b[16];
        if (names) { snprintf(a, sizeof(a), "%s", names[v - lo]); snprintf(b, sizeof(b), "%s", names[run - lo]); }
        else       { snprintf(a, sizeof(a), "%d", v); snprintf(b, sizeof(b), "%d", run); }
        int n = snprintf(out + o, cap - o, "%s%s%s%s", o ? "," : "", a, run > v ? "-" : "", run > v ? b : "");
        if (n < 0) break;
        o += (size_t)n;
        v = run + 1;
    }
}

static int cron_match_time(const cfield_t *mi, const cfield_t *hr, const cfield_t *dom,
                           const cfield_t *mo, const cfield_t *dow, const struct tm *t)
{
    if (!CBIT(*mi, t->tm_min) || !CBIT(*hr, t->tm_hour)) return 0;
    if (!CBIT(*mo, t->tm_mon + 1)) return 0;
    int dom_ok = dom->star ? 1 : (int)CBIT(*dom, t->tm_mday);
    int dow_ok = dow->star ? 1 : (int)CBIT(*dow, t->tm_wday);
    return (dom->star || dow->star) ? (dom_ok && dow_ok) : (dom_ok || dow_ok);
}

char *coder_cron(const char *in, size_t len, long long from, int use_now, int next, char *out, size_t cap)
{
    if (next < 0) next = 0;
    if (next > 10) next = 10;

    const char *p = in, *end = in + len;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
    size_t en = (size_t)(end - p);
    if (!en) { snprintf(out, cap, "Type a cron expression, e.g.  0 3 * * 1"); return out; }
    if (en >= 128) { snprintf(out, cap, "Expression too long"); return out; }
    char expr[128];
    memcpy(expr, p, en);
    expr[en] = 0;

    if (expr[0] == '@') {
        static const struct { const char *name, *repl; } mac[] = {
            { "@yearly", "0 0 1 1 *" }, { "@annually", "0 0 1 1 *" },
            { "@monthly", "0 0 1 * *" }, { "@weekly", "0 0 * * 0" },
            { "@daily", "0 0 * * *" }, { "@midnight", "0 0 * * *" },
            { "@hourly", "0 * * * *" },
        };
        if (!strcmp(expr, "@reboot")) {
            snprintf(out, cap, "Runs once at every boot (@reboot) - no schedule to explain.");
            return out;
        }
        int matched = 0;
        for (size_t i = 0; i < sizeof(mac) / sizeof(mac[0]); i++)
            if (!strcmp(expr, mac[i].name)) { snprintf(expr, sizeof(expr), "%s", mac[i].repl); matched = 1; break; }
        if (!matched) { snprintf(out, cap, "Unknown macro %s", expr); return out; }
    }

    char *tok[6];
    int ntok = 0;
    char *save = NULL;
    for (char *t = strtok_r(expr, " \t", &save); t; t = strtok_r(NULL, " \t", &save)) {
        if (ntok < 6) tok[ntok] = t;
        ntok++;
    }
    if (ntok != 5) {
        snprintf(out, cap, "Expected 5 fields: minute hour day month weekday (got %d)", ntok);
        return out;
    }

    cfield_t F[5];
    static const char *const *const NAMES[5] = { NULL, NULL, NULL, CRON_MON, CRON_DOW };
    static const int LO[5] = { 0, 0, 1, 1, 0 }, HI[5] = { 59, 23, 31, 12, 7 }, NN[5] = { 0, 0, 0, 12, 7 };
    for (int i = 0; i < 5; i++) {
        const char *err = "bad";
        if (!cron_field(tok[i], strlen(tok[i]), LO[i], HI[i], NAMES[i], NN[i], &F[i], &err)) {
            snprintf(out, cap, "Bad %s field '%s': %s", CRON_FIELD_NAME[i], tok[i], err);
            return out;
        }
    }
    if (CBIT(F[4], 7)) { F[4].bits |= 1ULL << 0; F[4].bits &= ~(1ULL << 7); }   /* 7 = Sunday too */

    char minl[192], hrl[96], doml[128], monl[64], dowl[64], sbuf[512], daybuf[384], monbuf[96];
    cron_list(&F[0], 0, 59, NULL, minl, sizeof(minl));
    cron_list(&F[1], 0, 23, NULL, hrl, sizeof(hrl));
    cron_list(&F[2], 1, 31, NULL, doml, sizeof(doml));
    cron_list(&F[3], 1, 12, CRON_MON, monl, sizeof(monl));
    cron_list(&F[4], 0, 6, CRON_DOW, dowl, sizeof(dowl));

    int mall = cron_count(&F[0], 0, 59) == 60;
    int hall = cron_count(&F[1], 0, 23) == 24;
    int m1 = cron_count(&F[0], 0, 59) == 1;
    int h1 = cron_count(&F[1], 0, 23) == 1;

    if (mall && hall) snprintf(sbuf, sizeof(sbuf), "every minute");
    else if (hall)    snprintf(sbuf, sizeof(sbuf), "at minute %s of every hour", minl);
    else if (mall)    snprintf(sbuf, sizeof(sbuf), "every minute of hour %s", hrl);
    else if (m1 && h1) {
        int mv = 0, hv = 0;
        while (mv < 60 && !CBIT(F[0], mv)) mv++;
        while (hv < 24 && !CBIT(F[1], hv)) hv++;
        snprintf(sbuf, sizeof(sbuf), "at %02d:%02d", hv, mv);
    } else {
        snprintf(sbuf, sizeof(sbuf), "at minute %s past hour %s", minl, hrl);
    }

    if (F[2].star && F[4].star)       snprintf(daybuf, sizeof(daybuf), "every day");
    else if (!F[2].star && F[4].star) snprintf(daybuf, sizeof(daybuf), "on day %s of the month", doml);
    else if (F[2].star && !F[4].star) snprintf(daybuf, sizeof(daybuf), "on %s", dowl);
    else                              snprintf(daybuf, sizeof(daybuf), "on day %s of the month or on %s", doml, dowl);

    monbuf[0] = 0;
    if (cron_count(&F[3], 1, 12) != 12) snprintf(monbuf, sizeof(monbuf), " in %s", monl);

    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o, "Runs %s %s%s.\n\n", sbuf, daybuf, monbuf);
    o += (size_t)snprintf(out + o, cap - o,
        "Minute   %-16s %s\n"
        "Hour     %-16s %s\n"
        "Day      %-16s %s\n"
        "Month    %-16s %s\n"
        "Weekday  %-16s %s\n",
        tok[0], mall ? "every minute" : minl,
        tok[1], hall ? "every hour" : hrl,
        tok[2], F[2].star ? "every day" : doml,
        tok[3], cron_count(&F[3], 1, 12) == 12 ? "every month" : monl,
        tok[4], F[4].star ? "every weekday" : dowl);
    if (!F[2].star && !F[4].star)
        o += (size_t)snprintf(out + o, cap - o,
                              "(A day matches if either the day-of-month or the weekday does.)\n");

    if (next > 0) {
        time_t base = use_now ? time(NULL) : (time_t)from;
        base = base - (base % 60) + 60;
        o += (size_t)snprintf(out + o, cap - o, "\nNext %d run%s (local time):\n", next, next == 1 ? "" : "s");
        int found = 0;
        long long limit = 366LL * 24 * 60;
        for (long long i = 0; i < limit && found < next; i++, base += 60) {
            struct tm t;
            localtime_r(&base, &t);
            if (!cron_match_time(&F[0], &F[1], &F[2], &F[3], &F[4], &t)) continue;
            char line[64];
            strftime(line, sizeof(line), "%Y-%m-%d %H:%M %a", &t);
            o += (size_t)snprintf(out + o, cap - o, "  %s\n", line);
            found++;
        }
        if (!found) o += (size_t)snprintf(out + o, cap - o, "  (none within a year)\n");
    }
    return out;
}

/* =========================================================================
 * Regex tester - a small backtracking engine, no POSIX regex on the device.
 * ========================================================================= */

enum { RX_CHAR = 0, RX_ANY, RX_CLS, RX_BOL, RX_EOL, RX_WORDB, RX_WORDB_N, RX_GROUP, RX_ALT, RX_REP };
#define RX_MAX_GROUPS 9

typedef struct rx_node {
    unsigned char type, greedy, neg;
    signed char cap;              /* capture index (1..9), -1 = none */
    unsigned char ch;
    short min, max;               /* RX_REP; max = -1 = unbounded */
    unsigned char cls[32];        /* RX_CLS bitmap */
    struct rx_node *child, *next, *alt;
} rx_node_t;

typedef struct {
    const char *p, *end;
    rx_node_t *pool;
    int used, cap_pool, ncap;
    const char *err;
} rx_parse_t;

static rx_node_t *rx_new(rx_parse_t *ps, int type)
{
    if (ps->used >= ps->cap_pool) { ps->err = "pattern too complex"; return NULL; }
    rx_node_t *n = &ps->pool[ps->used++];
    memset(n, 0, sizeof(*n));
    n->type = (unsigned char)type;
    n->cap = -1;
    return n;
}

static void cls_add(rx_node_t *n, int c)
{
    n->cls[((unsigned char)c) >> 3] |= (unsigned char)(1u << (((unsigned char)c) & 7));
}

static void cls_add_range(rx_node_t *n, int a, int b)
{
    for (int c = a; c <= b; c++) cls_add(n, c);
}

static void cls_add_escape(rx_node_t *n, int e)
{
    switch (e) {
    case 'd': cls_add_range(n, '0', '9'); break;
    case 'D':
        for (int c = 0; c < 256; c++) if (!(c >= '0' && c <= '9')) cls_add(n, c);
        break;
    case 'w':
        cls_add_range(n, 'a', 'z'); cls_add_range(n, 'A', 'Z'); cls_add_range(n, '0', '9'); cls_add(n, '_');
        break;
    case 'W':
        for (int c = 0; c < 256; c++) if (!(isalnum(c) || c == '_')) cls_add(n, c);
        break;
    case 's': cls_add(n, ' '); cls_add(n, '\t'); cls_add(n, '\n'); cls_add(n, '\r'); cls_add(n, '\f'); cls_add(n, '\v'); break;
    case 'S':
        for (int c = 0; c < 256; c++) if (!isspace(c)) cls_add(n, c);
        break;
    }
}

static int esc_char(int c)
{
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    default:  return c;
    }
}

static rx_node_t *rx_parse_alt(rx_parse_t *ps);

static rx_node_t *rx_parse_class(rx_parse_t *ps)
{
    ps->p++;                                    /* '[' */
    rx_node_t *n = rx_new(ps, RX_CLS);
    if (!n) return NULL;
    if (ps->p < ps->end && *ps->p == '^') { n->neg = 1; ps->p++; }
    int first = 1;
    while (ps->p < ps->end && (*ps->p != ']' || first)) {
        first = 0;
        int lo;
        if (*ps->p == '\\') {
            ps->p++;
            if (ps->p >= ps->end) { ps->err = "trailing \\ in class"; return NULL; }
            unsigned char e = (unsigned char)*ps->p++;
            if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') { cls_add_escape(n, e); continue; }
            lo = esc_char(e);
        } else {
            lo = (unsigned char)*ps->p++;
        }
        if (ps->p + 1 < ps->end && ps->p[0] == '-' && ps->p[1] != ']') {
            ps->p++;
            int hi;
            if (*ps->p == '\\') {
                ps->p++;
                if (ps->p >= ps->end) { ps->err = "trailing \\ in class"; return NULL; }
                hi = esc_char((unsigned char)*ps->p++);
            } else {
                hi = (unsigned char)*ps->p++;
            }
            if (lo > hi) { ps->err = "reversed class range"; return NULL; }
            cls_add_range(n, lo, hi);
        } else {
            cls_add(n, lo);
        }
    }
    if (ps->p >= ps->end || *ps->p != ']') { ps->err = "unbalanced ["; return NULL; }
    ps->p++;
    return n;
}

static rx_node_t *rx_parse_atom(rx_parse_t *ps)
{
    if (ps->p >= ps->end) return NULL;
    char c = *ps->p;
    if (c == '(') {
        ps->p++;
        int capturing = 1;
        if (ps->end - ps->p >= 2 && ps->p[0] == '?' && ps->p[1] == ':') { capturing = 0; ps->p += 2; }
        rx_node_t *g = rx_new(ps, RX_GROUP);
        if (!g) return NULL;
        if (capturing) {
            if (ps->ncap >= RX_MAX_GROUPS) { ps->err = "too many groups (max 9)"; return NULL; }
            g->cap = (signed char)++ps->ncap;
        }
        g->child = rx_parse_alt(ps);
        if (ps->err) return NULL;
        if (ps->p >= ps->end || *ps->p != ')') { ps->err = "unbalanced ("; return NULL; }
        ps->p++;
        return g;
    }
    if (c == '[') return rx_parse_class(ps);
    if (c == '.') { ps->p++; return rx_new(ps, RX_ANY); }
    if (c == '^') { ps->p++; return rx_new(ps, RX_BOL); }
    if (c == '$') { ps->p++; return rx_new(ps, RX_EOL); }
    if (c == '\\') {
        ps->p++;
        if (ps->p >= ps->end) { ps->err = "trailing \\"; return NULL; }
        unsigned char e = (unsigned char)*ps->p++;
        if (e == 'b') return rx_new(ps, RX_WORDB);
        if (e == 'B') return rx_new(ps, RX_WORDB_N);
        if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') {
            rx_node_t *n = rx_new(ps, RX_CLS);
            if (n) cls_add_escape(n, e);
            return n;
        }
        rx_node_t *n = rx_new(ps, RX_CHAR);
        if (n) n->ch = (unsigned char)esc_char(e);
        return n;
    }
    ps->p++;
    rx_node_t *n = rx_new(ps, RX_CHAR);
    if (n) n->ch = (unsigned char)c;
    return n;
}

static rx_node_t *rx_parse_quant(rx_parse_t *ps, rx_node_t *atom)
{
    if (!atom || ps->p >= ps->end) return atom;
    char c = *ps->p;
    int min, max, has = 1;
    if (c == '*') { min = 0; max = -1; ps->p++; }
    else if (c == '+') { min = 1; max = -1; ps->p++; }
    else if (c == '?') { min = 0; max = 1; ps->p++; }
    else if (c == '{') {
        const char *q = ps->p + 1;
        int a = 0, b = -2, nd = 0;
        while (q < ps->end && *q >= '0' && *q <= '9') { a = a * 10 + (*q - '0'); q++; nd++; }
        if (!nd) { ps->err = "bad {n}"; return NULL; }
        if (q < ps->end && *q == ',') {
            q++;
            if (q < ps->end && *q >= '0' && *q <= '9') {
                b = 0;
                while (q < ps->end && *q >= '0' && *q <= '9') { b = b * 10 + (*q - '0'); q++; }
            } else {
                b = -1;
            }
        } else {
            b = a;
        }
        if (q >= ps->end || *q != '}') { ps->err = "missing }"; return NULL; }
        ps->p = q + 1;
        min = a; max = b;
        if (max != -1 && max < min) { ps->err = "{n,m} with m < n"; return NULL; }
        if (min > 1000 || (max != -1 && max > 1000)) { ps->err = "repeat too large"; return NULL; }
    } else {
        has = 0;
    }
    if (!has) return atom;
    rx_node_t *r = rx_new(ps, RX_REP);
    if (!r) return NULL;
    r->child = atom;
    r->min = (short)min;
    r->max = (short)max;
    r->greedy = 1;
    if (ps->p < ps->end && *ps->p == '?') { r->greedy = 0; ps->p++; }
    atom->next = NULL;
    return r;
}

static rx_node_t *rx_parse_seq(rx_parse_t *ps)
{
    rx_node_t *head = NULL, *tail = NULL;
    while (ps->p < ps->end && *ps->p != ')' && *ps->p != '|') {
        rx_node_t *atom = rx_parse_atom(ps);
        if (ps->err) return NULL;
        if (!atom) break;
        atom = rx_parse_quant(ps, atom);
        if (ps->err) return NULL;
        if (tail) tail->next = atom;
        else head = atom;
        tail = atom;
    }
    return head;
}

static rx_node_t *rx_parse_alt(rx_parse_t *ps)
{
    rx_node_t *first = rx_parse_seq(ps);
    if (ps->err) return NULL;
    if (ps->p >= ps->end || *ps->p != '|') return first;
    rx_node_t *alt = rx_new(ps, RX_ALT);
    if (!alt) return NULL;
    alt->child = first;
    rx_node_t *cur = alt;
    while (ps->p < ps->end && *ps->p == '|') {
        ps->p++;
        rx_node_t *s = rx_parse_seq(ps);
        if (ps->err) return NULL;
        rx_node_t *a = rx_new(ps, RX_ALT);
        if (!a) return NULL;
        a->child = s;
        cur->alt = a;
        cur = a;
    }
    return alt;
}

typedef struct {
    const char *start, *end;
    const char *cap[RX_MAX_GROUPS + 1][2];
    unsigned flags;
    long steps, max_steps;
} rx_state_t;

typedef int (*rx_k)(void *ud, const char *s);
static int rx_match(rx_state_t *st, const rx_node_t *n, const char *s, rx_k k, void *kc);

static int ch_eq(rx_state_t *st, unsigned char a, unsigned char b)
{
    if (a == b) return 1;
    if (st->flags & CODER_RX_ICASE) return tolower(a) == tolower(b);
    return 0;
}

static int cls_match(rx_state_t *st, const rx_node_t *n, unsigned char c)
{
    int hit = (n->cls[c >> 3] >> (c & 7)) & 1;
    if (!hit && (st->flags & CODER_RX_ICASE)) {
        unsigned char l = (unsigned char)tolower(c), u = (unsigned char)toupper(c);
        hit = ((n->cls[l >> 3] >> (l & 7)) & 1) || ((n->cls[u >> 3] >> (u & 7)) & 1);
    }
    return n->neg ? !hit : hit;
}

static int is_word(unsigned char c) { return isalnum(c) || c == '_'; }

typedef struct { const char *p; } rx_end_ctx;
static int rx_end_k(void *ud, const char *s) { ((rx_end_ctx *)ud)->p = s; return 1; }

typedef struct { rx_state_t *st; const rx_node_t *g; rx_k k; void *kc; const char *sv0, *sv1; } rx_grp_ctx;
static int rx_grp_k(void *ud, const char *s)
{
    rx_grp_ctx *c = ud;
    if (c->g->cap > 0) c->st->cap[(int)c->g->cap][1] = s;
    return rx_match(c->st, c->g->next, s, c->k, c->kc);
}

/* After a quantifier finishes, matching continues with the rest of the
 * sequence; without this, `a+$` or `colou?r` would stop at the quantifier. */
typedef struct { rx_state_t *st; const rx_node_t *after; rx_k k; void *kc; } rx_after_ctx;
static int rx_after_k(void *ud, const char *s)
{
    rx_after_ctx *a = ud;
    return rx_match(a->st, a->after, s, a->k, a->kc);
}

typedef struct { rx_state_t *st; const rx_node_t *rep; const char *from; int count; rx_k k; void *kc; } rx_rep_ctx;
static int rx_rep_k(void *ud, const char *s);
static int rx_rep(rx_state_t *st, const rx_node_t *rep, const char *s, int count, rx_k k, void *kc)
{
    if (rep->max < 0 || count < rep->max) {
        rx_rep_ctx c = { st, rep, s, count, k, kc };
        if (rep->greedy) {
            if (rx_match(st, rep->child, s, rx_rep_k, &c)) return 1;
            return count >= rep->min ? k(kc, s) : 0;
        }
        if (count >= rep->min && k(kc, s)) return 1;
        return rx_match(st, rep->child, s, rx_rep_k, &c);
    }
    return count >= rep->min ? k(kc, s) : 0;
}
static int rx_rep_k(void *ud, const char *s)
{
    rx_rep_ctx *c = ud;
    if (s == c->from) return c->count >= c->rep->min ? c->k(c->kc, s) : 0;   /* zero-width: don't loop */
    return rx_rep(c->st, c->rep, s, c->count + 1, c->k, c->kc);
}

static int rx_match(rx_state_t *st, const rx_node_t *n, const char *s, rx_k k, void *kc)
{
    if (++st->steps > st->max_steps) return 0;
    if (!n) return k(kc, s);

    switch (n->type) {
    case RX_CHAR:
        if (s < st->end && ch_eq(st, (unsigned char)*s, n->ch)) return rx_match(st, n->next, s + 1, k, kc);
        return 0;
    case RX_ANY:
        if (s < st->end && ((st->flags & CODER_RX_DOTALL) || *s != '\n')) return rx_match(st, n->next, s + 1, k, kc);
        return 0;
    case RX_CLS:
        if (s < st->end && cls_match(st, n, (unsigned char)*s)) return rx_match(st, n->next, s + 1, k, kc);
        return 0;
    case RX_BOL:
        if (s == st->start || ((st->flags & CODER_RX_MULTILINE) && s > st->start && s[-1] == '\n'))
            return rx_match(st, n->next, s, k, kc);
        return 0;
    case RX_EOL:
        if (s == st->end || ((st->flags & CODER_RX_MULTILINE) && s < st->end && *s == '\n'))
            return rx_match(st, n->next, s, k, kc);
        return 0;
    case RX_WORDB:
    case RX_WORDB_N: {
        int prev = (s > st->start) && is_word((unsigned char)s[-1]);
        int cur = (s < st->end) && is_word((unsigned char)*s);
        int b = (prev != cur);
        if (n->type == RX_WORDB_N) b = !b;
        return b ? rx_match(st, n->next, s, k, kc) : 0;
    }
    case RX_GROUP: {
        int gi = n->cap > 0 ? (int)n->cap : 0;
        rx_grp_ctx c = { st, n, k, kc, st->cap[gi][0], st->cap[gi][1] };
        if (n->cap > 0) st->cap[gi][0] = s;
        if (rx_match(st, n->child, s, rx_grp_k, &c)) return 1;
        if (n->cap > 0) { st->cap[gi][0] = c.sv0; st->cap[gi][1] = c.sv1; }
        return 0;
    }
    case RX_ALT:
        for (const rx_node_t *a = n; a; a = a->alt)
            if (rx_match(st, a->child, s, k, kc)) return 1;
        return 0;
    case RX_REP: {
        rx_after_ctx a = { st, n->next, k, kc };
        return rx_rep(st, n, s, 0, rx_after_k, &a);
    }
    }
    return 0;
}

/* Print up to `limit` bytes of [s,s+n) into out, escaping control characters;
 * returns the length written (append "..." is the caller's job). */
static size_t rx_printable(const char *s, int n, int limit, char *out, size_t cap)
{
    size_t o = 0;
    if (n > limit) n = limit;
    for (int i = 0; i < n && o + 7 < cap; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (ch == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (ch == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (ch < 0x20 || ch == 0x7f) o += (size_t)snprintf(out + o, cap - o, "\\x%02x", ch);
        else out[o++] = (char)ch;
    }
    if (o < cap) out[o] = 0;
    return o;
}

char *coder_regex(const char *pattern, size_t plen, const char *subject, size_t slen,
                  unsigned flags, int max_matches, char *out, size_t cap)
{
    if (plen == 0) { snprintf(out, cap, "Type a pattern first"); return out; }
    if (plen > 256) { snprintf(out, cap, "Pattern too long (max 256 characters)"); return out; }
    if (max_matches < 1) max_matches = 1;
    if (max_matches > 100) max_matches = 100;

    rx_parse_t ps;
    ps.p = pattern;
    ps.end = pattern + plen;
    ps.cap_pool = (int)(plen + plen / 2 + 16);
    ps.pool = malloc((size_t)ps.cap_pool * sizeof(rx_node_t));
    if (!ps.pool) { snprintf(out, cap, "Out of memory"); return out; }
    ps.used = 0;
    ps.ncap = 0;
    ps.err = NULL;
    rx_node_t *root = rx_parse_alt(&ps);
    if (ps.err || ps.p < ps.end) {
        snprintf(out, cap, "Regex error: %s", ps.err ? ps.err : "unmatched )");
        free(ps.pool);
        return out;
    }

    rx_state_t st;
    memset(&st, 0, sizeof(st));
    st.start = subject;
    st.end = subject + slen;
    st.flags = flags;

    long budget = 2000000;
    long pos = 0;
    int count = 0, truncated = 0;
    size_t base = 16, o = base;                    /* room for the header backfill */
    while (pos <= (long)slen && count < max_matches && o + 128 < cap && budget > 0) {
        memset(st.cap, 0, sizeof(st.cap));
        st.steps = 0;
        st.max_steps = budget;
        st.cap[0][0] = subject + pos;
        rx_end_ctx ec = { NULL };
        if (!rx_match(&st, root, subject + pos, rx_end_k, &ec)) { budget -= st.steps; pos++; continue; }
        budget -= st.steps;
        st.cap[0][1] = ec.p;
        int ms = (int)(st.cap[0][0] - subject), me = (int)(st.cap[0][1] - subject);
        char buf[96];
        int mlen = me - ms;
        rx_printable(subject + ms, mlen, 60, buf, sizeof(buf));
        o += (size_t)snprintf(out + o, cap - o, "[%d] %d-%d  \"%s%s\"\n", count + 1, ms, me, buf, mlen > 60 ? "..." : "");
        for (int g = 1; g <= ps.ncap; g++) {
            if (!st.cap[g][0]) continue;
            int gs = (int)(st.cap[g][0] - subject), ge = (int)(st.cap[g][1] - subject);
            char gb[64];
            rx_printable(subject + gs, ge - gs, 40, gb, sizeof(gb));
            o += (size_t)snprintf(out + o, cap - o, "      $%d %d-%d  \"%s%s\"\n", g, gs, ge, gb, ge - gs > 40 ? "..." : "");
        }
        count++;
        if (me == ms) pos++;
        else pos = me;
    }
    if (count == 0) {
        snprintf(out, cap, "No match.");
        free(ps.pool);
        return out;
    }
    truncated = (pos <= (long)slen);   /* stopped early (cap, budget or max matches) */

    char head[48];
    int hn = snprintf(head, sizeof(head), "%d match%s%s\n\n", count, count == 1 ? "" : "es",
                      truncated ? " (more not shown)" : "");
    size_t body = o - base;
    memmove(out + hn, out + base, body);
    memcpy(out, head, (size_t)hn);
    out[hn + body] = 0;
    free(ps.pool);
    return out;
}
