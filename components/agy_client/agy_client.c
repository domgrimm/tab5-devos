/* agy_client: Antigravity bridge WebSocket engine. See agy_client.h. */
#include "agy_client.h"
#include "devos_json.h"
#include "devos_net.h"
#include "devos_config.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_random.h"
#endif

#ifndef ESP_PLATFORM
#define AGY_NVS_FILE TAB5_SD_MOUNT_POINT "/.devos/agy_nvs.json"
#endif

#define WS_TIMEOUT_TICKS 100      /* 10 s handshake/connect budget */
#define WS_RETRY_TICKS 30         /* 3 s between attempts */
#define WS_IDLE_TICKS 450         /* 45 s of silence: the bridge pings every 20 s */
#define WS_HDR_MAX 1024
#define AGY_MSG_MAX (96 * 1024)   /* bigger messages are skipped whole */
#define WS_TX_MAX (AGY_PROMPT_MAX * 2 + 256)
#define RX_CHUNK 2048
#define RX_CHUNKS_PER_POLL 24     /* ~48 KB per tick: history replays stay quick */

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
static uint32_t s_rev = 0;
static agy_permission_t s_perm;
static EXT_RAM_BSS_ATTR char s_perm_preview[AGY_PREVIEW_MAX];
static agy_question_t s_q;
static char s_conv[AGY_NAME_MAX] = "";
static char s_model[AGY_NAME_MAX] = "";
static char s_workspace[AGY_PATH_MAX] = "";
static EXT_RAM_BSS_ATTR char s_diff[AGY_DIFF_MAX];
static size_t s_diff_len = 0;
static uint32_t s_diff_rev = 0;
static int s_usage[3] = {0, 0, 0};
static EXT_RAM_BSS_ATTR agy_conv_t s_convs[AGY_MAX_CONVS];
static int s_conv_count = 0;
static char s_instance[AGY_NAME_MAX] = "";
static EXT_RAM_BSS_ATTR char s_prompts[AGY_MAX_PROMPTS][AGY_RECALL_MAX];
static int s_prompt_count = 0;
static bool s_busy = false;
static agy_status_t s_status = AGY_DOWN;
static char s_status_text[128] = "Offline";
static uint32_t s_gen = 0;

/* scratch for pulling long strings out of a message */
static EXT_RAM_BSS_ATTR char s_str[AGY_ARTIFACT_MAX];

/* Link state */
static int s_fd = -1;
static int s_ticks = 0;
static int s_retry = 0;
static int s_idle = 0;
static bool s_want = false;
static bool s_sent_upgrade = false;
static bool s_handshook = false;
static char s_hdr[WS_HDR_MAX];
static size_t s_hdr_len = 0;

/* Streaming frame parser */
static struct {
    uint8_t hdr[14];
    int hdr_len;
    int hdr_need;
    bool in_payload;
    int op;
    bool fin;
    bool masked;
    uint8_t mask[4];
    uint64_t remain;
    uint64_t pos;               /* payload offset, for unmasking */
    uint8_t ctrl[125];
    size_t ctrl_len;
    int msg_op;                 /* -1: no message in progress */
    size_t msg_len;
    bool msg_skip;              /* message too big: drop it */
} rx;
static EXT_RAM_BSS_ATTR char s_msg[AGY_MSG_MAX + 1];

static void bump(void) { s_gen++; }

static uint8_t rnd8(void)
{
#ifdef ESP_PLATFORM
    return (uint8_t)esp_random();
#else
    return (uint8_t)rand();
#endif
}

static void set_status(agy_status_t st, const char *text)
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

/* Cut a string back to a whole UTF-8 sequence (after a byte-limit copy). */
static void utf8_trim(char *s)
{
    size_t i = strlen(s);
    int cont = 0;
    while (i > 0 && cont < 3 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) {
        i--;
        cont++;
    }
    if (cont == 0) return;
    if (i == 0) {
        s[0] = '\0';
        return;
    }
    unsigned char lead = (unsigned char)s[i - 1];
    if (lead < 0xC0) {
        s[i] = '\0';                  /* stray continuation bytes */
        return;
    }
    int need = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : 1;
    if (cont < need) s[i - 1] = '\0'; /* sequence cut short */
}

/* Bounded copy that never splits a UTF-8 character. */
static void copy_text(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    utf8_trim(dst);
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
                snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", val);
            } else if (sscanf(buf, " \"port\": %d", &ival) == 1 && ival > 0 &&
                       ival < 65536) {
                s_cfg.port = ival;
            } else if (sscanf(buf, " \"token\": \"%127[^\"]\"", val) == 1) {
                snprintf(s_cfg.token, sizeof(s_cfg.token), "%s", val);
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
static void link_down(const char *text);

/* One masked client frame (FIN set). */
static int ws_send_frame(int op, const void *data, size_t len)
{
    if (s_fd < 0) return -1;
    uint8_t hdr[14];
    size_t hlen = 2;
    hdr[0] = (uint8_t)(0x80 | (op & 0x0F));
    if (len < 126) {
        hdr[1] = (uint8_t)(0x80 | len);
    } else if (len <= 0xFFFF) {
        hdr[1] = 0x80 | 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)len;
        hlen = 4;
    } else {
        hdr[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) hdr[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
        hlen = 10;
    }
    uint8_t *mask = hdr + hlen;
    for (int i = 0; i < 4; i++) mask[i] = rnd8();
    hlen += 4;
    if (devos_net_socket_send_all(s_fd, hdr, hlen) != 0) return -1;
    /* mask in 1 KB chunks instead of a heap copy */
    static uint8_t chunk[1024];
    size_t off = 0;
    while (off < len) {
        size_t n = len - off > sizeof(chunk) ? sizeof(chunk) : len - off;
        for (size_t i = 0; i < n; i++) {
            chunk[i] = ((const uint8_t *)data)[off + i] ^ mask[(off + i) & 3];
        }
        if (devos_net_socket_send_all(s_fd, chunk, n) != 0) return -1;
        off += n;
    }
    return 0;
}

static int ws_send_json(const char *json)
{
    if (ws_send_frame(0x1, json, strlen(json)) != 0) {
        if (s_fd >= 0) link_down("Link lost");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------ view model */
static agy_block_t *new_block(uint8_t role, uint8_t kind)
{
    if (s_block_count >= AGY_MAX_BLOCKS) {
        memmove(s_blocks, s_blocks + 1, sizeof(s_blocks[0]) * (AGY_MAX_BLOCKS - 1));
        s_block_count = AGY_MAX_BLOCKS - 1;
    }
    agy_block_t *b = &s_blocks[s_block_count++];
    memset(b, 0, offsetof(agy_block_t, text));
    b->text[0] = '\0';
    b->role = role;
    b->kind = kind;
    b->rev = ++s_rev;
    return b;
}

/* Streamed text: extend the last block of the same kind, else start one
 * (long text spills into further blocks, split on a UTF-8 boundary). */
static void append_text(uint8_t role, uint8_t kind, const char *text)
{
    if (!text || !*text) return;
    while (*text) {
        size_t add = strlen(text);
        agy_block_t *last = s_block_count > 0 ? &s_blocks[s_block_count - 1] : NULL;
        agy_block_t *b = (last && last->role == role && last->kind == kind &&
                          strlen(last->text) + add < sizeof(last->text))
                         ? last : new_block(role, kind);
        size_t have = strlen(b->text);
        size_t room = sizeof(b->text) - have - 1;
        if (add > room) {
            add = room;
            while (add > 0 && ((unsigned char)text[add] & 0xC0) == 0x80) add--;
        }
        if (add == 0) break;
        memcpy(b->text + have, text, add);
        b->text[have + add] = '\0';
        b->rev = ++s_rev;
        text += add;
    }
    bump();
}

static void clear_view(void)
{
    s_block_count = 0;
    s_artifact_count = 0;
    s_diff[0] = '\0';
    s_diff_len = 0;
    s_diff_rev++;
    s_perm.active = false;
    s_q.active = false;
    memset(s_usage, 0, sizeof(s_usage));
    bump();
}

static void diff_append(const char *file, const char *hunk)
{
    char head[2 * AGY_PATH_MAX + 32] = "";
    if (strncmp(hunk, "---", 3) != 0) {
        snprintf(head, sizeof(head), "--- a/%s\n+++ b/%s\n", file, file);
    }
    size_t hl = strlen(head), bl = strlen(hunk);
    size_t need = hl + bl + 2;   /* hunk + '\n' separator + NUL */
    if (need > sizeof(s_diff)) return;
    if (s_diff_len + need > sizeof(s_diff)) {
        /* drop the oldest edits, whole files at a time */
        size_t cut = s_diff_len + need - sizeof(s_diff);
        const char *next = strstr(s_diff + cut, "\n--- ");
        cut = next ? (size_t)(next + 1 - s_diff) : s_diff_len;
        memmove(s_diff, s_diff + cut, s_diff_len - cut + 1);
        s_diff_len -= cut;
    }
    memcpy(s_diff + s_diff_len, head, hl);
    s_diff_len += hl;
    memcpy(s_diff + s_diff_len, hunk, bl);
    s_diff_len += bl;
    if (s_diff_len == 0 || s_diff[s_diff_len - 1] != '\n') s_diff[s_diff_len++] = '\n';
    s_diff[s_diff_len] = '\0';
    s_diff_rev++;
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
        snprintf(a->state, sizeof(a->state), "%s", "idle");
    }
    s_agent_count++;
}

static void set_agents(const char *js, size_t len, const char *key)
{
    const char *end = js + len;
    const char *v = devos_json_find_key(js, end, key);
    if (!v || v >= end || *v != '[') return;
    const char *stop = devos_json_span(v, end);
    if (!stop) return;
    s_agent_count = 0;
    devos_json_array_each(v, (size_t)(stop - v), agents_each_cb, NULL);
    bump();
}

static void on_tool(const char *js, size_t len)
{
    char id[40] = "", name[48] = "", st[16] = "";
    devos_json_get_str(js, len, "id", id, sizeof(id));
    devos_json_get_str(js, len, "name", name, sizeof(name));
    devos_json_get_str(js, len, "state", st, sizeof(st));
    s_str[0] = '\0';
    devos_json_get_str(js, len, "detail", s_str, 2048);
    utf8_trim(s_str);
    agy_block_t *b = NULL;
    for (int i = s_block_count - 1; id[0] && i >= 0; i--) {
        if (s_blocks[i].kind == AGY_KIND_TOOL && strcmp(s_blocks[i].id, id) == 0) {
            b = &s_blocks[i];
            break;
        }
    }
    if (!b) {
        b = new_block(AGY_ROLE_AGY, AGY_KIND_TOOL);
        snprintf(b->id, sizeof(b->id), "%s", id);
    }
    snprintf(b->name, sizeof(b->name), "%s", name[0] ? name : "tool");
    b->state = strcmp(st, "error") == 0 ? AGY_TOOL_ERROR
             : strcmp(st, "running") == 0 ? AGY_TOOL_RUNNING : AGY_TOOL_DONE;
    copy_text(b->text, sizeof(b->text), s_str);
    b->rev = ++s_rev;
    bump();
}

static void conv_each_cb(const char *obj, size_t len, void *ud)
{
    (void)ud;
    if (s_conv_count >= AGY_MAX_CONVS) return;
    agy_conv_t *c = &s_convs[s_conv_count];
    memset(c, 0, sizeof(*c));
    if (devos_json_get_str(obj, len, "id", c->id, sizeof(c->id)) != 0 || !c->id[0]) return;
    devos_json_get_str(obj, len, "title", c->title, sizeof(c->title));
    utf8_trim(c->title);
    devos_json_get_str(obj, len, "ws", c->ws, sizeof(c->ws));
    utf8_trim(c->ws);
    devos_json_get_str(obj, len, "age", c->age, sizeof(c->age));
    devos_json_get_int(obj, len, "steps", &c->steps);
    const char *b = devos_json_find_key(obj, obj + len, "busy");
    c->busy = b && b < obj + len && *b == 't';
    s_conv_count++;
}

static void prompt_add(const char *text)
{
    if (!text || !*text) return;
    for (int i = 0; i < s_prompt_count; i++) {
        if (strncmp(s_prompts[i], text, AGY_RECALL_MAX - 1) == 0) {
            /* already known: move it to the newest slot */
            memmove(s_prompts[i], s_prompts[i + 1], (size_t)(s_prompt_count - 1 - i) * AGY_RECALL_MAX);
            s_prompt_count--;
            break;
        }
    }
    if (s_prompt_count >= AGY_MAX_PROMPTS) {
        memmove(s_prompts[0], s_prompts[1], (size_t)(AGY_MAX_PROMPTS - 1) * AGY_RECALL_MAX);
        s_prompt_count = AGY_MAX_PROMPTS - 1;
    }
    copy_text(s_prompts[s_prompt_count++], AGY_RECALL_MAX, text);
}

static void on_prompts(const char *js, size_t len)
{
    const char *end = js + len;
    const char *v = devos_json_find_key(js, end, "items");
    if (!v || v >= end || *v != '[') return;
    const char *stop = devos_json_span(v, end);
    if (!stop) return;
    s_prompt_count = 0;
    for (const char *p = v + 1; p < stop;) {
        while (p < stop && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')) p++;
        if (p >= stop || *p != '"') break;
        char tmp[AGY_RECALL_MAX];
        p = devos_json_parse_str(p, stop, tmp, sizeof(tmp));
        if (!p) break;
        utf8_trim(tmp);
        prompt_add(tmp);
    }
    bump();
}

static void on_question(const char *js, size_t len)
{
    const char *end = js + len;
    char qid[AGY_NAME_MAX] = "";
    if (devos_json_get_str(js, len, "id", qid, sizeof(qid)) != 0) return;
    memset(&s_q, 0, sizeof(s_q));
    devos_json_get_str(js, len, "prompt", s_q.prompt, sizeof(s_q.prompt));
    utf8_trim(s_q.prompt);
    snprintf(s_q.id, sizeof(s_q.id), "%s", qid);
    const char *v = devos_json_find_key(js, end, "choices");
    if (v && v < end && *v == '[') {
        const char *stop = devos_json_span(v, end);
        const char *p = stop ? v + 1 : stop;
        /* choices are plain strings */
        while (p && p < stop && s_q.choice_count < 4) {
            while (p < stop && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')) p++;
            if (p >= stop || *p != '"') break;
            char *out = s_q.choices[s_q.choice_count];
            p = devos_json_parse_str(p, stop, out, sizeof(s_q.choices[0]));
            if (!p) break;
            utf8_trim(out);
            s_q.choice_count++;
        }
    }
    s_q.active = true;
    bump();
}

static void on_message(const char *js, size_t len)
{
    char type[32] = "";
    if (devos_json_get_str(js, len, "type", type, sizeof(type)) != 0) return;

    if (strcmp(type, "TOKEN") == 0 || strcmp(type, "THINKING") == 0) {
        s_str[0] = '\0';
        if (devos_json_get_str(js, len, "text", s_str, sizeof(s_str)) == 0) {
            utf8_trim(s_str);
            append_text(AGY_ROLE_AGY, type[1] == 'O' ? AGY_KIND_TEXT : AGY_KIND_THINK, s_str);
        }
    } else if (strcmp(type, "USER") == 0) {
        s_str[0] = '\0';
        if (devos_json_get_str(js, len, "text", s_str, AGY_BLOCK_MAX) == 0 && s_str[0]) {
            utf8_trim(s_str);
            agy_block_t *b = new_block(AGY_ROLE_USER, AGY_KIND_TEXT);
            copy_text(b->text, sizeof(b->text), s_str);
            bump();
        }
    } else if (strcmp(type, "TOOL") == 0) {
        on_tool(js, len);
    } else if (strcmp(type, "RESET") == 0) {
        clear_view();
    } else if (strcmp(type, "WELCOME") == 0) {
        devos_json_get_str(js, len, "conversation_id", s_conv, sizeof(s_conv));
        devos_json_get_str(js, len, "model", s_model, sizeof(s_model));
        devos_json_get_str(js, len, "workspace", s_workspace, sizeof(s_workspace));
        set_agents(js, len, "subagents");
        set_status(AGY_UP, "Bridge live");
        bump();
    } else if (strcmp(type, "DIFF") == 0) {
        char file[AGY_PATH_MAX] = "";
        devos_json_get_str(js, len, "file", file, sizeof(file));
        s_str[0] = '\0';
        if (devos_json_get_str(js, len, "hunk", s_str, sizeof(s_str)) == 0 && s_str[0]) {
            diff_append(file, s_str);
        }
    } else if (strcmp(type, "ARTIFACT") == 0) {
        char name[AGY_NAME_MAX] = "";
        if (devos_json_get_str(js, len, "name", name, sizeof(name)) != 0) return;
        /* same-name artifact replaces (latest wins) */
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
        agy_artifact_t *a = &s_artifacts[slot];
        snprintf(a->name, sizeof(a->name), "%s", name);
        a->kind[0] = '\0';
        devos_json_get_str(js, len, "kind", a->kind, sizeof(a->kind));
        a->text[0] = '\0';
        devos_json_get_str(js, len, "text", a->text, sizeof(a->text));
        utf8_trim(a->text);
        bump();
    } else if (strcmp(type, "PERMISSION") == 0) {
        char pid[AGY_NAME_MAX] = "";
        if (devos_json_get_str(js, len, "id", pid, sizeof(pid)) != 0) return;
        s_perm.text[0] = '\0';
        devos_json_get_str(js, len, "text", s_perm.text, sizeof(s_perm.text));
        utf8_trim(s_perm.text);
        snprintf(s_perm.id, sizeof(s_perm.id), "%s", pid);
        s_perm_preview[0] = '\0';
        devos_json_get_str(js, len, "preview", s_perm_preview, sizeof(s_perm_preview));
        utf8_trim(s_perm_preview);
        if (!s_perm.text[0]) snprintf(s_perm.text, sizeof(s_perm.text), "%s", pid);
        s_perm.active = true;
        bump();
    } else if (strcmp(type, "QUESTION") == 0) {
        on_question(js, len);
    } else if (strcmp(type, "CONVERSATIONS") == 0) {
        devos_json_get_str(js, len, "instance", s_instance, sizeof(s_instance));
        const char *end = js + len;
        const char *v = devos_json_find_key(js, end, "items");
        const char *stop = (v && v < end && *v == '[') ? devos_json_span(v, end) : NULL;
        if (stop) {
            s_conv_count = 0;
            devos_json_array_each(v, (size_t)(stop - v), conv_each_cb, NULL);
        }
        bump();
    } else if (strcmp(type, "PROMPTS") == 0) {
        on_prompts(js, len);
    } else if (strcmp(type, "RESOLVED") == 0) {
        /* answered elsewhere, timed out or the turn was stopped */
        char rid[AGY_NAME_MAX] = "";
        devos_json_get_str(js, len, "id", rid, sizeof(rid));
        if (s_perm.active && strcmp(s_perm.id, rid) == 0) s_perm.active = false;
        if (s_q.active && strcmp(s_q.id, rid) == 0) s_q.active = false;
        bump();
    } else if (strcmp(type, "SUBAGENTS") == 0) {
        set_agents(js, len, "agents");
    } else if (strcmp(type, "STATUS") == 0) {
        char st[24] = "";
        if (devos_json_get_str(js, len, "state", st, sizeof(st)) == 0) {
            bool busy = strcmp(st, "busy") == 0;
            if (!busy) {
                /* the turn is over: nothing is still running */
                for (int i = 0; i < s_block_count; i++) {
                    if (s_blocks[i].kind == AGY_KIND_TOOL && s_blocks[i].state == AGY_TOOL_RUNNING) {
                        s_blocks[i].state = AGY_TOOL_DONE;
                        s_blocks[i].rev = ++s_rev;
                    }
                }
            }
            s_busy = busy;
            bump();
        }
    } else if (strcmp(type, "USAGE") == 0) {
        devos_json_get_int(js, len, "input", &s_usage[0]);
        devos_json_get_int(js, len, "output", &s_usage[1]);
        devos_json_get_int(js, len, "total", &s_usage[2]);
        bump();
    } else if (strcmp(type, "ERROR") == 0) {
        char m[300] = "";
        if (devos_json_get_str(js, len, "message", m, sizeof(m)) != 0 || !m[0]) return;
        utf8_trim(m);
        if (strstr(m, "Wrong bridge token")) {
            /* the bridge hangs up next; don't hammer it with the same token */
            s_want = false;
            link_down("Wrong bridge token: tap the session card to fix it");
            return;
        }
        agy_block_t *b = new_block(AGY_ROLE_AGY, AGY_KIND_ERROR);
        snprintf(b->text, sizeof(b->text), "%s", m);
        bump();
    }
    /* PONG and unknown types: ignored */
}

/* ------------------------------------------------------------ link poll */
static void rx_reset(void)
{
    memset(&rx, 0, sizeof(rx));
    rx.msg_op = -1;
}

static void link_down(const char *text)
{
    if (s_fd >= 0) {
        devos_net_socket_close(s_fd);
        s_fd = -1;
    }
    s_sent_upgrade = false;
    s_handshook = false;
    s_hdr_len = 0;
    rx_reset();
    /* a dead link ends the turn; pending asks are re-sent on reconnect */
    s_busy = false;
    s_perm.active = false;
    s_q.active = false;
    set_status(AGY_DOWN, text ? text : "Offline");
    bump();
}

static int send_hello(void)
{
    char esc[AGY_TOKEN_MAX * 2];
    devos_json_escape(s_cfg.token, esc, sizeof(esc));
    char body[512];
    snprintf(body, sizeof(body),
             "{\"type\":\"HELLO\",\"token\":\"%s\",\"client\":\"devos/0.2\"}", esc);
    return ws_send_json(body);
}

static void frame_done(void)
{
    rx.in_payload = false;
    rx.hdr_len = 0;
    if (rx.op >= 0x8) {
        if (rx.op == 0x9) {         /* PING: PONG with the same payload */
            if (ws_send_frame(0xA, rx.ctrl, rx.ctrl_len) != 0) link_down("Link lost");
        } else if (rx.op == 0x8) {  /* CLOSE: echo the status code, then hang up */
            ws_send_frame(0x8, rx.ctrl, rx.ctrl_len >= 2 ? 2 : 0);
            link_down("Bridge closed the link");
        }
        return;
    }
    if (!rx.fin || rx.msg_op < 0) return;
    if (!rx.msg_skip && rx.msg_op == 0x1) {
        s_msg[rx.msg_len] = '\0';
        on_message(s_msg, rx.msg_len);
    }
    rx.msg_op = -1;
    rx.msg_len = 0;
    rx.msg_skip = false;
}

static void frame_start(void)
{
    const uint8_t *h = rx.hdr;
    rx.fin = (h[0] & 0x80) != 0;
    rx.op = h[0] & 0x0F;
    rx.masked = (h[1] & 0x80) != 0;
    uint64_t plen = h[1] & 0x7F;
    int o = 2;
    if (plen == 126) {
        plen = ((uint64_t)h[2] << 8) | h[3];
        o = 4;
    } else if (plen == 127) {
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | h[2 + i];
        o = 10;
    }
    if (rx.masked) memcpy(rx.mask, h + o, 4);
    rx.remain = plen;
    rx.pos = 0;
    rx.ctrl_len = 0;
    if (rx.op >= 0x8) {
        if (plen > sizeof(rx.ctrl) || !rx.fin) {
            link_down("Bridge protocol error");
            return;
        }
    } else if (rx.op == 0x1 || rx.op == 0x2) {
        rx.msg_op = rx.op;
        rx.msg_len = 0;
        rx.msg_skip = false;
    } else if (rx.op != 0x0) {
        link_down("Bridge protocol error");
        return;
    }
    if (rx.op < 0x8 && rx.msg_op >= 0 && rx.msg_len + plen > AGY_MSG_MAX) rx.msg_skip = true;
    rx.in_payload = true;
    if (plen == 0) frame_done();
}

static void rx_feed(const uint8_t *d, size_t n)
{
    while (n > 0 && s_fd >= 0) {
        if (!rx.in_payload) {
            rx.hdr[rx.hdr_len++] = *d++;
            n--;
            if (rx.hdr_len == 2) {
                int l7 = rx.hdr[1] & 0x7F;
                rx.hdr_need = 2 + (l7 == 126 ? 2 : l7 == 127 ? 8 : 0) + ((rx.hdr[1] & 0x80) ? 4 : 0);
            }
            if (rx.hdr_len >= 2 && rx.hdr_len == rx.hdr_need) frame_start();
            continue;
        }
        size_t take = rx.remain < n ? (size_t)rx.remain : n;
        uint8_t *dst = NULL;
        if (rx.op >= 0x8) {
            dst = rx.ctrl + rx.ctrl_len;
            rx.ctrl_len += take;
        } else if (rx.msg_op >= 0 && !rx.msg_skip) {
            dst = (uint8_t *)s_msg + rx.msg_len;
            rx.msg_len += take;
        }
        if (dst) {
            for (size_t i = 0; i < take; i++) {
                dst[i] = rx.masked ? (uint8_t)(d[i] ^ rx.mask[(rx.pos + i) & 3]) : d[i];
            }
        }
        rx.pos += take;
        rx.remain -= take;
        d += take;
        n -= take;
        if (rx.remain == 0) frame_done();
    }
}

void agy_client_init(void)
{
    config_load();
#ifndef ESP_PLATFORM
    srand((unsigned)time(NULL));
#endif
    rx_reset();
    s_want = true;
    s_retry = WS_RETRY_TICKS;
    set_status(AGY_DOWN, "Offline");
}

void agy_client_poll(void)
{
    if (!s_want) {
        if (s_fd >= 0) link_down(NULL);
        return;
    }

    if (s_fd < 0) {
        if (++s_retry < WS_RETRY_TICKS) return;
        s_retry = 0;
        s_fd = devos_net_socket_connect_start(s_cfg.host, s_cfg.port);
        s_ticks = 0;
        s_hdr_len = 0;
        char t[128];
        if (s_fd < 0) {
            snprintf(t, sizeof(t), "No route to %s:%d", s_cfg.host, s_cfg.port);
            set_status(AGY_CONNECTING, t);
            return;
        }
        snprintf(t, sizeof(t), "Connecting %s:%d...", s_cfg.host, s_cfg.port);
        set_status(AGY_CONNECTING, t);
        return;
    }

    if (!s_handshook && !s_sent_upgrade) {
        int r = devos_net_socket_connect_wait(s_fd, 0);
        if (r > 0) {
            if (++s_ticks > WS_TIMEOUT_TICKS) link_down("Connect timeout");
            return;
        }
        if (r < 0) {
            char t[128];
            snprintf(t, sizeof(t), "Refused by %s:%d (is the bridge running?)", s_cfg.host, s_cfg.port);
            link_down(t);
            return;
        }
        /* TCP up: send the WS upgrade */
        uint8_t key_raw[16];
        for (int i = 0; i < 16; i++) key_raw[i] = rnd8();
        char key[32];
        b64_encode(key_raw, sizeof(key_raw), key);
        char req[512];
        int hlen = snprintf(req, sizeof(req),
                            "GET /ws HTTP/1.1\r\nHost: %s:%d\r\n"
                            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                            "Sec-WebSocket-Key: %s\r\n"
                            "Sec-WebSocket-Version: 13\r\n\r\n",
                            s_cfg.host, s_cfg.port, key);
        if (hlen <= 0 || devos_net_socket_send_all(s_fd, req, (size_t)hlen) != 0) {
            link_down("Upgrade failed");
            return;
        }
        s_sent_upgrade = true;
        s_ticks = 0;
    }

    static uint8_t chunk[RX_CHUNK];
    bool got = false;
    for (int it = 0; it < RX_CHUNKS_PER_POLL && s_fd >= 0; it++) {
        int n = devos_net_socket_recv(s_fd, chunk, sizeof(chunk), 1);
        if (n == 0) {
            link_down("Bridge closed the link");
            return;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                link_down("Link lost");
                return;
            }
            break;
        }
        got = true;
        if (s_handshook) {
            rx_feed(chunk, (size_t)n);
            continue;
        }
        if (s_hdr_len + (size_t)n >= sizeof(s_hdr)) {
            link_down("Bad reply from the bridge");
            return;
        }
        memcpy(s_hdr + s_hdr_len, chunk, (size_t)n);
        s_hdr_len += (size_t)n;
        s_hdr[s_hdr_len] = '\0';
        char *eoh = strstr(s_hdr, "\r\n\r\n");
        if (!eoh) continue;
        int code = 0;
        if (sscanf(s_hdr, "HTTP/%*d.%*d %d", &code) != 1 && sscanf(s_hdr, "HTTP/%*d %d", &code) != 1) code = 0;
        if (code != 101) {
            char t[80];
            snprintf(t, sizeof(t), "Not a devOS bridge (HTTP %d)", code);
            link_down(t);
            return;
        }
        s_handshook = true;
        rx_reset();
        set_status(AGY_UP, "Signing in...");
        if (send_hello() != 0) return;
        size_t hused = (size_t)(eoh - s_hdr) + 4;
        if (s_hdr_len > hused) rx_feed((const uint8_t *)s_hdr + hused, s_hdr_len - hused);
    }
    if (s_fd < 0) return;
    if (!s_handshook) {
        if (++s_ticks > WS_TIMEOUT_TICKS + 50) link_down("No reply from the bridge");
        return;
    }
    if (got) {
        s_idle = 0;
    } else if (++s_idle > WS_IDLE_TICKS) {
        s_idle = 0;
        link_down("Link timed out; reconnecting");
    }
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
    if (host && host_len) snprintf(host, host_len, "%s", s_cfg.host);
    if (port) *port = s_cfg.port;
    if (token && token_len) snprintf(token, token_len, "%s", s_cfg.token);
}

int agy_client_set_server(const char *host, int port)
{
    if (!host || !*host || port <= 0 || port > 65535) return -1;
    snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", host);
    s_cfg.port = port;
    config_save();
    return agy_client_reconnect();
}

int agy_client_set_token(const char *token)
{
    if (!token) return -1;
    snprintf(s_cfg.token, sizeof(s_cfg.token), "%s", token);
    config_save();
    return agy_client_reconnect();
}

int agy_client_send(const char *text, const char *command)
{
    if (!text || !*text) return -1;
    if (s_status != AGY_UP || !s_handshook) {
        agy_block_t *b = new_block(AGY_ROLE_AGY, AGY_KIND_ERROR);
        snprintf(b->text, sizeof(b->text), "Not sent: the bridge is not connected (%s).", s_status_text);
        bump();
        return -1;
    }
    static EXT_RAM_BSS_ATTR char clipped[AGY_PROMPT_MAX];
    static EXT_RAM_BSS_ATTR char esc[AGY_PROMPT_MAX * 2];
    static EXT_RAM_BSS_ATTR char body[WS_TX_MAX];
    copy_text(clipped, sizeof(clipped), text);
    devos_json_escape(clipped, esc, sizeof(esc));
    char cmdfrag[64] = "";
    if (command && *command) {
        char cesc[40];
        devos_json_escape(command, cesc, sizeof(cesc));
        snprintf(cmdfrag, sizeof(cmdfrag), ",\"command\":\"%s\"", cesc);
    }
    snprintf(body, sizeof(body), "{\"type\":\"PROMPT\",\"text\":\"%s\"%s}", esc, cmdfrag);
    /* the user's words show at once (the bridge doesn't echo them) */
    agy_block_t *b = new_block(AGY_ROLE_USER, AGY_KIND_TEXT);
    size_t n = 0;
    if (command && *command) n = (size_t)snprintf(b->text, sizeof(b->text), "%.40s ", command);
    copy_text(b->text + n, sizeof(b->text) - n, clipped);
    bump();
    prompt_add(b->text);
    if (ws_send_json(body) != 0) return -1;
    s_busy = true;
    return 0;
}

int agy_client_new(void)
{
    s_conv[0] = '\0';
    clear_view();
    if (s_status == AGY_UP && s_handshook) return ws_send_json("{\"type\":\"NEW\"}");
    return 0;
}

int agy_client_abort(void)
{
    if (s_status != AGY_UP || !s_handshook) return -1;
    return ws_send_json("{\"type\":\"ABORT\"}");
}

int agy_client_conv_count(void) { return s_conv_count; }

const agy_conv_t *agy_client_conv(int idx)
{
    if (idx < 0 || idx >= s_conv_count) return NULL;
    return &s_convs[idx];
}

const char *agy_client_instance(void) { return s_instance; }

int agy_client_open(const char *conversation_id)
{
    if (!conversation_id || !*conversation_id || s_status != AGY_UP || !s_handshook) return -1;
    char esc[96];
    devos_json_escape(conversation_id, esc, sizeof(esc));
    char body[160];
    snprintf(body, sizeof(body), "{\"type\":\"OPEN\",\"id\":\"%s\"}", esc);
    return ws_send_json(body);
}

int agy_client_list(void)
{
    if (s_status != AGY_UP || !s_handshook) return -1;
    return ws_send_json("{\"type\":\"LIST\"}");
}

int agy_client_prompt_count(void) { return s_prompt_count; }

const char *agy_client_prompt(int idx)
{
    if (idx < 0 || idx >= s_prompt_count) return NULL;
    return s_prompts[idx];
}

const char *agy_client_conversation_id(void) { return s_conv; }

const char *agy_client_model(void) { return s_model; }

const char *agy_client_workspace(void) { return s_workspace; }

bool agy_client_busy(void) { return s_busy; }

void agy_client_usage(int *input, int *output, int *total)
{
    if (input) *input = s_usage[0];
    if (output) *output = s_usage[1];
    if (total) *total = s_usage[2];
}

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

uint32_t agy_client_diff_rev(void) { return s_diff_rev; }

int agy_client_diff_file_count(void)
{
    /* distinct "+++ " targets */
    const char *seen[64];
    size_t seen_len[64];
    int n = 0;
    for (const char *p = s_diff; p && *p;) {
        const char *nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        if (l > 4 && strncmp(p, "+++ ", 4) == 0) {
            bool dup = false;
            for (int i = 0; i < n && !dup; i++) dup = seen_len[i] == l && memcmp(seen[i], p, l) == 0;
            if (!dup && n < 64) {
                seen[n] = p;
                seen_len[n++] = l;
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    return n;
}

bool agy_client_permission_pending(agy_permission_t *out)
{
    if (!s_perm.active) return false;
    if (out) *out = s_perm;
    return true;
}

const char *agy_client_permission_preview(void) { return s_perm.active ? s_perm_preview : ""; }

int agy_client_answer_permission(bool allow, bool always)
{
    if (!s_perm.active) return -1;
    char body[256];
    snprintf(body, sizeof(body),
             "{\"type\":\"PERMISSION_REPLY\",\"id\":\"%s\",\"allow\":%s,\"always\":%s}",
             s_perm.id, allow ? "true" : "false", always ? "true" : "false");
    s_perm.active = false;
    bump();
    return ws_send_json(body);
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
    if (choice >= s_q.choice_count) choice = s_q.choice_count - 1;
    if (choice < -1) choice = -1;
    char body[256];
    snprintf(body, sizeof(body), "{\"type\":\"QUESTION_REPLY\",\"id\":\"%s\",\"choice\":%d}", s_q.id, choice);
    s_q.active = false;
    bump();
    return ws_send_json(body);
}

int agy_client_answer_question_text(const char *text)
{
    if (!s_q.active || !text || !*text) return -1;
    char esc[600];
    devos_json_escape(text, esc, sizeof(esc));
    char body[760];
    snprintf(body, sizeof(body), "{\"type\":\"QUESTION_REPLY\",\"id\":\"%s\",\"choice\":-1,\"text\":\"%s\"}", s_q.id,
             esc);
    s_q.active = false;
    bump();
    return ws_send_json(body);
}
