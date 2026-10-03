/* devos_http: see devos_http.h. */
#include "devos_http.h"
#include "devos_net.h"
#include "devos_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#define DEVOS_HTTP_TLS 1
static SemaphoreHandle_t s_mx, s_wake;
#define LOCK()   xSemaphoreTake(s_mx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mx)
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}
static void *big_realloc(void *o, size_t n)
{
    void *p = heap_caps_realloc(o, n, MALLOC_CAP_SPIRAM);
    return p ? p : realloc(o, n);
}
#else
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
static pthread_mutex_t s_mx_sim = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cv_sim = PTHREAD_COND_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&s_mx_sim)
#define UNLOCK() pthread_mutex_unlock(&s_mx_sim)
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void *big_alloc(size_t n) { return malloc(n); }
static void *big_realloc(void *o, size_t n) { return realloc(o, n); }
#endif

#ifdef DEVOS_HTTP_TLS
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#if defined(MBEDTLS_PSA_CRYPTO_C)
#include "psa/crypto.h"
#endif
#ifdef ESP_PLATFORM
#include "esp_crt_bundle.h"
#endif
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define SLICE_MS 400                    /* socket wait slice (abort checks) */
#define HDR_MAX  (16 * 1024)

bool devos_http_tls_available(void)
{
#ifdef DEVOS_HTTP_TLS
    return true;
#else
    return false;
#endif
}

/* ------------------------------------------------------------------ helpers */
size_t devos_http_base64(const void *in, size_t n, char *out, size_t cap)
{
    static const char tb[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *s = in;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)s[i] << 16;
        if (i + 1 < n) v |= (uint32_t)s[i + 1] << 8;
        if (i + 2 < n) v |= s[i + 2];
        if (o + 4 >= cap) break;
        out[o++] = tb[(v >> 18) & 63];
        out[o++] = tb[(v >> 12) & 63];
        out[o++] = i + 1 < n ? tb[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? tb[v & 63] : '=';
    }
    if (cap) out[o < cap ? o : cap - 1] = '\0';
    return o;
}

int devos_http_parse_url(const char *url, bool *https, char *host, size_t host_cap, int *port,
                         char *path, size_t path_cap)
{
    if (!url) return -1;
    while (*url == ' ') url++;
    bool tls = false;
    if (!strncasecmp(url, "https://", 8)) {
        tls = true;
        url += 8;
    } else if (!strncasecmp(url, "http://", 7)) {
        url += 7;
    } else if (strstr(url, "://")) {
        return -1;                              /* another scheme */
    }
    const char *end = url + strcspn(url, "/?#");
    const char *at = memchr(url, '@', (size_t)(end - url));
    const char *h = at ? at + 1 : url;
    const char *colon = memchr(h, ':', (size_t)(end - h));
    const char *hend = colon ? colon : end;
    size_t hl = (size_t)(hend - h);
    if (!hl || hl >= host_cap) return -1;
    memcpy(host, h, hl);
    host[hl] = '\0';
    int p = tls ? 443 : 80;
    if (colon) {
        p = atoi(colon + 1);
        if (p <= 0 || p > 65535) return -1;
    }
    const char *rest = end;
    if (*rest == '#') rest = "";
    size_t rl = strcspn(rest, "#");
    if (*rest != '/') {
        if (path_cap < rl + 2) return -1;
        path[0] = '/';
        memcpy(path + 1, rest, rl);
        path[rl + 1] = '\0';
    } else {
        if (path_cap < rl + 1) return -1;
        memcpy(path, rest, rl);
        path[rl] = '\0';
    }
    if (https) *https = tls;
    if (port) *port = p;
    return 0;
}

/* user:pass@ in the URL -> "user:pass" (percent-decoding left out) */
static bool url_userinfo(const char *url, char *out, size_t cap)
{
    const char *s = strstr(url, "://");
    s = s ? s + 3 : url;
    const char *end = s + strcspn(s, "/?#");
    const char *at = memchr(s, '@', (size_t)(end - s));
    if (!at || (size_t)(at - s) >= cap) return false;
    memcpy(out, s, (size_t)(at - s));
    out[at - s] = '\0';
    return true;
}

bool devos_http_header(const devos_http_resp_t *resp, const char *name, char *out, size_t cap)
{
    if (!resp || !resp->headers || !name) return false;
    size_t nl = strlen(name);
    const char *p = resp->headers;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        if (ll > nl && p[nl] == ':' && !strncasecmp(p, name, nl)) {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vl = (size_t)(p + ll - v);
            if (out && cap) {
                if (vl >= cap) vl = cap - 1;
                memcpy(out, v, vl);
                out[vl] = '\0';
            }
            return true;
        }
        if (!eol) break;
        p = eol + 2;
    }
    return false;
}

static bool ci_contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) if (!strncasecmp(hay, needle, n)) return true;
    return false;
}

/* does the caller's header block set `name`? */
static bool has_header(const char *hdrs, const char *name)
{
    if (!hdrs) return false;
    size_t nl = strlen(name);
    const char *p = hdrs;
    while (*p) {
        while (*p == '\r' || *p == '\n' || *p == ' ') p++;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') return true;
        p += strcspn(p, "\n");
    }
    return false;
}

/* ------------------------------------------------------------------ connection */
typedef struct {
    int fd;
    bool tls;
    volatile bool *abort;
    int64_t deadline;                   /* for the current operation */
    int timeout_ms;
#ifdef DEVOS_HTTP_TLS
    bool ssl_ready;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context ent;
#endif
} conn_t;

static bool aborted(conn_t *c) { return c->abort && *c->abort; }

static void set_slice(int fd)
{
    struct timeval tv = { .tv_sec = 0, .tv_usec = SLICE_MS * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct timeval tw = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tw, sizeof(tw));
}

#ifdef DEVOS_HTTP_TLS
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    conn_t *c = ctx;
    int n = (int)send(c->fd, buf, len, MSG_NOSIGNAL);
    if (n >= 0) return n;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    conn_t *c = ctx;
    int n = (int)recv(c->fd, buf, len, 0);
    if (n >= 0) return n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

#ifndef ESP_PLATFORM
static mbedtls_x509_crt s_ca;
static int s_ca_state;                  /* 0 unloaded, 1 loaded, -1 none found */

static mbedtls_x509_crt *sim_ca(void)
{
    LOCK();
    if (!s_ca_state) {
        static const char *files[] = { "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/certs/ca-certificates.crt",
                                       "/etc/ssl/cert.pem" };
        mbedtls_x509_crt_init(&s_ca);
        s_ca_state = -1;
        for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
            if (mbedtls_x509_crt_parse_file(&s_ca, files[i]) >= 0 && s_ca.version) {
                s_ca_state = 1;
                break;
            }
        }
    }
    UNLOCK();
    return s_ca_state > 0 ? &s_ca : NULL;
}
#endif

static void tls_err(int rc, const char *what, char *err, size_t cap)
{
    char buf[80] = "";
#if defined(MBEDTLS_ERROR_C)
    mbedtls_strerror(rc, buf, sizeof(buf));
#endif
    snprintf(err, cap, "%s failed: %s (-0x%04x)", what, buf[0] ? buf : "TLS error", (unsigned)-rc);
}

static int tls_start(conn_t *c, const char *host, bool insecure, devos_http_resp_t *r)
{
#if defined(MBEDTLS_PSA_CRYPTO_C)
    static bool psa_ready;
    if (!psa_ready) {
        psa_crypto_init();
        psa_ready = true;
    }
#endif
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_entropy_init(&c->ent);
    c->ssl_ready = true;
    int rc = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->ent, (const unsigned char *)"devos_http", 10);
    if (rc) {
        tls_err(rc, "TLS random seed", r->error, sizeof(r->error));
        return -1;
    }
    rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc) {
        tls_err(rc, "TLS setup", r->error, sizeof(r->error));
        return -1;
    }
    /* insecure: still verify (to report it) but don't fail the handshake */
    mbedtls_ssl_conf_authmode(&c->conf, insecure ? MBEDTLS_SSL_VERIFY_OPTIONAL : MBEDTLS_SSL_VERIFY_REQUIRED);
#ifdef ESP_PLATFORM
    esp_crt_bundle_attach(&c->conf);
#else
    mbedtls_x509_crt *ca = sim_ca();
    if (ca) mbedtls_ssl_conf_ca_chain(&c->conf, ca, NULL);
    else if (!insecure) {
        snprintf(r->error, sizeof(r->error), "No CA certificates found on this computer");
        return -1;
    }
#endif
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    if ((rc = mbedtls_ssl_setup(&c->ssl, &c->conf)) != 0 || (rc = mbedtls_ssl_set_hostname(&c->ssl, host)) != 0) {
        tls_err(rc, "TLS setup", r->error, sizeof(r->error));
        return -1;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
    c->deadline = now_ms() + c->timeout_ms;
    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
            if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags) {
                char vi[128] = "";
                mbedtls_x509_crt_verify_info(vi, sizeof(vi), "", flags);
                size_t l = strlen(vi);
                while (l && (vi[l - 1] == '\n' || vi[l - 1] == ' ')) vi[--l] = '\0';
                snprintf(r->error, sizeof(r->error), "Certificate not trusted: %.80s", vi[0] ? vi : "verify failed");
            } else {
                tls_err(rc, "TLS handshake", r->error, sizeof(r->error));
            }
            return -1;
        }
        if (aborted(c)) {
            snprintf(r->error, sizeof(r->error), "Cancelled");
            return -1;
        }
        if (now_ms() > c->deadline) {
            snprintf(r->error, sizeof(r->error), "TLS handshake timed out");
            return -1;
        }
    }
    c->tls = true;
    r->tls = true;
    r->tls_verified = mbedtls_ssl_get_verify_result(&c->ssl) == 0;
    snprintf(r->tls_info, sizeof(r->tls_info), "%s %s", mbedtls_ssl_get_version(&c->ssl),
             mbedtls_ssl_get_ciphersuite(&c->ssl));
    return 0;
}
#endif

static int conn_send_all(conn_t *c, const void *data, size_t len)
{
    const unsigned char *p = data;
    while (len) {
        int n;
#ifdef DEVOS_HTTP_TLS
        if (c->tls) {
            n = mbedtls_ssl_write(&c->ssl, p, len);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (aborted(c)) return -1;
                continue;
            }
        } else
#endif
        {
            n = (int)send(c->fd, p, len, MSG_NOSIGNAL);
        }
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* >0 bytes, 0 closed, -1 error / timeout / abort (err set) */
static int conn_recv(conn_t *c, void *buf, size_t n, char *err, size_t errcap)
{
    int64_t deadline = now_ms() + c->timeout_ms;
    for (;;) {
        int r;
#ifdef DEVOS_HTTP_TLS
        if (c->tls) {
            r = mbedtls_ssl_read(&c->ssl, buf, n);
            if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == 0) return 0;
            if (r > 0) return r;
            if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
                && r != MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
#endif
            ) {
                if (r == MBEDTLS_ERR_NET_CONN_RESET) return 0;
                tls_err(r, "Read", err, errcap);
                return -1;
            }
        } else
#endif
        {
            r = (int)recv(c->fd, buf, n, 0);
            if (r >= 0) return r;
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                if (errno == ECONNRESET) return 0;
                snprintf(err, errcap, "Connection error (%s)", strerror(errno));
                return -1;
            }
        }
        if (aborted(c)) {
            snprintf(err, errcap, "Cancelled");
            return -1;
        }
        if (now_ms() > deadline) {
            snprintf(err, errcap, "Timed out waiting for the server");
            return -1;
        }
    }
}

static void conn_close(conn_t *c)
{
#ifdef DEVOS_HTTP_TLS
    if (c->tls) mbedtls_ssl_close_notify(&c->ssl);
    if (c->ssl_ready) {
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_ctr_drbg_free(&c->drbg);
        mbedtls_entropy_free(&c->ent);
        c->ssl_ready = false;
    }
#endif
    if (c->fd >= 0) devos_net_socket_close(c->fd);
    c->fd = -1;
}

/* ------------------------------------------------------------------ buffered reader */
typedef struct {
    conn_t *c;
    unsigned char buf[2048];
    int pos, len;
    bool eof;
    char *err;
    size_t errcap;
} rd_t;

static int rd_fill(rd_t *r)
{
    if (r->eof) return 0;
    int n = conn_recv(r->c, r->buf, sizeof(r->buf), r->err, r->errcap);
    if (n <= 0) {
        if (n == 0) r->eof = true;
        return n;
    }
    r->pos = 0;
    r->len = n;
    return n;
}

/* one line without CRLF; returns length or -1 */
static int rd_line(rd_t *r, char *out, size_t cap)
{
    size_t o = 0;
    for (;;) {
        if (r->pos >= r->len) {
            int n = rd_fill(r);
            if (n < 0) return -1;
            if (n == 0) {
                if (o) break;
                if (!r->err[0]) snprintf(r->err, r->errcap, "Connection closed by the server");
                return -1;
            }
        }
        char ch = (char)r->buf[r->pos++];
        if (ch == '\n') break;
        if (ch != '\r' && o + 1 < cap) out[o++] = ch;
    }
    out[o] = '\0';
    return (int)o;
}

/* up to n bytes; 0 at EOF, -1 error */
static int rd_read(rd_t *r, void *out, size_t n)
{
    if (r->pos >= r->len) {
        int f = rd_fill(r);
        if (f <= 0) return f;
    }
    size_t k = (size_t)(r->len - r->pos);
    if (k > n) k = n;
    memcpy(out, r->buf + r->pos, k);
    r->pos += (int)k;
    return (int)k;
}

/* ------------------------------------------------------------------ body sink */
typedef struct {
    devos_http_resp_t *r;
    size_t cap, max;
    volatile size_t *progress;
} sink_t;

static void sink_put(sink_t *s, const void *d, size_t n)
{
    devos_http_resp_t *r = s->r;
    r->body_total += n;
    if (s->progress) *s->progress = r->body_total;
    if (r->body_len >= s->max) {
        r->truncated = true;
        return;
    }
    if (r->body_len + n > s->max) {
        n = s->max - r->body_len;
        r->truncated = true;
    }
    if (r->body_len + n + 1 > s->cap) {
        size_t nc = s->cap ? s->cap : 4096;
        while (nc < r->body_len + n + 1) nc *= 2;
        if (nc > s->max + 1) nc = s->max + 1;
        char *nb = big_realloc(r->body, nc);
        if (!nb) {
            r->truncated = true;
            return;
        }
        r->body = nb;
        s->cap = nc;
    }
    memcpy(r->body + r->body_len, d, n);
    r->body_len += n;
    r->body[r->body_len] = '\0';
}

/* ------------------------------------------------------------------ one exchange */
static int exchange(const devos_http_req_t *q, const char *url, const char *method, const char *body, size_t body_len,
                    volatile bool *abort, devos_http_resp_t *r, char *location, size_t loc_cap)
{
    bool https;
    char host[256], path[DEVOS_HTTP_URL_MAX];
    int port;
    if (devos_http_parse_url(url, &https, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        snprintf(r->error, sizeof(r->error), "Not a valid http:// or https:// URL");
        return -1;
    }
#ifndef DEVOS_HTTP_TLS
    if (https) {
        snprintf(r->error, sizeof(r->error), "HTTPS isn't available in this simulator build");
        return -1;
    }
#endif
    int timeout = q->timeout_ms > 0 ? q->timeout_ms : 10000;
    int64_t t0 = now_ms();
    char ip[48];
    if (devos_net_resolve(host, ip, sizeof(ip)) != 0) {
        snprintf(r->error, sizeof(r->error), "Couldn't resolve %.90s", host);
        return -1;
    }
    int64_t t1 = now_ms();
    r->ms_dns += (int)(t1 - t0);

    conn_t *c = calloc(1, sizeof(conn_t));
    if (!c) {
        snprintf(r->error, sizeof(r->error), "Out of memory");
        return -1;
    }
    c->fd = -1;
    c->abort = abort;
    c->timeout_ms = timeout;
    int rc = -1;
    char *line = NULL;

    int fd = devos_net_socket_connect_start(ip, port);
    int st = fd < 0 ? -1 : 1;
    int64_t cdl = t1 + timeout;
    while (st == 1) {
        st = devos_net_socket_connect_wait(fd, 200);
        if (st == 1 && ((abort && *abort) || now_ms() > cdl)) break;
    }
    if (st != 0) {
        if (fd >= 0) devos_net_socket_close(fd);
        if (abort && *abort) snprintf(r->error, sizeof(r->error), "Cancelled");
        else if (st == 1) snprintf(r->error, sizeof(r->error), "Timed out connecting to %.60s:%d", host, port);
        else snprintf(r->error, sizeof(r->error), "Couldn't connect to %.60s:%d (refused or unreachable)", host, port);
        goto out;
    }
    c->fd = fd;
    set_slice(fd);
    int64_t t2 = now_ms();
    r->ms_connect += (int)(t2 - t1);
#ifdef DEVOS_HTTP_TLS
    if (https && tls_start(c, host, q->insecure, r) != 0) goto out;
#endif
    int64_t t3 = now_ms();
    r->ms_tls += (int)(t3 - t2);

    /* request head */
    size_t hcap = 1024 + strlen(path) + (q->headers ? strlen(q->headers) * 2 : 0);
    char *head = malloc(hcap);
    if (!head) {
        snprintf(r->error, sizeof(r->error), "Out of memory");
        goto out;
    }
    size_t o = (size_t)snprintf(head, hcap, "%s %s HTTP/1.1\r\n", method, path);
    const char *hs = q->headers;
    if (!has_header(hs, "Host")) {
        if ((https && port == 443) || (!https && port == 80)) o += (size_t)snprintf(head + o, hcap - o, "Host: %s\r\n", host);
        else o += (size_t)snprintf(head + o, hcap - o, "Host: %s:%d\r\n", host, port);
    }
    if (!has_header(hs, "User-Agent")) o += (size_t)snprintf(head + o, hcap - o, "User-Agent: devOS-Tab5\r\n");
    if (!has_header(hs, "Accept")) o += (size_t)snprintf(head + o, hcap - o, "Accept: */*\r\n");
    o += (size_t)snprintf(head + o, hcap - o, "Connection: close\r\n");
    char ui[160];
    if (!has_header(hs, "Authorization") && url_userinfo(url, ui, sizeof(ui))) {
        char b64[224];
        devos_http_base64(ui, strlen(ui), b64, sizeof(b64));
        o += (size_t)snprintf(head + o, hcap - o, "Authorization: Basic %s\r\n", b64);
    }
    bool wants_len = body_len || !strcasecmp(method, "POST") || !strcasecmp(method, "PUT") || !strcasecmp(method, "PATCH");
    if (wants_len && !has_header(hs, "Content-Length"))
        o += (size_t)snprintf(head + o, hcap - o, "Content-Length: %u\r\n", (unsigned)body_len);
    /* the caller's lines, normalised to CRLF, blank lines dropped */
    for (const char *p = hs; p && *p;) {
        size_t ll = strcspn(p, "\r\n");
        const char *s = p;
        while (ll && (*s == ' ' || *s == '\t')) { s++; ll--; }
        if (ll && memchr(s, ':', ll) && o + ll + 3 < hcap) {
            memcpy(head + o, s, ll);
            o += ll;
            head[o++] = '\r';
            head[o++] = '\n';
        }
        p = s + ll;
        while (*p == '\r' || *p == '\n') p++;
    }
    o += (size_t)snprintf(head + o, hcap - o, "\r\n");
    int sent = conn_send_all(c, head, o);
    free(head);
    if (sent != 0 || (body_len && conn_send_all(c, body, body_len) != 0)) {
        snprintf(r->error, sizeof(r->error), "Couldn't send the request");
        goto out;
    }

    /* response */
    rd_t *rd = calloc(1, sizeof(rd_t));
    line = malloc(HDR_MAX);
    if (!rd || !line) {
        free(rd);
        snprintf(r->error, sizeof(r->error), "Out of memory");
        goto out;
    }
    rd->c = c;
    rd->err = r->error;
    rd->errcap = sizeof(r->error);
    int status = 0;
    for (;;) {                                  /* skip 1xx interim responses */
        if (rd_line(rd, line, HDR_MAX) < 0) goto rd_out;
        if (!r->ms_first_byte) r->ms_first_byte = (int)(now_ms() - t3);
        char reason[48] = "";
        if (sscanf(line, "HTTP/%*d.%*d %d %47[^\r\n]", &status, reason) < 1 &&
            sscanf(line, "HTTP/%*d %d %47[^\r\n]", &status, reason) < 1) {
            snprintf(r->error, sizeof(r->error), "Not an HTTP response");
            goto rd_out;
        }
        snprintf(r->reason, sizeof(r->reason), "%s", reason);
        free(r->headers);
        r->headers = NULL;
        r->headers_len = 0;
        size_t hc = 0;
        for (;;) {
            int ll = rd_line(rd, line, HDR_MAX);
            if (ll < 0) goto rd_out;
            if (ll == 0) break;
            if (r->headers_len + (size_t)ll + 3 > hc) {
                size_t nc = hc ? hc * 2 : 1024;
                while (nc < r->headers_len + (size_t)ll + 3) nc *= 2;
                if (nc > HDR_MAX) continue;     /* absurd header block: drop the rest */
                char *nh = big_realloc(r->headers, nc);
                if (!nh) continue;
                r->headers = nh;
                hc = nc;
            }
            memcpy(r->headers + r->headers_len, line, (size_t)ll);
            r->headers_len += (size_t)ll;
            r->headers[r->headers_len++] = '\r';
            r->headers[r->headers_len++] = '\n';
            r->headers[r->headers_len] = '\0';
        }
        if (status >= 200 || status == 101) break;
    }
    r->status = status;
    char val[64];
    if (location && devos_http_header(r, "Location", location, loc_cap)) { /* caller decides */ }
    sink_t sk = { r, 0, q->max_body ? q->max_body : DEVOS_HTTP_DEFAULT_MAX_BODY, q->progress };
    bool no_body = !strcasecmp(method, "HEAD") || status == 204 || status == 304 || (status >= 100 && status < 200);
    if (!no_body) {
        unsigned char chunk[1024];
        if (devos_http_header(r, "Transfer-Encoding", val, sizeof(val)) && ci_contains(val, "chunked")) {
            for (;;) {
                if (rd_line(rd, line, 64) < 0) goto rd_out;
                unsigned long left = strtoul(line, NULL, 16);
                if (!left) {
                    while (rd_line(rd, line, HDR_MAX) > 0) {}     /* trailers */
                    break;
                }
                while (left) {
                    int n = rd_read(rd, chunk, left < sizeof(chunk) ? left : sizeof(chunk));
                    if (n <= 0) {
                        if (!n && !r->error[0]) snprintf(r->error, sizeof(r->error), "Connection closed mid-response");
                        goto rd_out;
                    }
                    sink_put(&sk, chunk, (size_t)n);
                    left -= (unsigned long)n;
                }
                rd_line(rd, line, 8);                           /* CRLF after the chunk */
            }
        } else if (devos_http_header(r, "Content-Length", val, sizeof(val))) {
            unsigned long long left = strtoull(val, NULL, 10);
            while (left) {
                int n = rd_read(rd, chunk, left < sizeof(chunk) ? (size_t)left : sizeof(chunk));
                if (n <= 0) {
                    if (!n && !r->error[0]) snprintf(r->error, sizeof(r->error), "Connection closed mid-response");
                    goto rd_out;
                }
                sink_put(&sk, chunk, (size_t)n);
                left -= (unsigned long long)n;
                if (r->truncated) break;                        /* rest isn't wanted: just close */
            }
        } else {
            for (;;) {
                int n = rd_read(rd, chunk, sizeof(chunk));
                if (n < 0) goto rd_out;
                if (n == 0) break;
                sink_put(&sk, chunk, (size_t)n);
                if (r->truncated && r->body_total > sk.max + 64 * 1024) break;
            }
        }
    }
    rc = 0;
rd_out:
    if (rc == 0) r->error[0] = '\0';
    else if (r->status && r->body_len) rc = 0;                 /* partial body: still a response */
    free(rd);
out:
    free(line);
    conn_close(c);
    free(c);
    return rc;
}

static void resolve_location(const char *base, const char *loc, char *out, size_t cap)
{
    if (strstr(loc, "://")) {
        snprintf(out, cap, "%s", loc);
        return;
    }
    const char *s = strstr(base, "://");
    s = s ? s + 3 : base;
    size_t origin = (size_t)(s - base) + strcspn(s, "/?#");
    if (loc[0] == '/' && loc[1] == '/') {                        /* scheme-relative */
        size_t sl = (size_t)(strstr(base, "://") ? strstr(base, "://") - base + 1 : 0);
        snprintf(out, cap, "%.*s%s", (int)sl, base, loc);
    } else if (loc[0] == '/') {
        snprintf(out, cap, "%.*s%s", (int)origin, base, loc);
    } else {
        const char *q = base + origin + strcspn(base + origin, "?#");
        const char *slash = q;
        while (slash > base + origin && slash[-1] != '/') slash--;
        if (slash == base + origin) snprintf(out, cap, "%.*s/%s", (int)origin, base, loc);
        else snprintf(out, cap, "%.*s%s", (int)(slash - base), base, loc);
    }
}

static int request_internal(const devos_http_req_t *req, devos_http_resp_t *r, volatile bool *abort)
{
    memset(r, 0, sizeof(*r));
    if (!req || !req->url) {
        snprintf(r->error, sizeof(r->error), "No URL");
        return -1;
    }
    int64_t t0 = now_ms();
    char *url = malloc(DEVOS_HTTP_URL_MAX), *loc = malloc(DEVOS_HTTP_URL_MAX);
    if (!url || !loc) {
        free(url);
        free(loc);
        snprintf(r->error, sizeof(r->error), "Out of memory");
        return -1;
    }
    snprintf(url, DEVOS_HTTP_URL_MAX, "%s", req->url);
    const char *method = req->method && req->method[0] ? req->method : "GET";
    char meth[16];
    snprintf(meth, sizeof(meth), "%s", method);
    const char *body = req->body;
    size_t body_len = req->body ? req->body_len : 0;
    int rc;
    for (;;) {
        loc[0] = '\0';
        rc = exchange(req, url, meth, body, body_len, abort, r, loc, DEVOS_HTTP_URL_MAX);
        bool redirect = rc == 0 && loc[0] && (r->status == 301 || r->status == 302 || r->status == 303 ||
                                              r->status == 307 || r->status == 308);
        if (!redirect || r->redirects >= req->max_redirects) break;
        r->redirects++;
        char *next = malloc(DEVOS_HTTP_URL_MAX);
        if (!next) break;
        resolve_location(url, loc, next, DEVOS_HTTP_URL_MAX);
        snprintf(url, DEVOS_HTTP_URL_MAX, "%s", next);
        free(next);
        if (r->status == 303 || ((r->status == 301 || r->status == 302) && strcasecmp(meth, "HEAD"))) {
            snprintf(meth, sizeof(meth), "GET");
            body = NULL;
            body_len = 0;
        }
        free(r->headers);
        free(r->body);
        r->headers = r->body = NULL;
        r->headers_len = r->body_len = r->body_total = 0;
        r->truncated = false;
        r->status = 0;
    }
    snprintf(r->final_url, sizeof(r->final_url), "%s", url);
    r->ms_total = (int)(now_ms() - t0);
    free(url);
    free(loc);
    return rc;
}

int devos_http_request(const devos_http_req_t *req, devos_http_resp_t *resp)
{
    return request_internal(req, resp, NULL);
}

void devos_http_resp_free(devos_http_resp_t *resp)
{
    if (!resp) return;
    free(resp->headers);
    free(resp->body);
    resp->headers = resp->body = NULL;
    resp->headers_len = resp->body_len = 0;
}

/* ------------------------------------------------------------------ worker */
#define JOBS 8
typedef enum { J_FREE = 0, J_QUEUED, J_RUNNING, J_DONE } jstate_t;
typedef struct {
    jstate_t state;
    int id;
    bool cancelled;
    volatile bool abort;
    uint32_t order;
    char method[16];
    char *url, *headers, *body;
    size_t body_len;
    devos_http_req_t req;
    devos_http_resp_t resp;
} job_t;

static EXT_RAM_BSS_ATTR job_t s_jobs[JOBS];
static int s_next_id = 1;
static uint32_t s_order;
static bool s_worker;
static volatile int s_init_state;   /* 0 = not, 1 = initializing, 2 = ready */
static bool http_ready(void) { return __atomic_load_n(&s_init_state, __ATOMIC_ACQUIRE) == 2; }

static void job_clear(job_t *j)
{
    free(j->url);
    free(j->headers);
    free(j->body);
    j->url = j->headers = j->body = NULL;
    j->state = J_FREE;
}

static job_t *next_job(void)            /* call locked */
{
    job_t *j = NULL;
    for (int i = 0; i < JOBS; i++) {
        if (s_jobs[i].state == J_QUEUED && (!j || s_jobs[i].order < j->order)) j = &s_jobs[i];
    }
    if (j) j->state = J_RUNNING;
    return j;
}

static void worker_loop(void)
{
    for (;;) {
        LOCK();
        job_t *j = next_job();
#ifdef ESP_PLATFORM
        UNLOCK();
        if (!j) {
            xSemaphoreTake(s_wake, portMAX_DELAY);    /* binary: a give before this take isn't lost */
            continue;
        }
#else
        while (!j) {
            pthread_cond_wait(&s_cv_sim, &s_mx_sim);
            j = next_job();
        }
        UNLOCK();
#endif
        devos_http_resp_t resp;
        request_internal(&j->req, &resp, &j->abort);
        LOCK();
        if (j->cancelled) {
            devos_http_resp_free(&resp);
            job_clear(j);
        } else {
            j->resp = resp;
            j->state = J_DONE;
        }
        UNLOCK();
    }
}

#ifdef ESP_PLATFORM
static void worker_task(void *arg)
{
    (void)arg;
    worker_loop();
}
#else
static void *worker_thread(void *arg)
{
    (void)arg;
    worker_loop();
    return NULL;
}
#endif

static bool worker_start(void)
{
    if (s_worker) return true;
#ifdef ESP_PLATFORM
    /* network core; TLS handshakes need a roomy stack */
    if (xTaskCreatePinnedToCore(worker_task, "http", 12288, NULL, 4, NULL, DEVOS_CORE_NET_CRYPTO) != pdPASS)
        return false;
#else
    pthread_t t;
    if (pthread_create(&t, NULL, worker_thread, NULL) != 0) return false;
    pthread_detach(t);
#endif
    s_worker = true;
    return true;
}

/* Explicit, idempotent initialization so the first submit never races to
 * create the mutex/worker. One caller initializes; any other waits briefly.
 * Returns false on allocation or task failure. */
bool devos_http_init(void)
{
    if (http_ready()) return true;
    int expect = 0;
    if (__atomic_compare_exchange_n(&s_init_state, &expect, 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
#ifdef ESP_PLATFORM
        if (!s_mx) {
            s_mx = xSemaphoreCreateMutex();
            s_wake = xSemaphoreCreateBinary();
            if (!s_mx || !s_wake) { __atomic_store_n(&s_init_state, 0, __ATOMIC_RELEASE); return false; }
        }
#endif
        if (!worker_start()) { __atomic_store_n(&s_init_state, 0, __ATOMIC_RELEASE); return false; }
        __atomic_store_n(&s_init_state, 2, __ATOMIC_RELEASE);
        return true;
    }
    while (__atomic_load_n(&s_init_state, __ATOMIC_ACQUIRE) == 1) { /* init is quick */ }
    return http_ready();
}

static char *dup_str(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = big_alloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

int devos_http_submit(const devos_http_req_t *req)
{
    if (!req || !req->url) return -1;
    if (!devos_http_init()) return -1;
    LOCK();
    job_t *j = NULL;
    for (int i = 0; i < JOBS && !j; i++) if (s_jobs[i].state == J_FREE) j = &s_jobs[i];
    if (!j) {
        UNLOCK();
        return -1;
    }
    memset(j, 0, sizeof(*j));
    snprintf(j->method, sizeof(j->method), "%s", req->method ? req->method : "GET");
    j->url = dup_str(req->url);
    j->headers = dup_str(req->headers);
    if (req->body && req->body_len) {
        j->body = big_alloc(req->body_len);
        if (j->body) memcpy(j->body, req->body, req->body_len);
        j->body_len = req->body_len;
    }
    if (!j->url || (req->headers && !j->headers) || (req->body_len && req->body && !j->body)) {
        job_clear(j);
        UNLOCK();
        return -1;
    }
    j->req = *req;
    j->req.method = j->method;
    j->req.url = j->url;
    j->req.headers = j->headers;
    j->req.body = j->body;
    j->req.body_len = j->body_len;
    j->id = s_next_id++;
    if (s_next_id <= 0) s_next_id = 1;
    j->order = s_order++;
    j->state = J_QUEUED;
    int id = j->id;
    UNLOCK();
#ifdef ESP_PLATFORM
    xSemaphoreGive(s_wake);
#else
    pthread_cond_signal(&s_cv_sim);
#endif
    return id;
}

int devos_http_poll(int job, devos_http_resp_t *resp)
{
    int rc = -1;
    if (job <= 0) return -1;
    if (!http_ready()) return -1;
    LOCK();
    for (int i = 0; i < JOBS; i++) {
        job_t *j = &s_jobs[i];
        if (j->state == J_FREE || j->id != job || j->cancelled) continue;
        if (j->state == J_DONE) {
            if (resp) *resp = j->resp;
            else devos_http_resp_free(&j->resp);
            job_clear(j);
            rc = 1;
        } else {
            rc = 0;
        }
        break;
    }
    UNLOCK();
    return rc;
}

void devos_http_cancel(int job)
{
    if (job <= 0) return;
    if (!http_ready()) return;
    LOCK();
    for (int i = 0; i < JOBS; i++) {
        job_t *j = &s_jobs[i];
        if (j->state == J_FREE || j->id != job) continue;
        if (j->state == J_DONE) {
            devos_http_resp_free(&j->resp);
            job_clear(j);
        } else if (j->state == J_QUEUED) {
            job_clear(j);
        } else {
            j->cancelled = true;
            j->abort = true;
        }
        break;
    }
    UNLOCK();
}
