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
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
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
#define CONNECT_GIVEUP_TICKS 100            /* 10 s at 100 ms poll */
#define RETRY_TICKS       30                /* 3 s between link attempts */
#define REFETCH_DEBOUNCE_TICKS 5            /* 500 ms coalescing */
#define MSG_LIMIT         40
#define ROLE_MAP_MAX      64
#define SESSIONS_PATH     "/session?roots=true&limit=40"

/* ------------------------------------------------------------------ config */
typedef struct {
    char host[OPENDEV_HOST_MAX];
    int port;
    opendev_mode_t mode;
    char token[OPENDEV_TOKEN_MAX];
} opendev_config_t;

static opendev_config_t s_cfg = {
    .host = "10.2.132.54",
    .port = 4096,
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
static char s_status_text[128] = "Offline";
static uint32_t s_gen = 0, s_blocks_gen = 0, s_diff_gen = 0;
static EXT_RAM_BSS_ATTR char s_diff[OPENDEV_DIFF_MAX] = "";
static EXT_RAM_BSS_ATTR opendev_diff_file_t s_diff_files[OPENDEV_DIFF_FILES];
static int s_diff_file_count = 0;
static bool s_diff_requested = false;       /* user asked for the diff of this session */
static char s_pending_select[OPENDEV_ID_MAX] = "";
static char *s_pending_send = NULL;         /* prompt waiting for a new session */
static int s_inflight = 0;
static int s_msg_inflight = 0;               /* message loads in flight */

static EXT_RAM_BSS_ATTR struct { char msg[OPENDEV_ID_MAX]; uint8_t role; } s_roles[ROLE_MAP_MAX];
static int s_role_next = 0;

/* SSE link */
static int s_sse_fd = -1;
static int s_connect_ticks = 0;
static int s_retry_ticks = 0;
static char *s_sse_buf = NULL;
static size_t s_sse_len = 0;
static bool s_sse_skip = false;              /* dropping an oversized event */
static char s_hdr_buf[1024];
static size_t s_hdr_len = 0;
static bool s_want_link = false;
static bool s_chunked = false;               /* SSE body is chunk-encoded */
static char s_chunk_hdr[16];
static size_t s_chunk_hdr_len = 0;
static long s_chunk_left = 0;

/* Deferred refetch flags (set by SSE, executed in poll) */
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

/* ------------------------------------------------------------ persistence */
static void config_save(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("opendev", NVS_READWRITE, &h) == ESP_OK) {
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
        fprintf(f, "{\n  \"host\": \"%s\",\n  \"port\": %d,\n  \"mode\": %d,\n  \"token\": \"%s\"\n}\n",
                s_cfg.host, s_cfg.port, (int)s_cfg.mode, s_cfg.token);
        fclose(f);
    }
#endif
}

static void config_load(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("opendev", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_cfg.host);
        nvs_get_str(h, "host", s_cfg.host, &len);
        uint16_t p16 = 0;
        if (nvs_get_u16(h, "port16", &p16) == ESP_OK && p16) s_cfg.port = p16;
        uint8_t m = 0;
        if (nvs_get_u8(h, "mode", &m) == ESP_OK && m <= OPENDEV_MODE_CHAMBER) s_cfg.mode = (opendev_mode_t)m;
        len = sizeof(s_cfg.token);
        nvs_get_str(h, "token", s_cfg.token, &len);
        nvs_close(h);
    }
#else
    FILE *f = fopen(OPENDEV_NVS_FILE, "r");
    if (f) {
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) {
            char val[128];
            int ival = 0;
            if (sscanf(buf, " \"host\": \"%127[^\"]\"", val) == 1) {
                snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", val);
            } else if (sscanf(buf, " \"port\": %d", &ival) == 1 && ival > 0 && ival < 65536) {
                s_cfg.port = ival;
            } else if (sscanf(buf, " \"mode\": %d", &ival) == 1 && (ival == 0 || ival == 1)) {
                s_cfg.mode = (opendev_mode_t)ival;
            } else if (sscanf(buf, " \"token\": \"%127[^\"]\"", val) == 1) {
                snprintf(s_cfg.token, sizeof(s_cfg.token), "%s", val);
            }
        }
        fclose(f);
    }
#endif
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
    JOB_SESSIONS, JOB_MESSAGES, JOB_NEW_SESSION, JOB_SEND, JOB_ABORT, JOB_PERM, JOB_PERM_V2, JOB_DIFF
} job_kind_t;

typedef struct job {
    job_kind_t kind;
    char method[8];
    char path[224];
    char *body;
    char sid[OPENDEV_ID_MAX];
    char aux[OPENDEV_ID_MAX];
    opendev_config_t cfg;
    size_t cap;
    int rc, status;
    char *resp;
    size_t resp_len;
} job_t;

static void job_free(job_t *j)
{
    if (!j) return;
    free(j->body);
    free(j->resp);
    free(j);
}

/* Minimal HTTP/1.1 client (worker thread only): Connection: close, read to
 * EOF, then split headers and de-chunk in place. */
static void http_run(job_t *j)
{
    j->rc = -1;
    j->status = 0;
    j->resp = BIG_ALLOC(j->cap);
    if (!j->resp) return;
    j->resp[0] = '\0';

    int fd = devos_net_socket_connect(j->cfg.host, j->cfg.port, REST_TIMEOUT_MS);
    if (fd < 0) return;
    size_t blen = j->body ? strlen(j->body) : 0;
    char req[640];
    int hlen = snprintf(req, sizeof(req),
                        "%s %s HTTP/1.1\r\nHost: %s:%d\r\nAccept: application/json\r\n"
                        "Content-Type: application/json\r\nContent-Length: %u\r\n%s%s%s"
                        "Connection: close\r\n\r\n",
                        j->method, j->path, j->cfg.host, j->cfg.port, (unsigned)blen,
                        j->cfg.token[0] ? "Authorization: Bearer " : "", j->cfg.token,
                        j->cfg.token[0] ? "\r\n" : "");
    if (hlen <= 0 || (size_t)hlen >= sizeof(req) || devos_net_socket_send_all(fd, req, (size_t)hlen) != 0 ||
        (blen && devos_net_socket_send_all(fd, j->body, blen) != 0)) {
        devos_net_socket_close(fd);
        return;
    }
    size_t total = 0;
    for (;;) {
        size_t room = j->cap - 1 - total;
        if (room == 0) break;
        int n = devos_net_socket_recv(fd, j->resp + total, room > 16384 ? 16384 : room, REST_TIMEOUT_MS);
        if (n <= 0) break;
        total += (size_t)n;
    }
    devos_net_socket_close(fd);
    j->resp[total] = '\0';

    int status = 0;
    if (sscanf(j->resp, "HTTP/%*d.%*d %d", &status) != 1 && sscanf(j->resp, "HTTP/%*d %d", &status) != 1) return;
    j->status = status;
    char *hdr_end = strstr(j->resp, "\r\n\r\n");
    if (!hdr_end) return;
    *hdr_end = '\0';
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

/* ---- job queues ---- */
#ifdef ESP_PLATFORM
static QueueHandle_t s_jobq, s_resq;

static bool job_submit(job_t *j) { return xQueueSend(s_jobq, &j, 0) == pdTRUE; }
static job_t *result_take(void)
{
    job_t *j = NULL;
    return xQueueReceive(s_resq, &j, 0) == pdTRUE ? j : NULL;
}

static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        job_t *j = NULL;
        if (xQueueReceive(s_jobq, &j, portMAX_DELAY) != pdTRUE || !j) continue;
        http_run(j);
        while (xQueueSend(s_resq, &j, pdMS_TO_TICKS(1000)) != pdTRUE) {
        }
    }
}

static void worker_start(void)
{
    s_jobq = xQueueCreate(16, sizeof(job_t *));
    s_resq = xQueueCreate(16, sizeof(job_t *));
    /* Pure network I/O (no flash writes), so the stack can live in PSRAM. */
    if (xTaskCreatePinnedToCoreWithCaps(worker_task, "opendev", 8192, NULL, 4, NULL, DEVOS_CORE_NET_CRYPTO,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        xTaskCreatePinnedToCore(worker_task, "opendev", 8192, NULL, 4, NULL, DEVOS_CORE_NET_CRYPTO);
    }
}
#else
#define QCAP 16
typedef struct { job_t *items[QCAP]; int head, count; } jq_t;
static jq_t s_jq, s_rq;
static pthread_mutex_t s_qmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_qcv = PTHREAD_COND_INITIALIZER;

static bool jq_push(jq_t *q, job_t *j)
{
    if (q->count >= QCAP) return false;
    q->items[(q->head + q->count++) % QCAP] = j;
    return true;
}

static job_t *jq_pop(jq_t *q)
{
    if (!q->count) return NULL;
    job_t *j = q->items[q->head];
    q->head = (q->head + 1) % QCAP;
    q->count--;
    return j;
}

static bool job_submit(job_t *j)
{
    pthread_mutex_lock(&s_qmx);
    bool ok = jq_push(&s_jq, j);
    pthread_cond_broadcast(&s_qcv);
    pthread_mutex_unlock(&s_qmx);
    return ok;
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
        pthread_mutex_lock(&s_qmx);
        while (!jq_push(&s_rq, j)) {
            pthread_mutex_unlock(&s_qmx);
            struct timespec ts = { 0, 50 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            pthread_mutex_lock(&s_qmx);
        }
        pthread_mutex_unlock(&s_qmx);
    }
    return NULL;
}

static void worker_start(void)
{
    pthread_t th;
    pthread_create(&th, NULL, worker_thread, NULL);
    pthread_detach(th);
}
#endif

static int enqueue(job_kind_t kind, const char *method, const char *path, const char *body, const char *sid,
                   const char *aux, size_t cap)
{
    job_t *j = calloc(1, sizeof(job_t));
    if (!j) return -1;
    j->kind = kind;
    snprintf(j->method, sizeof(j->method), "%s", method);
    snprintf(j->path, sizeof(j->path), "%s", path);
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

/* Complete events end with a blank line; OpenCode sends one `data:` line per
 * event (JSON with "type"). Multi-line data is joined per the SSE spec. */
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
                /* rare: join in place (the data only shrinks by the prefix) */
                data[dlen++] = '\n';
                memmove(data + dlen, v, vl);
                dlen += vl;
            }
        }
        ln = eol + 1;
    }
    if (data && dlen) {
        data[dlen] = '\0';
        on_sse_event(data, dlen);
    }
}

static void sse_feed(const char *buf, size_t len)
{
    if (!s_sse_buf || len == 0) return;
    for (size_t i = 0; i < len;) {
        size_t room = SSE_BUF_MAX - 1 - s_sse_len;
        size_t take = len - i < room ? len - i : room;
        memcpy(s_sse_buf + s_sse_len, buf + i, take);
        s_sse_len += take;
        s_sse_buf[s_sse_len] = '\0';
        i += take;
        for (;;) {
            char *term = strstr(s_sse_buf, "\n\n");
            char *crlf = strstr(s_sse_buf, "\r\n\r\n");
            size_t tl = 2;
            if (crlf && (!term || crlf < term)) { term = crlf; tl = 4; }
            if (!term) break;
            size_t evlen = (size_t)(term - s_sse_buf);
            if (!s_sse_skip) sse_dispatch(s_sse_buf, evlen);
            s_sse_skip = false;
            size_t used = evlen + tl;
            memmove(s_sse_buf, s_sse_buf + used, s_sse_len - used + 1);
            s_sse_len -= used;
        }
        if (s_sse_len >= SSE_BUF_MAX - 1) {
            /* An event bigger than the buffer: drop it and refetch instead. */
            s_sse_len = 0;
            s_sse_buf[0] = '\0';
            s_sse_skip = true;
            if (s_active_id[0]) s_want_messages = true;
        }
    }
}

/* De-chunk the SSE body when the server used Transfer-Encoding: chunked. */
static void sse_body(const char *buf, size_t len)
{
    if (!s_chunked) { sse_feed(buf, len); return; }
    size_t i = 0;
    while (i < len) {
        if (s_chunk_left > 0) {
            size_t take = (size_t)s_chunk_left < len - i ? (size_t)s_chunk_left : len - i;
            sse_feed(buf + i, take);
            s_chunk_left -= (long)take;
            i += take;
            if (s_chunk_left == 0) s_chunk_left = -2;          /* expect CRLF */
            continue;
        }
        if (s_chunk_left < 0) {                                /* skip CRLF after data */
            if (buf[i] == '\n') s_chunk_left = 0;
            i++;
            continue;
        }
        char c = buf[i++];
        if (c == '\n') {
            s_chunk_hdr[s_chunk_hdr_len] = '\0';
            s_chunk_left = strtol(s_chunk_hdr, NULL, 16);
            s_chunk_hdr_len = 0;
            if (s_chunk_left == 0) s_chunk_left = -2;
        } else if (c != '\r' && s_chunk_hdr_len < sizeof(s_chunk_hdr) - 1) {
            s_chunk_hdr[s_chunk_hdr_len++] = c;
        }
    }
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

static void handle_result(job_t *j)
{
    switch (j->kind) {
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
void opendev_client_init(void)
{
    config_load();
    if (!s_sse_buf) s_sse_buf = BIG_ALLOC(SSE_BUF_MAX);
    worker_start();
    s_want_link = true;
    set_status(OPENDEV_DOWN, "Offline");
}

int opendev_client_reconnect(void)
{
    if (s_sse_fd >= 0) {
        devos_net_socket_close(s_sse_fd);
        s_sse_fd = -1;
    }
    s_retry_ticks = RETRY_TICKS;
    s_want_link = true;
    set_status(OPENDEV_DOWN, "Reconnecting...");
    return 0;
}

static void link_drop(const char *why)
{
    if (s_sse_fd >= 0) devos_net_socket_close(s_sse_fd);
    s_sse_fd = -1;
    set_status(OPENDEV_DOWN, why);
}

void opendev_client_poll(void)
{
    /* finished REST jobs */
    for (int n = 0; n < 8; n++) {
        job_t *j = result_take();
        if (!j) break;
        if (s_inflight > 0) s_inflight--;
        if (j->kind == JOB_MESSAGES && s_msg_inflight > 0) s_msg_inflight--;
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

    if (!s_want_link) {
        if (s_sse_fd >= 0) link_drop("Offline");
        return;
    }

    if (s_sse_fd < 0) {
        if (++s_retry_ticks < RETRY_TICKS) return;
        s_retry_ticks = 0;
        s_sse_fd = devos_net_socket_connect_start(s_cfg.host, s_cfg.port);
        s_connect_ticks = 0;
        s_hdr_len = 0;
        char t[128];
        if (s_sse_fd < 0) {
            snprintf(t, sizeof(t), "No route to %s:%d", s_cfg.host, s_cfg.port);
            set_status(OPENDEV_DOWN, t);
            return;
        }
        snprintf(t, sizeof(t), "Connecting %s:%d...", s_cfg.host, s_cfg.port);
        set_status(OPENDEV_CONNECTING, t);
        return;
    }

    if (s_status == OPENDEV_CONNECTING && s_hdr_len == 0 && s_connect_ticks >= 0) {
        int r = devos_net_socket_connect_wait(s_sse_fd, 0);
        if (r > 0) {
            if (++s_connect_ticks > CONNECT_GIVEUP_TICKS) link_drop("Connect timeout");
            return;
        }
        if (r < 0) {
            char t[128];
            snprintf(t, sizeof(t), "Refused by %s:%d", s_cfg.host, s_cfg.port);
            link_drop(t);
            return;
        }
        char req[512];
        int hlen = snprintf(req, sizeof(req),
                            "GET /event HTTP/1.1\r\nHost: %s:%d\r\nAccept: text/event-stream\r\n"
                            "Cache-Control: no-cache\r\n%s%s%sConnection: keep-alive\r\n\r\n",
                            s_cfg.host, s_cfg.port, s_cfg.token[0] ? "Authorization: Bearer " : "", s_cfg.token,
                            s_cfg.token[0] ? "\r\n" : "");
        if (hlen <= 0 || devos_net_socket_send_all(s_sse_fd, req, (size_t)hlen) != 0) {
            link_drop("Subscribe failed");
            return;
        }
        s_connect_ticks = -1;                   /* now waiting for headers */
    }

    static EXT_RAM_BSS_ATTR char chunk[4096];
    for (int it = 0; it < 16; it++) {
        int n = devos_net_socket_recv(s_sse_fd, chunk, sizeof(chunk) - 1, 1);
        if (n == 0) { link_drop("Server closed the event stream"); break; }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ENOTCONN &&
                errno != EINPROGRESS) {
                link_drop("Event stream lost");
            }
            break;
        }
        if (s_status == OPENDEV_CONNECTING) {
            size_t take = (size_t)n;
            if (s_hdr_len + take >= sizeof(s_hdr_buf)) take = sizeof(s_hdr_buf) - 1 - s_hdr_len;
            memcpy(s_hdr_buf + s_hdr_len, chunk, take);
            s_hdr_len += take;
            s_hdr_buf[s_hdr_len] = '\0';
            char *eoh = strstr(s_hdr_buf, "\r\n\r\n");
            if (!eoh) {
                if (s_hdr_len >= sizeof(s_hdr_buf) - 1) { link_drop("Bad response headers"); return; }
                continue;
            }
            int code = 0;
            if (sscanf(s_hdr_buf, "HTTP/%*d.%*d %d", &code) != 1) code = 0;
            if (code != 200) {
                char t[64];
                snprintf(t, sizeof(t), code == 401 ? "Unauthorized (check the token)" : "Server HTTP %d", code);
                link_drop(t);
                return;
            }
            *eoh = '\0';
            s_chunked = ci_contains(s_hdr_buf, "transfer-encoding: chunked");
            s_chunk_left = 0;
            s_chunk_hdr_len = 0;
            size_t hused = (size_t)(eoh - s_hdr_buf) + 4;
            size_t extra = s_hdr_len - hused;
            set_status(OPENDEV_UP, "Live");
            s_sse_len = 0;
            s_sse_skip = false;
            if (s_sse_buf) s_sse_buf[0] = '\0';
            opendev_client_refresh_sessions();
            if (s_active_id[0]) s_want_messages = true;    /* catch up on what we missed */
            if (take < (size_t)n) {
                /* header buffer filled mid-read: pass the remainder on */
                sse_body(s_hdr_buf + hused, extra);
                sse_body(chunk + take, (size_t)n - take);
            } else if (extra) {
                sse_body(s_hdr_buf + hused, extra);
            }
        } else {
            sse_body(chunk, (size_t)n);
        }
    }
    if (s_status == OPENDEV_CONNECTING && s_sse_fd >= 0 && s_connect_ticks < 0) {
        if (--s_connect_ticks < -(CONNECT_GIVEUP_TICKS)) link_drop("No response from server");
    }
}

opendev_status_t opendev_client_status(void) { return s_status; }
const char *opendev_client_status_text(void) { return s_status_text; }
uint32_t opendev_client_generation(void) { return s_gen; }
uint32_t opendev_client_blocks_generation(void) { return s_blocks_gen; }
uint32_t opendev_client_diff_generation(void) { return s_diff_gen; }
bool opendev_client_loading(void) { return s_inflight > 0; }
bool opendev_client_messages_loading(void) { return s_msg_inflight > 0; }

void opendev_client_get_config(char *host, size_t host_len, int *port, opendev_mode_t *mode, char *token,
                               size_t token_len)
{
    if (host && host_len) snprintf(host, host_len, "%s", s_cfg.host);
    if (port) *port = s_cfg.port;
    if (mode) *mode = s_cfg.mode;
    if (token && token_len) snprintf(token, token_len, "%s", s_cfg.token);
}

static void relink(void)
{
    if (s_sse_fd >= 0) {
        devos_net_socket_close(s_sse_fd);
        s_sse_fd = -1;
    }
    s_retry_ticks = RETRY_TICKS;
    s_session_count = 0;
    s_active_id[0] = '\0';
    s_block_count = 0;
    s_diff[0] = '\0';
    s_diff_file_count = 0;
    s_perm.active = false;
    bump_blocks();
    s_diff_gen++;
}

int opendev_client_set_server(const char *host, int port)
{
    if (!host || !*host || port <= 0 || port > 65535) return -1;
    snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", host);
    s_cfg.port = port;
    s_cfg.mode = OPENDEV_MODE_CODE;
    s_cfg.token[0] = '\0';
    config_save();
    relink();
    return 0;
}

int opendev_client_set_chamber(const char *host, int port, const char *token)
{
    if (!host || !*host || port <= 0 || port > 65535) return -1;
    snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", host);
    s_cfg.port = port;
    s_cfg.mode = OPENDEV_MODE_CHAMBER;
    snprintf(s_cfg.token, sizeof(s_cfg.token), "%s", token ? token : "");
    config_save();
    relink();
    return 0;
}

int opendev_client_set_mode(opendev_mode_t mode)
{
    if (mode != OPENDEV_MODE_CODE && mode != OPENDEV_MODE_CHAMBER) return -1;
    s_cfg.mode = mode;
    config_save();
    bump();
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
    /* openchamber://connect?host=H&port=P&token=T (p= accepted for token);
     * also openchamber://host:port?token=T */
    if (!uri || strncmp(uri, "openchamber://", 14) != 0) return -1;
    const char *rest = uri + 14;
    const char *q = strchr(rest, '?');
    char host[OPENDEV_HOST_MAX] = "";
    int port = 0;
    char token[OPENDEV_TOKEN_MAX] = "";
    size_t auth_len = q ? (size_t)(q - rest) : strlen(rest);
    if (auth_len > 0 && auth_len < 64) {
        char auth[64];
        memcpy(auth, rest, auth_len);
        auth[auth_len] = '\0';
        if (auth[auth_len - 1] == '/') auth[auth_len - 1] = '\0';
        if (strcmp(auth, "connect") != 0 && auth[0] != '\0') {
            char *colon = strchr(auth, ':');
            if (colon) {
                *colon = '\0';
                port = atoi(colon + 1);
            }
            snprintf(host, sizeof(host), "%s", auth);
        }
    }
    if (q) {
        char query[256];
        snprintf(query, sizeof(query), "%s", q + 1);
        char *save = NULL;
        for (char *pair = strtok_r(query, "&", &save); pair; pair = strtok_r(NULL, "&", &save)) {
            char *eq = strchr(pair, '=');
            if (!eq) continue;
            *eq = '\0';
            url_decode(pair);
            url_decode(eq + 1);
            if (strcmp(pair, "host") == 0) snprintf(host, sizeof(host), "%s", eq + 1);
            else if (strcmp(pair, "port") == 0) port = atoi(eq + 1);
            else if (strcmp(pair, "token") == 0 || strcmp(pair, "p") == 0) snprintf(token, sizeof(token), "%s", eq + 1);
        }
    }
    if (!token[0]) return -1;
    snprintf(s_cfg.token, sizeof(s_cfg.token), "%s", token);
    if (host[0]) snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", host);
    if (port > 0 && port < 65536) s_cfg.port = port;
    s_cfg.mode = OPENDEV_MODE_CHAMBER;
    config_save();
    relink();
    return 0;
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
