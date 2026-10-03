#pragma once

/* devos_http: HTTP/1.1 client over the devos_net socket layer (so VPN routing
 * applies, invariant 7), with HTTPS through mbedTLS.
 *
 * - http:// and https://, any method, extra headers, a request body
 * - Content-Length, chunked and read-until-close responses, redirects
 * - certificates checked against the IDF CA bundle (the system CA file in
 *   the simulator); `insecure` skips the check for self-signed LAN services
 * - per-phase timings for the REST client
 *
 * Two ways to call it:
 *   devos_http_request()   blocking - only from a worker task, never the UI
 *   devos_http_submit()    queued for the Core 0 HTTP worker; poll the job from
 *                          an LVGL timer with devos_http_poll()
 *
 * The simulator does HTTPS only when built against mbedTLS headers
 * (-DDEVOS_MBEDTLS_ROOT=... or mbedtls-devel installed).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_HTTP_DEFAULT_MAX_BODY (256 * 1024)
#define DEVOS_HTTP_URL_MAX          1024

typedef struct {
    const char *method;         /* "GET", "POST", ... (NULL = GET) */
    const char *url;            /* http[s]://[user:pass@]host[:port][/path][?query] */
    const char *headers;        /* extra header lines "Name: value", LF or CRLF separated; may be NULL */
    const char *body;           /* may be NULL */
    size_t body_len;
    bool insecure;              /* https: accept any certificate */
    int timeout_ms;             /* connect / each read (0 = 10 s) */
    int max_redirects;          /* 0 = return redirects as they are */
    size_t max_body;            /* response body cap (0 = DEVOS_HTTP_DEFAULT_MAX_BODY) */
    volatile size_t *progress;  /* optional: body bytes received so far (for a progress bar) */
} devos_http_req_t;

typedef struct {
    int status;                 /* 0: no response (see error) */
    char reason[48];
    char *headers;              /* response header lines (CRLF), NUL-terminated, or NULL */
    size_t headers_len;
    char *body;                 /* NUL-terminated, or NULL */
    size_t body_len;            /* bytes kept */
    size_t body_total;          /* bytes the server sent (>= body_len) */
    bool truncated;             /* body_total > max_body */
    bool tls;                   /* went over TLS */
    bool tls_verified;          /* certificate chain + name checked */
    char tls_info[64];          /* e.g. "TLSv1.2 ECDHE-ECDSA-AES128-GCM-SHA256" */
    char error[112];            /* transport error, "" if status != 0 */
    char final_url[DEVOS_HTTP_URL_MAX];   /* after redirects */
    int redirects;
    int ms_dns, ms_connect, ms_tls, ms_first_byte, ms_total;
} devos_http_resp_t;

/* Blocking request. Always fills resp (free it with devos_http_resp_free).
 * Returns 0 if a response arrived (any status), -1 on transport error. */
int devos_http_request(const devos_http_req_t *req, devos_http_resp_t *resp);
void devos_http_resp_free(devos_http_resp_t *resp);

/* Create the worker and its synchronization primitives. Idempotent; safe to
 * call at boot before anything submits. false = out of memory or the worker
 * task/thread could not be created, in which case submit() returns -1. */
bool devos_http_init(void);
/* Queue a request (all strings are copied). Returns a job id > 0, or -1 when
 * the queue is full or the worker isn't available. */
int devos_http_submit(const devos_http_req_t *req);
/* 0 = still running, 1 = done (resp filled, now yours to free), -1 = no such job. */
int devos_http_poll(int job, devos_http_resp_t *resp);
/* Abandon a job. A queued job is released immediately; a running job is
 * signalled to abort and stops at its next read/connect step, with its slot
 * reclaimed by the worker when it returns (bounded by the per-phase timeout;
 * a blocking DNS resolution is not interruptible). */
void devos_http_cancel(int job);

/* Find header `name` (case-insensitive) in resp->headers; copies the value. */
bool devos_http_header(const devos_http_resp_t *resp, const char *name, char *out, size_t cap);

/* Split a URL. Returns 0 on success. path_out gets "/..." including the query. */
int devos_http_parse_url(const char *url, bool *https, char *host, size_t host_cap, int *port,
                         char *path, size_t path_cap);

/* Standard base64 (for Basic auth). Returns the output length. */
size_t devos_http_base64(const void *in, size_t n, char *out, size_t cap);

/* true if this build can do https:// */
bool devos_http_tls_available(void);

#ifdef __cplusplus
}
#endif
