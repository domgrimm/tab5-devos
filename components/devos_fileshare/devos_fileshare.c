/* devos_fileshare: see devos_fileshare.h. */
#include "devos_fileshare.h"
#include "devos_config.h"
#include "devos_clipboard.h"   /* the Universal Clipboard (no LVGL) */
#include "devos_crypto.h"
#include "devos_json.h"
#include "devos_net.h"
#include "devos_storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
static const char *TAG = "devos_fileshare";
static SemaphoreHandle_t s_mx;
#define LOCK()   do { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_mx) xSemaphoreGive(s_mx); } while (0)
#define LOG(...) ESP_LOGI(TAG, __VA_ARGS__)
static void sleep_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static void *big_calloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    return p ? p : calloc(1, n);
}
/* The SD driver DMAs straight from PSRAM only if the buffer is cache-line
 * aligned; otherwise it copies one 512-byte sector at a time. */
static void *io_alloc(size_t n) { return heap_caps_aligned_calloc(128, 1, n, MALLOC_CAP_SPIRAM); }
static void io_free(void *p) { heap_caps_free(p); }
#else
#include <pthread.h>
#include <sys/socket.h>
static pthread_mutex_t s_mx_sim = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&s_mx_sim)
#define UNLOCK() pthread_mutex_unlock(&s_mx_sim)
#define LOG(fmt, ...) printf("[fileshare] " fmt "\n", ##__VA_ARGS__)
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
static void *big_calloc(size_t n) { return calloc(1, n); }
static void *io_alloc(size_t n) { return calloc(1, n); }
static void io_free(void *p) { free(p); }
#endif

extern const char devos_fileshare_page[];

#define ROOT           TAB5_SD_MOUNT_POINT
#define HDR_MAX        8192            /* request line + headers */
#define IO_CHUNK       (32 * 1024)     /* file <-> socket: two FAT allocation units */
#define REL_CAP        256             /* card-relative path, "/notes/a.md" */
#define FULL_CAP       (sizeof(ROOT) + REL_CAP + 8)
#define TREE_CAP       1024            /* deleting a folder: paths below it */
#define HEADER_WAIT_MS 10000           /* for the request to arrive */
#define IO_TIMEOUT_MS  20000           /* a stalled transfer gives up */
#define WORKER_STACK   6144            /* buffers live in req_t (PSRAM) */

static devos_fileshare_status_t s_st;  /* LOCK */
static bool s_want;                    /* switched on (LOCK) */
static bool s_listener;                /* the listener exists (LOCK) */
static volatile bool s_running;        /* listening: devos_fileshare_running() */

typedef struct {
    int fd;
    char peer[16];
    char hdr[HDR_MAX + 1];
    size_t hdr_len, body_off;          /* bytes read; where the body starts */
    const char *method, *target;
    char *query;
    bool has_len, chunked, expect_continue, csrf_ok, auth_given, auth_ok;
    uint64_t content_len;
    char rel[2][REL_CAP], full[2][FULL_CAP];
    char tree[TREE_CAP];
    char esc[REL_CAP * 6 + 8];         /* a JSON-escaped name */
    char head[2048];                   /* response header */
    char *io;                          /* IO_CHUNK, aligned (io_alloc) */
} req_t;

/* ------------------------------------------------------------------ status */
static bool want(void)
{
    LOCK();
    bool w = s_want;
    UNLOCK();
    return w;
}

static void fmt_size(uint64_t b, char *out, size_t cap)
{
    if (b >= 1024ull * 1024 * 1024) snprintf(out, cap, "%.1f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024 * 1024) snprintf(out, cap, "%.1f MB", b / (1024.0 * 1024));
    else if (b >= 1024) snprintf(out, cap, "%llu KB", (unsigned long long)((b + 1023) / 1024));
    else snprintf(out, cap, "%llu bytes", (unsigned long long)b);
}

/* The latest thing done, for the Settings panel. */
static void note(const req_t *r, const char *fmt, const char *a, const char *b)
{
    LOCK();
    snprintf(s_st.last, sizeof(s_st.last), fmt, a, b ? b : "");
    snprintf(s_st.last_client, sizeof(s_st.last_client), "%s", r->peer);
    s_st.last_time = (int64_t)time(NULL);
    UNLOCK();
}

/* ------------------------------------------------------------------ helpers */
static char *find_end_of_headers(char *buf, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') return buf + i;
    }
    return NULL;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Query parameter `key`, %-decoded (RFC 3986: '+' is a plus). false if it's
 * missing, too long or decodes to a NUL. */
static bool query_param(const char *q, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = q; p && *p;) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        if ((size_t)(end - p) >= kl && strncmp(p, key, kl) == 0 && (p[kl] == '=' || p + kl == end)) {
            const char *v = p + kl + (p[kl] == '=' ? 1 : 0);
            size_t o = 0;
            while (v < end) {
                char c = *v++;
                if (c == '%') {
                    int h = end - v >= 2 ? hexval(v[0]) : -1, l = end - v >= 2 ? hexval(v[1]) : -1;
                    if (h < 0 || l < 0) return false;
                    c = (char)(h * 16 + l);
                    v += 2;
                    if (c == '\0') return false;
                }
                if (o + 1 >= cap) return false;
                out[o++] = c;
            }
            out[o] = '\0';
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

static bool flag_param(const char *q, const char *key)
{
    char v[8];
    return query_param(q, key, v, sizeof(v)) && (v[0] == '1' || v[0] == 't' || v[0] == 'y');
}

/* A card-relative path ("/a/b"; "/" = the card) from what the client sent:
 * empty and "." parts dropped; "..", other all-dot names, control characters
 * and the characters FAT can't store refused. */
static bool clean_path(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - s);
        if (n == 1 && s[0] == '.') continue;
        bool dots = true;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c < 0x20 || c == 0x7f || strchr("\\:*?\"<>|", c)) return false;
            if (c != '.' && c != ' ') dots = false;
        }
        if (dots) return false;
        if (o + 1 + n >= cap) return false;
        out[o++] = '/';
        memcpy(out + o, s, n);
        o += n;
    }
    if (o == 0) {
        if (cap < 2) return false;
        out[o++] = '/';
    }
    out[o] = '\0';
    return true;
}

/* needle (lower case) anywhere in s, ignoring case */
static bool contains_nocase(const char *s, const char *needle)
{
    size_t n = strlen(needle);
    for (; *s; s++) {
        if (strncasecmp(s, needle, n) == 0) return true;
    }
    return false;
}

static bool is_root(const char *rel) { return rel[0] == '/' && rel[1] == '\0'; }

static void full_path(const char *rel, char *out, size_t cap)
{
    snprintf(out, cap, "%s%s", ROOT, is_root(rel) ? "" : rel);
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Standard base64 -> bytes (NUL-terminated). -1 if malformed or too long. */
static int b64_decode(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *in && *in != '='; in++) {
        int v = b64val(*in);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o + 1 >= cap) return -1;
            out[o++] = (char)((acc >> bits) & 0xFF);
        }
    }
    out[o] = '\0';
    return (int)o;
}

/* "Basic base64(user:password)": any user name, the current password. */
static bool auth_ok(const char *value)
{
    if (strncasecmp(value, "Basic ", 6) != 0) return false;
    char cred[96];
    if (b64_decode(value + 6, cred, sizeof(cred)) < 0) return false;
    const char *pw = strchr(cred, ':');
    char cur[sizeof(s_st.password)];
    LOCK();
    memcpy(cur, s_st.password, sizeof(cur));
    UNLOCK();
    size_t n = strlen(cur);
    bool ok = pw && n > 0 && strlen(pw + 1) == n && devos_ct_equal(pw + 1, cur, n);
    devos_wipe(cred, sizeof(cred));
    devos_wipe(cur, sizeof(cur));
    return ok;
}

/* ------------------------------------------------------------------ responses */
static const char *reason(int code)
{
    switch (code) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    case 507: return "Insufficient Storage";
    default:  return "Internal Server Error";
    }
}

/* Status line + headers. len < 0: no Content-Length (the body ends when the
 * connection closes). extra: more header lines, each ending in \r\n. */
static bool send_head(req_t *r, int code, const char *type, long long len, const char *extra)
{
    int n = snprintf(r->head, sizeof(r->head),
                     "HTTP/1.1 %d %s\r\nConnection: close\r\nCache-Control: no-store\r\n"
                     "X-Content-Type-Options: nosniff\r\n",
                     code, reason(code));
    if (type && n < (int)sizeof(r->head)) {
        n += snprintf(r->head + n, sizeof(r->head) - (size_t)n, "Content-Type: %s\r\n", type);
    }
    if (len >= 0 && n < (int)sizeof(r->head)) {
        n += snprintf(r->head + n, sizeof(r->head) - (size_t)n, "Content-Length: %lld\r\n", len);
    }
    if (extra && n < (int)sizeof(r->head)) {
        n += snprintf(r->head + n, sizeof(r->head) - (size_t)n, "%s", extra);
    }
    if (n < (int)sizeof(r->head)) n += snprintf(r->head + n, sizeof(r->head) - (size_t)n, "\r\n");
    if (n >= (int)sizeof(r->head)) return false;
    return devos_net_socket_send_all(r->fd, r->head, (size_t)n) == 0;
}

/* A short plain-text answer; the web page shows it as the error. */
static void send_text(req_t *r, int code, const char *msg)
{
    char body[160];
    int n = snprintf(body, sizeof(body), "%s\n", msg);
    if (n < 0 || n >= (int)sizeof(body)) n = (int)sizeof(body) - 1;
    if (send_head(r, code, "text/plain; charset=utf-8", n, NULL)) devos_net_socket_send_all(r->fd, body, (size_t)n);
}

static void send_ok(req_t *r, int code)
{
    static const char ok[] = "{\"ok\":true}\n";
    if (send_head(r, code, "application/json", (long long)(sizeof(ok) - 1), NULL)) {
        devos_net_socket_send_all(r->fd, ok, sizeof(ok) - 1);
    }
}

/* {"ok":true,"length":N} - a change that reports how much it kept. */
static void send_ok_len(req_t *r, int code, size_t len)
{
    char body[48];
    int n = snprintf(body, sizeof(body), "{\"ok\":true,\"length\":%u}\n", (unsigned)len);
    if (send_head(r, code, "application/json", n, NULL)) devos_net_socket_send_all(r->fd, body, (size_t)n);
}

/* ------------------------------------------------------------------ files */
static bool ext_is(const char *name, const char *list)
{
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return false;
    size_t el = strlen(dot + 1);
    for (const char *p = list; *p;) {
        const char *sp = strchr(p, ' ');
        size_t n = sp ? (size_t)(sp - p) : strlen(p);
        if (n == el && strncasecmp(p, dot + 1, n) == 0) return true;
        p += n + (sp ? 1 : 0);
    }
    return false;
}

/* What the browser may show inline. Anything that could run script (HTML,
 * SVG) is shown as text; files also get a sandbox CSP. */
static const char *mime_type(const char *name)
{
    if (ext_is(name, "png")) return "image/png";
    if (ext_is(name, "jpg jpeg")) return "image/jpeg";
    if (ext_is(name, "gif")) return "image/gif";
    if (ext_is(name, "webp")) return "image/webp";
    if (ext_is(name, "bmp")) return "image/bmp";
    if (ext_is(name, "wav")) return "audio/wav";
    if (ext_is(name, "mp3")) return "audio/mpeg";
    if (ext_is(name, "pdf")) return "application/pdf";
    if (ext_is(name, "txt md markdown log conf cfg ini json csv tsv yml yaml toml xml html htm svg sh py c h "
                     "cpp hpp js ts css go rs pub pem known_hosts")) {
        return "text/plain; charset=utf-8";
    }
    if (!strrchr(name, '.')) return "text/plain; charset=utf-8";   /* known_hosts, README */
    return "application/octet-stream";
}

/* RFC 5987 filename*: everything but attr-chars %-encoded. */
static void pct_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            strchr("!#$&+-.^_`|~", *p)) {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }
    out[o] = '\0';
}

/* mkdir -p for the folders above full (a path under ROOT). */
static bool make_parents(char *full)
{
    size_t root = strlen(ROOT);
    if (strlen(full) <= root) return true;
    for (char *p = full + root + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        struct stat st;
        bool ok = stat(full, &st) == 0 ? S_ISDIR(st.st_mode) : mkdir(full, 0755) == 0;
        *p = '/';
        if (!ok) return false;
    }
    return true;
}

/* rm -rf path (a buffer of cap bytes, extended in place while descending).
 * One entry at a time: never deletes from a folder while reading it. */
static int remove_tree(char *path, size_t cap, int depth)
{
    if (depth > 32) return -1;
    size_t len = strlen(path);
    for (;;) {
        if (!want()) return -1;
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *e;
        bool found = false;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            size_t nl = strlen(e->d_name);
            if (len + 1 + nl >= cap) {
                closedir(d);
                return -1;
            }
            path[len] = '/';
            memcpy(path + len + 1, e->d_name, nl + 1);
            found = true;
            break;
        }
        closedir(d);
        if (!found) break;
        struct stat st;
        int rc = (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? remove_tree(path, cap, depth + 1) : unlink(path);
        path[len] = '\0';
        if (rc != 0) return -1;
    }
    return rmdir(path);
}

/* ------------------------------------------------------------------ handlers */
static void do_page(req_t *r)
{
    size_t len = strlen(devos_fileshare_page);
    static const char csp[] =
        "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
        "connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'\r\n";
    if (send_head(r, 200, "text/html; charset=utf-8", (long long)len, csp)) {
        devos_net_socket_send_all(r->fd, devos_fileshare_page, len);
    }
}

static void do_list(req_t *r)
{
    const char *rel = r->rel[0], *full = r->full[0];
    DIR *d = opendir(full);
    if (!d) {
        struct stat st;
        bool exists = stat(full, &st) == 0;
        send_text(r, exists ? 400 : 404, exists ? "That's a file, not a folder" : "No such folder");
        return;
    }
    if (!send_head(r, 200, "application/json; charset=utf-8", -1, NULL)) {
        closedir(d);
        return;
    }
    devos_json_escape(rel, r->esc, sizeof(r->esc));
    uint64_t total = (uint64_t)devos_storage_get_total_mb() << 20, avail = (uint64_t)devos_storage_get_free_mb() << 20;
    size_t n = (size_t)snprintf(r->io, IO_CHUNK, "{\"path\":\"%s\",\"free\":%llu,\"total\":%llu,\"entries\":[",
                                r->esc, (unsigned long long)avail, (unsigned long long)total);
    bool first = true, ok = true;
    struct dirent *e;
    while (ok && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(r->tree, sizeof(r->tree), "%s/%s", full, e->d_name);
        struct stat st;
        if (stat(r->tree, &st) != 0) continue;
        devos_json_escape(e->d_name, r->esc, sizeof(r->esc));
        if (n + strlen(r->esc) + 96 > IO_CHUNK) {
            ok = devos_net_socket_send_all(r->fd, r->io, n) == 0 && want();
            n = 0;
        }
        bool dir = S_ISDIR(st.st_mode);
        n += (size_t)snprintf(r->io + n, IO_CHUNK - n, "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%llu,\"mtime\":%lld}",
                              first ? "" : ",", r->esc, dir ? "true" : "false",
                              (unsigned long long)(dir ? 0 : st.st_size), (long long)st.st_mtime);
        first = false;
    }
    closedir(d);
    if (!ok) return;
    n += (size_t)snprintf(r->io + n, IO_CHUNK - n, "]}\n");
    devos_net_socket_send_all(r->fd, r->io, n);
}

static void do_download(req_t *r, bool dl)
{
    const char *rel = r->rel[0], *full = r->full[0];
    struct stat st;
    if (stat(full, &st) != 0) {
        send_text(r, 404, "No such file");
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        send_text(r, 400, "That's a folder");
        return;
    }
    FILE *f = fopen(full, "rb");
    if (!f) {
        send_text(r, 500, "Couldn't open the file");
        return;
    }
    setvbuf(f, NULL, _IONBF, 0);            /* read straight into r->io */
    char *name = r->esc, *extra = r->tree;  /* free while downloading */
    pct_encode(base_name(rel), name, sizeof(r->esc));
    snprintf(extra, sizeof(r->tree), "Content-Security-Policy: sandbox\r\nContent-Disposition: %s; filename*=UTF-8''%.900s\r\n",
             dl ? "attachment" : "inline", name);
    uint64_t sent = 0;
    if (send_head(r, 200, dl ? "application/octet-stream" : mime_type(rel), (long long)st.st_size, extra)) {
        size_t got;
        while (want() && (got = fread(r->io, 1, IO_CHUNK, f)) > 0) {
            if (devos_net_socket_send_all(r->fd, r->io, got) != 0) break;
            sent += got;
        }
    }
    fclose(f);
    LOCK();
    s_st.downloads++;
    s_st.bytes_out += sent;
    UNLOCK();
    char sz[24];
    fmt_size(sent, sz, sizeof(sz));
    note(r, "Downloaded %.96s (%s)", rel, sz);
}

static void do_upload(req_t *r)
{
    const char *rel = r->rel[0];
    char *full = r->full[0];
    if (is_root(rel)) {
        send_text(r, 400, "Give the file a name");
        return;
    }
    if (r->chunked || !r->has_len) {
        send_text(r, 411, "Content-Length needed");
        return;
    }
    if (r->content_len > 0xFFFFFFFFull) {
        send_text(r, 413, "Files over 4 GB don't fit on the card (FAT32)");
        return;
    }
    struct stat st;
    bool existed = stat(full, &st) == 0;
    if (existed && S_ISDIR(st.st_mode)) {
        send_text(r, 409, "A folder has that name");
        return;
    }
    uint64_t avail = (uint64_t)devos_storage_get_free_mb() << 20;
    if (devos_storage_get_total_mb() > 0 && r->content_len > avail + (existed ? (uint64_t)st.st_size : 0)) {
        send_text(r, 507, "Not enough space on the SD card");
        return;
    }
    if (!make_parents(full)) {
        send_text(r, 409, "Couldn't make its folder (a file has that name?)");
        return;
    }
    char *tmp = r->full[1];
    if (snprintf(tmp, FULL_CAP, "%s.part~", full) >= (int)FULL_CAP) {
        send_text(r, 400, "Name too long");
        return;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        send_text(r, 500, "Couldn't create the file");
        return;
    }
    setvbuf(f, NULL, _IONBF, 0);            /* write r->io straight to the card */
    if (r->expect_continue) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        devos_net_socket_send_all(r->fd, cont, sizeof(cont) - 1);
    }

    /* Collect whole chunks before writing: every write then starts at an
     * aligned buffer and file position, so the card gets multi-block DMA. */
    uint64_t got = 0, next_note = 256 * 1024;
    size_t fill = r->hdr_len - r->body_off;    /* body bytes that came with the headers */
    if (fill > r->content_len) fill = (size_t)r->content_len;
    memcpy(r->io, r->hdr + r->body_off, fill);  /* fill <= HDR_MAX < IO_CHUNK */
    got = fill;
    LOCK();
    s_st.bytes_in += fill;
    UNLOCK();
    bool cut = false, wfail = false;
    char total[24], sz[24];
    fmt_size(r->content_len, total, sizeof(total));
    for (;;) {
        bool done = got >= r->content_len;
        if (fill == IO_CHUNK || (done && fill)) {
            if (fwrite(r->io, 1, fill, f) != fill) {
                wfail = true;
                break;
            }
            fill = 0;
            continue;
        }
        if (done) break;
        if (!want()) {
            cut = true;
            break;
        }
        uint64_t left = r->content_len - got;
        size_t room = IO_CHUNK - fill;
        int n = devos_net_socket_recv(r->fd, r->io + fill, left < room ? (size_t)left : room, 0);
        if (n <= 0) {
            cut = true;
            break;
        }
        fill += (size_t)n;
        got += (uint64_t)n;
        LOCK();
        s_st.bytes_in += (uint64_t)n;
        UNLOCK();
        if (got >= next_note) {
            next_note = got + 256 * 1024;
            fmt_size(got, sz, sizeof(sz));
            char msg[64];
            snprintf(msg, sizeof(msg), "%s of %s", sz, total);
            note(r, "Receiving %.80s - %s", rel, msg);
        }
    }
    if (fclose(f) != 0) wfail = true;
    if (cut || wfail) {
        unlink(tmp);
        if (cut) send_text(r, 400, "Upload cut off");
        else send_text(r, 507, "Couldn't write to the SD card (full?)");
        note(r, "Upload of %.96s failed%s", rel, "");
        return;
    }
    if (existed) unlink(full);              /* FAT can't rename over a file */
    if (rename(tmp, full) != 0) {
        unlink(tmp);
        send_text(r, 500, "Couldn't save the file");
        return;
    }
    LOCK();
    s_st.uploads++;
    UNLOCK();
    devos_storage_refresh_stats();
    send_ok(r, existed ? 200 : 201);
    fmt_size(got, sz, sizeof(sz));
    note(r, "Uploaded %.96s (%s)", rel, sz);
}

static void do_delete(req_t *r)
{
    const char *rel = r->rel[0], *full = r->full[0];
    if (is_root(rel)) {
        send_text(r, 400, "Can't delete the whole card");
        return;
    }
    struct stat st;
    if (stat(full, &st) != 0) {
        send_text(r, 404, "It's not there (already deleted?)");
        return;
    }
    int rc;
    if (S_ISDIR(st.st_mode)) {
        if (flag_param(r->query, "recursive")) {
            snprintf(r->tree, sizeof(r->tree), "%s", full);
            rc = remove_tree(r->tree, sizeof(r->tree), 0);
        } else {
            rc = rmdir(full);
            if (rc != 0) {
                send_text(r, 409, "The folder isn't empty");
                return;
            }
        }
    } else {
        rc = unlink(full);
    }
    if (rc != 0) {
        send_text(r, 500, "Couldn't delete it (some of it may be gone)");
        return;
    }
    LOCK();
    s_st.deletes++;
    UNLOCK();
    devos_storage_refresh_stats();
    send_ok(r, 200);
    note(r, "Deleted %.100s%s", rel, NULL);
}

static void do_mkdir(req_t *r)
{
    const char *rel = r->rel[0];
    char *full = r->full[0];
    struct stat st;
    if (is_root(rel) || stat(full, &st) == 0) {
        send_text(r, 409, "That name is taken");
        return;
    }
    if (!make_parents(full) || mkdir(full, 0755) != 0) {
        send_text(r, 500, "Couldn't make the folder");
        return;
    }
    send_ok(r, 201);
    note(r, "New folder %.100s%s", rel, NULL);
}

static void do_rename(req_t *r)
{
    const char *from = r->rel[0], *to = r->rel[1];
    char *ffrom = r->full[0], *fto = r->full[1];
    if (is_root(from) || is_root(to)) {
        send_text(r, 400, "Can't rename the card");
        return;
    }
    struct stat st;
    if (stat(ffrom, &st) != 0) {
        send_text(r, 404, "It's not there");
        return;
    }
    size_t fl = strlen(from);
    if (strncmp(to, from, fl) == 0 && to[fl] == '/') {
        send_text(r, 400, "Can't move a folder into itself");
        return;
    }
    if (strcmp(from, to) == 0) {
        send_ok(r, 200);
        return;
    }
    /* taken - unless it's a case-only change, which on FAT finds the same file */
    struct stat dst;
    if (stat(fto, &dst) == 0 &&
        !(strcasecmp(from, to) == 0 && dst.st_ino == st.st_ino && dst.st_dev == st.st_dev)) {
        send_text(r, 409, "That name is taken");
        return;
    }
    if (!make_parents(fto) || rename(ffrom, fto) != 0) {
        send_text(r, 500, "Couldn't rename it");
        return;
    }
    send_ok(r, 200);
    note(r, "Renamed %.60s to %.60s", from, to);
}

/* ---- Universal Clipboard: text pasted from the browser ----
 * The text is handed to devos_clipboard_set (so the Tab5's own apps can paste
 * it) and also written to /.devos/clipboard.txt, which survives a restart and
 * is visible in the file list. */

#define CLIP_FILE  ROOT "/.devos/clipboard.txt"

/* Copy a body that may have arrived with the headers (r->io can't be used:
 * it already holds those bytes) into the clipboard and the file. */
static void do_clipboard_put(req_t *r)
{
    if (r->chunked || !r->has_len) {
        send_text(r, 411, "Content-Length needed");
        return;
    }
    if (r->content_len > DEVOS_CLIPBOARD_MAX) {
        send_text(r, 413, "The clipboard holds at most 64 KB");
        return;
    }
    size_t n = r->hdr_len - r->body_off;
    if (n > r->content_len) n = (size_t)r->content_len;
    const char *first = r->hdr + r->body_off;
    uint64_t got = n;

    size_t kept = devos_clipboard_set(first, n);      /* header bytes, then chunks */

    mkdir(ROOT "/.devos", 0755);                      /* usually already there */
    FILE *f = fopen(CLIP_FILE, "wb");
    if (!f) {
        send_text(r, 500, "Couldn't write the clipboard file");
        return;
    }
    setvbuf(f, NULL, _IONBF, 0);
    bool io_err = false;
    if (got) io_err = fwrite(first, 1, (size_t)got, f) != (size_t)got;
    if (!io_err && got < r->content_len && r->expect_continue) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        devos_net_socket_send_all(r->fd, cont, sizeof(cont) - 1);
    }
    /* absorb the rest in IO_CHUNK pieces, feeding editor and file together */
    while (!io_err && got < r->content_len) {
        if (!want()) { io_err = true; break; }
        uint64_t left = r->content_len - got;
        size_t room = left < IO_CHUNK ? (size_t)left : IO_CHUNK;
        int k = devos_net_socket_recv(r->fd, r->io, room, 0);
        if (k <= 0) { io_err = true; break; }
        if (fwrite(r->io, 1, (size_t)k, f) != (size_t)k) io_err = true;
        got += (uint64_t)k;
        if (kept < DEVOS_CLIPBOARD_MAX) {
            /* append: re-set with the old text plus the new piece */
            const char *old = devos_clipboard_get(&kept);
            char *merged = malloc(kept + (size_t)k + 1);
            if (merged) {
                memcpy(merged, old, kept);
                memcpy(merged + kept, r->io, (size_t)k);
                kept = devos_clipboard_set(merged, kept + (size_t)k);
                free(merged);
            }
        }
    }
    if (fclose(f) != 0) io_err = true;
    if (io_err) {
        send_text(r, 400, "Clipboard paste cut off");
        return;
    }
    LOCK();
    s_st.clipboards++;
    UNLOCK();
    send_ok_len(r, 200, kept);
    char sz[24];
    fmt_size(got, sz, sizeof(sz));
    if (got) note(r, "Copied %s to the devOS clipboard", sz, "");
    else     note(r, "Cleared the devOS clipboard%s", "", "");
}

/* The current clipboard, as JSON ({"text":...,"length":N}). */
static void do_clipboard_get(req_t *r)
{
    size_t len = 0;
    const char *text = devos_clipboard_get(&len);
    char *esc = big_calloc(len * 6 + 8);
    if (!esc) { send_text(r, 500, "Out of memory"); return; }
    devos_json_escape(text, esc, len * 6 + 8);
    size_t body = strlen(esc) + 40;
    char *json = big_calloc(body);
    if (!json) { free(esc); send_text(r, 500, "Out of memory"); return; }
    int n = snprintf(json, body, "{\"text\":\"%s\",\"length\":%u}\n", esc, (unsigned)len);
    if (send_head(r, 200, "application/json; charset=utf-8", n, NULL)) {
        devos_net_socket_send_all(r->fd, json, (size_t)n);
    }
    free(json);
    free(esc);
}

/* ------------------------------------------------------------------ requests */
static int read_headers(req_t *r)
{
    size_t n = 0;
    while (n < HDR_MAX) {
        int got = devos_net_socket_recv(r->fd, r->hdr + n, HDR_MAX - n, 0);
        if (got <= 0) return -1;
        size_t from = n > 3 ? n - 3 : 0;
        n += (size_t)got;
        char *e = find_end_of_headers(r->hdr + from, n - from);
        if (e) {
            r->hdr_len = n;
            r->body_off = (size_t)(e - r->hdr) + 4;
            return 0;
        }
    }
    return -2;
}

/* Split the request line and read the headers we use (in place). */
static bool parse_request(req_t *r)
{
    r->hdr[r->body_off - 4] = '\0';
    char *line = r->hdr, *next = strstr(line, "\r\n");
    if (next) {
        *next = '\0';
        next += 2;
    }
    char *sp1 = strchr(line, ' ');
    if (!sp1) return false;
    *sp1 = '\0';
    char *target = sp1 + 1, *sp2 = strchr(target, ' ');
    if (!sp2 || strncmp(sp2 + 1, "HTTP/1.", 7) != 0) return false;
    *sp2 = '\0';
    r->method = line;
    r->target = target;
    char *q = strchr(target, '?');
    if (q) *q++ = '\0';
    r->query = q ? q : target + strlen(target);

    for (char *h = next; h && *h; h = next) {
        next = strstr(h, "\r\n");
        if (next) {
            *next = '\0';
            next += 2;
        }
        char *colon = strchr(h, ':');
        if (!colon) continue;
        *colon = '\0';
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (strcasecmp(h, "Authorization") == 0) {
            r->auth_given = true;
            r->auth_ok = auth_ok(v);
        } else if (strcasecmp(h, "Content-Length") == 0) {
            char *end;
            errno = 0;
            unsigned long long len = strtoull(v, &end, 10);
            if (end == v || errno || (*end && *end != ' ')) return false;
            r->has_len = true;
            r->content_len = len;
        } else if (strcasecmp(h, "Transfer-Encoding") == 0) {
            r->chunked = contains_nocase(v, "chunked");
        } else if (strcasecmp(h, "Expect") == 0) {
            r->expect_continue = strncasecmp(v, "100-continue", 12) == 0;
        } else if (strcasecmp(h, "X-Devos") == 0) {
            r->csrf_ok = *v != '\0';
        }
    }
    return true;
}

static bool path_arg(req_t *r, const char *key, int slot)
{
    char raw[REL_CAP * 2];
    if (!query_param(r->query, key, raw, sizeof(raw)) || !clean_path(raw, r->rel[slot], REL_CAP)) {
        send_text(r, 400, "Bad or missing path (\"..\" and \\ : * ? \" < > | aren't allowed)");
        return false;
    }
    full_path(r->rel[slot], r->full[slot], FULL_CAP);
    return true;
}

static void handle(req_t *r)
{
    int rc = read_headers(r);
    if (rc == -2) {
        send_text(r, 431, "Request headers too large");
        return;
    }
    if (rc != 0) return;                    /* nothing came (a browser's spare connection) */
    if (!parse_request(r)) {
        send_text(r, 400, "Bad request");
        return;
    }
    LOCK();
    s_st.requests++;
    UNLOCK();
    if (!r->auth_ok) {
        if (r->auth_given) sleep_ms(1000);  /* slow down password guessing */
        static const char www[] = "WWW-Authenticate: Basic realm=\"devOS SD card\", charset=\"UTF-8\"\r\n";
        static const char body[] = "The password is in Settings > File Sharing on the Tab5.\n";
        if (send_head(r, 401, "text/plain; charset=utf-8", (long long)(sizeof(body) - 1), www)) {
            devos_net_socket_send_all(r->fd, body, sizeof(body) - 1);
        }
        return;
    }

    const char *m = r->method, *t = r->target;
    bool get = strcmp(m, "GET") == 0;
    if (!get && !r->csrf_ok) {
        send_text(r, 403, "Missing the X-Devos: 1 header");
        return;
    }
    if (strcmp(t, "/") == 0 || strcmp(t, "/index.html") == 0) {
        if (get) do_page(r);
        else send_text(r, 405, "GET only");
    } else if (strcmp(t, "/api/list") == 0) {
        if (!get) send_text(r, 405, "GET only");
        else if (path_arg(r, "path", 0)) do_list(r);
    } else if (strcmp(t, "/api/file") == 0) {
        if (!path_arg(r, "path", 0)) return;
        if (get) do_download(r, flag_param(r->query, "dl"));
        else if (strcmp(m, "PUT") == 0) do_upload(r);
        else if (strcmp(m, "DELETE") == 0) do_delete(r);
        else send_text(r, 405, "GET, PUT or DELETE");
    } else if (strcmp(t, "/api/mkdir") == 0) {
        if (strcmp(m, "POST") != 0) send_text(r, 405, "POST only");
        else if (path_arg(r, "path", 0)) do_mkdir(r);
    } else if (strcmp(t, "/api/rename") == 0) {
        if (strcmp(m, "POST") != 0) send_text(r, 405, "POST only");
        else if (path_arg(r, "path", 0) && path_arg(r, "to", 1)) do_rename(r);
    } else if (strcmp(t, "/api/clipboard") == 0) {
        if (get) do_clipboard_get(r);
        else if (strcmp(m, "POST") == 0) do_clipboard_put(r);
        else send_text(r, 405, "GET or POST");
    } else {
        send_text(r, 404, "Not found");
    }
}

/* Close so the client reads the whole answer rather than a reset: send FIN,
 * then briefly swallow anything it's still sending (an unread upload). */
static void finish(int fd)
{
    shutdown(fd, SHUT_WR);
    char sink[256];
    int budget = 64 * 1024;
    while (budget > 0) {
        int n = devos_net_socket_recv(fd, sink, sizeof(sink), 200);
        if (n <= 0) break;
        budget -= n;
    }
    devos_net_socket_close(fd);
}

typedef struct {
    int fd;
    char peer[16];
} conn_t;

static void worker_main(conn_t *c)
{
    req_t *r = big_calloc(sizeof(req_t));
    if (r && !(r->io = io_alloc(IO_CHUNK))) {
        free(r);
        r = NULL;
    }
    if (r) {
        r->fd = c->fd;
        memcpy(r->peer, c->peer, sizeof(r->peer));
        /* the first bytes must arrive soon; then the full I/O timeout */
        struct timeval tv = { HEADER_WAIT_MS / 1000, 0 };
        setsockopt(r->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        handle(r);
        io_free(r->io);
        free(r);
    }
    finish(c->fd);
    free(c);
    LOCK();
    s_st.clients--;
    UNLOCK();
}

/* ------------------------------------------------------------------ threads */
static void listener_main(void);

#ifdef ESP_PLATFORM
static void worker_task(void *arg)
{
    worker_main(arg);
    vTaskDelete(NULL);
}

static void listener_task(void *arg)
{
    (void)arg;
    listener_main();
    vTaskDelete(NULL);
}

static bool spawn_worker(conn_t *c)
{
    return xTaskCreatePinnedToCore(worker_task, "share_io", WORKER_STACK, c, 3, NULL, DEVOS_CORE_NET_CRYPTO) == pdPASS;
}

static bool spawn_listener(void)
{
    return xTaskCreatePinnedToCore(listener_task, "share", 4096, NULL, 3, NULL, DEVOS_CORE_NET_CRYPTO) == pdPASS;
}
#else
static void *worker_thread(void *arg)
{
    worker_main(arg);
    return NULL;
}

static void *listener_thread(void *arg)
{
    (void)arg;
    listener_main();
    return NULL;
}

static bool spawn_worker(conn_t *c)
{
    pthread_t t;
    if (pthread_create(&t, NULL, worker_thread, c) != 0) return false;
    pthread_detach(t);
    return true;
}

static bool spawn_listener(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, listener_thread, NULL) != 0) return false;
    pthread_detach(t);
    return true;
}
#endif

static void serve(int ls)
{
    int errors = 0;
    while (want() && errors < 20) {
        LOCK();
        bool full = s_st.clients >= DEVOS_FILESHARE_MAX_CLIENTS;
        UNLOCK();
        if (full) {                         /* the rest wait in the backlog */
            sleep_ms(50);
            continue;
        }
        char peer[16];
        int fd = devos_net_socket_accept(ls, 500, IO_TIMEOUT_MS, peer, sizeof(peer));
        if (fd < 0) {
            if (fd == -1) {
                errors++;
                sleep_ms(100);
            }
            continue;
        }
        errors = 0;
        conn_t *c = calloc(1, sizeof(*c));
        if (c) {
            c->fd = fd;
            memcpy(c->peer, peer, sizeof(c->peer));
            LOCK();
            s_st.clients++;
            UNLOCK();
        }
        if (c && !spawn_worker(c)) {
            LOCK();
            s_st.clients--;
            UNLOCK();
            free(c);
            c = NULL;
        }
        if (!c) {
            static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            devos_net_socket_send_all(fd, busy, sizeof(busy) - 1);
            devos_net_socket_close(fd);
        }
    }
}

static void listener_main(void)
{
    devos_storage_refresh_stats();          /* free space for uploads */
    for (;;) {
        int ls = -1;
        for (int tries = 0; ls < 0 && tries < 10 && want(); tries++) {
            ls = devos_net_socket_listen(DEVOS_FILESHARE_PORT, 4);
            if (ls < 0) sleep_ms(300);      /* the previous listener letting go */
        }
        if (ls >= 0) {
            LOG("sharing the SD card on port %d", DEVOS_FILESHARE_PORT);
            s_running = true;
            LOCK();
            s_st.running = true;
            UNLOCK();
            serve(ls);
            devos_net_socket_close(ls);
            s_running = false;
            LOG("stopped sharing the SD card");
        }
        LOCK();
        s_st.running = false;
        bool again = s_want && ls >= 0;     /* switched back on while closing */
        if (!again) {
            if (s_want) {                   /* couldn't listen, or the socket died */
                s_want = false;
                s_st.password[0] = '\0';
                snprintf(s_st.error, sizeof(s_st.error), ls < 0 ? "Couldn't open port %d" : "Port %d stopped working",
                         DEVOS_FILESHARE_PORT);
            }
            s_listener = false;
        }
        UNLOCK();
        if (!again) return;
    }
}

/* ------------------------------------------------------------------ api */
void devos_fileshare_init(void)
{
#ifdef ESP_PLATFORM
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
#endif
    LOCK();
    s_st.port = DEVOS_FILESHARE_PORT;
    UNLOCK();
}

/* 8 characters in two groups ("k7m2-x9qp"), without look-alikes (0 o 1 l i). */
static void new_password(char *out)
{
    static const char abc[] = "abcdefghjkmnpqrstuvwxyz23456789";
    uint8_t rnd[8];
    devos_random(rnd, sizeof(rnd));
    int o = 0;
    for (int i = 0; i < 8; i++) {
        if (i == 4) out[o++] = '-';
        out[o++] = abc[rnd[i] % (sizeof(abc) - 1)];
    }
    out[o] = '\0';
    devos_wipe(rnd, sizeof(rnd));
}

bool devos_fileshare_start(void)
{
    if (!devos_storage_is_mounted()) {
        LOCK();
        snprintf(s_st.error, sizeof(s_st.error), "No SD card");
        UNLOCK();
        return false;
    }
    LOCK();
    if (!s_want) new_password(s_st.password);
    s_want = true;
    s_st.error[0] = '\0';
    s_st.port = DEVOS_FILESHARE_PORT;
    bool spawn = !s_listener;
    s_listener = true;
    UNLOCK();
    if (spawn && !spawn_listener()) {
        LOCK();
        s_listener = false;
        s_want = false;
        s_st.password[0] = '\0';
        snprintf(s_st.error, sizeof(s_st.error), "Out of memory");
        UNLOCK();
        return false;
    }
    return true;
}

void devos_fileshare_stop(void)
{
    LOCK();
    s_want = false;
    devos_wipe(s_st.password, sizeof(s_st.password));
    UNLOCK();
}

bool devos_fileshare_running(void)
{
    return s_running;
}

void devos_fileshare_status(devos_fileshare_status_t *out)
{
    LOCK();
    *out = s_st;
    UNLOCK();
}
