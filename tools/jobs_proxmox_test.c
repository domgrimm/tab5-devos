/* Host end-to-end test for the Proxmox Jobs providers
 * (main/jobs_providers/jobs_proxmox.c) and the request-specific devos_proxmox
 * contract, driven against a fake Proxmox VE API over loopback. Covers: a guest
 * status read by vmid (the node and type resolved from the cluster resources),
 * a guest that is not in the cluster (a real failure, not an unknown outcome),
 * start accepted (a UPID, not a running guest), a transport error on a mutation
 * being outcome-unknown, two commands never overwriting, a config snapshot
 * surviving a later settings change, queue saturation, the UI action note,
 * cancellation before and after the command was sent, and release-exactly-once.
 *
 * Run from an ISOLATED CWD (the engine writes ./sim_sdcard):
 *
 *   mkdir -p /tmp/jobs_proxmox_test && cd /tmp/jobs_proxmox_test
 *   gcc -O2 -I$REPO/main/jobs_providers -I$REPO/components/devos_actions \
 *       -I$REPO/components/devos_err -I$REPO/components/devos_proxmox \
 *       -I$REPO/components/devos_http -I$REPO/components/devos_net \
 *       -I$REPO/components/devos_json -I$REPO/components/devos_config/include \
 *       -I$REPO/components/devos_jobs -I$REPO/components/devos_tailnet \
 *       $REPO/tools/jobs_proxmox_test.c $REPO/main/jobs_providers/jobs_proxmox.c \
 *       $REPO/components/devos_proxmox/devos_proxmox.c \
 *       $REPO/components/devos_jobs/jobs_model.c $REPO/components/devos_jobs/jobs_parse.c \
 *       $REPO/components/devos_jobs/jobs_validate.c $REPO/components/devos_jobs/jobs_serialize.c \
 *       $REPO/components/devos_actions/devos_actions.c \
 *       $REPO/components/devos_http/devos_http.c $REPO/components/devos_net/devos_net.c \
 *       $REPO/components/devos_json/devos_json.c -lpthread -o /tmp/jobs_proxmox_test && /tmp/jobs_proxmox_test
 */
#include "jobs_providers.h"
#include "devos_actions.h"
#include "devos_proxmox.h"
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

/* An app-enablement gate (the Proxmox app switched off). */
static bool gate_off(char *reason, size_t cap) { snprintf(reason, cap, "Proxmox app off"); return false; }

static int s_port;
static volatile int s_stop;
static volatile int s_slow_ms;           /* delay applied to vmid 9999's actions */
static volatile int s_auth_bad;
static volatile int s_empty_cluster;     /* /cluster/resources answers 200 with [] */
static char s_paths[64][160];
static volatile int s_npaths;

/* What Proxmox actually sends from /cluster/resources?type=vm: the full field
 * set (including pool/tags/maxcpu and a node entry that must be filtered out),
 * with whitespace after the separators - a stricter parse than the compact
 * form pveproxy usually emits. */
static const char *GUESTS_JSON =
    "{\n"
    "  \"data\" : [\n"
    "    { \"id\" : \"qemu/100\", \"type\" : \"qemu\", \"vmid\" : 100, \"node\" : \"pve\", \"pool\" : \"\",\n"
    "      \"name\" : \"web\", \"status\" : \"running\", \"template\" : 0, \"tags\" : \"prod\",\n"
    "      \"cpu\" : 0.04, \"mem\" : 2147483648, \"maxmem\" : 4294967296, \"maxcpu\" : 4,\n"
    "      \"disk\" : 10737418240, \"maxdisk\" : 21474836480, \"diskread\" : 0, \"diskwrite\" : 0,\n"
    "      \"netin\" : 0, \"netout\" : 0, \"uptime\" : 86400 },\n"
    "    { \"id\" : \"lxc/101\", \"type\" : \"lxc\", \"vmid\" : 101, \"node\" : \"pve\", \"pool\" : \"\",\n"
    "      \"name\" : \"db\", \"status\" : \"stopped\", \"template\" : 0, \"maxcpu\" : 2,\n"
    "      \"cpu\" : 0, \"mem\" : 0, \"maxmem\" : 1073741824,\n"
    "      \"disk\" : 0, \"maxdisk\" : 8589934592, \"uptime\" : 0 },\n"
    "    { \"id\" : \"qemu/9999\", \"type\" : \"qemu\", \"vmid\" : 9999, \"node\" : \"pve\", \"name\" : \"slow\",\n"
    "      \"status\" : \"running\", \"cpu\" : 0, \"mem\" : 0, \"maxmem\" : 1073741824,\n"
    "      \"disk\" : 0, \"maxdisk\" : 8589934592, \"uptime\" : 0 },\n"
    "    { \"id\" : \"node/pve\", \"type\" : \"node\", \"node\" : \"pve\", \"status\" : \"online\" },\n"
    "    { \"id\" : \"storage/local\", \"type\" : \"storage\", \"node\" : \"pve\", \"storage\" : \"local\" }\n"
    "  ]\n"
    "}";

/* /nodes/<node>/qemu and /lxc: the same guests without a "type" field (it is
 * the endpoint) and without a "node" field. Used by the per-node fallback. */
static const char *NODE_QEMU_JSON =
    "{ \"data\" : [ { \"vmid\" : 100, \"name\" : \"web\", \"status\" : \"running\",\n"
    "                 \"cpu\" : 0.04, \"mem\" : 2147483648, \"maxmem\" : 4294967296, \"uptime\" : 86400 } ] }";
static const char *NODE_LXC_JSON =
    "{ \"data\" : [ { \"vmid\" : 101, \"name\" : \"db\", \"status\" : \"stopped\",\n"
    "                 \"cpu\" : 0, \"mem\" : 0, \"maxmem\" : 1073741824, \"uptime\" : 0 } ] }";

static const char *STATUS_JSON =
    "{ \"data\" : { \"status\" : \"running\", \"name\" : \"web\", \"cpu\" : 0.04,\n"
    "               \"mem\" : 2147483648, \"maxmem\" : 4294967296, \"uptime\" : 86400 } }";

static void log_path(const char *req)
{
    char *sp = strchr(req, ' ');
    if (!sp || sp[1] != '/') return;
    char *e = strchr(sp + 1, ' ');
    if (!e || s_npaths >= 64) return;
    size_t l = (size_t)(e - (sp + 1));
    if (l > 159) l = 159;
    memcpy(s_paths[s_npaths], sp + 1, l);
    s_paths[s_npaths][l] = '\0';
    s_npaths++;
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
        log_path(req);

        int code = 200;
        const char *body = NULL;
        /* The engine always sends the token header; a bad one is 401. */
        if (s_auth_bad || !strstr(req, "Authorization: PVEAPIToken=")) {
            code = 401;
            body = "{\"data\":null,\"errors\":{\"auth\":\"no ticket\"}}";
        } else if (strstr(req, "/cluster/resources")) {
            body = s_empty_cluster ? "{ \"data\" : [] }" : GUESTS_JSON;
        } else if (strstr(req, "/api2/json/nodes HTTP")) {
            body = "{\"data\":[{\"node\":\"pve\",\"status\":\"online\",\"cpu\":0.07,"
                   "\"mem\":9000000000,\"maxmem\":34359738368,\"uptime\":500000}]}";
        } else if (strstr(req, "/status/current")) {
            body = STATUS_JSON;
        } else if (strstr(req, "/status/start") || strstr(req, "/status/stop") ||
                   strstr(req, "/status/shutdown") || strstr(req, "/status/reboot")) {
            if (strstr(req, "/9999/") && s_slow_ms) usleep((useconds_t)s_slow_ms * 1000);
            body = "{\"data\":\"UPID:pve:0000ABCD:00000000:00000000:qemu:100:root@pam!devos:start:\"}";
        } else if (strstr(req, "/nodes/") && strstr(req, "/qemu")) {
            body = NODE_QEMU_JSON;          /* the per-node fallback list */
        } else if (strstr(req, "/nodes/") && strstr(req, "/lxc")) {
            body = NODE_LXC_JSON;
        } else {
            code = 501;
            body = "{\"data\":null,\"errors\":{\"path\":\"not found\"}}";
        }
        char hdr[256];
        int hn = snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                          code, strlen(body));
        send(fd, hdr, (size_t)hn, MSG_NOSIGNAL);
        send(fd, body, strlen(body), MSG_NOSIGNAL);
        close(fd);
    }
    close(ls);
    return NULL;
}

static devos_value_t vint(int64_t i) { devos_value_t v; v.type = DEVOS_VAL_INT; v.v.i = i; return v; }
static devos_value_t vdur(int64_t ms) { devos_value_t v; v.type = DEVOS_VAL_DURATION; v.v.ms = ms; return v; }

static devos_action_state_t run(devos_action_handle_t h, devos_action_result_t *res)
{
    devos_action_state_t st = DEVOS_ACT_PENDING;
    for (int i = 0; i < 250 && st == DEVOS_ACT_PENDING; i++) {
        usleep(20 * 1000);
        if (devos_action_poll(h, &st, res) != DEVOS_OK) break;
    }
    return st;
}

static bool wait_ticket(uint32_t t, devos_proxmox_req_result_t *out)
{
    for (int i = 0; i < 250; i++) {
        if (!devos_proxmox_request_poll(t, out)) return false;
        if (out->state != DEVOS_PROXMOX_REQ_PENDING) return true;
        usleep(20 * 1000);
    }
    return false;
}

int main(void)
{
    pthread_t srv;
    pthread_create(&srv, NULL, server_thread, NULL);
    while (!s_port) usleep(10 * 1000);

    jobs_proxmox_register();
    CHECK(devos_actions_find("proxmox.guest_status") != NULL);
    CHECK(devos_actions_find("proxmox.guest_start") != NULL);
    CHECK(devos_actions_find("proxmox.guest_stop") != NULL);
    CHECK(devos_actions_find("proxmox.guest_shutdown") != NULL);
    CHECK(devos_actions_find("proxmox.guest_reboot") != NULL);

    /* The documented Proxmox example parses, validates against the real
     * schemas and round-trips canonically. */
    static const char *SRC =
        "version 1;\n"
        "job \"Restart web\" {\n"
        "    trigger manual;\n"
        "    proxmox.guest_status(vmid: 100) as before;\n"
        "    proxmox.guest_shutdown(vmid: 100, timeout: 15s) as shut;\n"
        "    wait 20s;\n"
        "    proxmox.guest_start(vmid: 100) as up;\n"
        "    if !up.accepted {\n"
        "        proxmox.guest_status(vmid: 100) as after;\n"
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

    /* Before init, and before a config, the provider is unavailable. */
    char why[80] = "";
    CHECK(!devos_actions_available("proxmox.guest_status", why, sizeof(why)));
    CHECK(why[0] != '\0');

    devos_proxmox_init();
    CHECK(devos_proxmox_ready());
    CHECK(!devos_actions_available("proxmox.guest_status", why, sizeof(why)));
    CHECK(strstr(why, "not configured") != NULL);

    /* A gate (the Proxmox app switched off) overrides even a configured engine. */
    jobs_proxmox_set_gate(gate_off);
    why[0] = '\0';
    CHECK(!devos_actions_available("proxmox.guest_status", why, sizeof(why)));
    CHECK(strstr(why, "Proxmox app off") != NULL);
    jobs_proxmox_set_gate(NULL);

    devos_proxmox_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.url, sizeof(cfg.url), "http://127.0.0.1:%d", s_port);
    snprintf(cfg.token_id, sizeof(cfg.token_id), "root@pam!devos");
    snprintf(cfg.secret, sizeof(cfg.secret), "11111111-2222-3333-4444-555555555555");
    cfg.interval_s = 2;                     /* so the fallback test can wait for a refresh */
    devos_proxmox_set_config(&cfg);
    CHECK(devos_actions_available("proxmox.guest_status", why, sizeof(why)));

    devos_value_t a[2];
    devos_action_args_t args = { .args = a, .arg_count = 2 };
    devos_action_handle_t h;
    devos_action_result_t res;

    /* Guest status by vmid: the node and type are resolved from the cluster
     * resources, so the job never has to know them. */
    a[0] = vint(100);
    a[1] = vdur(5000);
    CHECK(devos_action_start("proxmox.guest_status", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.out_count == 11);
    CHECK(res.outs[0].v.b == true && res.outs[1].v.i == 200);
    CHECK(strcmp(res.outs[2].v.str.s, "running") == 0);
    CHECK(strcmp(res.outs[3].v.str.s, "pve") == 0);
    CHECK(strcmp(res.outs[4].v.str.s, "web") == 0);
    CHECK(strcmp(res.outs[5].v.str.s, "qemu") == 0);
    CHECK(res.outs[7].v.i == 2147483648LL);
    CHECK(res.outs[9].v.i == 86400);
    devos_action_release(h);
    CHECK(devos_actions_outstanding() == 0);
    devos_action_release(h);                               /* stale: no-op */
    CHECK(devos_actions_outstanding() == 0);

    /* A vmid that is not in the cluster is a real failure (nothing was sent),
     * not an outcome-unknown. */
    a[0] = vint(4242);
    a[1] = vdur(5000);
    CHECK(devos_action_start("proxmox.guest_status", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_FAILED);
    CHECK(strstr(res.error, "no guest with vmid 4242") != NULL);
    devos_action_release(h);

    /* Start: HTTP 200 with a UPID means Proxmox accepted the task, not that the
     * guest is running. */
    a[0] = vint(101);
    CHECK(devos_action_start("proxmox.guest_start", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.out_count == 5);
    CHECK(res.outs[0].v.i == 200 && res.outs[1].v.b == true);
    CHECK(strncmp(res.outs[2].v.str.s, "UPID:", 5) == 0);
    CHECK(res.outs[3].v.b == false);
    devos_action_release(h);

    /* Two commands never overwrite each other. */
    devos_proxmox_req_result_t r1, r2;
    uint32_t t1 = devos_proxmox_request(100, "start", 5000);
    uint32_t t2 = devos_proxmox_request(101, "shutdown", 5000);
    CHECK(t1 && t2 && t1 != t2);
    CHECK(wait_ticket(t1, &r1) && r1.state == DEVOS_PROXMOX_REQ_DONE && r1.status == 200);
    CHECK(wait_ticket(t2, &r2) && r2.state == DEVOS_PROXMOX_REQ_DONE && r2.status == 200);
    CHECK(strcmp(r1.action, "start") == 0 && strcmp(r2.action, "shutdown") == 0);
    devos_proxmox_request_release(t1);
    devos_proxmox_request_release(t2);

    /* A submitted request keeps its config snapshot: a later settings change
     * cannot redirect it to another host. */
    t1 = devos_proxmox_request(100, "status", 5000);
    devos_proxmox_config_t cfg2 = cfg;
    snprintf(cfg2.url, sizeof(cfg2.url), "http://127.0.0.1:9");
    devos_proxmox_set_config(&cfg2);
    CHECK(wait_ticket(t1, &r1) && r1.state == DEVOS_PROXMOX_REQ_DONE && r1.status == 200);
    CHECK(strcmp(r1.inspect.name, "web") == 0);
    devos_proxmox_request_release(t1);
    devos_proxmox_set_config(&cfg);

    /* The UI wrapper shows a note and frees its own ticket. */
    CHECK(devos_proxmox_action(100, "start") == 0);
    bool noted = false;
    for (int i = 0; i < 100 && !noted; i++) {
        devos_proxmox_status_t st;
        devos_proxmox_status(&st);
        noted = strstr(st.note, "start vmid 100 accepted") != NULL;
        if (!noted) usleep(20 * 1000);
    }
    CHECK(noted);

    /* Queue saturation returns an error instead of overwriting. */
    s_slow_ms = 300;
    uint32_t tickets[DEVOS_PROXMOX_REQS];
    int filled = 0;
    for (int i = 0; i < DEVOS_PROXMOX_REQS; i++) {
        tickets[i] = devos_proxmox_request(9999, "start", 20000);
        if (tickets[i]) filled++;
    }
    CHECK(filled == DEVOS_PROXMOX_REQS);
    CHECK(devos_proxmox_request(9999, "start", 5000) == 0);    /* full: rejected */
    for (int i = 0; i < filled; i++) devos_proxmox_request_release(tickets[i]);
    s_slow_ms = 0;
    usleep(600 * 1000);                                        /* let the worker drain */

    /* Cancel before the command is sent: a clean cancellation. */
    s_slow_ms = 800;
    a[0] = vint(9999);
    CHECK(devos_action_start("proxmox.guest_status", &args, NULL, &h) == DEVOS_OK);
    devos_action_cancel(h);
    {
        devos_action_state_t st = DEVOS_ACT_PENDING;
        devos_action_result_t cr;
        memset(&cr, 0, sizeof(cr));
        CHECK(devos_action_poll(h, &st, &cr) == DEVOS_OK);
        CHECK(st == DEVOS_ACT_CANCELLED);
    }
    devos_action_release(h);
    s_slow_ms = 0;
    usleep(800 * 1000);

    /* Cancel a mutation after it was handed to Proxmox: the task may be running,
     * so the provider reports UNKNOWN with outcome_unknown readable. */
    s_slow_ms = 800;
    a[0] = vint(9999);
    CHECK(devos_action_start("proxmox.guest_start", &args, NULL, &h) == DEVOS_OK);
    usleep(200 * 1000);                                        /* let the worker start it */
    CHECK(devos_action_cancel(h) == DEVOS_OK);
    {
        devos_action_state_t st = DEVOS_ACT_PENDING;
        devos_action_result_t cr;
        memset(&cr, 0, sizeof(cr));
        CHECK(devos_action_poll(h, &st, &cr) == DEVOS_OK);
        CHECK(st == DEVOS_ACT_UNKNOWN);
        CHECK(cr.out_count == 5 && cr.outs[3].v.b == true);
        CHECK(cr.outs[4].v.str.len > 0);
    }
    devos_action_release(h);
    s_slow_ms = 0;
    usleep(800 * 1000);

    /* Transport error on a mutation is outcome-unknown, never success. */
    devos_proxmox_config_t dead = cfg;
    snprintf(dead.url, sizeof(dead.url), "http://127.0.0.1:9");
    devos_proxmox_set_config(&dead);
    a[0] = vint(100);
    CHECK(devos_action_start("proxmox.guest_start", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.outs[0].v.i == 0 && res.outs[1].v.b == false && res.outs[3].v.b == true);
    CHECK(res.outs[4].v.str.len > 0);
    devos_action_release(h);
    devos_proxmox_set_config(&cfg);

    /* A bad token is an access-denied read failure, not a crash. */
    s_auth_bad = 1;
    a[0] = vint(100);
    CHECK(devos_action_start("proxmox.guest_status", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.outs[0].v.b == false && res.outs[1].v.i == 401);
    s_auth_bad = 0;

    /* UI polling path: set_active refreshes the list; a Jobs command still
     * completes alongside it. */
    devos_proxmox_set_active(true);
    usleep(400 * 1000);
    devos_proxmox_guest_t g[DEVOS_PROXMOX_MAX];
    int ng = devos_proxmox_list(g, DEVOS_PROXMOX_MAX);
    CHECK(ng == 3);
    bool saw_web = false, saw_lxc = false;
    for (int i = 0; i < ng; i++) {
        if (g[i].vmid == 100 && !strcmp(g[i].name, "web") && g[i].kind == DEVOS_PROXMOX_QEMU) saw_web = true;
        if (g[i].vmid == 101 && g[i].kind == DEVOS_PROXMOX_LXC) saw_lxc = true;
    }
    CHECK(saw_web && saw_lxc);
    devos_proxmox_node_t nd[DEVOS_PROXMOX_MAX_NODES];
    int nn = devos_proxmox_nodes(nd, DEVOS_PROXMOX_MAX_NODES);
    CHECK(nn == 1 && strcmp(nd[0].node, "pve") == 0 && strcmp(nd[0].status, "online") == 0);
    devos_proxmox_set_active(false);

    /* A token with Sys.Audit but not VM.Audit: Proxmox answers
     * /cluster/resources with 200 and an empty list rather than an error, so
     * the engine asks each node directly. The guests must still appear (and
     * carry their node and type, which those endpoints do not send). */
    s_empty_cluster = 1;
    devos_proxmox_set_active(true);
    for (int i = 0; i < 40 && devos_proxmox_list(g, DEVOS_PROXMOX_MAX) != 2; i++) usleep(100 * 1000);
    ng = devos_proxmox_list(g, DEVOS_PROXMOX_MAX);
    CHECK(ng == 2);
    bool f_web = false, f_db = false;
    for (int i = 0; i < ng; i++) {
        if (g[i].vmid == 100 && g[i].kind == DEVOS_PROXMOX_QEMU && !strcmp(g[i].node, "pve") &&
            !strcmp(g[i].name, "web")) f_web = true;
        if (g[i].vmid == 101 && g[i].kind == DEVOS_PROXMOX_LXC && !strcmp(g[i].node, "pve")) f_db = true;
    }
    CHECK(f_web && f_db);
    devos_proxmox_set_active(false);
    s_empty_cluster = 0;

    s_stop = 1;
    pthread_join(srv, NULL);
    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}
