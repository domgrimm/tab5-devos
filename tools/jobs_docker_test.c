/* Host end-to-end test for the Docker Jobs providers
 * (main/jobs_providers/jobs_docker.c) and the request-specific devos_docker
 * contract, driven against a fake Docker Engine API over loopback. Covers:
 * inspect by name (ok/not-found/transport error), restart accepted vs outcome
 * unknown, two commands never overwriting, a config snapshot surviving a later
 * settings change, queue saturation returning an error instead of overwriting,
 * the UI action note, cancellation, and release-exactly-once.
 *
 * Run from an ISOLATED CWD (the engine writes ./sim_sdcard):
 *
 *   mkdir -p /tmp/jobs_docker_test && cd /tmp/jobs_docker_test
 *   gcc -O2 -I$REPO/main/jobs_providers -I$REPO/components/devos_actions \
 *       -I$REPO/components/devos_err -I$REPO/components/devos_docker \
 *       -I$REPO/components/devos_http -I$REPO/components/devos_net \
 *       -I$REPO/components/devos_json -I$REPO/components/devos_config/include \
 *       -I$REPO/components/devos_jobs -I$REPO/components/devos_tailnet \
 *       $REPO/tools/jobs_docker_test.c $REPO/main/jobs_providers/jobs_docker.c \
 *       $REPO/components/devos_docker/devos_docker.c \
 *       $REPO/components/devos_jobs/jobs_model.c $REPO/components/devos_jobs/jobs_parse.c \
 *       $REPO/components/devos_jobs/jobs_validate.c $REPO/components/devos_jobs/jobs_serialize.c \
 *       $REPO/components/devos_actions/devos_actions.c \
 *       $REPO/components/devos_http/devos_http.c $REPO/components/devos_net/devos_net.c \
 *       $REPO/components/devos_json/devos_json.c -lpthread -o /tmp/jobs_docker_test && /tmp/jobs_docker_test
 */
#include "jobs_providers.h"
#include "devos_actions.h"
#include "devos_docker.h"
#include "jobs_model.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* devos_net's resolver asks the tailnet first; not here */
int devos_tailnet_resolve(const char *name, char *out_ip, size_t out_len)
{
    (void)name; (void)out_ip; (void)out_len;
    return -1;
}

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* An app-enablement gate (the Docker app switched off). */
static bool gate_off(char *reason, size_t cap) { snprintf(reason, cap, "Docker app off"); return false; }

static int s_port;
static volatile int s_stop;
static volatile int s_slow_ms;
static char s_paths[64][128];
static volatile int s_npaths;

static const char *INSPECT_JSON =
    "{\"Id\":\"0123456789abcdef0123\",\"Created\":\"2024-01-01T00:00:00Z\","
    "\"Name\":\"/web\",\"Config\":{\"Image\":\"nginx:1.25\"},"
    "\"State\":{\"Status\":\"running\",\"StartedAt\":\"2024-01-02T03:04:05.123456789Z\","
    "\"Health\":{\"Status\":\"healthy\"}}}";

static void *server_thread(void *arg)
{
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0 };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, (struct sockaddr *)&a, sizeof(a));
    socklen_t al = sizeof(a);
    getsockname(ls, (struct sockaddr *)&a, &al);
    s_port = ntohs(a.sin_port);
    listen(ls, 8);
    while (!s_stop) {
        struct timeval tv = { 0, 200000 };
        setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        char req[2048];
        int n = (int)recv(fd, req, sizeof(req) - 1, 0);
        if (n <= 0) { close(fd); continue; }
        req[n] = '\0';
        char *sp = strchr(req, ' ');
        if (sp && sp[1] == '/') {
            char *e = strchr(sp + 1, ' ');
            if (e && s_npaths < 64) {
                size_t l = (size_t)(e - (sp + 1));
                if (l > 127) l = 127;
                /* keep the request line (path + query) for later assertions */
                const char *q = strchr(sp + 1, '?');
                size_t ql = q && q < e ? (size_t)(e - q) : 0;
                (void)ql;
                memcpy(s_paths[s_npaths], sp + 1, l);
                s_paths[s_npaths][l] = '\0';
                s_npaths++;
            }
        }
        int code = 200;
        const char *body = INSPECT_JSON;
        if (strstr(req, "/containers/missing/")) {
            code = 404;
            body = "{\"message\":\"No such container: missing\"}";
        } else if (strstr(req, "GET ") && strstr(req, "/containers/slow/")) {
            if (s_slow_ms) usleep((useconds_t)s_slow_ms * 1000);
            body = INSPECT_JSON;
        } else if (strstr(req, "POST ") &&
                   (strstr(req, "/containers/web/restart") || strstr(req, "/containers/web/stop") ||
                    strstr(req, "/containers/web/start") || strstr(req, "/containers/alpha/restart") ||
                    strstr(req, "/containers/beta/restart"))) {
            code = 204;
            body = NULL;
        } else if (!strstr(req, "GET ") || !strstr(req, "/containers/web/json")) {
            code = 404;
            body = "{\"message\":\"No such container\"}";
        }
        char hdr[256];
        int hn;
        if (body) hn = snprintf(hdr, sizeof(hdr),
                                "HTTP/1.1 %d X\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                                code, strlen(body));
        else hn = snprintf(hdr, sizeof(hdr), "HTTP/1.1 %d No Content\r\nConnection: close\r\n\r\n", code);
        send(fd, hdr, (size_t)hn, MSG_NOSIGNAL);
        if (body) send(fd, body, strlen(body), MSG_NOSIGNAL);
        close(fd);
    }
    close(ls);
    return NULL;
}

static devos_value_t vstr(const char *s) { devos_value_t v; v.type = DEVOS_VAL_STR; v.v.str.s = s; v.v.str.len = (uint32_t)strlen(s); return v; }
static devos_value_t vdur(int64_t ms) { devos_value_t v; v.type = DEVOS_VAL_DURATION; v.v.ms = ms; return v; }

/* Poll a provider op to a final state (up to ~4 s). */
static devos_action_state_t run(devos_action_handle_t h, devos_action_result_t *res)
{
    devos_action_state_t st = DEVOS_ACT_PENDING;
    for (int i = 0; i < 200 && st == DEVOS_ACT_PENDING; i++) {
        usleep(20 * 1000);
        if (devos_action_poll(h, &st, res) != DEVOS_OK) break;
    }
    return st;
}

/* Poll an engine ticket to a final state (up to ~4 s). */
static bool wait_ticket(uint32_t t, devos_docker_req_result_t *out)
{
    for (int i = 0; i < 200; i++) {
        if (!devos_docker_request_poll(t, out)) return false;
        if (out->state != DEVOS_DOCKER_REQ_PENDING) return true;
        usleep(20 * 1000);
    }
    return false;
}

int main(void)
{
    pthread_t srv;
    pthread_create(&srv, NULL, server_thread, NULL);
    while (!s_port) usleep(10 * 1000);

    jobs_docker_register();
    CHECK(devos_actions_find("docker.inspect") != NULL);
    CHECK(devos_actions_find("docker.start") != NULL);
    CHECK(devos_actions_find("docker.stop") != NULL);
    CHECK(devos_actions_find("docker.restart") != NULL);

    /* The documented Docker example parses, validates against the real schemas
     * and round-trips canonically. */
    static const char *SRC =
        "version 1;\n"
        "job \"Watch web\" {\n"
        "    trigger manual;\n"
        "    docker.inspect(container: \"web\", timeout: 5s) as web;\n"
        "    if !web.ok {\n"
        "        docker.restart(container: \"web\", timeout: 15s) as restart;\n"
        "        wait 10s;\n"
        "        docker.inspect(container: \"web\", timeout: 5s) as after;\n"
        "    }\n"
        "}\n";
    jobs_ast_t *ast = jobs_parse(SRC, strlen(SRC), NULL);
    CHECK(ast != NULL);
    if (ast) {
        CHECK(jobs_validate(ast));
        if (jobs_failed(ast)) printf("  diag: %s\n", ast->diag[0].msg);
        char out[1024];
        size_t n = jobs_serialize(ast, out, sizeof(out));
        CHECK(n > 0);
        jobs_ast_t *ast2 = jobs_parse(out, n, NULL);
        CHECK(ast2 != NULL && jobs_validate(ast2));
        if (ast2) {
            char out2[1024];
            size_t n2 = jobs_serialize(ast2, out2, sizeof(out2));
            CHECK(n2 == n && memcmp(out, out2, n) == 0);
            jobs_ast_free(ast2);
        }
        jobs_ast_free(ast);
    }

    /* Without a config the provider is unavailable, with a reason. */
    char why[80] = "";
    CHECK(!devos_actions_available("docker.inspect", why, sizeof(why)));
    CHECK(why[0] != '\0');

    /* A gate (the Docker app switched off) overrides even a configured engine. */
    jobs_docker_set_gate(gate_off);
    why[0] = '\0';
    CHECK(!devos_actions_available("docker.inspect", why, sizeof(why)));
    CHECK(strstr(why, "Docker app off") != NULL);
    jobs_docker_set_gate(NULL);

    devos_docker_init();
    devos_docker_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = DEVOS_DOCKER_DIRECT;
    snprintf(cfg.url, sizeof(cfg.url), "http://127.0.0.1:%d", s_port);
    devos_docker_set_config(&cfg);
    CHECK(devos_actions_available("docker.inspect", why, sizeof(why)));

    devos_value_t a[2];
    devos_action_args_t args = { .args = a, .arg_count = 2 };
    devos_action_handle_t h;
    devos_action_result_t res;

    /* Inspect by name: the daemon's current state, independent of any UI cache. */
    a[0] = vstr("web");
    a[1] = vdur(5000);
    CHECK(devos_action_start("docker.inspect", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.out_count == 8);
    CHECK(res.outs[0].v.b == true && res.outs[1].v.i == 200);
    CHECK(strcmp(res.outs[2].v.str.s, "running") == 0);
    CHECK(strcmp(res.outs[3].v.str.s, "healthy") == 0);
    CHECK(strcmp(res.outs[4].v.str.s, "0123456789ab") == 0);
    CHECK(strcmp(res.outs[5].v.str.s, "web") == 0);
    CHECK(res.outs[6].v.i > 0);
    devos_action_release(h);
    CHECK(devos_actions_outstanding() == 0);
    devos_action_release(h);                               /* stale: no-op */
    CHECK(devos_actions_outstanding() == 0);

    /* Not found: a completed negative observation, not a transport failure. */
    a[0] = vstr("missing");
    a[1] = vdur(5000);
    CHECK(devos_action_start("docker.inspect", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.outs[0].v.b == false && res.outs[1].v.i == 404);
    CHECK(strstr(res.outs[7].v.str.s, "not found") != NULL);
    devos_action_release(h);

    /* Restart: 204 means the request was accepted, not that it recovered. */
    a[0] = vstr("web");
    CHECK(devos_action_start("docker.restart", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.out_count == 4);
    CHECK(res.outs[0].v.i == 204 && res.outs[1].v.b == true && res.outs[2].v.b == false);
    devos_action_release(h);

    /* Two commands never overwrite each other. */
    devos_docker_req_result_t r1, r2;
    uint32_t t1 = devos_docker_request("alpha", "restart", 5000);
    uint32_t t2 = devos_docker_request("beta", "restart", 5000);
    CHECK(t1 && t2 && t1 != t2);
    CHECK(wait_ticket(t1, &r1) && r1.state == DEVOS_DOCKER_REQ_DONE && r1.status == 204);
    CHECK(wait_ticket(t2, &r2) && r2.state == DEVOS_DOCKER_REQ_DONE && r2.status == 204);
    CHECK(strcmp(r1.action, "restart") == 0 && strcmp(r2.action, "restart") == 0);
    devos_docker_request_release(t1);
    devos_docker_request_release(t2);

    /* A submitted request keeps its config snapshot: a later settings change
     * cannot redirect it to another host. */
    t1 = devos_docker_inspect("web", 5000);
    devos_docker_config_t cfg2 = cfg;
    snprintf(cfg2.url, sizeof(cfg2.url), "http://127.0.0.1:9");
    devos_docker_set_config(&cfg2);
    CHECK(wait_ticket(t1, &r1) && r1.state == DEVOS_DOCKER_REQ_DONE && r1.status == 200);
    CHECK(strcmp(r1.inspect.name, "web") == 0);
    devos_docker_request_release(t1);
    devos_docker_set_config(&cfg);

    /* The UI wrapper still shows a note and frees its own ticket. */
    CHECK(devos_docker_action("web", "restart") == 0);
    bool noted = false;
    for (int i = 0; i < 100 && !noted; i++) {
        devos_docker_status_t st;
        devos_docker_status(&st);
        noted = strstr(st.note, "Restarted") != NULL;
        if (!noted) usleep(20 * 1000);
    }
    CHECK(noted);

    /* Queue saturation returns an error instead of overwriting; commands run
     * while the UI is not polling. */
    s_slow_ms = 300;
    uint32_t tickets[DEVOS_DOCKER_REQS];
    int filled = 0;
    for (int i = 0; i < DEVOS_DOCKER_REQS; i++) {
        tickets[i] = devos_docker_inspect("slow", 20000);
        if (tickets[i]) filled++;
    }
    CHECK(filled == DEVOS_DOCKER_REQS);
    CHECK(devos_docker_inspect("slow", 5000) == 0);         /* full: rejected */
    for (int i = 0; i < filled; i++) devos_docker_request_release(tickets[i]);
    s_slow_ms = 0;
    usleep(600 * 1000);                                     /* let the worker drain */

    /* Cancel: polled as cancelled, released exactly once. */
    t1 = devos_docker_inspect("slow", 20000);
    CHECK(t1 != 0);
    devos_docker_request_cancel(t1);
    CHECK(devos_docker_request_poll(t1, &r1) && r1.state == DEVOS_DOCKER_REQ_FAILED);
    CHECK(strstr(r1.error, "cancelled") != NULL);
    devos_docker_request_release(t1);
    devos_docker_request_release(t1);                       /* safe once */

    /* Transport error on a mutation is outcome-unknown, never success. */
    devos_docker_config_t dead = cfg;
    snprintf(dead.url, sizeof(dead.url), "http://127.0.0.1:9");
    devos_docker_set_config(&dead);
    a[0] = vstr("web");
    CHECK(devos_action_start("docker.start", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.outs[0].v.i == 0 && res.outs[1].v.b == false && res.outs[2].v.b == true);
    CHECK(res.outs[3].v.str.len > 0);
    devos_action_release(h);
    devos_docker_set_config(&cfg);

    /* UI polling path: set_active starts the worker for the list/stats/logs
     * refresh; a Jobs command still completes alongside it. */
    devos_docker_set_active(true);
    usleep(400 * 1000);
    uint32_t t3 = devos_docker_inspect("web", 5000);
    CHECK(t3 != 0 && wait_ticket(t3, &r1) && r1.status == 200);
    devos_docker_request_release(t3);
    devos_docker_set_active(false);

    s_stop = 1;
    pthread_join(srv, NULL);
    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}