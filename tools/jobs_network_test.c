/* Host test for the Jobs network providers (main/jobs_providers/jobs_network.c)
 * and the request-specific netdiag engines: network.wol with a per-call result,
 * and network.dns over the per-ticket DNS contexts. A fake DNS server on
 * loopback answers A queries (1.2.3.4) and NXDOMAIN, and a UI singleton lookup
 * runs concurrently with a Jobs lookup to prove neither clobbers the other.
 *
 *   gcc -O2 -I$REPO/main/jobs_providers -I$REPO/components/devos_actions \
 *       -I$REPO/components/devos_err -I$REPO/components/devos_netdiag \
 *       -I$REPO/components/devos_net -I$REPO/components/devos_config/include \
 *       -I$REPO/components/devos_jobs -I$REPO/components/devos_tailnet \
 *       $REPO/tools/jobs_network_test.c $REPO/main/jobs_providers/jobs_network.c \
 *       $REPO/components/devos_netdiag/nd_dns.c $REPO/components/devos_netdiag/nd_wol.c \
 *       $REPO/components/devos_netdiag/nd_common.c $REPO/components/devos_actions/devos_actions.c \
 *       $REPO/components/devos_net/devos_net.c -lpthread -o /tmp/jobs_network_test && /tmp/jobs_network_test
 */
#include "jobs_providers.h"
#include "devos_actions.h"
#include "devos_netdiag.h"

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

/* The ICMP probe engine is not needed here; stub its symbols so the ping
 * provider links without the whole ping stack. */
int devos_probe_submit(const char *host, int timeout_ms) { (void)host; (void)timeout_ms; return -1; }
int devos_probe_poll(int handle, int *state, devos_probe_result_t *out) { (void)handle; (void)state; (void)out; return -1; }
void devos_probe_cancel(int handle) { (void)handle; }
void devos_probe_release(int handle) { (void)handle; }

void nd_dns_init(void);      /* internal init; avoids the whole netdiag stack */
void nd_wol_init(void);

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int s_dns_port;
static volatile int s_dns_stop;

static void *dns_server(void *arg)
{
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0 };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (struct sockaddr *)&a, sizeof(a));
    socklen_t al = sizeof(a);
    getsockname(s, (struct sockaddr *)&a, &al);
    s_dns_port = ntohs(a.sin_port);
    uint8_t buf[1024];
    while (!s_dns_stop) {
        struct timeval tv = { 0, 200000 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = (int)recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        int o = 12;
        char name[128];
        size_t w = 0;
        while (o < n && buf[o]) {
            int l = buf[o++];
            if (l > 63 || o + l > n) { w = 0; break; }
            if (w) name[w++] = '.';
            for (int i = 0; i < l && w < sizeof(name) - 1; i++) name[w++] = (char)buf[o + i];
            o += l;
        }
        if (w == 0 || o >= n) continue;
        name[w] = '\0';
        if (strcmp(name, "slow.test") == 0) usleep(400 * 1000);
        o++;                                            /* the root label */
        int qend = o + 4;
        if (qend > n) continue;
        bool nx = strcmp(name, "nx.test") == 0;
        uint8_t out[512];
        int m = 0;
        out[m++] = buf[0];
        out[m++] = buf[1];
        out[m++] = 0x81;                                /* QR | RD */
        out[m++] = nx ? 0x83 : 0x80;                    /* RA | rcode */
        out[m++] = 0; out[m++] = 1;                     /* qdcount */
        out[m++] = 0; out[m++] = nx ? 0 : 1;            /* ancount */
        out[m++] = 0; out[m++] = 0; out[m++] = 0; out[m++] = 0;
        memcpy(out + m, buf + 12, (size_t)(qend - 12)); /* the question */
        m += qend - 12;
        if (!nx) {
            out[m++] = 0xC0; out[m++] = 0x0C;           /* name -> qname */
            out[m++] = 0; out[m++] = 1;                 /* type A */
            out[m++] = 0; out[m++] = 1;                 /* class IN */
            out[m++] = 0; out[m++] = 0; out[m++] = 0; out[m++] = 60;
            out[m++] = 0; out[m++] = 4;                 /* rdlength */
            out[m++] = 1; out[m++] = 2; out[m++] = 3; out[m++] = 4;
        }
        sendto(s, out, (size_t)m, 0, (struct sockaddr *)&from, fl);
    }
    close(s);
    return NULL;
}

static devos_value_t vstr(const char *s) { devos_value_t v; v.type = DEVOS_VAL_STR; v.v.str.s = s; v.v.str.len = (uint32_t)strlen(s); return v; }

static devos_action_state_t run(devos_action_handle_t h, devos_action_result_t *res)
{
    devos_action_state_t st = DEVOS_ACT_PENDING;
    for (int i = 0; i < 250 && st == DEVOS_ACT_PENDING; i++) {
        usleep(20 * 1000);
        if (devos_action_poll(h, &st, res) != DEVOS_OK) break;
    }
    return st;
}

int main(void)
{
    pthread_t srv;
    pthread_create(&srv, NULL, dns_server, NULL);
    while (!s_dns_port) usleep(5 * 1000);

    nd_dns_init();
    nd_wol_init();
    jobs_network_register();
    CHECK(devos_actions_find("network.wol") != NULL);
    CHECK(devos_actions_find("network.dns") != NULL);

    devos_action_args_t args;
    devos_action_handle_t h;
    devos_action_result_t res;

    /* WoL: a per-call result, sent to the given address */
    devos_value_t w[2] = { vstr("aa:bb:cc:dd:ee:ff"), vstr("127.0.0.1") };
    args.args = w; args.arg_count = 2;
    CHECK(devos_action_start("network.wol", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.out_count == 3 && res.outs[0].v.b == true);
    CHECK(strcmp(res.outs[1].v.str.s, "127.0.0.1:9") == 0 && res.outs[2].v.str.len == 0);
    devos_action_release(h);

    /* a malformed MAC is refused; a hostname target is accepted and resolved
     * on a worker (never the scheduler) */
    devos_value_t bad[2] = { vstr("not-a-mac"), vstr("127.0.0.1") };
    args.args = bad;
    CHECK(devos_action_start("network.wol", &args, NULL, &h) == DEVOS_ERR_INVALID_ARG);
    devos_value_t host[2] = { vstr("aa:bb:cc:dd:ee:ff"), vstr("localhost") };
    args.args = host;
    CHECK(devos_action_start("network.wol", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);        /* sent depends on the resolver */
    devos_action_release(h);

    /* DNS A query against the fake server */
    char srvip[48];
    snprintf(srvip, sizeof(srvip), "127.0.0.1:%d", s_dns_port);
    devos_value_t d[3] = { vstr("example.test"), vstr(srvip), vstr("A") };
    args.args = d;
    CHECK(devos_action_start("network.dns", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.out_count == 5);
    CHECK(res.outs[0].v.b == true);
    CHECK(strcmp(res.outs[1].v.str.s, "NOERROR") == 0);
    CHECK(res.outs[2].v.i == 1 && strcmp(res.outs[3].v.str.s, "1.2.3.4") == 0);
    devos_action_release(h);

    /* NXDOMAIN is a completed negative result, not a transport failure */
    devos_value_t nx[3] = { vstr("nx.test"), vstr(srvip), vstr("A") };
    args.args = nx;
    CHECK(devos_action_start("network.dns", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.outs[0].v.b == false && strcmp(res.outs[1].v.str.s, "NXDOMAIN") == 0 && res.outs[2].v.i == 0);
    devos_action_release(h);

    /* Jobs DNS admission: one lookup at a time (the engine pool is separate
     * from the UI's singleton) */
    devos_value_t sl[3] = { vstr("slow.test"), vstr(srvip), vstr("A") };
    args.args = sl;
    CHECK(devos_action_start("network.dns", &args, NULL, &h) == DEVOS_OK);
    devos_action_handle_t h2;
    devos_value_t ex[3] = { vstr("example.test"), vstr(srvip), vstr("A") };
    args.args = ex;
    CHECK(devos_action_start("network.dns", &args, NULL, &h2) == DEVOS_ERR_INVALID_STATE);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE && res.outs[0].v.b == true);
    devos_action_release(h);

    /* The UI singleton and a Jobs context run at the same time without either
     * clobbering the other's result. */
    CHECK(devos_dns_lookup_start(srvip, "ui.test", DEVOS_DNS_A) == 0);
    devos_value_t dj[3] = { vstr("jobs.test"), vstr(srvip), vstr("A") };
    args.args = dj;
    CHECK(devos_action_start("network.dns", &args, NULL, &h) == DEVOS_OK);
    memset(&res, 0, sizeof(res));
    CHECK(run(h, &res) == DEVOS_ACT_DONE);
    CHECK(res.outs[0].v.b == true && strcmp(res.outs[3].v.str.s, "1.2.3.4") == 0);
    devos_action_release(h);
    devos_dns_result_t ur;
    for (int i = 0; i < 250; i++) {
        devos_dns_lookup_result(&ur);
        if (!ur.busy) break;
        usleep(20 * 1000);
    }
    CHECK(!ur.busy && strstr(ur.query, "ui.test") != NULL && ur.rcode == 0 && ur.n == 1);

    s_dns_stop = 1;
    pthread_join(srv, NULL);
    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}