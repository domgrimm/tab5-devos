/* devos_json: minimal JSON reader. See devos_json.h. */
#include "devos_json.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *js_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

const char *devos_json_parse_str(const char *p, const char *end, char *out,
                                 size_t cap)
{
    if (p >= end || *p != '"') return NULL;
    p++;
    size_t o = 0;
    while (p < end && *p != '"') {
        char c = *p;
        if (c == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                /* ponytail: BMP only, no surrogate math; enough for chat */
                unsigned v = 0;
                for (int k = 0; k < 4 && p + 1 + k < end; k++) {
                    char h = p[1 + k];
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                    else break;
                }
                p += 4;
                if (v < 0x80) c = (char)v;
                else if (v < 0x800 && o + 2 < cap) {
                    if (o + 1 < cap) out[o++] = (char)(0xC0 | (v >> 6));
                    c = (char)(0x80 | (v & 0x3F));
                } else if (o + 3 < cap) {
                    if (o + 1 < cap) out[o++] = (char)(0xE0 | (v >> 12));
                    if (o + 1 < cap) out[o++] = (char)(0x80 | ((v >> 6) & 0x3F));
                    c = (char)(0x80 | (v & 0x3F));
                } else {
                    c = '?';
                }
                break;
            }
            default: break; /* keep the char after backslash as-is */
            }
        }
        if (o + 1 < cap) out[o++] = c;
        p++;
    }
    if (p >= end) return NULL;
    out[o] = '\0';
    return p + 1;
}

const char *devos_json_find_key(const char *p, const char *end,
                                const char *key)
{
    size_t klen = strlen(key);
    while (p < end) {
        if (*p == '"') {
            if ((size_t)(end - p) > klen + 1 && memcmp(p + 1, key, klen) == 0 &&
                p[1 + klen] == '"') {
                const char *q = js_ws(p + 2 + klen, end);
                if (q < end && *q == ':') return js_ws(q + 1, end);
            }
            /* skip the string */
            p++;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) p++;
                p++;
            }
            if (p < end) p++;
        } else {
            p++;
        }
    }
    return NULL;
}

const char *devos_json_span(const char *p, const char *end)
{
    if (p >= end || (*p != '{' && *p != '[')) return NULL;
    char open = *p;
    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    while (p < end) {
        if (*p == '"') {
            p++;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) p++;
                p++;
            }
            if (p < end) p++;
        } else {
            if (*p == open) depth++;
            else if (*p == close && --depth == 0) return p + 1;
            p++;
        }
    }
    return NULL;
}

int devos_json_get_str(const char *js, size_t len, const char *key, char *out,
                       size_t cap)
{
    const char *end = js + len;
    const char *v = devos_json_find_key(js, end, key);
    if (!v || v >= end || *v != '"') return -1;
    return devos_json_parse_str(v, end, out, cap) ? 0 : -1;
}

int devos_json_get_int(const char *js, size_t len, const char *key, int *out)
{
    const char *end = js + len;
    const char *v = devos_json_find_key(js, end, key);
    if (!v || v >= end) return -1;
    char *stop = NULL;
    long val = strtol(v, &stop, 10);
    if (stop == v || val < -2147483647L - 1 || val > 2147483647L) return -1;
    if (out) *out = (int)val;
    return 0;
}

void devos_json_array_each(const char *p, size_t len,
                           void (*cb)(const char *, size_t, void *), void *ud)
{
    const char *end = p + len;
    if (!p || p >= end || *p != '[') return;
    p++;
    while (p < end) {
        p = js_ws(p, end);
        if (p < end && *p == ']') return;
        if (p < end && *p == ',') { p++; continue; }
        const char *q = p;
        if (*q == '{' || *q == '[') {
            q = devos_json_span(p, end);
            if (!q) return;
        } else {
            while (q < end && *q != ',' && *q != ']') q++;
        }
        cb(p, (size_t)(q - p), ud);
        p = q;
    }
}

size_t devos_json_escape(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (const char *s = src; *s && o + 1 < cap; s++) {
        char c = *s;
        const char *esc = NULL;
        char tmp[7];
        switch (c) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) {
                snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                esc = tmp;
            }
            break;
        }
        if (esc) {
            size_t el = strlen(esc);
            if (o + el + 1 > cap) break;
            memcpy(dst + o, esc, el);
            o += el;
        } else {
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
    return o;
}
