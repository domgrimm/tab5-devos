/* agy_client: Antigravity bridge WebSocket engine. See agy_client.h. */
#include "agy_client.h"
#include "devos_json.h"
#include "devos_net.h"
#include "devos_config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "nvs_flash.h"
#include "nvs.h"
#endif

#ifndef ESP_PLATFORM
#define AGY_NVS_FILE TAB5_SD_MOUNT_POINT "/.devos/agy_nvs.json"
#endif

#define WS_TIMEOUT_TICKS 100      /* 10 s handshake/connect budget */
#define WS_RETRY_TICKS 30         /* 3 s between attempts */
#define WS_RX_MAX 8192
#define WS_TX_MAX 4352            /* prompts truncate past 4 KB */
#define WS_HDR_MAX 1024

typedef struct {
    char host[AGY_HOST_MAX];
    int port;
    char token[AGY_TOKEN_MAX];
} agy_config_t;

static agy_config_t s_cfg = {
    .host = "100.77.11.92",
    .port = 8420,
    .token = "",
};

static agy_agent_t s_agents[AGY_MAX_AGENTS];
static int s_agent_count = 0;
static EXT_RAM_BSS_ATTR agy_artifact_t s_artifacts[AGY_MAX_ARTIFACTS];
static int s_artifact_count = 0;
static EXT_RAM_BSS_ATTR agy_block_t s_blocks[AGY_MAX_BLOCKS];
static int s_block_count = 0;
static agy_permission_t s_perm;
static agy_question_t s_q;
static char s_conv[AGY_NAME_MAX] = "";
static char s_model[AGY_NAME_MAX] = "";
static EXT_RAM_BSS_ATTR char s_diff[AGY_ARTIFACT_MAX] = "";
static bool s_busy = false;
static agy_status_t s_status = AGY_DOWN;
static char s_status_text[128] = "Offline";
static uint32_t s_gen = 0;

/* Link state */
static int s_fd = -1;
static int s_ticks = 0;
static int s_retry = 0;
static bool s_want = false;
static bool s_sent_upgrade = false;
static bool s_handshook = false;
static EXT_RAM_BSS_ATTR char s_rx[WS_RX_MAX];
static size_t s_rx_len = 0;
static char s_hdr[WS_HDR_MAX];
static size_t s_hdr_len = 0;
/* fragmented text accumulation */
static EXT_RAM_BSS_ATTR char s_frag[WS_RX_MAX];
static size_t s_frag_len = 0;
static int s_frag_op = -1;

static void bump(void) { s_gen++; }

static void set_status(agy_status_t st, const char *text)
{
    if (s_status != st) {
        s_status = st;
        bump();
    }
    if (text && strcmp(s_status_text, text) != 0) {
        strncpy(s_status_text, text, sizeof(s_status_text) - 1);
        s_status_text[sizeof(s_status_text) - 1] = '\0';
        bump();
    }
}

/* ------------------------------------------------------------ persistence */
static void config_save(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("agy", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "host", s_cfg.host);
        nvs_set_u16(h, "port", (uint16_t)s_cfg.port);
        nvs_set_str(h, "token", s_cfg.token);
        nvs_commit(h);
        nvs_close(h);
    }
#else
    FILE *f = fopen(AGY_NVS_FILE, "w");
    if (f) {
        fprintf(f, "{\n  \"host\": \"%s\",\n  \"port\": %d,\n  \"token\": \"%s\"\n}\n",
                s_cfg.host, s_cfg.port, s_cfg.token);
        fclose(f);
    }
#endif
}

static void config_load(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("agy", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_cfg.host);
        nvs_get_str(h, "host", s_cfg.host, &len);
        uint16_t p16 = 0;
        if (nvs_get_u16(h, "port", &p16) == ESP_OK && p16) {
            s_cfg.port = p16;
        }
        len = sizeof(s_cfg.token);
        nvs_get_str(h, "token", s_cfg.token, &len);
        nvs_close(h);
    }
#else
    FILE *f = fopen(AGY_NVS_FILE, "r");
    if (f) {
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) {
            char val[128];
            int ival = 0;
            if (sscanf(buf, " \"host\": \"%127[^\"]\"", val) == 1) {
                strncpy(s_cfg.host, val, sizeof(s_cfg.host) - 1);
            } else if (sscanf(buf, " \"port\": %d", &ival) == 1 && ival > 0 &&
                       ival < 65536) {
                s_cfg.port = ival;
            } else if (sscanf(buf, " \"token\": \"%127[^\"]\"", val) == 1) {
                strncpy(s_cfg.token, val, sizeof(s_cfg.token) - 1);
            }
        }
        fclose(f);
    }
#endif
}

/* ------------------------------------------------------------------ b64 */
static void b64_encode(const uint8_t *in, size_t len, char *out)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)in[i] << 16;
        if (i + 1 < len) n |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) n |= in[i + 2];
        out[o++] = tab[(n >> 18) & 63];
        out[o++] = tab[(n >> 12) & 63];
        out[o++] = (i + 1 < len) ? tab[(n >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? tab[n & 63] : '=';
    }
    out[o] = '\0';
}

/* ---------------------------------------------------------------- frames */
static int ws_send_raw(const void *buf, size_t len)
{
    return devos_net_socket_send_all(s_fd, buf, len);
}

/* One masked client text frame (payload capped, longer is truncated). */
static int ws_send_text(const char *text, size_t len)
{
    if (s_fd < 0 || !text) return -1;
    if (len > WS_TX_MAX) len = WS_TX_MAX;
    uint8_t hdr[10];
    size_t hlen = 2;
    hdr[0] = 0x81; /* FIN + text */
    if (len < 126) {
        hdr[1] = 0x80 | (uint8_t)len;
    } else {
        hdr[1] = 0x80 | 126;
        hdr[2] = (uint8_t)((len >> 8) & 0xFF);
        hdr[3] = (uint8_t)(len & 0xFF);
        hlen = 4;
    }
    uint8_t mask[4] = {(uint8_t)rand(), (uint8_t)rand(),
                       (uint8_t)rand(), (uint8_t)rand()};
    if (ws_send_raw(hdr, hlen) != 0 || ws_send_raw(mask, 4) != 0) return -1;
    /* ponytail: mask in 1 KB stack chunks instead of a heap copy */
    static uint8_t chunk[1024];
    size_t off = 0;
    while (off < len) {
        size_t n = len - off > sizeof(chunk) ? sizeof(chunk) : len - off;
        for (size_t i = 0; i < n; i++) {
            chunk[i] = ((const uint8_t *)text)[off + i] ^ mask[(off + i) & 3];
        }
        if (ws_send_raw(chunk, n) != 0) return -1;
        off += n;
    }
    return 0;
}

static int ws_send_json(const char *json, size_t len)
{
    return ws_send_text(json, len);
}

/* -------------------------------------------------------------- messages */
static void push_block(uint8_t role, uint8_t kind, const char *text)
{
    if (!text || !*text) return;
    if (kind == AGY_KIND_TEXT && s_block_count > 0) {
        agy_block_t *last = &s_blocks[s_block_count - 1];
        if (last->role == role && last->kind == AGY_KIND_TEXT) {
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
    if (s_block_count >= AGY_MAX_BLOCKS) {
        memmove(s_blocks, s_blocks + 1,
                sizeof(s_blocks[0]) * (AGY_MAX_BLOCKS - 1));
        s_block_count = AGY_MAX_BLOCKS - 1;
    }
    agy_block_t *b = &s_blocks[s_block_count++];
    b->role = role;
    b->kind = kind;
    strncpy(b->text, text, sizeof(b->text) - 1);
    b->text[sizeof(b->text) - 1] = '\0';
    bump();
}

static void agents_each_cb(const char *obj, size_t len, void *ud)
{
    (void)ud;
    if (s_agent_count >= AGY_MAX_AGENTS) return;
    agy_agent_t *a = &s_agents[s_agent_count];
    memset(a, 0, sizeof(*a));
    if (devos_json_get_str(obj, len, "name", a->name, sizeof(a->name)) != 0) {
        return;
    }
    if (devos_json_get_str(obj, len, "state", a->state, sizeof(a->state)) != 0) {
        strncpy(a->state, "idle", sizeof(a->state) - 1);
    }
    s_agent_count++;
}

static void set_agents(const char *arr, size_t len)
{
    s_agent_count = 0;
    devos_json_array_each(arr, len, agents_each_cb, NULL);
    bump();
}

static void on_message(const char *js, size_t len)
{
    char type[32] = "";
    if (devos_json_get_str(js, len, "type", type, sizeof(type)) != 0) return;
    const char *end = js + len;

    if (strcmp(type, "WELCOME") == 0) {
        devos_json_get_str(js, len, "conversation_id", s_conv, sizeof(s_conv));
        devos_json_get_str(js, len, "model", s_model, sizeof(s_model));
        const char *v = devos_json_find_key(js, end, "subagents");
        if (v && v < end && *v == '[') {
            const char *stop = devos_json_span(v, end);
            if (stop) set_agents(v, (size_t)(stop - v));
        }
        set_status(AGY_UP, "Bridge live");
        return;
    }
    if (strcmp(type, "THINKING") == 0) {
        char t[1024] = "";
        if (devos_json_get_str(js, len, "text", t, sizeof(t)) == 0) {
            if (s_block_count > 0) {
                agy_block_t *last = &s_blocks[s_block_count - 1];
                if (last->role == AGY_ROLE_AGY &&
                    last->kind == AGY_KIND_THINK) {
                    size_t have = strlen(last->text);
                    size_t room = sizeof(last->text) - have - 1;
                    size_t add = strlen(t);
                    if (add > room) add = room;
                    if (have > 0 && room > 0) {
                        last->text[have++] = '\n';
                        room--;
                        if (add > room) add = room;
                    }
                    memcpy(last->text + have, t, add);
                    last->text[have + add] = '\0';
                    bump();
                    return;
                }
            }
            push_block(AGY_ROLE_AGY, AGY_KIND_THINK, t);
        }
        return;
    }
    if (strcmp(type, "TOKEN") == 0) {
        char t[1024] = "";
        if (devos_json_get_str(js, len, "text", t, sizeof(t)) == 0) {
            push_block(AGY_ROLE_AGY, AGY_KIND_TEXT, t);
        }
        return;
    }
    if (strcmp(type, "TOOL") == 0) {
        char name[96] = "";
        char detail[512] = "";
        devos_json_get_str(js, len, "name", name, sizeof(name));
        devos_json_get_str(js, len, "detail", detail, sizeof(detail));
        char buf[768];
        snprintf(buf, sizeof(buf), "%s%s%s", name[0] ? name : "tool",
                 detail[0] ? "\n" : "", detail);
        push_block(AGY_ROLE_AGY, AGY_KIND_TOOL, buf);
        return;
    }
    if (strcmp(type, "DIFF") == 0) {
        char file[128] = "";
        char hunk[2048] = "";
        devos_json_get_str(js, len, "file", file, sizeof(file));
        devos_json_get_str(js, len, "hunk", hunk, sizeof(hunk));
        char buf[2300];
        snprintf(buf, sizeof(buf), "--- %s\n%s", file, hunk);
        size_t have = strlen(s_diff);
        size_t room = sizeof(s_diff) - have - 1;
        size_t add = strlen(buf);
        if (add > room) add = room;
        if (have > 0 && room > 0) {
            s_diff[have++] = '\n';
            room--;
            if (add > room) add = room;
        }
        memcpy(s_diff + have, buf, add);
        s_diff[have + add] = '\0';
        bump();
        return;
    }
    if (strcmp(type, "ARTIFACT") == 0) {
        char name[AGY_NAME_MAX] = "";
        char kind[AGY_STATE_MAX] = "";
        char text[AGY_ARTIFACT_MAX] = "";
        if (devos_json_get_str(js, len, "name", name, sizeof(name)) != 0) {
            return;
        }
        devos_json_get_str(js, len, "kind", kind, sizeof(kind));
        devos_json_get_str(js, len, "text", text, sizeof(text));
        /* ponytail: same-name artifact replaces (latest wins) */
        int slot = -1;
        for (int i = 0; i < s_artifact_count; i++) {
            if (strcmp(s_artifacts[i].name, name) == 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            if (s_artifact_count >= AGY_MAX_ARTIFACTS) {
                memmove(s_artifacts, s_artifacts + 1,
                        sizeof(s_artifacts[0]) * (AGY_MAX_ARTIFACTS - 1));
                s_artifact_count = AGY_MAX_ARTIFACTS - 1;
            }
            slot = s_artifact_count++;
        }
        strncpy(s_artifacts[slot].name, name,
                sizeof(s_artifacts[slot].name) - 1);
        strncpy(s_artifacts[slot].kind, kind,
                sizeof(s_artifacts[slot].kind) - 1);
        strncpy(s_artifacts[slot].text, text,
                sizeof(s_artifacts[slot].text) - 1);
        bump();
        return;
    }
    if (strcmp(type, "PERMISSION") == 0) {
        char pid[AGY_NAME_MAX] = "";
        char text[512] = "";
        if (devos_json_get_str(js, len, "id", pid, sizeof(pid)) != 0) return;
        devos_json_get_str(js, len, "text", text, sizeof(text));
        s_perm.active = true;
        strncpy(s_perm.id, pid, sizeof(s_perm.id) - 1);
        strncpy(s_perm.text, text[0] ? text : pid, sizeof(s_perm.text) - 1);
        bump();
        return;
    }
    if (strcmp(type, "QUESTION") == 0) {
        char qid[AGY_NAME_MAX] = "";
        char prompt[512] = "";
        if (devos_json_get_str(js, len, "id", qid, sizeof(qid)) != 0) return;
        devos_json_get_str(js, len, "prompt", prompt, sizeof(prompt));
        s_q.active = true;
        strncpy(s_q.id, qid, sizeof(s_q.id) - 1);
        strncpy(s_q.prompt, prompt[0] ? prompt : qid, sizeof(s_q.prompt) - 1);
        s_q.choice_count = 0;
        const char *v = devos_json_find_key(js, end, "choices");
        if (v && v < end && *v == '[') {
            const char *stop = devos_json_span(v, end);
            if (stop) {
                /* ponytail: choices are plain strings; walk manually */
                const char *p = v + 1;
                while (p < stop && s_q.choice_count < 4) {
                    while (p < stop &&
                           (*p == ' ' || *p == '\t' || *p == '\n' ||
                            *p == '\r' || *p == ',')) {
                        p++;
                    }
                    if (p >= stop || *p != '"') break;
                    char tmp[128];
                    const char *np =
                        devos_json_parse_str(p, stop, tmp, sizeof(tmp));
                    if (!np) break;
                    strncpy(s_q.choices[s_q.choice_count], tmp,
                            sizeof(s_q.choices[0]) - 1);
                    s_q.choice_count++;
                    p = np;
                }
            }
        }
        bump();
        return;
    }
    if (strcmp(type, "SUBAGENTS") == 0) {
        const char *v = devos_json_find_key(js, end, "agents");
        if (v && v < end && *v == '[') {
            const char *stop = devos_json_span(v, end);
            if (stop) set_agents(v, (size_t)(stop - v));
        }
        return;
    }
    if (strcmp(type, "STATUS") == 0) {
        char st[24] = "";
        if (devos_json_get_str(js, len, "state", st, sizeof(st)) == 0) {
            bool busy = strcmp(st, "busy") == 0;
            if (s_busy != busy) {
                s_busy = busy;
                bump();
            }
        }
        return;
    }
    if (strcmp(type, "ERROR") == 0) {
        char m[256] = "";
        if (devos_json_get_str(js, len, "message", m, sizeof(m)) == 0 && m[0]) {
            push_block(AGY_ROLE_AGY, AGY_KIND_TEXT, m);
        }
        return;
    }
    /* PONG and unknown types: ignored */
}

/* ------------------------------------------------------------ link poll */
static void link_down(const char *text)
{
    if (s_fd >= 0) {
        devos_net_socket_close(s_fd);
        s_fd = -1;
    }
    s_sent_upgrade = false;
    s_handshook = false;
    s_rx_len = 0;
    s_frag_len = 0;
    s_frag_op = -1;
    /* ponytail: a dead link ends the turn too (no orphan busy badge) */
    s_busy = false;
    set_status(AGY_DOWN, text ? text : "Offline");
}

static int send_hello(void)
{
    char esc[AGY_TOKEN_MAX * 2];
    devos_json_escape(s_cfg.token, esc, sizeof(esc));
    char body[512];
    snprintf(body, sizeof(body),
             "{\"type\":\"HELLO\",\"token\":\"%s\",\"client\":\"devos/0.1\"}",
             esc);
    return ws_send_json(body, strlen(body));
}

/* handle one complete server frame payload */
static void on_frame(int opcode, const uint8_t *pl, size_t len)
{
    if (opcode == 0x9) { /* PING -> empty masked PONG */
        if (s_fd >= 0) {
            uint8_t mask[4] = {(uint8_t)rand(), (uint8_t)rand(),
                               (uint8_t)rand(), (uint8_t)rand()};
            uint8_t hdr[6];
            hdr[0] = 0x8A; /* FIN + pong */
            hdr[1] = 0x80; /* masked, len 0 */
            memcpy(hdr + 2, mask, 4);
            if (devos_net_socket_send_all(s_fd, hdr, sizeof(hdr)) != 0) {
                link_down("Link lost");
            }
        }
        return;
    }
    if (opcode == 0xA) return; /* PONG */
    if (opcode == 0x8) {       /* CLOSE */
        link_down("Bridge closed");
        return;
    }
    if (opcode != 0x0 && opcode != 0x1 && opcode != 0x2) return;
    /* ponytail: accumulate continuations (opcode 0) into one message */
    if (opcode != 0x0) {
        s_frag_len = 0;
        s_frag_op = opcode;
    }
    if (s_frag_op != 0x1 && s_frag_op != 0x2) {
        s_frag_len = 0;
        s_frag_op = -1;
        return;
    }
    size_t room = sizeof(s_frag) - s_frag_len - 1;
    size_t take = len > room ? room : len;
    memcpy(s_frag + s_frag_len, pl, take);
    s_frag_len += take;
    /* caller (frame_pump) completes FIN-terminated messages only.
     * (Single-frame messages dominate; see frame pump below.) */
}

static void frame_pump(void)
{
    /* parse complete frames from s_rx; leaves partial tail buffered */
    size_t pos = 0;
    while (pos + 2 <= s_rx_len) {
        const uint8_t *h = (const uint8_t *)s_rx + pos;
        bool fin = (h[0] & 0x80) != 0;
        int op = h[0] & 0x0F;
        bool masked = (h[1] & 0x80) != 0;
        uint64_t plen = h[1] & 0x7F;
        size_t hlen = 2;
        if (plen == 126) {
            if (pos + 4 > s_rx_len) break;
            plen = ((uint64_t)(uint8_t)s_rx[pos + 2] << 8) |
                   (uint64_t)(uint8_t)s_rx[pos + 3];
            hlen = 4;
        } else if (plen == 127) {
            if (pos + 10 > s_rx_len) break;
            plen = 0;
            for (int i = 0; i < 8; i++) {
                plen = (plen << 8) | (uint64_t)(uint8_t)s_rx[pos + 2 + i];
            }
            hlen = 10;
            if (plen > WS_RX_MAX) {
                link_down("Oversize frame");
                return;
            }
        }
        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (pos + hlen + 4 > s_rx_len) break;
            memcpy(mask, s_rx + pos + hlen, 4);
            hlen += 4;
        }
        if (pos + hlen + plen > s_rx_len) break; /* incomplete payload */
        /* unmask in place (server frames are normally unmasked) */
        if (masked) {
            for (uint64_t i = 0; i < plen; i++) {
                s_rx[pos + hlen + i] ^= mask[i & 3];
            }
        }
        on_frame(op, (const uint8_t *)s_rx + pos + hlen, (size_t)plen);
        if (s_fd < 0) {
            /* on_frame dropped the link; discard everything */
            s_rx_len = 0;
            return;
        }
        if (fin && (op == 0x1 || op == 0x2 ||
                    (op == 0x0 && s_frag_op != -1))) {
            s_frag[s_frag_len] = '\0';
            on_message(s_frag, s_frag_len);
            s_frag_len = 0;
            s_frag_op = -1;
        }
        pos += hlen + (size_t)plen;
    }
    if (pos > 0) {
        memmove(s_rx, s_rx + pos, s_rx_len - pos);
        s_rx_len -= pos;
    }
}

void agy_client_init(void)
{
    config_load();
#ifndef ESP_PLATFORM
    srand((unsigned)time(NULL));
#else
    /* ponytail: key/mask material isn't secret; seed anyway, not fixed */
    srand(0x9E3779B9u);
#endif
    s_want = true;
    set_status(AGY_DOWN, "Offline");
}

void agy_client_poll(void)
{
    if (!s_want) {
        if (s_fd >= 0) link_down("Offline");
        return;
    }

    if (s_fd < 0) {
        if (++s_retry < WS_RETRY_TICKS) return;
        s_retry = 0;
        s_fd = devos_net_socket_connect_start(s_cfg.host, s_cfg.port);
        s_ticks = 0;
        s_hdr_len = 0;
        if (s_fd < 0) {
            char t[128];
            snprintf(t, sizeof(t), "No route to %s:%d", s_cfg.host,
                     s_cfg.port);
            set_status(AGY_CONNECTING, t);
            return;
        }
        char t[128];
        snprintf(t, sizeof(t), "Connecting %s:%d...", s_cfg.host, s_cfg.port);
        set_status(AGY_CONNECTING, t);
        return;
    }

    if (s_status == AGY_CONNECTING && !s_handshook) {
        if (!s_sent_upgrade) {
            int r = devos_net_socket_connect_wait(s_fd, 0);
            if (r > 0) {
                if (++s_ticks > WS_TIMEOUT_TICKS) link_down("Connect timeout");
                return;
            }
            if (r < 0) {
                char t[128];
                snprintf(t, sizeof(t), "Refused by %s:%d", s_cfg.host, s_cfg.port);
                link_down(t);
                return;
            }
            /* TCP up: send the WS upgrade */
            uint8_t key_raw[16];
            for (int i = 0; i < 16; i++) key_raw[i] = (uint8_t)rand();
            char key[32];
            b64_encode(key_raw, sizeof(key_raw), key);
            char req[512];
            int hlen = snprintf(req, sizeof(req),
                                "GET /ws HTTP/1.1\r\nHost: %s:%d\r\n"
                                "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Key: %s\r\n"
                                "Sec-WebSocket-Version: 13\r\n\r\n",
                                s_cfg.host, s_cfg.port, key);
            if (hlen <= 0 ||
                devos_net_socket_send_all(s_fd, req, (size_t)hlen) != 0) {
                link_down("Upgrade failed");
                return;
            }
            s_sent_upgrade = true;
            s_ticks = 0;
        }
        /* fall through to header accumulation */
    }

    /* bounded read; headers first, then frames */
    char chunk[1024];
    for (int it = 0; it < 4; it++) {
        int n = devos_net_socket_recv(s_fd, chunk, sizeof(chunk) - 1, 1);
        if (n <= 0) {
            if (n == 0) link_down("Bridge closed");
            break;
        }
        if (!s_handshook) {
            if (s_hdr_len + (size_t)n >= sizeof(s_hdr)) {
                link_down("Bad headers");
                return;
            }
            memcpy(s_hdr + s_hdr_len, chunk, (size_t)n);
            s_hdr_len += (size_t)n;
            s_hdr[s_hdr_len] = '\0';
            char *eoh = strstr(s_hdr, "\r\n\r\n");
            if (!eoh) continue;
            int code = 0;
            if (sscanf(s_hdr, "HTTP/%*d.%*d %d", &code) != 1 &&
                sscanf(s_hdr, "HTTP/%*d %d", &code) != 1) {
                code = 0;
            }
            if (code != 101) {
                char t[64];
                snprintf(t, sizeof(t), "Bridge HTTP %d", code);
                link_down(t);
                return;
            }
            s_handshook = true;
            set_status(AGY_UP, "Bridge live");
            send_hello();
            size_t hused = (size_t)(eoh - s_hdr) + 4;
            if (s_hdr_len > hused) {
                size_t extra = s_hdr_len - hused;
                if (extra > sizeof(s_rx) - s_rx_len - 1) {
                    extra = sizeof(s_rx) - s_rx_len - 1;
                }
                memcpy(s_rx + s_rx_len, s_hdr + hused, extra);
                s_rx_len += extra;
            }
        } else {
            if (s_rx_len + (size_t)n >= sizeof(s_rx)) {
                /* ponytail: drop oldest half rather than stall on floods */
                size_t drop = s_rx_len / 2;
                memmove(s_rx, s_rx + drop, s_rx_len - drop);
                s_rx_len -= drop;
            }
            size_t take = (size_t)n;
            if (s_rx_len + take >= sizeof(s_rx)) {
                take = sizeof(s_rx) - s_rx_len - 1;
            }
            memcpy(s_rx + s_rx_len, chunk, take);
            s_rx_len += take;
        }
    }
    if (s_fd < 0) return;
    if (!s_handshook) {
        /* ponytail: bound slowloris-style header dribbles */
        if (++s_ticks > WS_TIMEOUT_TICKS + 50) link_down("Header timeout");
        return;
    }
    frame_pump();
}

int agy_client_reconnect(void)
{
    link_down("Reconnecting...");
    s_retry = WS_RETRY_TICKS;
    s_want = true;
    return 0;
}

agy_status_t agy_client_status(void) { return s_status; }

const char *agy_client_status_text(void) { return s_status_text; }

uint32_t agy_client_generation(void) { return s_gen; }

void agy_client_get_config(char *host, size_t host_len, int *port,
                           char *token, size_t token_len)
{
    if (host && host_len) {
        strncpy(host, s_cfg.host, host_len - 1);
        host[host_len - 1] = '\0';
    }
    if (port) *port = s_cfg.port;
    if (token && token_len) {
        strncpy(token, s_cfg.token, token_len - 1);
        token[token_len - 1] = '\0';
    }
}

int agy_client_set_server(const char *host, int port)
{
    if (!host || !*host || port <= 0 || port > 65535) return -1;
    strncpy(s_cfg.host, host, sizeof(s_cfg.host) - 1);
    s_cfg.host[sizeof(s_cfg.host) - 1] = '\0';
    s_cfg.port = port;
    config_save();
    link_down("Reconnecting...");
    s_retry = WS_RETRY_TICKS;
    bump();
    return 0;
}

int agy_client_set_token(const char *token)
{
    if (!token) return -1;
    strncpy(s_cfg.token, token, sizeof(s_cfg.token) - 1);
    s_cfg.token[sizeof(s_cfg.token) - 1] = '\0';
    config_save();
    link_down("Reconnecting...");
    s_retry = WS_RETRY_TICKS;
    bump();
    return 0;
}

static int send_prompt_obj(const char *text, const char *command)
{
    if (s_status != AGY_UP) return -1;
    static EXT_RAM_BSS_ATTR char body[WS_TX_MAX];
    char esc[WS_TX_MAX - 128];
    devos_json_escape(text ? text : "", esc, sizeof(esc));
    /* ponytail: command fragment carries its own leading comma */
    char cmdfrag[64] = "";
    if (command && *command) {
        char cesc[48];
        devos_json_escape(command, cesc, sizeof(cesc));
        snprintf(cmdfrag, sizeof(cmdfrag), ",\"command\":\"%s\"", cesc);
    }
    snprintf(body, sizeof(body), "{\"type\":\"PROMPT\",\"text\":\"%s\"%s}",
             esc, cmdfrag);
    return ws_send_json(body, strlen(body));
}

int agy_client_send(const char *text, const char *command)
{
    if (!text || !*text) return -1;
    char clipped[AGY_BLOCK_MAX];
    strncpy(clipped, text, sizeof(clipped) - 1);
    clipped[sizeof(clipped) - 1] = '\0';
    push_block(AGY_ROLE_USER, AGY_KIND_TEXT, clipped);
    if (send_prompt_obj(text, command) != 0) {
        push_block(AGY_ROLE_AGY, AGY_KIND_TEXT,
                   "(send failed - bridge unreachable)");
        return -1;
    }
    s_busy = true;
    bump();
    return 0;
}

int agy_client_new(void)
{
    s_block_count = 0;
    s_artifact_count = 0;
    s_diff[0] = '\0';
    s_conv[0] = '\0';
    s_model[0] = '\0';
    s_perm.active = false;
    s_q.active = false;
    s_busy = false;
    bump();
    return 0;
}

const char *agy_client_conversation_id(void) { return s_conv; }

const char *agy_client_model(void) { return s_model; }

bool agy_client_busy(void) { return s_busy; }

int agy_client_agent_count(void) { return s_agent_count; }

const agy_agent_t *agy_client_agent(int idx)
{
    if (idx < 0 || idx >= s_agent_count) return NULL;
    return &s_agents[idx];
}

int agy_client_artifact_count(void) { return s_artifact_count; }

const agy_artifact_t *agy_client_artifact(int idx)
{
    if (idx < 0 || idx >= s_artifact_count) return NULL;
    return &s_artifacts[idx];
}

int agy_client_block_count(void) { return s_block_count; }

const agy_block_t *agy_client_block(int idx)
{
    if (idx < 0 || idx >= s_block_count) return NULL;
    return &s_blocks[idx];
}

const char *agy_client_diff_text(void) { return s_diff; }

bool agy_client_permission_pending(agy_permission_t *out)
{
    if (!s_perm.active) return false;
    if (out) *out = s_perm;
    return true;
}

int agy_client_answer_permission(bool allow, bool always)
{
    if (!s_perm.active) return -1;
    char body[256];
    snprintf(body, sizeof(body),
             "{\"type\":\"PERMISSION_REPLY\",\"id\":\"%s\","
             "\"allow\":%s,\"always\":%s}",
             s_perm.id, allow ? "true" : "false",
             always ? "true" : "false");
    int rc = ws_send_json(body, strlen(body));
    s_perm.active = false;
    bump();
    return rc;
}

bool agy_client_question_pending(agy_question_t *out)
{
    if (!s_q.active) return false;
    if (out) *out = s_q;
    return true;
}

int agy_client_answer_question(int choice)
{
    if (!s_q.active) return -1;
    if (choice < 0) choice = 0;
    if (choice > 3) choice = 3;
    char body[256];
    snprintf(body, sizeof(body),
             "{\"type\":\"QUESTION_REPLY\",\"id\":\"%s\",\"choice\":%d}",
             s_q.id, choice);
    int rc = ws_send_json(body, strlen(body));
    s_q.active = false;
    bump();
    return rc;
}
