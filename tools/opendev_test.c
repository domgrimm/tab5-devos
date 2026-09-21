/* Ponytail check: opendev_client JSON/SSE/pairing logic (no display needed).
 *
 * IMPORTANT: run from an isolated CWD — pairing tests persist to
 * ./sim_sdcard/.devos/opendev_nvs.json relative to CWD:
 *
 *   mkdir -p /tmp/opencode/engtest && cd /tmp/opencode/engtest && \
 *   gcc -o opendev_test /home/dom/dev/tab5-devos/tools/opendev_test.c \
 *     /home/dom/dev/tab5-devos/components/devos_json/devos_json.c \
 *     -I/home/dom/dev/tab5-devos/main/include \
 *     -I/home/dom/dev/tab5-devos/components/opendev_client \
 *     -I/home/dom/dev/tab5-devos/components/devos_json \
 *     -I/home/dom/dev/tab5-devos/components/devos_net \
 *     -I/home/dom/dev/tab5-devos/components/devos_core && ./opendev_test
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Link stubs for transport (engine must fail closed without a server) */
int devos_net_socket_connect(const char *h, int p, int t)
{
    (void)h; (void)p; (void)t; return -1;
}
int devos_net_socket_send(int s, const void *d, size_t l)
{
    (void)s; (void)d; (void)l; return -1;
}
int devos_net_socket_send_all(int s, const void *d, size_t l)
{
    (void)s; (void)d; (void)l; return -1;
}
int devos_net_socket_recv(int s, void *b, size_t l, int t)
{
    (void)s; (void)b; (void)l; (void)t; return -1;
}
int devos_net_socket_close(int s) { (void)s; return 0; }
int devos_net_socket_connect_start(const char *h, int p)
{
    (void)h; (void)p; return -1;
}
int devos_net_socket_connect_wait(int s, int t) { (void)s; (void)t; return -1; }
int devos_net_resolve(const char *h, char *o, size_t l)
{
    (void)h; (void)o; (void)l; return -1;
}

#include "opendev_client.c"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } \
} while (0)

static int each_count;
static void count_cb(const char *o, size_t l, void *ud)
{
    (void)o; (void)l;
    each_count += (int)(intptr_t)ud + 1;
}

int main(void)
{
    char out[256];

    /* devos_json_parse_str escapes incl \u */
    const char *j1 = "\"a\\\"b\\nc\\u0041\"";
    CHECK(devos_json_parse_str(j1, j1 + strlen(j1), out, sizeof(out)) != NULL);
    CHECK(strcmp(out, "a\"b\ncA") == 0);
    CHECK(devos_json_parse_str("\"unterminated", "\"unterminated\" + 13", out,
                       sizeof(out)) == NULL);

    /* devos_json_get_str first match + missing key */
    const char *j2 = "{\"id\":\"ses_1\",\"title\":\"Dev\",\"n\":3}";
    CHECK(devos_json_get_str(j2, strlen(j2), "id", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "ses_1") == 0);
    CHECK(devos_json_get_str(j2, strlen(j2), "nope", out, sizeof(out)) != 0);
    /* key inside a string value must not match */
    const char *j3 = "{\"text\":\"say \\\"id\\\" loud\",\"id\":\"real\"}";
    CHECK(devos_json_get_str(j3, strlen(j3), "id", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "real") == 0);

    /* devos_json_array_each: objects, ws, empty, malformed */
    each_count = 0;
    const char *a1 = "[{\"a\":1}, {\"b\": [1,2]}, 3 ]";
    devos_json_array_each(a1, strlen(a1), count_cb, (void *)0);
    CHECK(each_count == 3);
    each_count = 0;
    devos_json_array_each("[]", 2, count_cb, (void *)0);
    CHECK(each_count == 0);
    each_count = 0;
    devos_json_array_each("[{\"a\":1}, {\"b\":2", 16, count_cb, (void *)0);
    CHECK(each_count == 1); /* second element truncated: only first fires */

    /* sessions list end-to-end (exercises sessions_each_cb) */
    const char *sess =
        "[{\"id\":\"ses_aaa111\",\"title\":\"Fix login\",\"modelID\":\"claude\"},"
        " {\"id\":\"ses_bbb222\"}]";
    extern int s_session_count;
    s_session_count = 0;
    devos_json_array_each(sess, strlen(sess), sessions_each_cb, NULL);
    CHECK(opendev_client_session_count() == 2);
    CHECK(strcmp(opendev_client_session(0)->id, "ses_aaa111") == 0);
    CHECK(strcmp(opendev_client_session(0)->title, "Fix login") == 0);
    CHECK(strcmp(opendev_client_session(0)->model, "claude") == 0);
    CHECK(strcmp(opendev_client_session(1)->title, "ses_bbb2") == 0);

    /* messages rebuild: text + reasoning + tool + unknown part */
    const char *msgs =
        "[{\"info\":{\"role\":\"user\"},\"parts\":[{\"type\":\"text\",\"text\":\"hi\"}]},"
        " {\"info\":{\"role\":\"assistant\"},\"parts\":["
        "  {\"type\":\"reasoning\",\"text\":\"let me think\"},"
        "  {\"type\":\"tool\",\"tool\":\"bash\",\"input\":\"ls -la\"},"
        "  {\"type\":\"weird\",\"text\":\"surprise\"}]}]";
    extern int s_block_count;
    extern opendev_block_t s_blocks[];
    s_block_count = 0;
    devos_json_array_each(msgs, strlen(msgs), messages_each_cb, NULL);
    CHECK(opendev_client_block_count() == 4);
    CHECK(opendev_client_block(0)->role == OPENDEV_ROLE_USER);
    CHECK(strcmp(opendev_client_block(0)->text, "hi") == 0);
    CHECK(opendev_client_block(1)->kind == OPENDEV_KIND_THINK);
    CHECK(strcmp(opendev_client_block(1)->text, "let me think") == 0);
    CHECK(opendev_client_block(2)->kind == OPENDEV_KIND_TOOL);
    CHECK(strncmp(opendev_client_block(2)->text, "bash\nls -la", 11) == 0);
    CHECK(strcmp(opendev_client_block(3)->text, "surprise") == 0);

    /* coalescing: consecutive same-role text merges ("surprise"+"a"+"b") */
    push_block(OPENDEV_ROLE_USER, OPENDEV_KIND_TEXT, "q");
    CHECK(opendev_client_block_count() == 5);
    push_block(OPENDEV_ROLE_USER, OPENDEV_KIND_TEXT, "a");
    push_block(OPENDEV_ROLE_USER, OPENDEV_KIND_TEXT, "b");
    CHECK(opendev_client_block_count() == 5);
    CHECK(strcmp(opendev_client_block(4)->text, "qab") == 0);

    /* SSE feed: split event across calls, comments, unknown events */
    extern void sse_feed(const char *, size_t);
    extern opendev_permission_t s_perm;
    s_perm.active = false;
    const char *e1 = "event: message.updated\ndata: {\"sessionID\":\"x\"}\n\n";
    sse_feed(e1, 10); /* partial: nothing complete yet */
    CHECK(s_perm.active == false);
    sse_feed(e1 + 10, strlen(e1) - 10);
    sse_feed(":keep-alive\n\n", 13);
    const char *e2 = "event: bogus-type\ndata: {}\n\n";
    sse_feed(e2, strlen(e2));
    const char *e3 = "event: permission.asked\ndata: "
                     "{\"id\":\"perm_9\",\"sessionID\":\"ses_aaa111\","
                     "\"title\":\"run tests?\"}\n\n";
    sse_feed(e3, strlen(e3));
    CHECK(s_perm.active == true);
    CHECK(strcmp(s_perm.id, "perm_9") == 0);
    CHECK(strcmp(s_perm.text, "run tests?") == 0);
    opendev_permission_t po;
    CHECK(opendev_client_permission_pending(&po) == true);
    /* answer with dead transport fails but clears the pending flag */
    CHECK(opendev_client_answer_permission(true, false) != 0);
    CHECK(opendev_client_permission_pending(NULL) == false);

    /* pairing URIs */
    CHECK(opendev_client_pair("http://nope") != 0);
    CHECK(opendev_client_pair("openchamber://connect?token=") != 0);
    CHECK(opendev_client_pair(
              "openchamber://connect?host=10.0.0.9&port=8421&p=tok%20123") == 0);
    char host[64];
    int port = 0;
    opendev_mode_t mode = OPENDEV_MODE_CODE;
    char token[128];
    opendev_client_get_config(host, sizeof(host), &port, &mode, token,
                              sizeof(token));
    CHECK(strcmp(host, "10.0.0.9") == 0);
    CHECK(port == 8421);
    CHECK(mode == OPENDEV_MODE_CHAMBER);
    CHECK(strcmp(token, "tok 123") == 0);
    /* switching back to opencode server clears chamber token and mode */
    CHECK(opendev_client_set_server("192.168.1.50", 4096) == 0);
    opendev_client_get_config(host, sizeof(host), &port, &mode, token, sizeof(token));
    CHECK(strcmp(host, "192.168.1.50") == 0);
    CHECK(port == 4096);
    CHECK(mode == OPENDEV_MODE_CODE);
    CHECK(token[0] == '\0');

    /* explicit chamber configuration with custom port */
    CHECK(opendev_client_set_chamber("100.77.11.92", 8422, "secret_tok") == 0);
    opendev_client_get_config(host, sizeof(host), &port, &mode, token, sizeof(token));
    CHECK(strcmp(host, "100.77.11.92") == 0);
    CHECK(port == 8422);
    CHECK(mode == OPENDEV_MODE_CHAMBER);
    CHECK(strcmp(token, "secret_tok") == 0);

    /* pairing URI with authority format host:port?token=... */
    CHECK(opendev_client_pair("openchamber://10.0.0.12:9000?token=tok_auth") == 0);
    opendev_client_get_config(host, sizeof(host), &port, &mode, token, sizeof(token));
    CHECK(strcmp(host, "10.0.0.12") == 0);
    CHECK(port == 9000);
    CHECK(mode == OPENDEV_MODE_CHAMBER);
    CHECK(strcmp(token, "tok_auth") == 0);

    /* reset back to opencode mode for subsequent empty-token checks */
    CHECK(opendev_client_set_server("192.168.1.50", 4096) == 0);

    /* devos_json_escape roundtrip essentials */
    char esc[64];
    devos_json_escape("a\"b\\c\nd", esc, sizeof(esc));
    CHECK(strcmp(esc, "a\\\"b\\\\c\\nd") == 0);

    /* adversarial: failed send must not leave a stuck busy badge */
    extern opendev_session_t s_sessions[];
    extern int s_active;
    extern int s_block_count;
    s_session_count = 0;
    devos_json_array_each(sess, strlen(sess), sessions_each_cb, NULL);
    opendev_client_select(0); /* fetch fails (dead transport), keeps active */
    CHECK(opendev_client_active() == 0);
    s_block_count = 0;
    CHECK(opendev_client_send("hello?") != 0);
    CHECK(opendev_client_session(0)->busy == false);
    CHECK(opendev_client_block_count() == 2); /* optimistic + failure note */
    CHECK(strstr(opendev_client_block(1)->text, "send failed") != NULL);

    /* token auth header formatting must have proper CRLF separation when token present */
    char test_req[512];
    const char *sample_token = "tok_abc_123";
    snprintf(test_req, sizeof(test_req),
             "GET /event HTTP/1.1\r\nHost: %s:%d\r\n"
             "Accept: text/event-stream\r\n"
             "Cache-Control: no-cache\r\n"
             "%s%s%s"
             "Connection: keep-alive\r\n\r\n",
             s_cfg.host, s_cfg.port,
             sample_token[0] ? "Authorization: Bearer " : "",
             sample_token[0] ? sample_token : "",
             sample_token[0] ? "\r\n" : "");
    CHECK(strstr(test_req, "\r\nAuthorization: Bearer tok_abc_123\r\n") != NULL);
    CHECK(strstr(test_req, "Connection: keep-aliveAuthorization") == NULL);

    /* when token is empty (standard OpenCode mode), no Authorization header is injected */
    snprintf(test_req, sizeof(test_req),
             "GET /event HTTP/1.1\r\nHost: %s:%d\r\n"
             "Accept: text/event-stream\r\n"
             "Cache-Control: no-cache\r\n"
             "%s%s%s"
             "Connection: keep-alive\r\n\r\n",
             s_cfg.host, s_cfg.port,
             s_cfg.token[0] ? "Authorization: Bearer " : "",
             s_cfg.token[0] ? s_cfg.token : "",
             s_cfg.token[0] ? "\r\n" : "");
    CHECK(strstr(test_req, "Authorization") == NULL);
    CHECK(strstr(test_req, "Connection: keep-alive\r\n\r\n") != NULL);

    if (failures == 0) printf("opendev unit tests: ALL PASS\n");
    return failures != 0;
}
