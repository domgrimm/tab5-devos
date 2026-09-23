/* opendev_client: OpenCode / OpenChamber HTTP+SSE engine.
 *
 * Design notes:
 * - Only the SSE link lives across poll() ticks, and every tick is
 *   non-blocking (non-blocking connect + 1 ms bounded reads).
 * - REST calls are short-timeout and synchronous, issued from UI actions.
 * - JSON parsing is a minimal reader for exactly the shapes we consume;
 *   unknown fields and events are ignored, never fatal.
 */
#include "opendev_client.h"
#include "devos_json.h"
#include "devos_net.h"
#include "devos_config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef ESP_PLATFORM
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"
#else
#include <sys/socket.h>
#endif

#ifndef ESP_PLATFORM
#define OPENDEV_NVS_FILE TAB5_SD_MOUNT_POINT "/.devos/opendev_nvs.json"
#endif

#define REST_TIMEOUT_MS 4000
#define HTTP_REQ_MAX 5120
#define HTTP_RESP_MAX 32768
#define SSE_BUF_MAX 8192
#define SSE_LINE_MAX 2048
#define CONNECT_GIVEUP_TICKS 100   /* 10 s at 100 ms poll */
#define RETRY_TICKS 30             /* 3 s between link attempts */
#define REFETCH_DEBOUNCE_TICKS 5   /* 500 ms coalescing for message refetch */

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

static opendev_session_t s_sessions[OPENDEV_MAX_SESSIONS];
static int s_session_count = 0;
static int s_active = -1;
static EXT_RAM_BSS_ATTR opendev_block_t s_blocks[OPENDEV_MAX_BLOCKS];
static int s_block_count = 0;
static opendev_permission_t s_perm;
static opendev_status_t s_status = OPENDEV_DOWN;
static char s_status_text[128] = "Offline";
static uint32_t s_gen = 0;
static EXT_RAM_BSS_ATTR char s_diff[OPENDEV_DIFF_MAX] = "";

/* SSE link */
static int s_sse_fd = -1;
static int s_connect_ticks = 0;
static int s_retry_ticks = 0;
static EXT_RAM_BSS_ATTR char s_sse_buf[SSE_BUF_MAX];
static size_t s_sse_len = 0;
static char s_hdr_buf[1024];
static size_t s_hdr_len = 0;
static bool s_want_link = false;

/* Deferred refetch flags (set by SSE, executed in poll) */
static bool s_want_sessions = false;
static bool s_want_messages = false;
static int s_refetch_ticks = 0;

static void bump(void) { s_gen++; }

static void set_status(opendev_status_t st, const char *text)
{
    if (s_status != st) {
        s_status = st;
        bump();
    }
    if (text && strcmp(s_status_text, text) != 0) {
        snprintf(s_status_text, sizeof(s_status_text), "%s", text);
        s_status_text[sizeof(s_status_text) - 1] = '\0';
        bump();
    }
}

/* ------------------------------------------------------------ persistence */
static void config_save(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("opendev", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "host", s_cfg.host);
        nvs_set_u8(h, "port", (uint8_t)(s_cfg.port > 255 ? 0 : s_cfg.port));
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
        if (nvs_get_u16(h, "port16", &p16) == ESP_OK && p16) {
            s_cfg.port = p16;
        }
        uint8_t m = 0;
        if (nvs_get_u8(h, "mode", &m) == ESP_OK && m <= OPENDEV_MODE_CHAMBER) {
            s_cfg.mode = (opendev_mode_t)m;
        }
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
            } else if (sscanf(buf, " \"port\": %d", &ival) == 1 && ival > 0 &&
                       ival < 65536) {
                s_cfg.port = ival;
            } else if (sscanf(buf, " \"mode\": %d", &ival) == 1 &&
                       (ival == 0 || ival == 1)) {
                s_cfg.mode = (opendev_mode_t)ival;
            } else if (sscanf(buf, " \"token\": \"%127[^\"]\"", val) == 1) {
                snprintf(s_cfg.token, sizeof(s_cfg.token), "%s", val);
            }
        }
        fclose(f);
    }
#endif
}


/* ------------------------------------------------------------------- HTTP */
static int send_all(int fd, const char *buf, size_t len)
{
    return devos_net_socket_send_all(fd, buf, len);
}

/* Minimal HTTP/1.1 client. Returns 0 with status+body, -1 on transport error.
 * Reads Content-Length bodies, de-chunks, or reads-until-close (capped). */
static int http_do(const char *method, const char *path, const char *body,
                   char *resp, size_t cap, int *status_out)
{
    if (!method || !path || !resp || cap < 64) return -1;
    if (status_out) *status_out = 0;
    resp[0] = '\0';

    int fd = devos_net_socket_connect(s_cfg.host, s_cfg.port, REST_TIMEOUT_MS);
    if (fd < 0) return -1;

    char req[HTTP_REQ_MAX];
    size_t blen = body ? strlen(body) : 0;
    int hlen = snprintf(req, sizeof(req),
                        "%s %s HTTP/1.1\r\nHost: %s:%d\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: %zu\r\n"
                        "%s%s%s"
                        "Connection: close\r\n\r\n",
                        method, path, s_cfg.host, s_cfg.port, blen,
                        s_cfg.token[0] ? "Authorization: Bearer " : "",
                        s_cfg.token[0] ? s_cfg.token : "",
                        s_cfg.token[0] ? "\r\n" : "");
    if (hlen <= 0 || (size_t)hlen >= sizeof(req)) {
        devos_net_socket_close(fd);
        return -1;
    }

    int rc = -1;
    size_t total = 0;
    if (send_all(fd, req, (size_t)hlen) == 0 &&
        (!body || send_all(fd, body, blen) == 0)) {
        /* ponytail: room-guarded read loop (a fixed 1K floor starves
         * small buffers entirely) */
        for (;;) {
            size_t room = cap - 1 - total;
            if (room == 0) break;
            size_t want = room > 1024 ? 1024 : room;
            int n = devos_net_socket_recv(fd, resp + total, want,
                                          REST_TIMEOUT_MS);
            if (n <= 0) break;
            total += (size_t)n;
        }
        resp[total] = '\0';
        rc = 0;
    }
    devos_net_socket_close(fd);
    if (rc != 0) return -1;

    /* status line */
    int status = 0;
    if (sscanf(resp, "HTTP/%*d.%*d %d", &status) != 1 &&
        sscanf(resp, "HTTP/%*d %d", &status) != 1) {
        return -1;
    }
    if (status_out) *status_out = status;

    /* split headers/body */
    char *hdr_end = strstr(resp, "\r\n\r\n");
    if (!hdr_end) return -1;
    char *body_start = hdr_end + 4;
    bool chunked = strstr(resp, "Transfer-Encoding: chunked") != NULL ||
                   strstr(resp, "transfer-encoding: chunked") != NULL;
    if (chunked) {
        /* ponytail: in-place de-chunker; malformed chunks end the body */
        static EXT_RAM_BSS_ATTR char flat[HTTP_RESP_MAX];
        size_t fo = 0;
        char *p = body_start;
        char *resp_end = resp + total;
        while (p < resp_end && fo + 1 < sizeof(flat) && fo + 1 < cap) {
            char *eol = strstr(p, "\r\n");
            if (!eol) break;
            unsigned long cl = strtoul(p, NULL, 16);
            p = eol + 2;
            if (cl == 0) break;
            if (p + cl > resp_end) break;
            size_t take = cl;
            if (fo + take >= sizeof(flat)) take = sizeof(flat) - fo - 1;
            if (fo + take >= cap) take = cap - fo - 1;
            memcpy(flat + fo, p, take);
            fo += take;
            p += cl;
            if (p + 2 <= resp_end && p[0] == '\r' && p[1] == '\n') p += 2;
            if (fo + 1 >= sizeof(flat) || fo + 1 >= cap) break;
        }
        flat[fo] = '\0';
        if (fo + 1 > cap) return -1;
        memmove(resp, flat, fo + 1);
        return 0;
    }
    /* Content-Length or close-delimited: shift body to front */
    size_t bl = strlen(body_start);
    memmove(resp, body_start, bl + 1);
    return 0;
}

/* ------------------------------------------------------------- SSE events */
static void push_block(uint8_t role, uint8_t kind, const char *text)
{
    if (!text || !*text) return;
    /* ponytail: coalesce consecutive same-role text (streaming deltas) */
    if (kind == OPENDEV_KIND_TEXT && s_block_count > 0) {
        opendev_block_t *last = &s_blocks[s_block_count - 1];
        if (last->role == role && last->kind == OPENDEV_KIND_TEXT) {
            size_t have = strlen(last->text);
            size_t add = strlen(text);
            size_t room = sizeof(last->text) - have - 1;
            if (add > room) add = room;
            memcpy(last->text + have, text, add);
            last->text[have + add] = '\0';
            bump();
            return;
        }
    }
    if (s_block_count >= OPENDEV_MAX_BLOCKS) {
        memmove(s_blocks, s_blocks + 1,
                sizeof(s_blocks[0]) * (OPENDEV_MAX_BLOCKS - 1));
        s_block_count = OPENDEV_MAX_BLOCKS - 1;
    }
    opendev_block_t *b = &s_blocks[s_block_count++];
    b->role = role;
    b->kind = kind;
    snprintf(b->text, sizeof(b->text), "%s", text);
    b->text[sizeof(b->text) - 1] = '\0';
    bump();
}

static void set_busy(const char *session_id, bool busy)
{
    for (int i = 0; i < s_session_count; i++) {
        if (!session_id || !*session_id ||
            strcmp(s_sessions[i].id, session_id) == 0) {
            if (s_sessions[i].busy != busy) {
                s_sessions[i].busy = busy;
                bump();
            }
            if (session_id && *session_id) break;
        }
    }
}

static void on_sse_event(const char *name, const char *data, size_t dlen)
{
    if (!name || !*name) return;

    if (strcmp(name, "server.connected") == 0) {
        return;
    }
    if (strcmp(name, "session.created") == 0) {
        s_want_sessions = true;
        return;
    }
    if (strcmp(name, "session.idle") == 0) {
        char sid[OPENDEV_ID_MAX] = "";
        devos_json_get_str(data, dlen, "sessionID", sid, sizeof(sid));
        set_busy(sid[0] ? sid : NULL, false);
        return;
    }
    if (strcmp(name, "session.status") == 0) {
        char sid[OPENDEV_ID_MAX] = "";
        devos_json_get_str(data, dlen, "sessionID", sid, sizeof(sid));
        const char *end = data + dlen;
        const char *st = devos_json_find_key(data, end, "status");
        bool busy = false;
        if (st && st < end && *st == '{') {
            const char *sp = devos_json_span(st, end);
            char type[32] = "";
            if (sp) devos_json_get_str(st, (size_t)(sp - st), "type", type, sizeof(type));
            busy = strcmp(type, "busy") == 0;
        }
        set_busy(sid[0] ? sid : NULL, busy);
        return;
    }
    if (strcmp(name, "message.updated") == 0 ||
        strcmp(name, "message.part.updated") == 0) {
        /* ponytail: refetch (debounced) beats tracking part-ID deltas */
        char sid[OPENDEV_ID_MAX] = "";
        devos_json_get_str(data, dlen, "sessionID", sid, sizeof(sid));
        if (s_active >= 0 && s_active < s_session_count &&
            (!sid[0] || strcmp(s_sessions[s_active].id, sid) == 0)) {
            s_want_messages = true;
            s_refetch_ticks = 0;
        }
        return;
    }
    if (strcmp(name, "permission.asked") == 0) {
        char pid[OPENDEV_ID_MAX] = "";
        char sid[OPENDEV_ID_MAX] = "";
        char title[256] = "";
        if (devos_json_get_str(data, dlen, "id", pid, sizeof(pid)) != 0) {
            devos_json_get_str(data, dlen, "permissionID", pid, sizeof(pid));
        }
        devos_json_get_str(data, dlen, "sessionID", sid, sizeof(sid));
        if (devos_json_get_str(data, dlen, "title", title, sizeof(title)) != 0) {
            devos_json_get_str(data, dlen, "text", title, sizeof(title));
        }
        if (pid[0]) {
            s_perm.active = true;
            snprintf(s_perm.id, sizeof(s_perm.id), "%s", pid);
            snprintf(s_perm.session_id, sizeof(s_perm.session_id), "%s", sid);
            snprintf(s_perm.text, sizeof(s_perm.text), "%s", title[0] ? title : pid);
            bump();
        }
        return;
    }
}

/* feed raw bytes; extracts complete events (blank-line terminated) */
static void sse_feed(const char *buf, size_t len)
{
    if (len == 0) return;
    if (s_sse_len + len >= sizeof(s_sse_buf)) {
        /* ponytail: compact by dropping the oldest half (headers never split) */
        size_t drop = s_sse_len / 2;
        memmove(s_sse_buf, s_sse_buf + drop, s_sse_len - drop);
        s_sse_len -= drop;
    }
    size_t take = len;
    if (s_sse_len + take >= sizeof(s_sse_buf)) {
        take = sizeof(s_sse_buf) - s_sse_len - 1;
    }
    memcpy(s_sse_buf + s_sse_len, buf, take);
    s_sse_len += take;
    s_sse_buf[s_sse_len] = '\0';

    for (;;) {
        char *dbl = strstr(s_sse_buf, "\n\n");
        if (!dbl) return;
        *dbl = '\0';
        /* parse one event */
        char ev_name[64] = "";
        static char ev_data[SSE_LINE_MAX * 4];
        size_t ev_len = 0;
        ev_data[0] = '\0';
        for (char *ln = s_sse_buf; ln < dbl;) {
            char *eol = strchr(ln, '\n');
            if (!eol || eol > dbl) eol = dbl;
            size_t llen = (size_t)(eol - ln);
            while (llen > 0 && (ln[llen - 1] == '\r')) llen--;
            if (llen == 0) {
                /* blank handled by outer split; skip */
            } else if (ln[0] == ':') {
                /* SSE comment / keep-alive */
            } else if (llen > 6 && memcmp(ln, "event:", 6) == 0) {
                const char *v = ln + 6;
                while (v < ln + llen && (*v == ' ')) v++;
                size_t vl = (size_t)((ln + llen) - v);
                if (vl > sizeof(ev_name) - 1) vl = sizeof(ev_name) - 1;
                memcpy(ev_name, v, vl);
                ev_name[vl] = '\0';
            } else if (llen > 5 && memcmp(ln, "data:", 5) == 0) {
                const char *v = ln + 5;
                while (v < ln + llen && (*v == ' ')) v++;
                size_t vl = (size_t)((ln + llen) - v);
                if (ev_len > 0 && ev_len + 1 < sizeof(ev_data)) {
                    ev_data[ev_len++] = '\n';
                }
                if (ev_len + vl >= sizeof(ev_data)) {
                    vl = sizeof(ev_data) - ev_len - 1;
                }
                memcpy(ev_data + ev_len, v, vl);
                ev_len += vl;
                ev_data[ev_len] = '\0';
            }
            ln = (*eol == '\n') ? eol + 1 : eol;
            if (ln >= dbl) break;
        }
        if (ev_name[0]) on_sse_event(ev_name, ev_data, ev_len);
        /* consume through the blank line */
        size_t used = (size_t)(dbl - s_sse_buf) + 2;
        memmove(s_sse_buf, s_sse_buf + used, s_sse_len - used + 1);
        s_sse_len -= used;
    }
}

/* ------------------------------------------------------------- REST ops */
static void sessions_each_cb(const char *obj, size_t len, void *ud)
{
    (void)ud;
    if (s_session_count >= OPENDEV_MAX_SESSIONS) return;
    opendev_session_t *s = &s_sessions[s_session_count];
    memset(s, 0, sizeof(*s));
    if (devos_json_get_str(obj, len, "id", s->id, sizeof(s->id)) != 0) return;
    if (devos_json_get_str(obj, len, "title", s->title, sizeof(s->title)) != 0) {
        s->title[0] = '\0';
        strncat(s->title, s->id, 8);
    }
    devos_json_get_str(obj, len, "modelID", s->model, sizeof(s->model));
    if (!s->model[0]) devos_json_get_str(obj, len, "model", s->model, sizeof(s->model));
    s_session_count++;
}

int opendev_client_refresh_sessions(void)
{
    static EXT_RAM_BSS_ATTR char resp[HTTP_RESP_MAX];
    int status = 0;
    if (http_do("GET", "/session", NULL, resp, sizeof(resp), &status) != 0 ||
        status != 200) {
        return -1;
    }
    int before = s_session_count;
    /* preserve busy flags across refresh */
    bool busy[OPENDEV_MAX_SESSIONS] = {false};
    char ids[OPENDEV_MAX_SESSIONS][OPENDEV_ID_MAX];
    for (int i = 0; i < s_session_count && i < OPENDEV_MAX_SESSIONS; i++) {
        busy[i] = s_sessions[i].busy;
        memcpy(ids[i], s_sessions[i].id, sizeof(ids[i]));
    }
    s_session_count = 0;
    devos_json_array_each(resp, strlen(resp), sessions_each_cb, NULL);
    for (int i = 0; i < s_session_count; i++) {
        for (int j = 0; j < before; j++) {
            if (strcmp(s_sessions[i].id, ids[j]) == 0) {
                s_sessions[i].busy = busy[j];
                break;
            }
        }
    }
    if (s_active >= s_session_count) s_active = s_session_count - 1;
    bump();
    return 0;
}

static uint8_t s_msg_role_tmp = OPENDEV_ROLE_ASST;

static void parts_each_cb(const char *obj, size_t len, opendev_block_t *tmp);
static void opendev_parts_cb(const char *obj, size_t len, void *ud);

static void messages_each_cb(const char *obj, size_t len, void *ud)
{
    (void)ud;
    char role[16] = "";
    devos_json_get_str(obj, len, "role", role, sizeof(role));
    s_msg_role_tmp =
        (strcmp(role, "user") == 0) ? OPENDEV_ROLE_USER : OPENDEV_ROLE_ASST;
    const char *end = obj + len;
    const char *pv = devos_json_find_key(obj, end, "parts");
    if (!pv || pv >= end || *pv != '[') {
        char direct[OPENDEV_BLOCK_MAX] = "";
        if (devos_json_get_str(obj, len, "content", direct, sizeof(direct)) == 0 ||
            devos_json_get_str(obj, len, "text", direct, sizeof(direct)) == 0) {
            push_block(s_msg_role_tmp, OPENDEV_KIND_TEXT, direct);
        }
        return;
    }
    const char *stop = devos_json_span(pv, end);
    if (!stop) return;
    devos_json_array_each(pv, (size_t)(stop - pv), opendev_parts_cb, NULL);
}

static void opendev_parts_cb(const char *obj, size_t len, void *ud)
{
    (void)ud;
    opendev_block_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.role = s_msg_role_tmp;
    parts_each_cb(obj, len, &tmp);
    if (tmp.text[0]) push_block(tmp.role, tmp.kind, tmp.text);
}

static void parts_each_cb(const char *obj, size_t len, opendev_block_t *tmp)
{
    char type[32] = "";
    devos_json_get_str(obj, len, "type", type, sizeof(type));
    if (strcmp(type, "text") == 0) {
        tmp->kind = OPENDEV_KIND_TEXT;
        devos_json_get_str(obj, len, "text", tmp->text, sizeof(tmp->text));
    } else if (strcmp(type, "reasoning") == 0) {
        tmp->kind = OPENDEV_KIND_THINK;
        devos_json_get_str(obj, len, "text", tmp->text, sizeof(tmp->text));
        if (!tmp->text[0]) {
            devos_json_get_str(obj, len, "reasoning", tmp->text, sizeof(tmp->text));
        }
    } else if (strncmp(type, "tool", 4) == 0) {
        tmp->kind = OPENDEV_KIND_TOOL;
        char name[96] = "";
        if (devos_json_get_str(obj, len, "tool", name, sizeof(name)) != 0) {
            devos_json_get_str(obj, len, "name", name, sizeof(name));
        }
        char detail[512] = "";
        if (devos_json_get_str(obj, len, "input", detail, sizeof(detail)) != 0) {
            if (devos_json_get_str(obj, len, "command", detail, sizeof(detail)) != 0 &&
                devos_json_get_str(obj, len, "path", detail, sizeof(detail)) != 0 &&
                devos_json_get_str(obj, len, "query", detail, sizeof(detail)) != 0) {
                devos_json_get_str(obj, len, "state", detail, sizeof(detail));
            }
        }
        if (devos_json_get_str(obj, len, "title", detail, sizeof(detail)) == 0) {
            /* prefer human title when present */
        }
        snprintf(tmp->text, sizeof(tmp->text), "%s%s%s",
                 name[0] ? name : type,
                 detail[0] ? "\n" : "", detail[0] ? detail : "");
    } else {
        /* ponytail: unknown part types surface as text, never vanish */
        tmp->kind = OPENDEV_KIND_TEXT;
        if (devos_json_get_str(obj, len, "text", tmp->text, sizeof(tmp->text)) != 0) {
            tmp->text[0] = '\0';
        }
    }
}

static int fetch_messages(void)
{
    if (s_active < 0 || s_active >= s_session_count) return -1;
    static EXT_RAM_BSS_ATTR char resp[HTTP_RESP_MAX];
    char path[160];
    snprintf(path, sizeof(path), "/session/%s/message?limit=50",
             s_sessions[s_active].id);
    int status = 0;
    if (http_do("GET", path, NULL, resp, sizeof(resp), &status) != 0 ||
        status != 200) {
        return -1;
    }
    s_block_count = 0;
    devos_json_array_each(resp, strlen(resp), messages_each_cb, NULL);
    bump();
    return 0;
}

/* ------------------------------------------------------------- public API */
void opendev_client_init(void)
{
    config_load();
    s_want_link = true;
    set_status(OPENDEV_DOWN, "Offline");
}

int opendev_client_reconnect(void)
{
    if (s_sse_fd >= 0) {
        devos_net_socket_close(s_sse_fd);
        s_sse_fd = -1;
    }
    s_retry_ticks = RETRY_TICKS; /* next poll dials immediately */
    s_want_link = true;
    set_status(OPENDEV_DOWN, "Reconnecting...");
    return 0;
}

void opendev_client_poll(void)
{
    /* debounced refetches */
    if ((s_want_sessions || s_want_messages) && s_status == OPENDEV_UP) {
        if (++s_refetch_ticks >= REFETCH_DEBOUNCE_TICKS) {
            s_refetch_ticks = 0;
            if (s_want_sessions) {
                s_want_sessions = false;
                opendev_client_refresh_sessions();
            }
            if (s_want_messages) {
                s_want_messages = false;
                fetch_messages();
            }
        }
    }

    if (!s_want_link) {
        if (s_sse_fd >= 0) {
            devos_net_socket_close(s_sse_fd);
            s_sse_fd = -1;
        }
        if (s_status != OPENDEV_DOWN) set_status(OPENDEV_DOWN, "Offline");
        return;
    }

    if (s_sse_fd < 0) {
        /* link down: retry with backoff */
        if (++s_retry_ticks < RETRY_TICKS) return;
        s_retry_ticks = 0;
        s_sse_fd = devos_net_socket_connect_start(s_cfg.host, s_cfg.port);
        s_connect_ticks = 0;
        s_hdr_len = 0;
        if (s_sse_fd < 0) {
            char t[128];
            snprintf(t, sizeof(t), "No route to %s:%d", s_cfg.host, s_cfg.port);
            set_status(OPENDEV_DOWN, t);
            return;
        }
        char t[128];
        snprintf(t, sizeof(t), "Connecting %s:%d...", s_cfg.host, s_cfg.port);
        set_status(OPENDEV_CONNECTING, t);
        return;
    }

    if (s_status == OPENDEV_CONNECTING) {
        int r = devos_net_socket_connect_wait(s_sse_fd, 0);
        if (r > 0) {
            if (++s_connect_ticks > CONNECT_GIVEUP_TICKS) {
                devos_net_socket_close(s_sse_fd);
                s_sse_fd = -1;
                set_status(OPENDEV_DOWN, "Connect timeout");
            }
            return;
        }
        if (r < 0) {
            devos_net_socket_close(s_sse_fd);
            s_sse_fd = -1;
            char t[128];
            snprintf(t, sizeof(t), "Refused by %s:%d", s_cfg.host, s_cfg.port);
            set_status(OPENDEV_DOWN, t);
            return;
        }
        /* connected: issue the SSE subscribe */
        char req[512];
        int hlen = snprintf(req, sizeof(req),
                            "GET /event HTTP/1.1\r\nHost: %s:%d\r\n"
                            "Accept: text/event-stream\r\n"
                            "Cache-Control: no-cache\r\n"
                            "%s%s%s"
                            "Connection: keep-alive\r\n\r\n",
                            s_cfg.host, s_cfg.port,
                            s_cfg.token[0] ? "Authorization: Bearer " : "",
                            s_cfg.token[0] ? s_cfg.token : "",
                            s_cfg.token[0] ? "\r\n" : "");
        if (hlen <= 0 ||
            send_all(s_sse_fd, req, (size_t)hlen) != 0) {
            devos_net_socket_close(s_sse_fd);
            s_sse_fd = -1;
            set_status(OPENDEV_DOWN, "Subscribe failed");
            return;
        }
        s_hdr_len = 0;
        /* fall through to header accumulation below */
    }

    /* read available bytes (1 ms bounded); first headers, then events */
    char chunk[1024];
    for (int it = 0; it < 4; it++) {
        int n = devos_net_socket_recv(s_sse_fd, chunk, sizeof(chunk) - 1, 1);
        if (n <= 0) {
            /* ponytail: n==0 is orderly close; the next tick's read fails
             * the same way and we recycle below */
            if (n == 0) {
                devos_net_socket_close(s_sse_fd);
                s_sse_fd = -1;
                set_status(OPENDEV_DOWN, "Server closed stream");
            }
            break;
        }
        if (s_status == OPENDEV_CONNECTING) {
            if (s_hdr_len + (size_t)n >= sizeof(s_hdr_buf)) {
                devos_net_socket_close(s_sse_fd);
                s_sse_fd = -1;
                set_status(OPENDEV_DOWN, "Bad headers");
                return;
            }
            memcpy(s_hdr_buf + s_hdr_len, chunk, (size_t)n);
            s_hdr_len += (size_t)n;
            s_hdr_buf[s_hdr_len] = '\0';
            char *eoh = strstr(s_hdr_buf, "\r\n\r\n");
            if (!eoh) continue;
            int code = 0;
            if (sscanf(s_hdr_buf, "HTTP/%*d.%*d %d", &code) != 1 &&
                sscanf(s_hdr_buf, "HTTP/%*d %d", &code) != 1) {
                code = 0;
            }
            if (code != 200) {
                devos_net_socket_close(s_sse_fd);
                s_sse_fd = -1;
                char t[64];
                snprintf(t, sizeof(t), "Server HTTP %d", code);
                set_status(OPENDEV_DOWN, t);
                return;
            }
            /* stream is live; any bytes past headers belong to events */
            size_t hused = (size_t)(eoh - s_hdr_buf) + 4;
            set_status(OPENDEV_UP, "SSE live");
            opendev_client_refresh_sessions();
            s_sse_len = 0;
            s_sse_buf[0] = '\0';
            if (s_hdr_len > hused) {
                sse_feed(s_hdr_buf + hused, s_hdr_len - hused);
            }
        } else {
            chunk[n] = '\0';
            sse_feed(chunk, (size_t)n);
        }
    }
    if (s_status == OPENDEV_CONNECTING && s_sse_fd >= 0) {
        /* ponytail: headers dribbling forever (slowloris) must not stall */
        if (++s_connect_ticks > CONNECT_GIVEUP_TICKS + 50) {
            devos_net_socket_close(s_sse_fd);
            s_sse_fd = -1;
            set_status(OPENDEV_DOWN, "Header timeout");
        }
    }
}

opendev_status_t opendev_client_status(void) { return s_status; }

const char *opendev_client_status_text(void) { return s_status_text; }

uint32_t opendev_client_generation(void) { return s_gen; }

void opendev_client_get_config(char *host, size_t host_len, int *port,
                               opendev_mode_t *mode, char *token,
                               size_t token_len)
{
    if (host && host_len) {
        snprintf(host, host_len, "%s", s_cfg.host);
        host[host_len - 1] = '\0';
    }
    if (port) *port = s_cfg.port;
    if (mode) *mode = s_cfg.mode;
    if (token && token_len) {
        snprintf(token, token_len, "%s", s_cfg.token);
        token[token_len - 1] = '\0';
    }
}

int opendev_client_set_server(const char *host, int port)
{
    if (!host || !*host || port <= 0 || port > 65535) return -1;
    snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", host);
    s_cfg.host[sizeof(s_cfg.host) - 1] = '\0';
    s_cfg.port = port;
    s_cfg.mode = OPENDEV_MODE_CODE;
    s_cfg.token[0] = '\0';
    config_save();
    /* force re-link */
    if (s_sse_fd >= 0) {
        devos_net_socket_close(s_sse_fd);
        s_sse_fd = -1;
    }
    s_retry_ticks = RETRY_TICKS;
    bump();
    return 0;
}

int opendev_client_set_chamber(const char *host, int port, const char *token)
{
    if (!host || !*host || port <= 0 || port > 65535) return -1;
    snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", host);
    s_cfg.host[sizeof(s_cfg.host) - 1] = '\0';
    s_cfg.port = port;
    s_cfg.mode = OPENDEV_MODE_CHAMBER;
    if (token) {
        snprintf(s_cfg.token, sizeof(s_cfg.token), "%s", token);
        s_cfg.token[sizeof(s_cfg.token) - 1] = '\0';
    } else {
        s_cfg.token[0] = '\0';
    }
    config_save();
    /* force re-link */
    if (s_sse_fd >= 0) {
        devos_net_socket_close(s_sse_fd);
        s_sse_fd = -1;
    }
    s_retry_ticks = RETRY_TICKS;
    bump();
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
        if (*r == '%' && isxdigit((unsigned char)r[1]) &&
            isxdigit((unsigned char)r[2])) {
            char hex[3] = {r[1], r[2], '\0'};
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
    /* openchamber://connect?host=H&port=P&token=T (p= accepted for token) */
    /* Also accepts openchamber://host:port?token=T or openchamber://host?token=T */
    if (!uri || strncmp(uri, "openchamber://", 14) != 0) return -1;
    const char *rest = uri + 14;
    const char *q = strchr(rest, '?');
    char host[OPENDEV_HOST_MAX] = "";
    int port = 0;
    char token[OPENDEV_TOKEN_MAX] = "";

    /* If there is an authority before '?' (e.g. openchamber://10.0.0.1:8421?token=...) */
    size_t auth_len = q ? (size_t)(q - rest) : strlen(rest);
    if (auth_len > 0 && auth_len < 64) {
        char auth[64];
        memcpy(auth, rest, auth_len);   /* fixed-length substring, not a C-string copy */
        auth[auth_len] = '\0';
        if (auth[auth_len - 1] == '/') auth[auth_len - 1] = '\0';
        if (strcmp(auth, "connect") != 0 && auth[0] != '\0') {
            char *colon = strchr(auth, ':');
            if (colon) {
                *colon = '\0';
                snprintf(host, sizeof(host), "%s", auth);
                port = atoi(colon + 1);
            } else {
                snprintf(host, sizeof(host), "%s", auth);
            }
        }
    }

    if (q) {
        char query[256];
        snprintf(query, sizeof(query), "%s", q + 1);
        query[sizeof(query) - 1] = '\0';
        for (char *pair = strtok(query, "&"); pair; pair = strtok(NULL, "&")) {
            char *eq = strchr(pair, '=');
            if (!eq) continue;
            *eq = '\0';
            url_decode(pair);
            url_decode(eq + 1);
            if (strcmp(pair, "host") == 0) {
                snprintf(host, sizeof(host), "%s", eq + 1);
            } else if (strcmp(pair, "port") == 0) {
                port = atoi(eq + 1);
            } else if (strcmp(pair, "token") == 0 || strcmp(pair, "p") == 0) {
                snprintf(token, sizeof(token), "%s", eq + 1);
            }
        }
    }
    if (!token[0]) return -1;
    snprintf(s_cfg.token, sizeof(s_cfg.token), "%s", token);
    if (host[0]) snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", host);
    if (port > 0 && port < 65536) s_cfg.port = port;
    s_cfg.mode = OPENDEV_MODE_CHAMBER;
    config_save();
    if (s_sse_fd >= 0) {
        devos_net_socket_close(s_sse_fd);
        s_sse_fd = -1;
    }
    s_retry_ticks = RETRY_TICKS;
    bump();
    return 0;
}

int opendev_client_session_count(void) { return s_session_count; }

const opendev_session_t *opendev_client_session(int idx)
{
    if (idx < 0 || idx >= s_session_count) return NULL;
    return &s_sessions[idx];
}

int opendev_client_active(void) { return s_active; }

int opendev_client_select(int idx)
{
    if (idx < 0 || idx >= s_session_count) return -1;
    s_active = idx;
    s_block_count = 0;
    s_diff[0] = '\0'; /* ponytail: never show the previous session's diff */
    bump();
    return fetch_messages();
}

int opendev_client_new_session(void)
{
    static char resp[4096];
    char body[160];
    snprintf(body, sizeof(body), "{\"title\":\"Tab5 %s\"}",
             s_cfg.mode == OPENDEV_MODE_CHAMBER ? "chamber" : "session");
    int status = 0;
    if (http_do("POST", "/session", body, resp, sizeof(resp), &status) != 0 ||
        (status != 200 && status != 201)) {
        return -1;
    }
    char id[OPENDEV_ID_MAX] = "";
    if (devos_json_get_str(resp, strlen(resp), "id", id, sizeof(id)) != 0) {
        return -1;
    }
    opendev_client_refresh_sessions();
    for (int i = 0; i < s_session_count; i++) {
        if (strcmp(s_sessions[i].id, id) == 0) {
            return opendev_client_select(i);
        }
    }
    return 0;
}

int opendev_client_send(const char *text)
{
    if (!text || !*text) return -1;
    if (s_active < 0 || s_active >= s_session_count) {
        if (opendev_client_new_session() != 0) return -1;
    }
    /* optimistic user block */
    char clipped[OPENDEV_BLOCK_MAX];
    snprintf(clipped, sizeof(clipped), "%s", text);
    clipped[sizeof(clipped) - 1] = '\0';
    push_block(OPENDEV_ROLE_USER, OPENDEV_KIND_TEXT, clipped);

    static char body[4096];
    char esc[3600];
    devos_json_escape(text, esc, sizeof(esc));
    snprintf(body, sizeof(body),
             "{\"parts\":[{\"type\":\"text\",\"text\":\"%s\"}]}", esc);
    char path[160];
    snprintf(path, sizeof(path), "/session/%s/prompt_async",
             s_sessions[s_active].id);
    static char resp[1024];
    int status = 0;
    int rc = http_do("POST", path, body, resp, sizeof(resp), &status);
    if (rc != 0 || (status != 200 && status != 204)) {
        /* ponytail: never leave a stuck busy badge on a failed send */
        set_busy(s_sessions[s_active].id, false);
        push_block(OPENDEV_ROLE_ASST, OPENDEV_KIND_TEXT,
                   "(send failed - server unreachable)");
        return -1;
    }
    set_busy(s_sessions[s_active].id, true);
    return 0;
}

int opendev_client_abort(void)
{
    if (s_active < 0 || s_active >= s_session_count) return -1;
    char path[160];
    snprintf(path, sizeof(path), "/session/%s/abort", s_sessions[s_active].id);
    static char resp[256];
    int status = 0;
    int rc = http_do("POST", path, "{}", resp, sizeof(resp), &status);
    set_busy(s_sessions[s_active].id, false);
    return (rc == 0 && (status == 200 || status == 204)) ? 0 : -1;
}

int opendev_client_block_count(void) { return s_block_count; }

const opendev_block_t *opendev_client_block(int idx)
{
    if (idx < 0 || idx >= s_block_count) return NULL;
    return &s_blocks[idx];
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
    const char *resp_str = !allow ? "reject" : (always ? "always" : "once");
    char path[192];
    snprintf(path, sizeof(path), "/session/%s/permissions/%s",
             s_perm.session_id[0] ? s_perm.session_id : "unknown", s_perm.id);
    char body[64];
    snprintf(body, sizeof(body), "{\"response\":\"%s\"}", resp_str);
    static char resp[256];
    int status = 0;
    int rc = http_do("POST", path, body, resp, sizeof(resp), &status);
    s_perm.active = false;
    bump();
    return (rc == 0 && status == 200) ? 0 : -1;
}

int opendev_client_fetch_diff(void)
{
    if (s_active < 0 || s_active >= s_session_count) return -1;
    char path[160];
    snprintf(path, sizeof(path), "/session/%s/diff", s_sessions[s_active].id);
    int status = 0;
    int rc = http_do("GET", path, NULL, s_diff, sizeof(s_diff), &status);
    if (rc != 0 || status != 200) {
        snprintf(s_diff, sizeof(s_diff), "(diff unavailable)");
        bump();
        return -1;
    }
    if (!s_diff[0]) snprintf(s_diff, sizeof(s_diff), "(no changes)");
    bump();
    return 0;
}

const char *opendev_client_diff_text(void) { return s_diff; }
