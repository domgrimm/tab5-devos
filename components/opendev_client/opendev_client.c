/* opendev_client: OpenCode / OpenChamber HTTP+SSE engine. See opendev_client.h.
 *
 * Threading: REST requests are "jobs" executed by one background worker
 * (FreeRTOS task on the device, pthread in the simulator). The worker only
 * does network I/O; finished jobs are handed back and applied to the store in
 * opendev_client_poll(), so every store mutation happens on the polling (UI)
 * thread. The SSE link is non-blocking and also driven from poll.
 */
#include "opendev_client.h"
#include "devos_json.h"
#include "devos_net.h"
#include "devos_config.h"
#include "devos_conn.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#define BIG_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#else
#include <pthread.h>
#include <time.h>
#define BIG_ALLOC(n) malloc(n)
#define OPENDEV_NVS_FILE TAB5_SD_MOUNT_POINT "/.devos/opendev_nvs.json"
#endif

#define REST_TIMEOUT_MS   6000
#define SSE_BUF_MAX       (192 * 1024)      /* one event can carry a whole tool output */
#define REFETCH_DEBOUNCE_TICKS 5            /* 500 ms coalescing */
#define MSG_LIMIT         40
#define ROLE_MAP_MAX      64
#define SESSIONS_PATH     "/session?roots=true&limit=40"

/* ------------------------------------------------------------------ config */
/* The server is a URL: http://host:4096 for `opencode serve`, or an
 * OpenChamber URL such as https://dev-server.tail1234.ts.net. OpenChamber is
 * detected by probing /auth/session; its OpenCode API lives under /api and
 * needs a trusted-device token (oc_client_...) obtained once with the UI
 * password. Only the token is stored, never the password. */
typedef struct {
    char url[OPENDEV_URL_MAX];
    char host[OPENDEV_HOST_MAX];
    int port;
    bool tls;
    char base[64];                  /* path prefix from the URL, usually "" */
    opendev_mode_t mode;
    char token[OPENDEV_TOKEN_MAX];  /* OpenChamber client token */
} opendev_config_t;

static opendev_config_t s_cfg = {
    .url = "http://10.2.132.54:4096",
    .host = "10.2.132.54",
    .port = 4096,
    .tls = false,
    .base = "",
    .mode = OPENDEV_MODE_CODE,
    .token = "",
};

/* ------------------------------------------------------------------- store */
static EXT_RAM_BSS_ATTR opendev_session_t s_sessions[OPENDEV_MAX_SESSIONS];
static int s_session_count = 0;
static char s_active_id[OPENDEV_ID_MAX] = "";
static EXT_RAM_BSS_ATTR opendev_block_t s_blocks[OPENDEV_MAX_BLOCKS];
static int s_block_count = 0;
static uint32_t s_rev_counter = 0;
static opendev_permission_t s_perm;
static opendev_status_t s_status = OPENDEV_DOWN;
static char s_status_text[160] = "Offline";
static uint32_t s_gen = 0, s_blocks_gen = 0, s_diff_gen = 0;
static EXT_RAM_BSS_ATTR char s_diff[OPENDEV_DIFF_MAX] = "";
static EXT_RAM_BSS_ATTR opendev_diff_file_t s_diff_files[OPENDEV_DIFF_FILES];
static int s_diff_file_count = 0;
static bool s_diff_requested = false;       /* user asked for the diff of this session */
static char s_pending_select[OPENDEV_ID_MAX] = "";
static char *s_pending_send = NULL;         /* prompt waiting for a new session */
static char *s_pending_password = NULL;     /* used by the next login, then wiped */
static int s_inflight = 0;
static int s_msg_inflight = 0;               /* message loads in flight */

static EXT_RAM_BSS_ATTR struct { char msg[OPENDEV_ID_MAX]; uint8_t role; } s_roles[ROLE_MAP_MAX];
static int s_role_next = 0;

/* Event-stream link (owned by the SSE thread; these two are the controls) */
static volatile uint32_t s_link_gen = 0;    /* bump = reconnect with the current config */
static volatile bool s_link_want = false;

/* Deferred refetch flags (set by events, executed in poll) */
static bool s_want_sessions = false;
static bool s_want_messages = false;
static int s_refetch_ticks = 0;

static void bump(void) { s_gen++; }

/* Case-insensitive substring test (strcasestr is not portable). */
static bool ci_contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}
static void bump_blocks(void) { s_gen++; s_blocks_gen++; }

static void set_status(opendev_status_t st, const char *text)
{
    if (s_status != st) {
        s_status = st;
        bump();
    }
    if (text && strcmp(s_status_text, text) != 0) {
        snprintf(s_status_text, sizeof(s_status_text), "%s", text);
        bump();
    }
}

static int active_index(void)
{
    if (!s_active_id[0]) return -1;
    for (int i = 0; i < s_session_count; i++) {
        if (strcmp(s_sessions[i].id, s_active_id) == 0) return i;
    }
    return -1;
}

/* The SSE thread reads the config while the UI may change it. */
#ifdef ESP_PLATFORM
static SemaphoreHandle_t s_cfg_mx;
static void cfg_lock(void) { if (s_cfg_mx) xSemaphoreTake(s_cfg_mx, portMAX_DELAY); }
static void cfg_unlock(void) { if (s_cfg_mx) xSemaphoreGive(s_cfg_mx); }
#else
static pthread_mutex_t s_cfg_mx = PTHREAD_MUTEX_INITIALIZER;
static void cfg_lock(void) { pthread_mutex_lock(&s_cfg_mx); }
static void cfg_unlock(void) { pthread_mutex_unlock(&s_cfg_mx); }
#endif

/* Fill host/port/tls/base from cfg->url. */
static int cfg_apply_url(opendev_config_t *c, const char *url)
{
    char host[OPENDEV_HOST_MAX], path[64];
    int port = 0;
    bool tls = false;
    if (devos_url_parse(url, host, sizeof(host), &port, &tls, path, sizeof(path)) != 0) return -1;
    snprintf(c->host, sizeof(c->host), "%s", host);
    c->port = port;
    c->tls = tls;
    snprintf(c->base, sizeof(c->base), "%s", path);
    if (tls && port == 443) snprintf(c->url, sizeof(c->url), "https://%.95s%.50s", host, path);
    else snprintf(c->url, sizeof(c->url), "%s://%.95s:%d%.40s", tls ? "https" : "http", host, port, path);
    return 0;
}

/* ------------------------------------------------------------ persistence */
static void config_save(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("opendev", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "url", s_cfg.url);
        nvs_set_str(h, "host", s_cfg.host);
        nvs_set_u16(h, "port16", (uint16_t)s_cfg.port);
        nvs_set_u8(h, "mode", (uint8_t)s_cfg.mode);
        nvs_set_str(h, "token", s_cfg.token);
        nvs_commit(h);
        nvs_close(h);
    }
#else
    FILE *f = fopen(OPENDEV_NVS_FILE, "w");
    if (f) {
        fprintf(f, "{\n  \"url\": \"%s\",\n  \"mode\": %d,\n  \"token\": \"%s\"\n}\n", s_cfg.url, (int)s_cfg.mode,
                s_cfg.token);
        fclose(f);
    }
#endif
}

static void config_load(void)
{
    char url[OPENDEV_URL_MAX] = "";
    char host[OPENDEV_HOST_MAX] = "";
    int port = 0;
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("opendev", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(url);
        if (nvs_get_str(h, "url", url, &len) != ESP_OK) url[0] = '\0';
        len = sizeof(host);
        if (nvs_get_str(h, "host", host, &len) != ESP_OK) host[0] = '\0';
        uint16_t p16 = 0;
        if (nvs_get_u16(h, "port16", &p16) == ESP_OK) port = p16;
        uint8_t m = 0;
        if (nvs_get_u8(h, "mode", &m) == ESP_OK && m <= OPENDEV_MODE_CHAMBER) s_cfg.mode = (opendev_mode_t)m;
        len = sizeof(s_cfg.token);
        if (nvs_get_str(h, "token", s_cfg.token, &len) != ESP_OK) s_cfg.token[0] = '\0';
        nvs_close(h);
    }
#else
    FILE *f = fopen(OPENDEV_NVS_FILE, "r");
    if (f) {
        char buf[320], val[256];
        int ival = 0;
        while (fgets(buf, sizeof(buf), f)) {
            if (sscanf(buf, " \"url\": \"%255[^\"]\"", val) == 1) snprintf(url, sizeof(url), "%s", val);
            else if (sscanf(buf, " \"host\": \"%63[^\"]\"", val) == 1) snprintf(host, sizeof(host), "%s", val);
            else if (sscanf(buf, " \"port\": %d", &ival) == 1) port = ival;
            else if (sscanf(buf, " \"mode\": %d", &ival) == 1 && (ival == 0 || ival == 1)) s_cfg.mode = (opendev_mode_t)ival;
            else if (sscanf(buf, " \"token\": \"%127[^\"]\"", val) == 1) snprintf(s_cfg.token, sizeof(s_cfg.token), "%s", val);
        }
        fclose(f);
    }
#endif
    /* older configs stored host + port only */
    if (!url[0] && host[0]) snprintf(url, sizeof(url), "http://%s:%d", host, port > 0 ? port : 4096);
    if (url[0]) cfg_apply_url(&s_cfg, url);
}

/* ------------------------------------------------------------ JSON helpers */
/* Span of the object/array value of `key` inside [p,end). */
static bool json_obj(const char *p, const char *end, const char *key, const char **op, const char **oe)
{
    const char *v = devos_json_find_key(p, end, key);
    if (!v || v >= end || (*v != '{' && *v != '[')) return false;
    const char *e = devos_json_span(v, end);
    if (!e) return false;
    *op = v;
    *oe = e;
    return true;
}

static int json_str(const char *p, const char *end, const char *key, char *out, size_t cap)
{
    if (!p || p >= end) { if (cap) out[0] = '\0'; return -1; }
    int rc = devos_json_get_str(p, (size_t)(end - p), key, out, cap);
    if (rc != 0 && cap) out[0] = '\0';
    return rc;
}

static long long json_i64(const char *p, const char *end, const char *key)
{
    const char *v = devos_json_find_key(p, end, key);
    if (!v || v >= end) return 0;
    return strtoll(v, NULL, 10);
}

static bool json_true(const char *p, const char *end, const char *key)
{
    const char *v = devos_json_find_key(p, end, key);
    return v && v < end && *v == 't';
}

/* Length of the raw (still escaped) JSON string value of key, or -1. */
static long json_str_raw_len(const char *p, const char *end, const char *key, const char **start)
{
    const char *v = devos_json_find_key(p, end, key);
    if (!v || v >= end || *v != '"') return -1;
    const char *q = v + 1;
    while (q < end && *q != '"') q += (*q == '\\') ? 2 : 1;
    if (q >= end) return -1;
    *start = v;
    return (long)(q - v);
}

/* Heap copy of a (possibly huge) unescaped string value; caller frees. */
static char *json_str_dup(const char *p, const char *end, const char *key)
{
    const char *start = NULL;
    long raw = json_str_raw_len(p, end, key, &start);
    if (raw < 0) return NULL;
    char *out = BIG_ALLOC((size_t)raw + 1);
    if (!out) return NULL;
    if (!devos_json_parse_str(start, end, out, (size_t)raw + 1)) {
        free(out);
        return NULL;
    }
    return out;
}

/* ================================================================== jobs */
typedef enum {
    JOB_SESSIONS, JOB_MESSAGES, JOB_NEW_SESSION, JOB_SEND, JOB_ABORT, JOB_PERM, JOB_PERM_V2, JOB_DIFF,
    JOB_PROBE,          /* GET /auth/session: OpenChamber or plain OpenCode? */
    JOB_LOGIN,          /* POST /auth/session with the password */
    JOB_EVENT,          /* from the SSE thread: one event (resp = data JSON, NULL = resync) */
    JOB_LINK,           /* from the SSE thread: link state change (status, err) */
} job_kind_t;

typedef struct job {
    job_kind_t kind;
    char method[8];
    char path[256];
    char *body;
    char sid[OPENDEV_ID_MAX];
    char aux[OPENDEV_ID_MAX];
    opendev_config_t cfg;
    size_t cap;
    int rc, status;
    char *resp;
    size_t resp_len;
    char *hdrs;             /* response headers (login: Set-Cookie) */
    char err[128];
} job_t;

static void job_free(job_t *j)
{
    if (!j) return;
    if (j->body && j->kind == JOB_LOGIN) memset(j->body, 0, strlen(j->body));   /* holds the password */
    free(j->body);
    free(j->resp);
    free(j->hdrs);
    free(j);
}

static int auth_header(const opendev_config_t *c, char *out, size_t n)
{
    if (!c->token[0]) { out[0] = '\0'; return 0; }
    if (strncmp(c->token, "cookie:", 7) == 0) return snprintf(out, n, "Cookie: %s\r\n", c->token + 7);
    return snprintf(out, n, "Authorization: Bearer %s\r\n", c->token);
}

static void host_header(const opendev_config_t *c, char *out, size_t n)
{
    if ((c->tls && c->port == 443) || (!c->tls && c->port == 80)) snprintf(out, n, "%.90s", c->host);
    else snprintf(out, n, "%.80s:%d", c->host, c->port);
}

/* Minimal HTTP/1.1 client (worker thread only): Connection: close, read to
 * EOF, then split headers and de-chunk in place. Plain TCP or TLS. */
static void http_run(job_t *j)
{
    j->rc = -1;
    j->status = 0;
    j->resp = BIG_ALLOC(j->cap);
    if (!j->resp) { snprintf(j->err, sizeof(j->err), "Out of memory"); return; }
    j->resp[0] = '\0';

    devos_conn_t *c = devos_conn_open(j->cfg.host, j->cfg.port, j->cfg.tls, REST_TIMEOUT_MS, j->err, sizeof(j->err));
    if (!c) return;
    size_t blen = j->body ? strlen(j->body) : 0;
    char auth[200], hosth[96], req[900];
    auth_header(&j->cfg, auth, sizeof(auth));
    host_header(&j->cfg, hosth, sizeof(hosth));
    int hlen = snprintf(req, sizeof(req),
                        "%s %s HTTP/1.1\r\nHost: %s\r\nAccept: application/json\r\nUser-Agent: devOS-Tab5\r\n"
                        "Content-Type: application/json\r\nContent-Length: %u\r\n%sConnection: close\r\n\r\n",
                        j->method, j->path, hosth, (unsigned)blen, auth);
    if (hlen <= 0 || (size_t)hlen >= sizeof(req) || devos_conn_write_all(c, req, (size_t)hlen) != 0 ||
        (blen && devos_conn_write_all(c, j->body, blen) != 0)) {
        snprintf(j->err, sizeof(j->err), "Request failed");
        devos_conn_close(c);
        return;
    }
    size_t total = 0;
    for (;;) {
        size_t room = j->cap - 1 - total;
        if (room == 0) break;
        int n = devos_conn_read(c, j->resp + total, room > 16384 ? 16384 : room, REST_TIMEOUT_MS);
        if (n <= 0) break;
        total += (size_t)n;
    }
    devos_conn_close(c);
    j->resp[total] = '\0';

    int status = 0;
    if (sscanf(j->resp, "HTTP/%*d.%*d %d", &status) != 1 && sscanf(j->resp, "HTTP/%*d %d", &status) != 1) {
        snprintf(j->err, sizeof(j->err), total ? "Not an HTTP server" : "No response");
        return;
    }
    j->status = status;
    char *hdr_end = strstr(j->resp, "\r\n\r\n");
    if (!hdr_end) return;
    *hdr_end = '\0';
    j->hdrs = strdup(j->resp);
    bool chunked = ci_contains(j->resp, "transfer-encoding: chunked");
    char *body = hdr_end + 4;
    char *end = j->resp + total;
    size_t out = 0;
    if (chunked) {
        char *p = body;
        while (p < end) {
            char *eol = strstr(p, "\r\n");
            if (!eol) break;
            unsigned long cl = strtoul(p, NULL, 16);
            p = eol + 2;
            if (cl == 0) break;
            if (cl > (unsigned long)(end - p)) cl = (unsigned long)(end - p);   /* truncated response */
            memmove(j->resp + out, p, cl);                                   /* out <= p always */
            out += cl;
            p += cl;
            if (p + 2 <= end && p[0] == '\r' && p[1] == '\n') p += 2;
        }
    } else {
        out = (size_t)(end - body);
        memmove(j->resp, body, out);
    }
    j->resp[out] = '\0';
    j->resp_len = out;
    j->rc = 0;
}

/* ---- queues: REST jobs in, results (and SSE events) out ---- */
#define QDEPTH 64
#ifdef ESP_PLATFORM
static QueueHandle_t s_jobq, s_resq;

static bool job_submit(job_t *j) { return xQueueSend(s_jobq, &j, 0) == pdTRUE; }
static void post_result(job_t *j)
{
    while (xQueueSend(s_resq, &j, pdMS_TO_TICKS(1000)) != pdTRUE) {
    }
}
static job_t *result_take(void)
{
    job_t *j = NULL;
    return xQueueReceive(s_resq, &j, 0) == pdTRUE ? j : NULL;
}
static void sleep_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        job_t *j = NULL;
        if (xQueueReceive(s_jobq, &j, portMAX_DELAY) != pdTRUE || !j) continue;
        http_run(j);
        post_result(j);
    }
}
#else
typedef struct { job_t *items[QDEPTH]; int head, count; } jq_t;
static jq_t s_jq, s_rq;
static pthread_mutex_t s_qmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_qcv = PTHREAD_COND_INITIALIZER;

static bool jq_push(jq_t *q, job_t *j)
{
    if (q->count >= QDEPTH) return false;
    q->items[(q->head + q->count++) % QDEPTH] = j;
    return true;
}

static job_t *jq_pop(jq_t *q)
{
    if (!q->count) return NULL;
    job_t *j = q->items[q->head];
    q->head = (q->head + 1) % QDEPTH;
    q->count--;
    return j;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static bool job_submit(job_t *j)
{
    pthread_mutex_lock(&s_qmx);
    bool ok = jq_push(&s_jq, j);
    pthread_cond_broadcast(&s_qcv);
    pthread_mutex_unlock(&s_qmx);
    return ok;
}

static void post_result(job_t *j)
{
    for (;;) {
        pthread_mutex_lock(&s_qmx);
        bool ok = jq_push(&s_rq, j);
        pthread_mutex_unlock(&s_qmx);
        if (ok) return;
        sleep_ms(50);
    }
}

static job_t *result_take(void)
{
    pthread_mutex_lock(&s_qmx);
    job_t *j = jq_pop(&s_rq);
    pthread_mutex_unlock(&s_qmx);
    return j;
}

static void *worker_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_qmx);
        while (!s_jq.count) pthread_cond_wait(&s_qcv, &s_qmx);
        job_t *j = jq_pop(&s_jq);
        pthread_mutex_unlock(&s_qmx);
        http_run(j);
        post_result(j);
    }
    return NULL;
}
#endif

static int enqueue(job_kind_t kind, const char *method, const char *path, const char *body, const char *sid,
                   const char *aux, size_t cap)
{
    job_t *j = calloc(1, sizeof(job_t));
    if (!j) return -1;
    j->kind = kind;
    snprintf(j->method, sizeof(j->method), "%s", method);
    /* OpenChamber proxies the OpenCode API under /api; its own auth routes are not */
    bool api = kind != JOB_PROBE && kind != JOB_LOGIN && s_cfg.mode == OPENDEV_MODE_CHAMBER;
    snprintf(j->path, sizeof(j->path), "%s%s%s", s_cfg.base, api ? "/api" : "", path);
    j->body = body ? strdup(body) : NULL;
    snprintf(j->sid, sizeof(j->sid), "%s", sid ? sid : "");
    snprintf(j->aux, sizeof(j->aux), "%s", aux ? aux : "");
    j->cfg = s_cfg;
    j->cap = cap;
    if (!job_submit(j)) {
        job_free(j);
        return -1;
    }
    s_inflight++;
    if (kind == JOB_MESSAGES) s_msg_inflight++;
    bump();
    return 0;
}

/* ============================================================ blocks */
static void role_remember(const char *msg_id, uint8_t role)
{
    if (!msg_id || !*msg_id) return;
    for (int i = 0; i < ROLE_MAP_MAX; i++) {
        if (strcmp(s_roles[i].msg, msg_id) == 0) { s_roles[i].role = role; return; }
    }
    snprintf(s_roles[s_role_next].msg, sizeof(s_roles[0].msg), "%s", msg_id);
    s_roles[s_role_next].role = role;
    s_role_next = (s_role_next + 1) % ROLE_MAP_MAX;
}

static int role_lookup(const char *msg_id)
{
    for (int i = 0; i < ROLE_MAP_MAX; i++) {
        if (msg_id && *msg_id && strcmp(s_roles[i].msg, msg_id) == 0) return s_roles[i].role;
    }
    return -1;
}

/* Append `src` (first `max_lines` lines) to dst. */
static void append_lines(char *dst, size_t cap, const char *src, int max_lines)
{
    size_t have = strlen(dst);
    int lines = 0;
    for (const char *p = src; *p && have + 4 < cap; p++) {
        if (*p == '\n' && ++lines >= max_lines) {
            snprintf(dst + have, cap - have, "\n...");
            return;
        }
        dst[have++] = *p;
    }
    dst[have] = '\0';
}

/* Turn one OpenCode message part into a block. false = not shown. */
static bool part_to_block(const char *p, const char *e, opendev_block_t *b)
{
    char type[24];
    json_str(p, e, "type", type, sizeof(type));
    json_str(p, e, "id", b->part_id, sizeof(b->part_id));
    json_str(p, e, "messageID", b->msg_id, sizeof(b->msg_id));
    b->text[0] = '\0';
    if (strcmp(type, "text") == 0) {
        if (json_true(p, e, "synthetic")) return false;
        b->kind = OPENDEV_KIND_TEXT;
        json_str(p, e, "text", b->text, sizeof(b->text));
        return b->text[0] != '\0';
    }
    if (strcmp(type, "reasoning") == 0) {
        b->kind = OPENDEV_KIND_THINK;
        json_str(p, e, "text", b->text, sizeof(b->text));
        return b->text[0] != '\0';
    }
    if (strcmp(type, "tool") == 0) {
        b->kind = OPENDEV_KIND_TOOL;
        char tool[48], status[24] = "", title[160] = "";
        json_str(p, e, "tool", tool, sizeof(tool));
        const char *sp, *se;
        char detail[400] = "";
        if (json_obj(p, e, "state", &sp, &se)) {
            json_str(sp, se, "status", status, sizeof(status));
            json_str(sp, se, "title", title, sizeof(title));
            const char *ip, *ie;
            if (json_obj(sp, se, "input", &ip, &ie)) {
                static const char *const keys[] = { "command", "filePath", "path", "pattern", "url", "query",
                                                    "description", "prompt" };
                for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]) && !detail[0]; k++) {
                    json_str(ip, ie, keys[k], detail, sizeof(detail));
                }
            }
        }
        snprintf(b->text, sizeof(b->text), "%s  [%s]%s%s", tool[0] ? tool : "tool", status[0] ? status : "?",
                 title[0] ? "  " : "", title);
        if (detail[0] && strcmp(detail, title) != 0) {
            size_t n = strlen(b->text);
            snprintf(b->text + n, sizeof(b->text) - n, "\n%s%.380s", strcmp(tool, "bash") == 0 ? "$ " : "", detail);
        }
        if (json_obj(p, e, "state", &sp, &se)) {
            char *out = json_str_dup(sp, se, "output");
            if (!out) out = json_str_dup(sp, se, "error");
            if (out) {
                size_t ol = strlen(out);
                while (ol > 0 && (out[ol - 1] == '\n' || out[ol - 1] == '\r' || out[ol - 1] == ' ')) out[--ol] = '\0';
            }
            if (out && out[0]) {
                size_t n = strlen(b->text);
                snprintf(b->text + n, sizeof(b->text) - n, "\n");
                append_lines(b->text, sizeof(b->text), out, 12);
            }
            free(out);
        }
        return true;
    }
    return false;       /* step-start/-finish, snapshot, patch, file, agent... */
}

static void drop_oldest_if_full(void)
{
    if (s_block_count >= OPENDEV_MAX_BLOCKS) {
        memmove(s_blocks, s_blocks + 1, sizeof(s_blocks[0]) * (OPENDEV_MAX_BLOCKS - 1));
        s_block_count = OPENDEV_MAX_BLOCKS - 1;
    }
}

static void push_local(uint8_t role, uint8_t kind, const char *text)
{
    if (!text || !*text) return;
    drop_oldest_if_full();
    opendev_block_t *b = &s_blocks[s_block_count++];
    memset(b, 0, sizeof(*b) - sizeof(b->text));
    b->role = role;
    b->kind = kind;
    b->rev = ++s_rev_counter;
    snprintf(b->text, sizeof(b->text), "%s", text);
    bump_blocks();
}

/* Insert or update a block from a streamed/fetched part. */
static void upsert_block(const opendev_block_t *nb)
{
    for (int i = s_block_count - 1; i >= 0; i--) {
        opendev_block_t *b = &s_blocks[i];
        if (b->part_id[0] && strcmp(b->part_id, nb->part_id) == 0) {
            if (b->kind != nb->kind || strcmp(b->text, nb->text) != 0) {
                b->kind = nb->kind;
                memcpy(b->text, nb->text, strlen(nb->text) + 1);
                b->rev = ++s_rev_counter;
                bump_blocks();
            }
            return;
        }
    }
    /* Our optimistic copy of a prompt becomes the real part. */
    if (nb->role == OPENDEV_ROLE_USER && nb->kind == OPENDEV_KIND_TEXT) {
        for (int i = s_block_count - 1; i >= 0 && i >= s_block_count - 4; i--) {
            opendev_block_t *b = &s_blocks[i];
            if (!b->part_id[0] && b->role == OPENDEV_ROLE_USER && strcmp(b->text, nb->text) == 0) {
                snprintf(b->part_id, sizeof(b->part_id), "%s", nb->part_id);
                snprintf(b->msg_id, sizeof(b->msg_id), "%s", nb->msg_id);
                return;
            }
        }
    }
    drop_oldest_if_full();
    opendev_block_t *b = &s_blocks[s_block_count++];
    memcpy(b, nb, sizeof(*b) - sizeof(b->text));
    memcpy(b->text, nb->text, strlen(nb->text) + 1);
    b->rev = ++s_rev_counter;
    bump_blocks();
}

static void remove_blocks(const char *part_id, const char *msg_id)
{
    int w = 0;
    for (int i = 0; i < s_block_count; i++) {
        bool drop = (part_id && part_id[0] && strcmp(s_blocks[i].part_id, part_id) == 0) ||
                    (msg_id && msg_id[0] && strcmp(s_blocks[i].msg_id, msg_id) == 0);
        if (!drop) {
            if (w != i) s_blocks[w] = s_blocks[i];
            w++;
        }
    }
    if (w != s_block_count) {
        s_block_count = w;
        bump_blocks();
    }
}

static void set_busy(const char *session_id, bool busy)
{
    for (int i = 0; i < s_session_count; i++) {
        if (!session_id || !*session_id || strcmp(s_sessions[i].id, session_id) == 0) {
            if (s_sessions[i].busy != busy) {
                s_sessions[i].busy = busy;
                bump();
            }
            if (session_id && *session_id) break;
        }
    }
}

static void set_session_model(const char *sid, const char *model)
{
    for (int i = 0; i < s_session_count && model && *model; i++) {
        if (strcmp(s_sessions[i].id, sid) == 0 && strcmp(s_sessions[i].model, model) != 0) {
            snprintf(s_sessions[i].model, sizeof(s_sessions[i].model), "%s", model);
            bump();
        }
    }
}

/* ============================================================ diffs */
typedef struct { const char *p; int len; } dline_t;
typedef struct { char *buf; size_t cap, len; bool full; } dstr_t;

static void ds_add(dstr_t *d, const char *s, size_t n)
{
    if (d->full) return;
    if (d->len + n + 32 >= d->cap) {
        const char *msg = "\n... (diff truncated)\n";
        size_t m = strlen(msg);
        if (d->len + m < d->cap) { memcpy(d->buf + d->len, msg, m); d->len += m; }
        d->full = true;
        d->buf[d->len] = '\0';
        return;
    }
    memcpy(d->buf + d->len, s, n);
    d->len += n;
    d->buf[d->len] = '\0';
}

static void ds_printf(dstr_t *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ds_printf(dstr_t *d, const char *fmt, ...)
{
    char tmp[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) ds_add(d, tmp, (size_t)(n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1));
}

static int split_lines(const char *text, dline_t **out)
{
    int n = 0;
    if (text && *text) {
        n = 1;
        for (const char *p = text; *p; p++) if (*p == '\n' && p[1]) n++;
    }
    *out = NULL;
    if (!n) return 0;
    dline_t *a = BIG_ALLOC(sizeof(dline_t) * (size_t)n);
    if (!a) return -1;
    int i = 0;
    const char *s = text;
    while (*s && i < n) {
        const char *e = strchr(s, '\n');
        int len = e ? (int)(e - s) : (int)strlen(s);
        a[i].p = s;
        a[i].len = len;
        i++;
        s = e ? e + 1 : s + len;
    }
    *out = a;
    return i;
}

static bool line_eq(const dline_t *a, const dline_t *b)
{
    return a->len == b->len && memcmp(a->p, b->p, (size_t)a->len) == 0;
}

/* Edit script ('=', '-', '+') for a -> b: common prefix/suffix, LCS on the
 * middle when it is small enough, else replace the middle wholesale. */
static char *edit_script(const dline_t *a, int na, const dline_t *b, int nb, int *nops)
{
    int pre = 0;
    while (pre < na && pre < nb && line_eq(&a[pre], &b[pre])) pre++;
    int suf = 0;
    while (suf < na - pre && suf < nb - pre && line_eq(&a[na - 1 - suf], &b[nb - 1 - suf])) suf++;
    int n = na - pre - suf, m = nb - pre - suf;
    char *ops = BIG_ALLOC((size_t)(na + nb + 1));
    if (!ops) return NULL;
    int k = 0;
    for (int i = 0; i < pre; i++) ops[k++] = '=';
    long cells = (long)(n + 1) * (long)(m + 1);
    uint8_t *dir = (n > 0 && m > 0 && cells <= 4000000L) ? BIG_ALLOC((size_t)((cells + 3) / 4)) : NULL;
    int *prev = dir ? calloc((size_t)m + 1, sizeof(int)) : NULL, *cur = dir ? calloc((size_t)m + 1, sizeof(int)) : NULL;
    if (dir && prev && cur) {
        /* 2-bit directions: 1 diag, 2 up (delete), 3 left (insert) */
        memset(dir, 0, (size_t)((cells + 3) / 4));
        for (int i = 1; i <= n; i++) {
            cur[0] = 0;
            for (int j = 1; j <= m; j++) {
                long c = (long)i * (m + 1) + j;
                int d;
                if (line_eq(&a[pre + i - 1], &b[pre + j - 1])) { cur[j] = prev[j - 1] + 1; d = 1; }
                else if (prev[j] > cur[j - 1]) { cur[j] = prev[j]; d = 2; }   /* ties: '-' before '+' */
                else { cur[j] = cur[j - 1]; d = 3; }
                dir[c >> 2] |= (uint8_t)(d << ((c & 3) * 2));
            }
            int *t = prev; prev = cur; cur = t;
        }
        char *rev = BIG_ALLOC((size_t)(n + m + 1));
        int r = 0, i = n, j = m;
        while (rev && (i > 0 || j > 0)) {
            int d = (i > 0 && j > 0) ? (dir[((long)i * (m + 1) + j) >> 2] >> ((((long)i * (m + 1) + j) & 3) * 2)) & 3
                                     : (i > 0 ? 2 : 3);
            if (d == 1) { rev[r++] = '='; i--; j--; }
            else if (d == 2) { rev[r++] = '-'; i--; }
            else { rev[r++] = '+'; j--; }
        }
        while (r > 0 && rev) ops[k++] = rev[--r];
        free(rev);
    } else {
        for (int i = 0; i < n; i++) ops[k++] = '-';
        for (int j = 0; j < m; j++) ops[k++] = '+';
    }
    free(dir);
    free(prev);
    free(cur);
    for (int i = 0; i < suf; i++) ops[k++] = '=';
    *nops = k;
    return ops;
}

static void emit_line(dstr_t *d, char tag, const dline_t *l)
{
    char c[1] = { tag };
    ds_add(d, c, 1);
    ds_add(d, l->p, (size_t)(l->len > 400 ? 400 : l->len));
    ds_add(d, "\n", 1);
}

static void unified_diff(dstr_t *d, const char *file, const char *before, const char *after)
{
    dline_t *a = NULL, *b = NULL;
    int na = split_lines(before, &a), nb = split_lines(after, &b);
    if (na < 0 || nb < 0) { free(a); free(b); ds_printf(d, "(out of memory diffing %s)\n", file); return; }
    int nops = 0;
    char *ops = edit_script(a, na, b, nb, &nops);
    ds_printf(d, "diff --git a/%s b/%s\n", file, file);
    ds_printf(d, "--- %s%s\n", na ? "a/" : "/dev/null", na ? file : "");
    ds_printf(d, "+++ %s%s\n", nb ? "b/" : "/dev/null", nb ? file : "");
    const int ctx = 3;
    int *ia = ops ? BIG_ALLOC(sizeof(int) * (size_t)(nops + 1)) : NULL;
    int *ib = ops ? BIG_ALLOC(sizeof(int) * (size_t)(nops + 1)) : NULL;
    if (ops && ia && ib) {
        ia[0] = ib[0] = 0;
        for (int k = 0; k < nops; k++) {
            ia[k + 1] = ia[k] + (ops[k] != '+');
            ib[k + 1] = ib[k] + (ops[k] != '-');
        }
        int k = 0;
        while (k < nops && !d->full) {
            if (ops[k] == '=') { k++; continue; }
            int start = k - ctx < 0 ? 0 : k - ctx;
            int last = k, e = k;
            while (e < nops) {
                if (ops[e] != '=') { last = e; e++; continue; }
                int run = 0;
                while (e + run < nops && ops[e + run] == '=') run++;
                if (e + run >= nops || run > 2 * ctx) break;
                e += run;
            }
            int end = last + 1 + ctx > nops ? nops : last + 1 + ctx;
            int oc = ia[end] - ia[start], nc = ib[end] - ib[start];
            ds_printf(d, "@@ -%d,%d +%d,%d @@\n", oc ? ia[start] + 1 : ia[start], oc, nc ? ib[start] + 1 : ib[start], nc);
            for (int x = start; x < end && !d->full; x++) {
                if (ops[x] == '=') emit_line(d, ' ', &a[ia[x]]);
                else if (ops[x] == '-') emit_line(d, '-', &a[ia[x]]);
                else emit_line(d, '+', &b[ib[x]]);
            }
            k = end;
        }
    } else {
        ds_printf(d, "(file too large to diff on the device)\n");
    }
    free(ia);
    free(ib);
    free(ops);
    free(a);
    free(b);
}

static void diff_file_cb(const char *obj, size_t len, void *ud)
{
    dstr_t *d = ud;
    const char *e = obj + len;
    char file[128] = "";
    json_str(obj, e, "file", file, sizeof(file));
    if (!file[0]) json_str(obj, e, "path", file, sizeof(file));
    if (s_diff_file_count < OPENDEV_DIFF_FILES) {
        opendev_diff_file_t *f = &s_diff_files[s_diff_file_count++];
        snprintf(f->path, sizeof(f->path), "%s", file[0] ? file : "?");
        f->additions = (int)json_i64(obj, e, "additions");
        f->deletions = (int)json_i64(obj, e, "deletions");
    }
    char *patch = json_str_dup(obj, e, "patch");
    if (!patch) patch = json_str_dup(obj, e, "diff");
    if (patch) {
        ds_add(d, patch, strlen(patch));
        if (patch[0] && patch[strlen(patch) - 1] != '\n') ds_add(d, "\n", 1);
        free(patch);
        return;
    }
    char *before = json_str_dup(obj, e, "before");
    char *after = json_str_dup(obj, e, "after");
    unified_diff(d, file[0] ? file : "?", before ? before : "", after ? after : "");
    free(before);
    free(after);
}

/* OpenCode returns [{file, before, after, additions, deletions}]; plain
 * unified text is accepted as-is. */
static void diff_build(const char *body, size_t len)
{
    s_diff_file_count = 0;
    dstr_t d = { s_diff, sizeof(s_diff), 0, false };
    s_diff[0] = '\0';
    const char *p = body;
    while (p < body + len && isspace((unsigned char)*p)) p++;
    if (p < body + len && *p == '[') {
        devos_json_array_each(p, len - (size_t)(p - body), diff_file_cb, &d);
    } else if (p < body + len && *p == '{') {
        const char *ap, *ae;
        if (json_obj(p, body + len, "diff", &ap, &ae) && *ap == '[') {
            devos_json_array_each(ap, (size_t)(ae - ap), diff_file_cb, &d);
        }
    } else if (len) {
        ds_add(&d, p, len - (size_t)(p - body));
    }
    if (!s_diff[0]) snprintf(s_diff, sizeof(s_diff), "(no changes)");
    s_diff_gen++;
    bump();
}

/* ============================================================ SSE events */
static bool is_active(const char *sid)
{
    return s_active_id[0] && sid && strcmp(sid, s_active_id) == 0;
}

static void on_part_updated(const char *pp, const char *pe)
{
    char sid[OPENDEV_ID_MAX];
    json_str(pp, pe, "sessionID", sid, sizeof(sid));
    if (!is_active(sid)) return;
    static EXT_RAM_BSS_ATTR opendev_block_t nb;
    memset(&nb, 0, sizeof(nb) - sizeof(nb.text));
    if (!part_to_block(pp, pe, &nb)) return;
    int role = role_lookup(nb.msg_id);
    nb.role = role < 0 ? OPENDEV_ROLE_ASST : (uint8_t)role;
    upsert_block(&nb);
}

static void on_sse_event(const char *data, size_t dlen)
{
    const char *end = data + dlen;
    char type[48];
    if (json_str(data, end, "type", type, sizeof(type)) != 0) return;
    const char *pp = data, *pe = end;
    json_obj(data, end, "properties", &pp, &pe);
    char sid[OPENDEV_ID_MAX] = "";

    if (strcmp(type, "message.part.updated") == 0) {
        const char *ap, *ae;
        if (json_obj(pp, pe, "part", &ap, &ae)) on_part_updated(ap, ae);
        else if (s_active_id[0]) s_want_messages = true;
    } else if (strcmp(type, "message.part.delta") == 0) {
        /* streaming: only the new characters of one field of a part */
        char part[OPENDEV_ID_MAX], mid[OPENDEV_ID_MAX], field[16];
        json_str(pp, pe, "sessionID", sid, sizeof(sid));
        if (!is_active(sid)) return;
        json_str(pp, pe, "partID", part, sizeof(part));
        json_str(pp, pe, "messageID", mid, sizeof(mid));
        json_str(pp, pe, "field", field, sizeof(field));
        if (strcmp(field, "text") != 0) return;
        char *delta = json_str_dup(pp, pe, "delta");
        if (!delta) return;
        opendev_block_t *b = NULL;
        for (int i = s_block_count - 1; i >= 0 && !b; i--) {
            if (strcmp(s_blocks[i].part_id, part) == 0) b = &s_blocks[i];
        }
        if (!b) {
            drop_oldest_if_full();
            b = &s_blocks[s_block_count++];
            memset(b, 0, sizeof(*b) - sizeof(b->text));
            int role = role_lookup(mid);
            b->role = role < 0 ? OPENDEV_ROLE_ASST : (uint8_t)role;
            b->kind = OPENDEV_KIND_TEXT;                /* corrected by the next part.updated */
            snprintf(b->part_id, sizeof(b->part_id), "%s", part);
            snprintf(b->msg_id, sizeof(b->msg_id), "%s", mid);
            b->text[0] = '\0';
        }
        size_t have = strlen(b->text), add = strlen(delta);
        if (have + add >= sizeof(b->text)) add = sizeof(b->text) - 1 - have;
        memcpy(b->text + have, delta, add);
        b->text[have + add] = '\0';
        b->rev = ++s_rev_counter;
        bump_blocks();
        free(delta);
    } else if (strcmp(type, "question.asked") == 0) {
        json_str(pp, pe, "sessionID", sid, sizeof(sid));
        char q[300] = "";
        const char *ap, *ae;
        if (json_obj(pp, pe, "questions", &ap, &ae)) json_str(ap, ae, "question", q, sizeof(q));
        if (!sid[0] || is_active(sid)) {
            char line[380];
            snprintf(line, sizeof(line), "The agent is asking: %s\n(Answer it in OpenCode on your computer.)", q[0] ? q : "?");
            push_local(OPENDEV_ROLE_ASST, OPENDEV_KIND_TEXT, line);
        }
    } else if (strcmp(type, "message.part.removed") == 0) {
        char part[OPENDEV_ID_MAX];
        json_str(pp, pe, "sessionID", sid, sizeof(sid));
        json_str(pp, pe, "partID", part, sizeof(part));
        if (is_active(sid)) remove_blocks(part, NULL);
    } else if (strcmp(type, "message.updated") == 0) {
        const char *ip = pp, *ie = pe;
        json_obj(pp, pe, "info", &ip, &ie);
        char mid[OPENDEV_ID_MAX], role[16], model[OPENDEV_TITLE_MAX];
        json_str(ip, ie, "id", mid, sizeof(mid));
        json_str(ip, ie, "role", role, sizeof(role));
        json_str(ip, ie, "sessionID", sid, sizeof(sid));
        role_remember(mid, strcmp(role, "user") == 0 ? OPENDEV_ROLE_USER : OPENDEV_ROLE_ASST);
        if (strcmp(role, "assistant") == 0 && json_str(ip, ie, "modelID", model, sizeof(model)) == 0) {
            set_session_model(sid, model);
        }
    } else if (strcmp(type, "message.removed") == 0) {
        char mid[OPENDEV_ID_MAX];
        json_str(pp, pe, "sessionID", sid, sizeof(sid));
        json_str(pp, pe, "messageID", mid, sizeof(mid));
        if (is_active(sid)) remove_blocks(NULL, mid);
    } else if (strcmp(type, "session.status") == 0) {
        json_str(pp, pe, "sessionID", sid, sizeof(sid));
        const char *st, *se;
        char st_type[16] = "";
        if (json_obj(pp, pe, "status", &st, &se)) json_str(st, se, "type", st_type, sizeof(st_type));
        set_busy(sid[0] ? sid : NULL, strcmp(st_type, "busy") == 0 || strcmp(st_type, "retry") == 0);
    } else if (strcmp(type, "session.idle") == 0) {
        json_str(pp, pe, "sessionID", sid, sizeof(sid));
        set_busy(sid[0] ? sid : NULL, false);
    } else if (strcmp(type, "session.updated") == 0) {
        const char *ip = pp, *ie = pe;
        json_obj(pp, pe, "info", &ip, &ie);
        char id[OPENDEV_ID_MAX], title[OPENDEV_TITLE_MAX];
        json_str(ip, ie, "id", id, sizeof(id));
        bool found = false;
        for (int i = 0; i < s_session_count; i++) {
            if (strcmp(s_sessions[i].id, id) == 0) {
                found = true;
                if (json_str(ip, ie, "title", title, sizeof(title)) == 0 && strcmp(title, s_sessions[i].title) != 0) {
                    snprintf(s_sessions[i].title, sizeof(s_sessions[i].title), "%s", title);
                    bump();
                }
            }
        }
        if (!found && !devos_json_find_key(ip, ie, "parentID")) s_want_sessions = true;
    } else if (strcmp(type, "session.created") == 0 || strcmp(type, "session.deleted") == 0) {
        s_want_sessions = true;
    } else if (strcmp(type, "session.error") == 0) {
        json_str(pp, pe, "sessionID", sid, sizeof(sid));
        char name[64] = "", msg[256] = "";
        const char *ep, *ee;
        if (json_obj(pp, pe, "error", &ep, &ee)) {
            json_str(ep, ee, "name", name, sizeof(name));
            json_str(ep, ee, "message", msg, sizeof(msg));
        }
        if (!sid[0] || is_active(sid)) {
            char line[360];
            snprintf(line, sizeof(line), "Error: %s%s%s", name[0] ? name : "session error", msg[0] ? " - " : "", msg);
            push_local(OPENDEV_ROLE_ASST, OPENDEV_KIND_TEXT, line);
        }
        set_busy(sid[0] ? sid : NULL, false);
    } else if (strcmp(type, "permission.updated") == 0 || strcmp(type, "permission.asked") == 0) {
        char pid[OPENDEV_ID_MAX], title[256] = "", perm[64] = "", pattern[160] = "";
        json_str(pp, pe, "id", pid, sizeof(pid));
        json_str(pp, pe, "sessionID", sid, sizeof(sid));
        json_str(pp, pe, "title", title, sizeof(title));
        if (json_str(pp, pe, "permission", perm, sizeof(perm)) != 0) json_str(pp, pe, "type", perm, sizeof(perm));
        const char *ap, *ae;
        if (json_obj(pp, pe, "patterns", &ap, &ae) || json_obj(pp, pe, "pattern", &ap, &ae)) {
            const char *q = ap + 1;                 /* first string of the array */
            while (q < ae && *q != '"' && *q != ']') q++;
            if (q < ae && *q == '"') devos_json_parse_str(q, ae, pattern, sizeof(pattern));
        } else {
            json_str(pp, pe, "pattern", pattern, sizeof(pattern));
        }
        if (pid[0]) {
            s_perm.active = true;
            snprintf(s_perm.id, sizeof(s_perm.id), "%s", pid);
            snprintf(s_perm.session_id, sizeof(s_perm.session_id), "%s", sid);
            if (title[0]) snprintf(s_perm.text, sizeof(s_perm.text), "%s", title);
            else snprintf(s_perm.text, sizeof(s_perm.text), "%s%s%s", perm[0] ? perm : "tool", pattern[0] ? ": " : "",
                          pattern);
            bump();
        }
    } else if (strcmp(type, "permission.replied") == 0) {
        char pid[OPENDEV_ID_MAX] = "";
        if (json_str(pp, pe, "permissionID", pid, sizeof(pid)) != 0) json_str(pp, pe, "requestID", pid, sizeof(pid));
        if (s_perm.active && (!pid[0] || strcmp(pid, s_perm.id) == 0)) {
            s_perm.active = false;
            bump();
        }
    } else if (strcmp(type, "session.diff") == 0) {
        json_str(pp, pe, "sessionID", sid, sizeof(sid));
        const char *ap, *ae;
        if (is_active(sid) && s_diff_requested && json_obj(pp, pe, "diff", &ap, &ae)) {
            diff_build(ap, (size_t)(ae - ap));
        }
    }
}

/* ============================================================ SSE link
 * One thread keeps GET /event open (plain or TLS, blocking reads with a
 * timeout) and hands every complete event to the UI thread as a JOB_EVENT
 * result. OpenCode sends one `data:` line (JSON with "type") per event,
 * usually over a chunked response. */
static struct {
    char *buf;
    size_t len;
    bool skip;                              /* dropping an oversized event */
    bool chunked;
    char chunk_hdr[16];
    size_t chunk_hdr_len;
    long chunk_left;
} S;

static void post_link(opendev_status_t st, const char *text)
{
    job_t *j = calloc(1, sizeof(job_t));
    if (!j) return;
    j->kind = JOB_LINK;
    j->status = (int)st;
    snprintf(j->err, sizeof(j->err), "%s", text ? text : "");
    post_result(j);
}

static void post_event(const char *data, size_t len)
{
    job_t *j = calloc(1, sizeof(job_t));
    if (!j) return;
    j->kind = JOB_EVENT;
    if (data) {
        j->resp = BIG_ALLOC(len + 1);
        if (!j->resp) { free(j); return; }
        memcpy(j->resp, data, len);
        j->resp[len] = '\0';
        j->resp_len = len;
    }
    post_result(j);
}

static void sse_dispatch(char *ev, size_t len)
{
    char *data = NULL;
    size_t dlen = 0;
    int data_lines = 0;
    for (char *ln = ev; ln < ev + len;) {
        char *eol = memchr(ln, '\n', (size_t)(ev + len - ln));
        if (!eol) eol = ev + len;
        size_t llen = (size_t)(eol - ln);
        if (llen && ln[llen - 1] == '\r') llen--;
        if (llen >= 5 && memcmp(ln, "data:", 5) == 0) {
            char *v = ln + 5;
            if (*v == ' ') v++;
            size_t vl = (size_t)(ln + llen - v);
            if (data_lines++ == 0) {
                data = v;
                dlen = vl;
            } else {
                data[dlen++] = '\n';            /* SSE multi-line data: join */
                memmove(data + dlen, v, vl);
                dlen += vl;
            }
        }
        ln = eol + 1;
    }
    if (data && dlen) post_event(data, dlen);
}

static void sse_feed(const char *buf, size_t len)
{
    for (size_t i = 0; i < len;) {
        size_t room = SSE_BUF_MAX - 1 - S.len;
        size_t take = len - i < room ? len - i : room;
        memcpy(S.buf + S.len, buf + i, take);
        S.len += take;
        S.buf[S.len] = '\0';
        i += take;
        for (;;) {
            char *term = strstr(S.buf, "\n\n");
            char *crlf = strstr(S.buf, "\r\n\r\n");
            size_t tl = 2;
            if (crlf && (!term || crlf < term)) { term = crlf; tl = 4; }
            if (!term) break;
            size_t evlen = (size_t)(term - S.buf);
            if (!S.skip) sse_dispatch(S.buf, evlen);
            S.skip = false;
            size_t used = evlen + tl;
            memmove(S.buf, S.buf + used, S.len - used + 1);
            S.len -= used;
        }
        if (S.len >= SSE_BUF_MAX - 1) {
            /* An event bigger than the buffer: drop it; the UI refetches. */
            S.len = 0;
            S.buf[0] = '\0';
            S.skip = true;
            post_event(NULL, 0);
        }
    }
}

static void sse_body(const char *buf, size_t len)
{
    if (!S.chunked) { sse_feed(buf, len); return; }
    size_t i = 0;
    while (i < len) {
        if (S.chunk_left > 0) {
            size_t take = (size_t)S.chunk_left < len - i ? (size_t)S.chunk_left : len - i;
            sse_feed(buf + i, take);
            S.chunk_left -= (long)take;
            i += take;
            if (S.chunk_left == 0) S.chunk_left = -2;          /* expect CRLF */
            continue;
        }
        if (S.chunk_left < 0) {                                /* skip CRLF after data */
            if (buf[i] == '\n') S.chunk_left = 0;
            i++;
            continue;
        }
        char c = buf[i++];
        if (c == '\n') {
            S.chunk_hdr[S.chunk_hdr_len] = '\0';
            S.chunk_left = strtol(S.chunk_hdr, NULL, 16);
            S.chunk_hdr_len = 0;
            if (S.chunk_left == 0) S.chunk_left = -2;
        } else if (c != '\r' && S.chunk_hdr_len < sizeof(S.chunk_hdr) - 1) {
            S.chunk_hdr[S.chunk_hdr_len++] = c;
        }
    }
}

/* Sleep up to ms (forever if < 0), returning early when a relink is asked. */
static void link_wait(uint32_t gen, int ms)
{
    for (int t = 0; (ms < 0 || t < ms) && gen == s_link_gen; t += 100) sleep_ms(100);
}

static void sse_run(void)
{
    static EXT_RAM_BSS_ATTR char rb[4096];
    char err[128], hdr[2048];
    for (;;) {
        if (!s_link_want) { sleep_ms(200); continue; }
        uint32_t gen = s_link_gen;
        opendev_config_t cfg;
        cfg_lock();
        cfg = s_cfg;
        cfg_unlock();

        char t[160];
        snprintf(t, sizeof(t), "Connecting to %.100s...", cfg.host);
        post_link(OPENDEV_CONNECTING, t);
        devos_conn_t *c = devos_conn_open(cfg.host, cfg.port, cfg.tls, REST_TIMEOUT_MS, err, sizeof(err));
        if (!c) {
            post_link(OPENDEV_DOWN, err);
            link_wait(gen, 3000);
            continue;
        }
        char auth[200], hosth[96], req[700];
        auth_header(&cfg, auth, sizeof(auth));
        host_header(&cfg, hosth, sizeof(hosth));
        int hlen = snprintf(req, sizeof(req),
                            "GET %s%s/event HTTP/1.1\r\nHost: %s\r\nAccept: text/event-stream\r\n"
                            "Cache-Control: no-cache\r\nUser-Agent: devOS-Tab5\r\n%sConnection: keep-alive\r\n\r\n",
                            cfg.base, cfg.mode == OPENDEV_MODE_CHAMBER ? "/api" : "", hosth, auth);
        if (hlen <= 0 || devos_conn_write_all(c, req, (size_t)hlen) != 0) {
            devos_conn_close(c);
            post_link(OPENDEV_DOWN, "Could not subscribe to events");
            link_wait(gen, 3000);
            continue;
        }

        /* response headers */
        size_t hl = 0;
        char *eoh = NULL;
        for (int waited = 0; !eoh && waited < 10000 && gen == s_link_gen;) {
            int n = devos_conn_read(c, hdr + hl, sizeof(hdr) - 1 - hl, 500);
            if (n == DEVOS_CONN_TIMEOUT) { waited += 500; continue; }
            if (n <= 0) break;
            hl += (size_t)n;
            hdr[hl] = '\0';
            eoh = strstr(hdr, "\r\n\r\n");
            if (!eoh && hl >= sizeof(hdr) - 1) break;
        }
        if (!eoh) {
            devos_conn_close(c);
            if (gen == s_link_gen) post_link(OPENDEV_DOWN, "No response from the server");
            link_wait(gen, 3000);
            continue;
        }
        int code = 0;
        sscanf(hdr, "HTTP/%*d.%*d %d", &code);
        if (code == 401 || code == 403) {
            devos_conn_close(c);
            post_link(OPENDEV_LOGIN, cfg.mode == OPENDEV_MODE_CHAMBER
                                         ? (cfg.token[0] ? "Sign-in expired: enter the OpenChamber password"
                                                         : "Enter the OpenChamber password")
                                         : "Server needs a password");
            link_wait(gen, -1);                             /* until the user signs in */
            continue;
        }
        if (code != 200) {
            devos_conn_close(c);
            snprintf(t, sizeof(t), "Event stream: HTTP %d", code);
            post_link(OPENDEV_DOWN, t);
            link_wait(gen, 5000);
            continue;
        }
        *eoh = '\0';
        if (!ci_contains(hdr, "text/event-stream")) {
            devos_conn_close(c);
            post_link(OPENDEV_DOWN, "Not an OpenCode event stream (check the URL)");
            link_wait(gen, -1);
            continue;
        }
        S.chunked = ci_contains(hdr, "transfer-encoding: chunked");
        S.chunk_left = 0;
        S.chunk_hdr_len = 0;
        S.len = 0;
        S.skip = false;
        S.buf[0] = '\0';
        post_link(OPENDEV_UP, cfg.tls ? "Live (TLS)" : "Live");
        size_t extra = hl - (size_t)(eoh + 4 - hdr);
        if (extra) sse_body(eoh + 4, extra);

        while (gen == s_link_gen && s_link_want) {
            int n = devos_conn_read(c, rb, sizeof(rb), 1000);
            if (n > 0) { sse_body(rb, (size_t)n); continue; }
            if (n == DEVOS_CONN_TIMEOUT) continue;
            post_link(OPENDEV_DOWN, n == 0 ? "Server closed the event stream" : "Event stream lost");
            break;
        }
        devos_conn_close(c);
        if (gen == s_link_gen && s_link_want) link_wait(gen, 2000);
    }
}

#ifdef ESP_PLATFORM
static void sse_task(void *arg)
{
    (void)arg;
    sse_run();
}
#else
static void *sse_thread(void *arg)
{
    (void)arg;
    sse_run();
    return NULL;
}
#endif

static void workers_start(void)
{
    if (!S.buf) S.buf = BIG_ALLOC(SSE_BUF_MAX);
#ifdef ESP_PLATFORM
    s_cfg_mx = xSemaphoreCreateMutex();
    s_jobq = xQueueCreate(QDEPTH, sizeof(job_t *));
    s_resq = xQueueCreate(QDEPTH, sizeof(job_t *));
    /* Internal-RAM stacks: TLS handshakes use the crypto accelerators. */
    xTaskCreatePinnedToCore(worker_task, "opendev", 10240, NULL, 4, NULL, DEVOS_CORE_NET_CRYPTO);
    xTaskCreatePinnedToCore(sse_task, "opendev_sse", 10240, NULL, 4, NULL, DEVOS_CORE_NET_CRYPTO);
#else
    pthread_t th;
    pthread_create(&th, NULL, worker_thread, NULL);
    pthread_detach(th);
    pthread_create(&th, NULL, sse_thread, NULL);
    pthread_detach(th);
#endif
}

/* ============================================================ results */
#define SESS_SCAN (OPENDEV_MAX_SESSIONS * 8)
typedef struct { opendev_session_t s; long long updated; } sess_tmp_t;
typedef struct { sess_tmp_t *items; int n; } sess_list_t;

static void sessions_each_cb(const char *obj, size_t len, void *ud)
{
    sess_list_t *l = ud;
    const char *e = obj + len;
    if (l->n >= SESS_SCAN || devos_json_find_key(obj, e, "parentID")) return;   /* skip sub-agent sessions */
    sess_tmp_t *t = &l->items[l->n];
    memset(t, 0, sizeof(*t));
    if (json_str(obj, e, "id", t->s.id, sizeof(t->s.id)) != 0) return;
    json_str(obj, e, "title", t->s.title, sizeof(t->s.title));
    const char *tp, *te;
    if (json_obj(obj, e, "time", &tp, &te)) t->updated = json_i64(tp, te, "updated");
    l->n++;
}

static int sess_cmp(const void *a, const void *b)
{
    long long x = ((const sess_tmp_t *)a)->updated, y = ((const sess_tmp_t *)b)->updated;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void handle_sessions(job_t *j)
{
    if (j->rc == 0 && j->status == 400 && strchr(j->path, '?')) {
        /* older server that does not know roots/limit */
        enqueue(JOB_SESSIONS, "GET", "/session", NULL, NULL, NULL, 256 * 1024);
        return;
    }
    if (j->rc != 0 || j->status != 200) return;
    static EXT_RAM_BSS_ATTR sess_tmp_t tmp[SESS_SCAN];
    sess_list_t l = { tmp, 0 };
    devos_json_array_each(j->resp, j->resp_len, sessions_each_cb, &l);
    int n = l.n;
    qsort(tmp, (size_t)n, sizeof(tmp[0]), sess_cmp);                 /* newest first */
    /* keep the active session listed even if it is not among the newest */
    if (s_active_id[0] && n > OPENDEV_MAX_SESSIONS) {
        for (int i = OPENDEV_MAX_SESSIONS; i < n; i++) {
            if (strcmp(tmp[i].s.id, s_active_id) == 0) { tmp[OPENDEV_MAX_SESSIONS - 1] = tmp[i]; break; }
        }
    }
    if (n > OPENDEV_MAX_SESSIONS) n = OPENDEV_MAX_SESSIONS;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < s_session_count; k++) {
            if (strcmp(tmp[i].s.id, s_sessions[k].id) == 0) {
                tmp[i].s.busy = s_sessions[k].busy;
                memcpy(tmp[i].s.model, s_sessions[k].model, sizeof(tmp[i].s.model));
            }
        }
        if (!tmp[i].s.title[0]) snprintf(tmp[i].s.title, sizeof(tmp[i].s.title), "%.8s", tmp[i].s.id);
    }
    for (int i = 0; i < n; i++) s_sessions[i] = tmp[i].s;
    s_session_count = n;
    bump();
    if (s_pending_select[0]) {
        for (int i = 0; i < n; i++) {
            if (strcmp(s_sessions[i].id, s_pending_select) == 0) {
                s_pending_select[0] = '\0';
                opendev_client_select(i);
                break;
            }
        }
    }
}

static void message_each_cb(const char *obj, size_t len, void *ud)
{
    (void)ud;
    const char *e = obj + len;
    const char *ip = obj, *ie = e;
    json_obj(obj, e, "info", &ip, &ie);
    char mid[OPENDEV_ID_MAX], role[16], model[OPENDEV_TITLE_MAX];
    json_str(ip, ie, "id", mid, sizeof(mid));
    json_str(ip, ie, "role", role, sizeof(role));
    uint8_t r = strcmp(role, "user") == 0 ? OPENDEV_ROLE_USER : OPENDEV_ROLE_ASST;
    role_remember(mid, r);
    if (r == OPENDEV_ROLE_ASST && json_str(ip, ie, "modelID", model, sizeof(model)) == 0) {
        set_session_model(s_active_id, model);
    }
    const char *pp, *pe;
    if (!json_obj(obj, e, "parts", &pp, &pe) || *pp != '[') return;
    /* walk the parts array */
    const char *q = pp + 1;
    while (q < pe) {
        while (q < pe && *q != '{' && *q != ']') q++;
        if (q >= pe || *q == ']') break;
        const char *qe = devos_json_span(q, pe);
        if (!qe) break;
        static EXT_RAM_BSS_ATTR opendev_block_t nb;
        memset(&nb, 0, sizeof(nb) - sizeof(nb.text));
        if (part_to_block(q, qe, &nb)) {
            nb.role = r;
            drop_oldest_if_full();
            opendev_block_t *b = &s_blocks[s_block_count++];
            memcpy(b, &nb, sizeof(nb) - sizeof(nb.text));
            memcpy(b->text, nb.text, strlen(nb.text) + 1);
            b->rev = ++s_rev_counter;
        }
        q = qe;
    }
}

static void handle_messages(job_t *j)
{
    if (!is_active(j->sid)) return;                 /* user switched meanwhile */
    if (j->rc != 0 || j->status != 200) {
        char line[96];
        snprintf(line, sizeof(line), "(could not load messages: %s)", j->rc ? "server unreachable" : "HTTP error");
        s_block_count = 0;
        push_local(OPENDEV_ROLE_ASST, OPENDEV_KIND_TEXT, line);
        return;
    }
    s_block_count = 0;
    devos_json_array_each(j->resp, j->resp_len, message_each_cb, NULL);
    bump_blocks();
}

static void send_prompt(const char *sid, const char *text);
static void link_result(job_t *j);
static void probe_result(job_t *j);
static void login_result(job_t *j);

static void handle_result(job_t *j)
{
    if (j->rc == 0 && j->status == 401 && j->kind != JOB_PROBE && j->kind != JOB_LOGIN && j->kind != JOB_LINK &&
        j->kind != JOB_EVENT) {
        s_link_want = false;
        set_status(OPENDEV_LOGIN, s_cfg.mode == OPENDEV_MODE_CHAMBER ? "Sign-in expired: enter the OpenChamber password"
                                                                      : "Server needs a password");
        return;
    }
    switch (j->kind) {
    case JOB_EVENT:
        if (j->resp) on_sse_event(j->resp, j->resp_len);
        else if (s_active_id[0]) s_want_messages = true;
        break;
    case JOB_LINK:
        link_result(j);
        break;
    case JOB_PROBE:
        probe_result(j);
        break;
    case JOB_LOGIN:
        login_result(j);
        break;
    case JOB_SESSIONS:
        handle_sessions(j);
        break;
    case JOB_MESSAGES:
        handle_messages(j);
        break;
    case JOB_NEW_SESSION: {
        char id[OPENDEV_ID_MAX] = "";
        if (j->rc == 0 && (j->status == 200 || j->status == 201) &&
            json_str(j->resp, j->resp + j->resp_len, "id", id, sizeof(id)) == 0) {
            snprintf(s_active_id, sizeof(s_active_id), "%s", id);
            snprintf(s_pending_select, sizeof(s_pending_select), "%s", id);
            s_block_count = 0;
            bump_blocks();
            if (s_pending_send) {
                char *t = s_pending_send;
                s_pending_send = NULL;
                push_local(OPENDEV_ROLE_USER, OPENDEV_KIND_TEXT, t);
                send_prompt(id, t);
                free(t);
            }
            enqueue(JOB_SESSIONS, "GET", SESSIONS_PATH, NULL, NULL, NULL, 256 * 1024);
        } else {
            free(s_pending_send);
            s_pending_send = NULL;
            push_local(OPENDEV_ROLE_ASST, OPENDEV_KIND_TEXT, "(could not create a session - server unreachable?)");
        }
        break;
    }
    case JOB_SEND:
        if (j->rc != 0 || (j->status != 200 && j->status != 204)) {
            set_busy(j->sid, false);
            push_local(OPENDEV_ROLE_ASST, OPENDEV_KIND_TEXT,
                       j->status == 404 ? "(send failed: this opencode server has no prompt_async - update opencode)"
                                        : "(send failed - server unreachable)");
        } else {
            set_busy(j->sid, true);
        }
        break;
    case JOB_ABORT:
        set_busy(j->sid, false);
        break;
    case JOB_PERM:
        if (j->rc == 0 && j->status == 404) {
            /* older servers: POST /session/:sid/permissions/:id {"response": ...} */
            char path[192];
            snprintf(path, sizeof(path), "/session/%s/permissions/%s", j->sid[0] ? j->sid : "unknown", j->aux);
            const char *resp = strstr(j->body ? j->body : "", "reject") ? "reject"
                               : strstr(j->body ? j->body : "", "always") ? "always" : "once";
            char body[48];
            snprintf(body, sizeof(body), "{\"response\":\"%s\"}", resp);
            enqueue(JOB_PERM_V2, "POST", path, body, j->sid, j->aux, 4096);
        }
        break;
    case JOB_PERM_V2:
        break;
    case JOB_DIFF:
        if (!is_active(j->sid)) break;
        if (j->rc != 0 || j->status != 200) {
            snprintf(s_diff, sizeof(s_diff), "(diff unavailable%s)", j->rc ? ": server unreachable" : "");
            s_diff_file_count = 0;
            s_diff_gen++;
            bump();
        } else {
            diff_build(j->resp, j->resp_len);
        }
        break;
    }
}

/* ============================================================ public API */
static void clear_store(void)
{
    s_session_count = 0;
    s_active_id[0] = '\0';
    s_block_count = 0;
    s_diff[0] = '\0';
    s_diff_file_count = 0;
    s_perm.active = false;
    bump_blocks();
    s_diff_gen++;
}

/* (Re)start the event stream with the current config. */
static void relink(void)
{
    s_link_want = true;
    s_link_gen++;
    bump();
}

static void start_probe(void)
{
    s_link_want = false;
    s_link_gen++;                               /* drop any current stream */
    char t[160];
    snprintf(t, sizeof(t), "Checking %.120s ...", s_cfg.url);
    set_status(OPENDEV_CONNECTING, t);
    enqueue(JOB_PROBE, "GET", "/auth/session", NULL, NULL, NULL, 8192);
}

void opendev_client_init(void)
{
    config_load();
    workers_start();
    start_probe();          /* OpenChamber or opencode serve? password needed? */
}

int opendev_client_reconnect(void)
{
    start_probe();
    return 0;
}

/* Device identity for OpenChamber's trusted-device list. */
static void device_key(char *out, size_t n)
{
#ifdef ESP_PLATFORM
    uint8_t mac[6] = { 0 };
    esp_efuse_mac_get_default(mac);
    snprintf(out, n, "devos-tab5-%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);
#else
    snprintf(out, n, "devos-tab5-simulator");
#endif
}

static int start_login(const char *password)
{
    if (!password || !*password) return -1;
    size_t cap = strlen(password) * 6 + 512;
    char *esc = malloc(strlen(password) * 6 + 8), *body = malloc(cap);
    if (!esc || !body) { free(esc); free(body); return -1; }
    devos_json_escape(password, esc, strlen(password) * 6 + 8);
    char key[48];
    device_key(key, sizeof(key));
    snprintf(body, cap,
             "{\"password\":\"%s\",\"trustDevice\":true,\"issueClientToken\":true,\"clientLabel\":\"devOS Tab5\","
             "\"clientKind\":\"devos-tab5\",\"deviceName\":\"Tab5\",\"devicePlatform\":\"devOS\","
             "\"deviceModel\":\"M5Stack Tab5 (ESP32-P4)\",\"appVersion\":\"%s\",\"dedupeKey\":\"%s\"}",
             esc, DEVOS_VERSION_STR, key);
    memset(esc, 0, strlen(esc));
    free(esc);
    set_status(OPENDEV_CONNECTING, "Signing in...");
    int rc = enqueue(JOB_LOGIN, "POST", "/auth/session", body, NULL, NULL, 8192);
    memset(body, 0, strlen(body));
    free(body);
    return rc;
}

static void link_result(job_t *j)
{
    opendev_status_t st = (opendev_status_t)j->status;
    if (st == OPENDEV_LOGIN) s_link_want = false;
    set_status(st, j->err);
    if (st == OPENDEV_UP) {
        opendev_client_refresh_sessions();
        if (s_active_id[0]) s_want_messages = true;    /* catch up on what we missed */
    }
}

static void probe_result(job_t *j)
{
    if (j->rc != 0) {
        char t[160];
        snprintf(t, sizeof(t), "%s", j->err[0] ? j->err : "Server unreachable");
        set_status(OPENDEV_DOWN, t);
        free(s_pending_password);
        s_pending_password = NULL;
        return;
    }
    bool chamber = (j->status == 200 || j->status == 401) && strstr(j->resp, "\"authenticated\"");
    if (!chamber) {
        /* plain `opencode serve` */
        s_cfg.mode = OPENDEV_MODE_CODE;
        config_save();
        free(s_pending_password);
        s_pending_password = NULL;
        relink();
        return;
    }
    s_cfg.mode = OPENDEV_MODE_CHAMBER;
    config_save();
    if (strstr(j->resp, "\"authenticated\":true") && s_cfg.token[0]) {
        relink();
    } else if (s_pending_password) {
        start_login(s_pending_password);
        memset(s_pending_password, 0, strlen(s_pending_password));
        free(s_pending_password);
        s_pending_password = NULL;
    } else if (strstr(j->resp, "\"authenticated\":true")) {
        relink();                               /* OpenChamber without a password */
    } else {
        set_status(OPENDEV_LOGIN, s_cfg.token[0] ? "Sign-in expired: enter the OpenChamber password"
                                                 : "Enter the OpenChamber password");
    }
}

static void login_result(job_t *j)
{
    if (j->rc != 0) {
        set_status(OPENDEV_LOGIN, j->err[0] ? j->err : "Server unreachable");
        return;
    }
    if (j->status == 401) { set_status(OPENDEV_LOGIN, "Wrong password"); return; }
    if (j->status == 429) { set_status(OPENDEV_LOGIN, "Too many attempts: wait a minute and try again"); return; }
    if (j->status == 403) { set_status(OPENDEV_LOGIN, "Password sign-in is disabled on this route"); return; }
    if (j->status != 200) {
        char t[64];
        snprintf(t, sizeof(t), "Sign-in failed (HTTP %d)", j->status);
        set_status(OPENDEV_LOGIN, t);
        return;
    }
    char token[OPENDEV_TOKEN_MAX] = "";
    if (json_str(j->resp, j->resp + j->resp_len, "clientToken", token, sizeof(token)) != 0 || !token[0]) {
        /* older OpenChamber: fall back to the session cookie */
        const char *h = j->hdrs ? j->hdrs : "";
        for (const char *p = h; (p = strstr(p, "\n")) != NULL; p++) {
            if (strncasecmp(p + 1, "set-cookie:", 11) == 0) {
                const char *v = p + 12;
                while (*v == ' ') v++;
                if (strncmp(v, "oc_ui_session", 13) == 0) {
                    size_t n = strcspn(v, ";\r\n");
                    snprintf(token, sizeof(token), "cookie:%.*s", (int)(n < 110 ? n : 110), v);
                    break;
                }
            }
        }
    }
    if (!token[0]) {
        set_status(OPENDEV_LOGIN, "Signed in, but the server issued no device token");
        return;
    }
    cfg_lock();
    snprintf(s_cfg.token, sizeof(s_cfg.token), "%s", token);
    cfg_unlock();
    config_save();
    set_status(OPENDEV_CONNECTING, "Signed in");
    relink();
}

void opendev_client_poll(void)
{
    /* finished REST jobs and events from the stream */
    for (int n = 0; n < QDEPTH; n++) {
        job_t *j = result_take();
        if (!j) break;
        if (j->kind != JOB_EVENT && j->kind != JOB_LINK) {
            if (s_inflight > 0) s_inflight--;
            if (j->kind == JOB_MESSAGES && s_msg_inflight > 0) s_msg_inflight--;
        }
        handle_result(j);
        job_free(j);
        bump();
    }

    /* debounced refetches requested by events */
    if ((s_want_sessions || s_want_messages) && s_status == OPENDEV_UP) {
        if (++s_refetch_ticks >= REFETCH_DEBOUNCE_TICKS) {
            s_refetch_ticks = 0;
            if (s_want_sessions) {
                s_want_sessions = false;
                opendev_client_refresh_sessions();
            }
            if (s_want_messages && s_active_id[0]) {
                s_want_messages = false;
                char path[160];
                snprintf(path, sizeof(path), "/session/%s/message?limit=%d", s_active_id, MSG_LIMIT);
                enqueue(JOB_MESSAGES, "GET", path, NULL, s_active_id, NULL, 768 * 1024);
            }
        }
    }
}

opendev_status_t opendev_client_status(void) { return s_status; }
const char *opendev_client_status_text(void) { return s_status_text; }
uint32_t opendev_client_generation(void) { return s_gen; }
uint32_t opendev_client_blocks_generation(void) { return s_blocks_gen; }
uint32_t opendev_client_diff_generation(void) { return s_diff_gen; }
bool opendev_client_loading(void) { return s_inflight > 0; }
bool opendev_client_messages_loading(void) { return s_msg_inflight > 0; }
bool opendev_client_needs_login(void) { return s_status == OPENDEV_LOGIN; }

void opendev_client_get_config(char *host, size_t host_len, int *port, opendev_mode_t *mode, char *token,
                               size_t token_len)
{
    if (host && host_len) snprintf(host, host_len, "%s", s_cfg.host);
    if (port) *port = s_cfg.port;
    if (mode) *mode = s_cfg.mode;
    if (token && token_len) snprintf(token, token_len, "%s", s_cfg.token);
}

void opendev_client_get_url(char *out, size_t len)
{
    if (out && len) snprintf(out, len, "%s", s_cfg.url);
}

static int connect_url_ex(const char *url, const char *password, const char *token)
{
    opendev_config_t c = s_cfg;
    if (!url || cfg_apply_url(&c, url) != 0) return -1;
    bool same = strcmp(c.url, s_cfg.url) == 0;
    cfg_lock();
    s_cfg = c;
    if (!same) s_cfg.token[0] = '\0';           /* tokens belong to one server */
    if (token && *token) {
        snprintf(s_cfg.token, sizeof(s_cfg.token), "%s", token);
        s_cfg.mode = OPENDEV_MODE_CHAMBER;
    }
    cfg_unlock();
    config_save();
    if (!same) clear_store();
    free(s_pending_password);
    s_pending_password = (password && *password) ? strdup(password) : NULL;
    start_probe();
    return 0;
}

int opendev_client_connect_url(const char *url, const char *password)
{
    return connect_url_ex(url, password, NULL);
}

int opendev_client_login(const char *password)
{
    return start_login(password);
}

int opendev_client_sign_out(void)
{
    cfg_lock();
    s_cfg.token[0] = '\0';
    cfg_unlock();
    config_save();
    clear_store();
    start_probe();
    return 0;
}

int opendev_client_set_server(const char *host, int port)
{
    if (!host || !*host || port <= 0 || port > 65535) return -1;
    char url[OPENDEV_URL_MAX];
    if (strstr(host, "://")) snprintf(url, sizeof(url), "%s", host);
    else snprintf(url, sizeof(url), "http://%s:%d", host, port);
    return opendev_client_connect_url(url, NULL);
}

int opendev_client_set_chamber(const char *host, int port, const char *token)
{
    if (!host || !*host || port <= 0 || port > 65535) return -1;
    char url[OPENDEV_URL_MAX];
    if (strstr(host, "://")) snprintf(url, sizeof(url), "%s", host);
    else snprintf(url, sizeof(url), "http://%s:%d", host, port);
    return connect_url_ex(url, NULL, token);
}

int opendev_client_set_mode(opendev_mode_t mode)
{
    if (mode != OPENDEV_MODE_CODE && mode != OPENDEV_MODE_CHAMBER) return -1;
    cfg_lock();
    s_cfg.mode = mode;
    cfg_unlock();
    config_save();
    relink();
    return 0;
}

static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else if (*r == '+') {
            *w++ = ' ';
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

int opendev_client_pair(const char *uri)
{
    /* openchamber://connect?url=U&token=T (or host=H&port=P; p= accepted for token) */
    if (!uri || strncmp(uri, "openchamber://", 14) != 0) return -1;
    const char *q = strchr(uri, '?');
    char url[OPENDEV_URL_MAX] = "", host[OPENDEV_HOST_MAX] = "", token[OPENDEV_TOKEN_MAX] = "";
    int port = 0;
    if (q) {
        char query[400];
        snprintf(query, sizeof(query), "%s", q + 1);
        char *save = NULL;
        for (char *pair = strtok_r(query, "&", &save); pair; pair = strtok_r(NULL, "&", &save)) {
            char *eq = strchr(pair, '=');
            if (!eq) continue;
            *eq = '\0';
            url_decode(pair);
            url_decode(eq + 1);
            if (strcmp(pair, "url") == 0) snprintf(url, sizeof(url), "%s", eq + 1);
            else if (strcmp(pair, "host") == 0) snprintf(host, sizeof(host), "%s", eq + 1);
            else if (strcmp(pair, "port") == 0) port = atoi(eq + 1);
            else if (strcmp(pair, "token") == 0 || strcmp(pair, "p") == 0) snprintf(token, sizeof(token), "%s", eq + 1);
        }
    }
    if (!url[0] && host[0]) snprintf(url, sizeof(url), "http://%s:%d", host, port > 0 ? port : 80);
    if (!url[0] || !token[0]) return -1;
    return connect_url_ex(url, NULL, token);
}

int opendev_client_session_count(void) { return s_session_count; }

const opendev_session_t *opendev_client_session(int idx)
{
    return (idx >= 0 && idx < s_session_count) ? &s_sessions[idx] : NULL;
}

int opendev_client_active(void) { return active_index(); }

int opendev_client_select(int idx)
{
    if (idx < 0 || idx >= s_session_count) return -1;
    snprintf(s_active_id, sizeof(s_active_id), "%s", s_sessions[idx].id);
    s_block_count = 0;
    s_diff[0] = '\0';
    s_diff_file_count = 0;
    s_diff_requested = false;
    s_diff_gen++;
    bump_blocks();
    char path[160];
    snprintf(path, sizeof(path), "/session/%s/message?limit=%d", s_active_id, MSG_LIMIT);
    return enqueue(JOB_MESSAGES, "GET", path, NULL, s_active_id, NULL, 768 * 1024);
}

int opendev_client_refresh_sessions(void)
{
    return enqueue(JOB_SESSIONS, "GET", SESSIONS_PATH, NULL, NULL, NULL, 256 * 1024);
}

int opendev_client_new_session(void)
{
    char body[96];
    snprintf(body, sizeof(body), "{\"title\":\"Tab5 %s\"}", s_cfg.mode == OPENDEV_MODE_CHAMBER ? "chamber" : "session");
    return enqueue(JOB_NEW_SESSION, "POST", "/session", body, NULL, NULL, 16384);
}

static void send_prompt(const char *sid, const char *text)
{
    size_t cap = strlen(text) * 6 + 64;
    char *esc = malloc(cap);
    char *body = malloc(cap + 64);
    if (!esc || !body) { free(esc); free(body); return; }
    devos_json_escape(text, esc, cap);
    snprintf(body, cap + 64, "{\"parts\":[{\"type\":\"text\",\"text\":\"%s\"}]}", esc);
    char path[160];
    snprintf(path, sizeof(path), "/session/%s/prompt_async", sid);
    if (enqueue(JOB_SEND, "POST", path, body, sid, NULL, 4096) == 0) set_busy(sid, true);
    free(esc);
    free(body);
}

int opendev_client_send(const char *text)
{
    if (!text || !*text) return -1;
    if (active_index() < 0 && !s_active_id[0]) {
        /* no session yet: create one, then send (see JOB_NEW_SESSION) */
        free(s_pending_send);
        s_pending_send = strdup(text);
        return opendev_client_new_session();
    }
    push_local(OPENDEV_ROLE_USER, OPENDEV_KIND_TEXT, text);
    send_prompt(s_active_id, text);
    return 0;
}

int opendev_client_abort(void)
{
    if (!s_active_id[0]) return -1;
    char path[160];
    snprintf(path, sizeof(path), "/session/%s/abort", s_active_id);
    return enqueue(JOB_ABORT, "POST", path, "{}", s_active_id, NULL, 4096);
}

int opendev_client_block_count(void) { return s_block_count; }

const opendev_block_t *opendev_client_block(int idx)
{
    return (idx >= 0 && idx < s_block_count) ? &s_blocks[idx] : NULL;
}

bool opendev_client_permission_pending(opendev_permission_t *out)
{
    if (!s_perm.active) return false;
    if (out) *out = s_perm;
    return true;
}

int opendev_client_answer_permission(bool allow, bool always)
{
    if (!s_perm.active) return -1;
    const char *r = !allow ? "reject" : (always ? "always" : "once");
    char path[160], body[48];
    snprintf(path, sizeof(path), "/permission/%s/reply", s_perm.id);
    snprintf(body, sizeof(body), "{\"reply\":\"%s\"}", r);
    int rc = enqueue(JOB_PERM, "POST", path, body, s_perm.session_id, s_perm.id, 4096);
    s_perm.active = false;
    bump();
    return rc;
}

int opendev_client_fetch_diff(void)
{
    if (!s_active_id[0]) return -1;
    s_diff_requested = true;
    snprintf(s_diff, sizeof(s_diff), "Loading diff...");
    s_diff_file_count = 0;
    s_diff_gen++;
    bump();
    char path[160];
    snprintf(path, sizeof(path), "/session/%s/diff", s_active_id);
    return enqueue(JOB_DIFF, "GET", path, NULL, s_active_id, NULL, 1536 * 1024);
}

const char *opendev_client_diff_text(void) { return s_diff; }
int opendev_client_diff_file_count(void) { return s_diff_file_count; }

const opendev_diff_file_t *opendev_client_diff_file(int idx)
{
    return (idx >= 0 && idx < s_diff_file_count) ? &s_diff_files[idx] : NULL;
}
