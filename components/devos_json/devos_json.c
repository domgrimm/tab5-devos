/* devos_json: minimal JSON reader. See devos_json.h. */
#include "devos_json.h"
#include <ctype.h>
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

/* End of the value starting at p (string, object, array or scalar). */
static const char *value_end(const char *p, const char *end)
{
    if (p >= end) return NULL;
    if (*p == '{' || *p == '[') return devos_json_span(p, end);
    if (*p == '"') {
        p++;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) p++;
            p++;
        }
        return p < end ? p + 1 : NULL;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

const char *devos_json_member(const char *p, const char *end, const char *key)
{
    if (!p || p >= end || *p != '{') return NULL;
    size_t klen = strlen(key);
    p++;
    for (;;) {
        p = js_ws(p, end);
        if (p >= end || *p != '"') return NULL;          /* '}' or malformed */
        const char *k = p + 1;
        const char *kend = value_end(p, end);
        if (!kend) return NULL;
        bool match = (size_t)(kend - 1 - k) == klen && memcmp(k, key, klen) == 0;
        p = js_ws(kend, end);
        if (p >= end || *p != ':') return NULL;
        p = js_ws(p + 1, end);
        if (match) return p;
        p = value_end(p, end);
        if (!p) return NULL;
        p = js_ws(p, end);
        if (p >= end || *p != ',') return NULL;
        p++;
    }
}

int devos_json_member_str(const char *obj, const char *end, const char *key, char *out, size_t cap)
{
    const char *v = devos_json_member(obj, end, key);
    if (!v || *v != '"') return -1;
    return devos_json_parse_str(v, end, out, cap) ? 0 : -1;
}

bool devos_json_member_num(const char *obj, const char *end, const char *key, double *out)
{
    const char *v = devos_json_member(obj, end, key);
    if (!v) return false;
    if (*v == '"') v++;
    char *stop = NULL;
    double d = strtod(v, &stop);
    if (stop == v || stop > end) return false;
    if (out) *out = d;
    return true;
}

/* ---- narrow dot-path extraction (Jobs json_get) ---- */
static bool js_array_nth(const char *p, const char *end, long idx, const char **vstart, const char **vend)
{
    if (p >= end || *p != '[') return false;
    p++;
    long i = 0;
    for (;;) {
        p = js_ws(p, end);
        if (p >= end || *p == ']') return false;
        const char *q = value_end(p, end);
        if (!q) return false;
        if (i == idx) { *vstart = p; *vend = q; return true; }
        i++;
        p = js_ws(q, end);
        if (p < end && *p == ',') { p++; continue; }
        return false;
    }
}

bool devos_json_path(const char *js, size_t len, const char *path, devos_json_val_t *out)
{
    if (!js || !path || !out || len == 0) return false;
    if (strlen(path) > 128) return false;
    memset(out, 0, sizeof(*out));
    const char *end = js + len;
    const char *p = js_ws(js, end);
    const char *ps = path;
    while (*ps) {
        if (*ps == '[') {
            while (*ps == '[') {
                ps++;
                if (*ps < '0' || *ps > '9') return false;
                long idx = 0;
                while (*ps >= '0' && *ps <= '9') {
                    idx = idx * 10 + (*ps - '0');
                    if (idx > 100000) return false;
                    ps++;
                }
                if (*ps != ']') return false;
                ps++;
                const char *v, *ve;
                if (!js_array_nth(p, end, idx, &v, &ve)) return false;
                p = v;
            }
            if (*ps == '.') { ps++; continue; }
            if (*ps) return false;
        } else {
            char key[64];
            size_t kl = 0;
            while (*ps && *ps != '.' && *ps != '[') {
                char c = *ps;
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '-'))
                    return false;
                if (kl + 1 >= sizeof(key)) return false;
                key[kl++] = c;
                ps++;
            }
            if (kl == 0) return false;
            key[kl] = '\0';
            if (p >= end || *p != '{') return false;
            const char *v = devos_json_member(p, end, key);
            if (!v) return false;
            p = v;
            if (*ps == '.') { ps++; continue; }
            if (*ps == '[') continue;
            if (*ps) return false;
        }
    }
    /* classify the resolved value; objects and arrays keep their raw JSON */
    if (p >= end) return false;
    if (*p == '{' || *p == '[') {
        const char *e = devos_json_span(p, end);
        if (!e) return false;
        out->kind = DEVOS_JSON_RAW;
        out->s = p;
        out->len = (uint32_t)(e - p);
        return true;
    }
    if (*p == '"') {
        const char *e = value_end(p, end);
        if (!e) return false;
        out->kind = DEVOS_JSON_STR;
        out->s = p;
        out->len = (uint32_t)(e - p);
        return true;
    }
    size_t rem = (size_t)(end - p);
    if (rem >= 4 && strncmp(p, "true", 4) == 0)  { out->kind = DEVOS_JSON_BOOL; out->b = true;  return true; }
    if (rem >= 5 && strncmp(p, "false", 5) == 0) { out->kind = DEVOS_JSON_BOOL; out->b = false; return true; }
    if (rem >= 4 && strncmp(p, "null", 4) == 0)  { out->kind = DEVOS_JSON_NULL; return true; }
    {
        char tmp[48];
        size_t n = rem < sizeof(tmp) - 1 ? rem : sizeof(tmp) - 1;
        memcpy(tmp, p, n);
        tmp[n] = '\0';
        bool integral = !strchr(tmp, '.') && !strchr(tmp, 'e') && !strchr(tmp, 'E');
        char *stop = NULL;
        if (integral) {
            long long iv = strtoll(tmp, &stop, 10);
            if (stop != tmp) { out->kind = DEVOS_JSON_INT; out->i = iv; return true; }
        }
        double d = strtod(tmp, &stop);
        if (stop != tmp) { out->kind = DEVOS_JSON_NUM; out->n = d; return true; }
    }
    return false;
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

/* ---- pretty printer ---- */
typedef struct {
    char *out;
    size_t cap, n;
    bool full;
} jp_buf_t;

static void jp_put(jp_buf_t *b, char c)
{
    if (b->n + 1 >= b->cap) { b->full = true; return; }
    b->out[b->n++] = c;
}

static void jp_newline(jp_buf_t *b, int depth)
{
    jp_put(b, '\n');
    for (int i = 0; i < depth * 2; i++) jp_put(b, ' ');
}

size_t devos_json_pretty(const char *src, size_t len, char *out, size_t cap)
{
    if (!src || !out || cap < 3) return 0;
    const char *p = src, *end = src + len;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p >= end || (*p != '{' && *p != '[')) return 0;
    jp_buf_t b = { out, cap, 0, false };
    char stack[64];
    int depth = 0;
    bool after_open = false;     /* just emitted { or [ */
    for (; p < end; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (depth == 0 && b.n > 0) return 0;     /* trailing garbage */
        if (c == '"') {
            if (after_open) jp_newline(&b, depth);
            after_open = false;
            jp_put(&b, '"');
            for (p++; p < end && *p != '"'; p++) {
                if ((unsigned char)*p < 0x20) return 0;
                jp_put(&b, *p);
                if (*p == '\\' && p + 1 < end) jp_put(&b, *++p);
            }
            if (p >= end) return 0;
            jp_put(&b, '"');
        } else if (c == '{' || c == '[') {
            if (after_open) jp_newline(&b, depth);
            if (depth >= (int)sizeof(stack)) return 0;
            stack[depth++] = c;
            jp_put(&b, c);
            after_open = true;
        } else if (c == '}' || c == ']') {
            if (!depth || stack[depth - 1] != (c == '}' ? '{' : '[')) return 0;
            depth--;
            if (!after_open) jp_newline(&b, depth);  /* {} and [] stay on one line */
            after_open = false;
            jp_put(&b, c);
        } else if (c == ',') {
            if (!depth || after_open) return 0;
            jp_put(&b, ',');
            jp_newline(&b, depth);
        } else if (c == ':') {
            if (!depth || stack[depth - 1] != '{' || after_open) return 0;
            jp_put(&b, ':');
            jp_put(&b, ' ');
        } else if ((c >= '0' && c <= '9') || c == '-' || c == 't' || c == 'f' || c == 'n') {
            if (!depth) return 0;
            if (after_open) jp_newline(&b, depth);
            after_open = false;
            /* number or literal: copy the run */
            while (p < end && (isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '+')) jp_put(&b, *p++);
            p--;
        } else {
            return 0;
        }
        if (b.full) return 0;
    }
    if (depth != 0 || b.full) return 0;
    out[b.n] = '\0';
    return b.n;
}
