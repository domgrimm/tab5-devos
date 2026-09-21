/* Ponytail check: agy_client link + protocol (no display needed).
 *
 * IMPORTANT: run from an isolated CWD — token tests persist to
 * ./sim_sdcard/.devos/agy_nvs.json relative to CWD:
 *
 *   mkdir -p /tmp/opencode/agytest && cd /tmp/opencode/agytest && \
 *   gcc -o agy_test /home/dom/dev/tab5-devos/tools/agy_test.c \
 *     /home/dom/dev/tab5-devos/components/agy_client/agy_client.c \
 *     /home/dom/dev/tab5-devos/components/devos_json/devos_json.c \
 *     -I/home/dom/dev/tab5-devos/main/include \
 *     -I/home/dom/dev/tab5-devos/components/agy_client \
 *     -I/home/dom/dev/tab5-devos/components/devos_json \
 *     -I/home/dom/dev/tab5-devos/components/devos_net \
 *     -I/home/dom/dev/tab5-devos/components/devos_core && ./agy_test
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Scripted transport: handshake, then canned server frames */
static int stub_step = 0;
size_t test_remain = 0;
char test_rem[2048];

static void srv_frame(const char *payload, char *out, size_t *n)
{
    size_t len = strlen(payload);
    out[0] = (char)0x81;
    out[1] = (char)len; /* payloads here are all < 126 */
    memcpy(out + 2, payload, len);
    *n = len + 2;
}

int devos_net_socket_connect_start(const char *h, int p)
{
    (void)h; (void)p;
    stub_step = 1;
    return 42;
}
int devos_net_socket_connect_wait(int s, int t)
{
    (void)s; (void)t;
    return 0;
}
int devos_net_socket_send(int s, const void *d, size_t l)
{
    (void)s; (void)d; (void)l; return -1;
}
static char send_cap[8192];
static size_t send_cap_len = 0;
int devos_net_socket_send_all(int s, const void *d, size_t l)
{
    (void)s;
    size_t take = l;
    if (send_cap_len + take > sizeof(send_cap)) {
        take = sizeof(send_cap) - send_cap_len;
    }
    memcpy(send_cap + send_cap_len, d, take);
    send_cap_len += take;
    return 0;
}
int devos_net_socket_recv(int s, void *b, size_t l, int t)
{
    (void)s; (void)t;
    static char fr[2048];
    size_t n = 0;
    if (stub_step == 1) {
        const char *h = "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: x\r\n\r\n";
        n = strlen(h);
        if (n > l) n = l;
        memcpy(b, h, n);
        stub_step = 2;
        return (int)n;
    }
    if (stub_step == 2) {
        /* split WELCOME frame across two reads (partial-frame buffering) */
        srv_frame("{\"type\":\"WELCOME\",\"conversation_id\":\"c1\","
                  "\"model\":\"m1\",\"subagents\":[{\"name\":\"research\","
                  "\"state\":\"running\"}]}",
                  fr, &n);
        size_t half = n / 2;
        if (half > l) half = l;
        memcpy(b, fr, half);
        /* stash remainder for next call */
        memmove(fr, fr + half, n - half);
        stub_step = 3;
        test_remain = n - half;
        memcpy(test_rem, fr, n - half);
        return (int)half;
    }
    if (stub_step == 3) {
        size_t n2 = test_remain > l ? l : test_remain;
        memcpy(b, test_rem, n2);
        test_remain = 0;
        stub_step = 4;
        return (int)n2;
    }
    if (stub_step == 4) {
        /* server PING -> engine must answer masked PONG */
        char *o = b;
        o[0] = (char)0x89;
        o[1] = 0;
        stub_step = 5;
        return 2;
    }
    if (stub_step == 5) {
        srv_frame("{\"type\":\"THINKING\",\"text\":\"step one\"}", fr, &n);
        if (n > l) n = l;
        memcpy(b, fr, n);
        stub_step = 6;
        return (int)n;
    }
    return -1; /* quiet: EAGAIN, link stays up */
}

int devos_net_socket_close(int s) { (void)s; return 0; }
int devos_net_resolve(const char *h, char *o, size_t l)
{
    (void)h; (void)o; (void)l; return -1;
}

#include "agy_client.c"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } \
} while (0)
#define MSG(s) s, strlen(s)

int main(void)
{
    agy_client_init();
    CHECK(agy_client_status() == AGY_DOWN);

    /* drive the link: retry ticks (30) + connect + handshake +
     * partial WELCOME + PING + THINKING */
    for (int i = 0; i < 60; i++) agy_client_poll();
    CHECK(agy_client_status() == AGY_UP);
    CHECK(strcmp(agy_client_conversation_id(), "c1") == 0);
    CHECK(strcmp(agy_client_model(), "m1") == 0);
    CHECK(agy_client_agent_count() == 1);
    CHECK(strcmp(agy_client_agent(0)->name, "research") == 0);
    CHECK(strcmp(agy_client_agent(0)->state, "running") == 0);

    /* link behavior: upgrade request sent, HELLO with empty token,
     * masked PONG answered to the server PING.
     * ponytail: client frames are masked; unmask to inspect. */
    send_cap[send_cap_len] = '\0';
    CHECK(strstr(send_cap, "GET /ws HTTP/1.1") != NULL);
    CHECK(strstr(send_cap, "Sec-WebSocket-Key: ") != NULL);
    static char plain[8192];
    size_t po = 0;
    for (size_t i = 0; i + 2 < send_cap_len && po + 1 < sizeof(plain);) {
        uint8_t b0 = (uint8_t)send_cap[i];
        uint8_t b1 = (uint8_t)send_cap[i + 1];
        if ((b0 & 0x80) == 0 || (b1 & 0x80) == 0) {
            i++;
            continue; /* server frames / headers: skip a byte */
        }
        uint64_t plen = b1 & 0x7F;
        size_t hlen = 2;
        if (plen == 126) {
            if (i + 4 > send_cap_len) break;
            plen = ((uint64_t)(uint8_t)send_cap[i + 2] << 8) |
                   (uint64_t)(uint8_t)send_cap[i + 3];
            hlen = 4;
        } else if (plen == 127) {
            break; /* not emitted by the engine (4 KB cap) */
        }
        if (i + hlen + 4 > send_cap_len) break;
        const char *mask = send_cap + i + hlen;
        size_t start = i + hlen + 4;
        if (start + plen > send_cap_len) break;
        for (uint64_t k = 0; k < plen && po + 1 < sizeof(plain); k++) {
            plain[po++] = send_cap[start + k] ^ mask[k & 3];
        }
        plain[po] = '\0';
        i = start + (size_t)plen;
    }
    plain[po] = '\0';
    CHECK(strstr(plain, "\"type\":\"HELLO\"") != NULL);
    {
        /* masked empty PONG = 8A 80 + 4 mask bytes */
        bool pong = false;
        for (size_t i = 0; i + 5 < send_cap_len; i++) {
            if ((uint8_t)send_cap[i] == 0x8A &&
                (uint8_t)send_cap[i + 1] == 0x80) {
                pong = true;
                break;
            }
        }
        CHECK(pong == true);
    }

    /* THINKING coalescing across frames tested via direct dispatch.
     * Block 0 already holds "step one" from the scripted stream. */
    on_message(MSG("{\"type\":\"THINKING\",\"text\":\"line a\"}"));
    on_message(MSG("{\"type\":\"THINKING\",\"text\":\"line b\"}"));
    on_message(MSG("{\"type\":\"THINKING\",\"text\":\"line b\"}"));
    CHECK(agy_client_block_count() == 1);
    CHECK(agy_client_block(0)->kind == AGY_KIND_THINK);
    CHECK(strstr(agy_client_block(0)->text, "step one\nline a\nline b") != NULL);

    on_message(MSG("{\"type\":\"TOKEN\",\"text\":\"hi \"}"));
    on_message(MSG("{\"type\":\"TOKEN\",\"text\":\"there\"}"));
    CHECK(agy_client_block_count() == 2);
    CHECK(strcmp(agy_client_block(1)->text, "hi there") == 0);

    on_message(MSG("{\"type\":\"TOOL\",\"name\":\"bash\",\"detail\":\"ls\"}"));
    CHECK(agy_client_block(2)->kind == AGY_KIND_TOOL);

    on_message(MSG("{\"type\":\"ARTIFACT\",\"name\":\"p.md\","
                   "\"kind\":\"markdown\",\"text\":\"v1\"}"));
    on_message(MSG("{\"type\":\"ARTIFACT\",\"name\":\"p.md\","
                   "\"kind\":\"markdown\",\"text\":\"v2\"}"));
    CHECK(agy_client_artifact_count() == 1);
    CHECK(strcmp(agy_client_artifact(0)->text, "v2") == 0);

    on_message(MSG("{\"type\":\"DIFF\",\"file\":\"a.c\",\"hunk\":\"+x\"}"));
    CHECK(strstr(agy_client_diff_text(), "--- a.c") != NULL);

    on_message(MSG("{\"type\":\"PERMISSION\",\"id\":\"pm1\","
                   "\"text\":\"run it?\"}"));
    agy_permission_t pm;
    CHECK(agy_client_permission_pending(&pm) == true);
    CHECK(strcmp(pm.id, "pm1") == 0);

    on_message(MSG("{\"type\":\"QUESTION\",\"id\":\"q1\",\"prompt\":\"pick\","
                   "\"choices\":[\"a\",\"b\",\"c\"]}"));
    agy_question_t qq;
    CHECK(agy_client_question_pending(&qq) == true);
    CHECK(qq.choice_count == 3);
    CHECK(strcmp(qq.choices[1], "b") == 0);

    on_message(MSG("{\"type\":\"STATUS\",\"state\":\"busy\"}"));
    CHECK(agy_client_busy() == true);
    on_message(MSG("{\"type\":\"STATUS\",\"state\":\"idle\"}"));
    CHECK(agy_client_busy() == false);

    on_message(MSG("{\"type\":\"NOPE\",\"x\":1}")); /* unknown: ignored */
    on_message(MSG("not json at all"));
    CHECK(agy_client_question_pending(NULL) == true); /* still pending */

    /* config + token persist to isolated CWD file */
    CHECK(agy_client_set_server("10.9.9.9", 8421) == 0);
    CHECK(agy_client_set_token("sekret") == 0);
    char host[64];
    int port = 0;
    char tok[128];
    agy_client_get_config(host, sizeof(host), &port, tok, sizeof(tok));
    CHECK(strcmp(host, "10.9.9.9") == 0 && port == 8421);
    CHECK(strcmp(tok, "sekret") == 0);

    /* adversarial: dead-link send leaves a failure note, not silence */
    extern int s_fd;
    s_fd = -1; /* simulate a dropped link under our feet */
    CHECK(agy_client_send("dropped?", NULL) != 0);
    CHECK(strstr(agy_client_block(agy_client_block_count() - 1)->text,
                 "send failed") != NULL);

    /* adversarial: agy_client_new resets the conversation header */
    agy_client_new();
    CHECK(agy_client_conversation_id()[0] == '\0');
    CHECK(agy_client_block_count() == 0);
    CHECK(agy_client_busy() == false);

    if (failures == 0) printf("agy unit tests: ALL PASS\n");
    return failures != 0;
}
