/* devos_conn: see devos_conn.h. */
#include "devos_conn.h"
#include "devos_core.h"
#include "devos_net.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#include "mbedtls/net_sockets.h"
#include "esp_crt_bundle.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#else
#include <dlfcn.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

int devos_url_parse(const char *url, char *host, size_t hlen, int *port, bool *tls, char *path, size_t plen)
{
    if (!url || !host || hlen < 2) return -1;
    while (*url == ' ') url++;
    bool secure = false;
    const char *p = url;
    if (strncasecmp(p, "https://", 8) == 0) { secure = true; p += 8; }
    else if (strncasecmp(p, "http://", 7) == 0) { p += 7; }
    size_t n = strcspn(p, ":/?# ");
    if (n == 0 || n >= hlen) return -1;
    memcpy(host, p, n);
    host[n] = '\0';
    p += n;
    int pt = secure ? 443 : 80;
    if (*p == ':') {
        pt = atoi(p + 1);
        while (*++p && isdigit((unsigned char)*p)) {
        }
    }
    if (pt <= 0 || pt > 65535) return -1;
    if (path && plen) {
        size_t m = strcspn(p, " ?#");
        if (m >= plen) m = plen - 1;
        memcpy(path, p, m);
        path[m] = '\0';
        while (m > 0 && path[m - 1] == '/') path[--m] = '\0';   /* no trailing slash */
    }
    if (port) *port = pt;
    if (tls) *tls = secure;
    return 0;
}

static void unreachable(const char *host, int port, char *err, size_t errlen)
{
    if (devos_net_is_tailnet_target(host) && !devos_telemetry_get()->tailscale_online) {
        snprintf(err, errlen, "%.80s is on your tailnet: connect Tailscale first", host);
    } else {
        snprintf(err, errlen, "Could not reach %.80s:%d", host, port);
    }
}

static void set_timeout(int fd, int ms)
{
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static int plain_write(int fd, const void *buf, size_t len)
{
    return devos_net_socket_send_all(fd, buf, len);
}

static int plain_read(int fd, void *buf, size_t len, int timeout_ms)
{
    set_timeout(fd, timeout_ms > 0 ? timeout_ms : 1);
    int n = (int)recv(fd, buf, len, 0);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? DEVOS_CONN_TIMEOUT : -1;
    return n;
}

#ifdef ESP_PLATFORM
/* ========================================================================
 * Device: mbedTLS + ESP-IDF certificate bundle
 * ======================================================================== */
struct devos_conn {
    int fd;
    bool tls;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context ent;
};

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    devos_conn_t *c = ctx;
    int n = (int)send(c->fd, buf, len, 0);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    devos_conn_t *c = ctx;
    int n = (int)recv(c->fd, buf, len, 0);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? MBEDTLS_ERR_SSL_WANT_READ
                                                                               : MBEDTLS_ERR_NET_RECV_FAILED;
    if (n == 0) return MBEDTLS_ERR_NET_CONN_RESET;
    return n;
}

static void tls_free(devos_conn_t *c)
{
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_entropy_free(&c->ent);
}

devos_conn_t *devos_conn_open(const char *host, int port, bool tls, int timeout_ms, char *err, size_t errlen)
{
    int fd = devos_net_socket_connect(host, port, timeout_ms);
    if (fd < 0) {
        unreachable(host, port, err, errlen);
        return NULL;
    }
    devos_conn_t *c = calloc(1, sizeof(*c));
    if (!c) { close(fd); snprintf(err, errlen, "Out of memory"); return NULL; }
    c->fd = fd;
    c->tls = tls;
    if (!tls) return c;

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_entropy_init(&c->ent);
    int rc = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->ent, (const unsigned char *)"devos-conn", 10);
    if (rc == 0) rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                                  MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc == 0) {
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
        rc = esp_crt_bundle_attach(&c->conf);
    }
    if (rc == 0) rc = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (rc == 0) rc = mbedtls_ssl_set_hostname(&c->ssl, host);
    if (rc != 0) {
        snprintf(err, errlen, "TLS setup failed (-0x%04x)", -rc);
        devos_conn_close(c);
        return NULL;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
    set_timeout(fd, timeout_ms);
    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            snprintf(err, errlen, "TLS handshake with %.80s timed out", host);
        } else {
            uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
            if (flags) {
                char vi[96];
                mbedtls_x509_crt_verify_info(vi, sizeof(vi), "", flags);
                vi[strcspn(vi, "\n")] = '\0';
                snprintf(err, errlen, "Certificate rejected for %.60s: %.90s", host, vi);
            } else {
                char eb[80];
                mbedtls_strerror(rc, eb, sizeof(eb));
                snprintf(err, errlen, "TLS handshake failed: %.100s", eb);
            }
        }
        devos_conn_close(c);
        return NULL;
    }
    return c;
}

int devos_conn_write_all(devos_conn_t *c, const void *buf, size_t len)
{
    if (!c) return -1;
    if (!c->tls) return plain_write(c->fd, buf, len);
    size_t off = 0;
    while (off < len) {
        int n = mbedtls_ssl_write(&c->ssl, (const unsigned char *)buf + off, len - off);
        if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) continue;
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

int devos_conn_read(devos_conn_t *c, void *buf, size_t len, int timeout_ms)
{
    if (!c) return -1;
    if (!c->tls) return plain_read(c->fd, buf, len, timeout_ms);
    set_timeout(c->fd, timeout_ms > 0 ? timeout_ms : 1);
    int n = mbedtls_ssl_read(&c->ssl, buf, len);
    if (n > 0) return n;
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) return DEVOS_CONN_TIMEOUT;
    if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_NET_CONN_RESET) return 0;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
    if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) return DEVOS_CONN_TIMEOUT;   /* TLS 1.3 ticket */
#endif
    return -1;
}

void devos_conn_close(devos_conn_t *c)
{
    if (!c) return;
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        tls_free(c);
    }
    if (c->fd >= 0) close(c->fd);
    free(c);
}

#else
/* ========================================================================
 * Simulator: the host's OpenSSL (libssl.so.3), loaded with dlopen so no
 * development headers are needed.
 * ======================================================================== */
typedef struct {
    const void *(*TLS_client_method)(void);
    void *(*SSL_CTX_new)(const void *);
    int (*SSL_CTX_set_default_verify_paths)(void *);
    void (*SSL_CTX_set_verify)(void *, int, void *);
    void (*SSL_CTX_free)(void *);
    void *(*SSL_new)(void *);
    int (*SSL_set_fd)(void *, int);
    long (*SSL_ctrl)(void *, int, long, void *);
    int (*SSL_set1_host)(void *, const char *);
    int (*SSL_connect)(void *);
    int (*SSL_read)(void *, void *, int);
    int (*SSL_write)(void *, const void *, int);
    int (*SSL_get_error)(const void *, int);
    long (*SSL_get_verify_result)(const void *);
    int (*SSL_shutdown)(void *);
    void (*SSL_free)(void *);
    int (*SSL_pending)(const void *);
    const char *(*X509_verify_cert_error_string)(long);
} ossl_t;

static ossl_t O;
static void *s_ctx;
static int s_ossl_state;                 /* 0 untried, 1 ok, -1 unavailable */
static pthread_mutex_t s_ossl_mx = PTHREAD_MUTEX_INITIALIZER;

#define SSL_VERIFY_PEER 1
#define SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define SSL_ERROR_WANT_READ 2
#define SSL_ERROR_WANT_WRITE 3
#define SSL_ERROR_ZERO_RETURN 6

static bool ossl_load(void)
{
    pthread_mutex_lock(&s_ossl_mx);
    if (s_ossl_state == 0) {
        void *ssl = dlopen("libssl.so.3", RTLD_NOW);
        void *crypto = dlopen("libcrypto.so.3", RTLD_NOW);
        bool ok = ssl && crypto;
#define LOAD(lib, name) do { if (ok) { *(void **)&O.name = dlsym(lib, #name); ok = O.name != NULL; } } while (0)
        LOAD(ssl, TLS_client_method); LOAD(ssl, SSL_CTX_new); LOAD(ssl, SSL_CTX_set_default_verify_paths);
        LOAD(ssl, SSL_CTX_set_verify); LOAD(ssl, SSL_CTX_free); LOAD(ssl, SSL_new); LOAD(ssl, SSL_set_fd);
        LOAD(ssl, SSL_ctrl); LOAD(ssl, SSL_set1_host); LOAD(ssl, SSL_connect); LOAD(ssl, SSL_read);
        LOAD(ssl, SSL_write); LOAD(ssl, SSL_get_error); LOAD(ssl, SSL_get_verify_result); LOAD(ssl, SSL_shutdown);
        LOAD(ssl, SSL_free); LOAD(ssl, SSL_pending); LOAD(crypto, X509_verify_cert_error_string);
#undef LOAD
        if (ok) {
            s_ctx = O.SSL_CTX_new(O.TLS_client_method());
            ok = s_ctx && O.SSL_CTX_set_default_verify_paths(s_ctx) == 1;
            if (ok) O.SSL_CTX_set_verify(s_ctx, SSL_VERIFY_PEER, NULL);
        }
        s_ossl_state = ok ? 1 : -1;
    }
    bool ok = s_ossl_state == 1;
    pthread_mutex_unlock(&s_ossl_mx);
    return ok;
}

struct devos_conn {
    int fd;
    void *ssl;
};

devos_conn_t *devos_conn_open(const char *host, int port, bool tls, int timeout_ms, char *err, size_t errlen)
{
    if (tls && !ossl_load()) {
        snprintf(err, errlen, "HTTPS needs the host's libssl.so.3 (simulator)");
        return NULL;
    }
    int fd = devos_net_socket_connect(host, port, timeout_ms);
    if (fd < 0) {
        unreachable(host, port, err, errlen);
        return NULL;
    }
    devos_conn_t *c = calloc(1, sizeof(*c));
    c->fd = fd;
    if (!tls) return c;
    c->ssl = O.SSL_new(s_ctx);
    O.SSL_set_fd(c->ssl, fd);
    O.SSL_ctrl(c->ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, 0, (void *)host);
    O.SSL_set1_host(c->ssl, host);
    set_timeout(fd, timeout_ms);
    if (O.SSL_connect(c->ssl) != 1) {
        long v = O.SSL_get_verify_result(c->ssl);
        if (v != 0) snprintf(err, errlen, "Certificate rejected for %.60s: %.90s", host, O.X509_verify_cert_error_string(v));
        else snprintf(err, errlen, "TLS handshake with %.80s failed", host);
        devos_conn_close(c);
        return NULL;
    }
    return c;
}

int devos_conn_write_all(devos_conn_t *c, const void *buf, size_t len)
{
    if (!c) return -1;
    if (!c->ssl) return plain_write(c->fd, buf, len);
    size_t off = 0;
    while (off < len) {
        int n = O.SSL_write(c->ssl, (const char *)buf + off, (int)(len - off));
        if (n <= 0) {
            int e = O.SSL_get_error(c->ssl, n);
            if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

int devos_conn_read(devos_conn_t *c, void *buf, size_t len, int timeout_ms)
{
    if (!c) return -1;
    if (!c->ssl) return plain_read(c->fd, buf, len, timeout_ms);
    set_timeout(c->fd, timeout_ms > 0 ? timeout_ms : 1);
    int n = O.SSL_read(c->ssl, buf, (int)len);
    if (n > 0) return n;
    int e = O.SSL_get_error(c->ssl, n);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return DEVOS_CONN_TIMEOUT;
    if (e == SSL_ERROR_ZERO_RETURN) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return DEVOS_CONN_TIMEOUT;
    return n == 0 ? 0 : -1;
}

void devos_conn_close(devos_conn_t *c)
{
    if (!c) return;
    if (c->ssl) {
        O.SSL_shutdown(c->ssl);
        O.SSL_free(c->ssl);
    }
    if (c->fd >= 0) close(c->fd);
    free(c);
}
#endif
