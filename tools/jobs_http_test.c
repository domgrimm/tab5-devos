/* Host end-to-end test for the http.request Jobs provider
 * (main/jobs_providers/jobs_http.c) against a local HTTP fixture: a real
 * devos_http worker is driven through the devos_actions runtime. Covers
 * completion vs result semantics (200 vs 503 vs transport timeout), body
 * truncation, admission cap, cancel, and exactly-once slot/response release.
 *
 *   gcc -O2 -Imain/jobs_providers -Icomponents/devos_actions -Icomponents/devos_err \
 *       -Icomponents/devos_http -Icomponents/devos_net -Icomponents/devos_config/include \
 *       -Icomponents/devos_jobs -Icomponents/devos_tailnet \
 *       tools/jobs_http_test.c main/jobs_providers/jobs_http.c \
 *       components/devos_actions/devos_actions.c components/devos_http/devos_http.c \
 *       components/devos_net/devos_net.c -lpthread -o /tmp/jobs_http_test && /tmp/jobs_http_test
 */
#include "jobs_providers.h"
#include "devos_actions.h"
#include "devos_http.h"

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

static int s_port;
static volatile int s_stop;

/* A port nothing is listening on (bind then close), for a transport failure. */
static int closed_port(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0 };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (struct sockaddr *)&a, sizeof(a));
    socklen_t al = sizeof(a);
    getsockname(s, (struct sockaddr *)&a, &al);
    int p = ntohs(a.sin_port);
    close(s);
    return p;
}

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
        const char *body = "hello";
        int code = 200;
        char loc[64] = "";
        if (strstr(req, "/fail")) { code = 503; body = "nope"; }
        else if (strstr(req, "/big")) body = NULL;         /* generated below */
        else if (strstr(req, "/redirect")) { code = 302; body = ""; snprintf(loc, sizeof(loc), "Location: /ok\r\n"); }
        else if (strstr(req, "/slow")) { usleep(1200 * 1000); body = "late"; }

        char big[8000];
        size_t blen;
        if (!body) { memset(big, 'x', sizeof(big)); blen = sizeof(big); body = big; }
        else blen = strlen(body);
        char hdr[320];
        int hn = snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 %d X\r\nContent-Length: %zu\r\n%sConnection: close\r\n\r\n", code, blen, loc);
        send(fd, hdr, (size_t)hn, MSG_NOSIGNAL);
        send(fd, body, blen, MSG_NOSIGNAL);
        close(fd);
    }
    close(ls);
    return NULL;
}

static devos_value_t vstr(const char *s) { devos_value_t v; v.type = DEVOS_VAL_STR; v.v.str.s = s; v.v.str.len = (uint32_t)strlen(s); return v; }
static devos_value_t vdur(int64_t ms) { devos_value_t v; v.type = DEVOS_VAL_DURATION; v.v.ms = ms; return v; }
static devos_value_t vint(int64_t i) { devos_value_t v; v.type = DEVOS_VAL_INT; v.v.i = i; return v; }

/* method, url, timeout, max_body, bearer, body (descriptor order) */
static void mk_args(devos_value_t *a, const char *method, const char *url, int64_t timeout, int64_t max_body)
{
    a[0] = vstr(method);
    a[1] = vstr(url);
    a[2] = vdur(timeout);
    a[3] = vint(max_body);
    a[4] = vstr("");
    a[5] = vstr("");
}

/* Poll to a final state (up to ~3 s). Returns the state. */
static devos_action_state_t run(devos_action_handle_t h, devos_action_result_t *res)
{
    devos_action_state_t st = DEVOS_ACT_PENDING;
    for (int i = 0; i < 60 && st == DEVOS_ACT_PENDING; i++) {
        usleep(50 * 1000);
        if (devos_action_poll(h, &st, res) != DEVOS_OK) break;
    }
    return st;
}

int main(void)
{
    pthread_t srv;
    pthread_create(&srv, NULL, server_thread, NULL);
    while (!s_port) usleep(10 * 1000);

    jobs_http_register();
    CHECK(devos_actions_find("http.request") != NULL);

    char url[64];
    devos_value_t a[6];
    devos_action_result_t res;
    devos_action_handle_t h;

    /* 200: transport succeeded and the body came through */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/ok", s_port);
    mk_args(a, "GET", url, 3000, 16384);
    devos_action_args_t args = { .args = a, .arg_count = 6 };
    CHECK(devos_action_start("http.request", &args, NULL, &h) == DEVOS_OK);
    CHECK(devos_actions_outstanding() == 1);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.out_count == 6 && res.outs[0].v.b == true && res.outs[1].v.i == 200);
    CHECK(res.outs[2].v.str.len == 5 && strncmp(res.outs[2].v.str.s, "hello", 5) == 0);
    CHECK(res.outs[3].v.b == false);
    devos_action_release(h);
    CHECK(devos_actions_outstanding() == 0);
    devos_action_release(h);                              /* stale: no-op */
    CHECK(devos_actions_outstanding() == 0);

    /* 503: a completed HTTP exchange, not a transport failure */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/fail", s_port);
    mk_args(a, "GET", url, 3000, 16384);
    CHECK(devos_action_start("http.request", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.outs[0].v.b == true && res.outs[1].v.i == 503);
    devos_action_release(h);

    /* truncation is reported, not hidden */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/big", s_port);
    mk_args(a, "GET", url, 3000, 100);
    CHECK(devos_action_start("http.request", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.outs[0].v.b == true && res.outs[1].v.i == 200 && res.outs[3].v.b == true);
    devos_action_release(h);

    /* a bare integer max_body (NUM) is honoured, not ignored for the default */
    mk_args(a, "GET", url, 3000, 100);
    a[3].type = DEVOS_VAL_NUM; a[3].v.n = 100.0;
    CHECK(devos_action_start("http.request", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.outs[0].v.b == true && res.outs[3].v.b == true);
    devos_action_release(h);

    /* redirects are followed (req.max_redirects defaults to 5) */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/redirect", s_port);
    mk_args(a, "GET", url, 3000, 4096);
    CHECK(devos_action_start("http.request", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.outs[1].v.i == 200 && res.outs[2].v.str.len == 5);
    devos_action_release(h);

    /* a closed port: the transport failure still binds a non-empty error output */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/ok", closed_port());
    mk_args(a, "GET", url, 3000, 16384);
    CHECK(devos_action_start("http.request", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.outs[0].v.b == false && res.outs[1].v.i == 0);
    CHECK(res.outs[5].v.str.len > 0);
    devos_action_release(h);

    /* a bearer token too long for the header is refused, never truncated into
     * a wrong Authorization line */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/ok", s_port);
    mk_args(a, "GET", url, 3000, 16384);
    char bigtok[320];
    memset(bigtok, 'x', sizeof(bigtok) - 1);
    bigtok[sizeof(bigtok) - 1] = '\0';
    a[4] = vstr(bigtok);
    CHECK(devos_action_start("http.request", &args, NULL, &h) == DEVOS_ERR_INVALID_ARG);

    /* a transport timeout is ok=false, status=0, with an error */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/slow", s_port);
    mk_args(a, "GET", url, 300, 4096);
    CHECK(devos_action_start("http.request", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.outs[0].v.b == false && res.outs[1].v.i == 0);
    CHECK(res.outs[5].v.str.len > 0);
    devos_action_release(h);

    /* a bare integer timeout means milliseconds (arg coercion), not the 10 s
     * fallback: 300 against the 1.2 s endpoint must time out */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/slow", s_port);
    mk_args(a, "GET", url, 300, 4096);
    a[2].type = DEVOS_VAL_INT; a[2].v.i = 300;
    CHECK(devos_action_start("http.request", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.outs[0].v.b == false && res.outs[1].v.i == 0);
    devos_action_release(h);

    /* admission: at most two Jobs HTTP tickets */
    devos_action_handle_t h1, h2, h3;
    mk_args(a, "GET", url, 5000, 4096);                   /* /slow keeps them busy */
    CHECK(devos_action_start("http.request", &args, NULL, &h1) == DEVOS_OK);
    CHECK(devos_action_start("http.request", &args, NULL, &h2) == DEVOS_OK);
    CHECK(devos_action_start("http.request", &args, NULL, &h3) == DEVOS_ERR_INVALID_STATE);
    devos_action_release(h1);
    devos_action_release(h2);
    CHECK(devos_actions_outstanding() == 0);

    /* If the underlying HTTP job slot vanishes (here cancelled out from under
     * the provider), the FAILED result still binds the declared error output
     * and a real message instead of the runtime's bare "action failed". */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/slow", s_port);
    mk_args(a, "GET", url, 5000, 4096);
    CHECK(devos_action_start("http.request", &args, NULL, &h) == DEVOS_OK);
    for (int j = 1; j <= 512; j++) devos_http_cancel(j);
    memset(&res, 0, sizeof(res));
    {
        devos_action_state_t st = DEVOS_ACT_PENDING;
        for (int i = 0; i < 20 && st == DEVOS_ACT_PENDING; i++) {
            usleep(20 * 1000);
            if (devos_action_poll(h, &st, &res) != DEVOS_OK) break;
        }
        CHECK(st == DEVOS_ACT_FAILED);
        CHECK(res.out_count == 6 && res.outs[5].v.str.len > 0);
        CHECK(res.error[0] != '\0');
    }
    devos_action_release(h);

    /* cancel a running request, then release */
    CHECK(devos_action_start("http.request", &args, NULL, &h) == DEVOS_OK);
    CHECK(devos_action_cancel(h) == DEVOS_OK);
    devos_action_release(h);
    CHECK(devos_actions_outstanding() == 0);

    s_stop = 1;
    pthread_join(srv, NULL);
    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}
