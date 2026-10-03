/* devos_netdiag: a request-specific one-shot ICMP probe for Jobs. It reuses
 * the shared nd_icmp_* helpers but owns its socket, id and deadline, so it
 * never touches the Network app's ping singleton. One probe at a time. */
#include "nd_int.h"
#include "devos_net.h"

typedef struct {
    nd_mutex_t mx;
    uint32_t gen;                    /* handle; bumped on each submit */
    int state;                       /* 0 idle, 1 running, 2 done, 3 failed */
    char host[64];
    int timeout_ms;
    devos_probe_result_t res;
    volatile bool stop;
    volatile bool task;
} probe_t;

static probe_t PB = { .mx = ND_MUTEX_INIT };

static void probe_task(void *arg)
{
    (void)arg;
    devos_probe_result_t r;
    memset(&r, 0, sizeof(r));
    r.latency_ms = -1;

    char ip[48];
    if (devos_net_resolve(PB.host, ip, sizeof(ip)) != 0) {
        snprintf(r.error, sizeof(r.error), "Couldn't resolve %.40s", PB.host);
        goto done;
    }
    snprintf(r.ip, sizeof(r.ip), "%s", ip);
    struct in_addr a;
    inet_aton(ip, &a);
    int sock = nd_icmp_socket();
    if (sock < 0) {
        snprintf(r.error, sizeof(r.error), "Can't open an ICMP socket (errno %d)", errno);
        goto done;
    }
    devos_net_socket_route(sock, a.s_addr);
    uint16_t id = (uint16_t)(nd_ms() ^ 0x9e37);
    uint16_t seq = 1;
    int64_t t0 = nd_ms();
    if (nd_icmp_send(sock, a.s_addr, id, seq, 32) != 0) {
        snprintf(r.error, sizeof(r.error), "Send failed (errno %d) - no route?", errno);
        close(sock);
        goto done;
    }
    int64_t end = t0 + PB.timeout_ms;
    for (;;) {
        if (PB.stop) { snprintf(r.error, sizeof(r.error), "cancelled"); break; }
        int left = (int)(end - nd_ms());
        if (left <= 0) { snprintf(r.error, sizeof(r.error), "timed out"); break; }
        uint32_t from;
        uint16_t rs, rid;
        int ttl = -1;
        int rc = nd_icmp_recv(sock, left < 200 ? left : 200, &from, &rs, &rid, &ttl);
        if (rc < 0) { snprintf(r.error, sizeof(r.error), "recv failed"); break; }
        if (rc == 0) continue;
        if (from != a.s_addr || rs != seq) continue;
#ifdef ESP_PLATFORM
        if (rid != id) continue;
#endif
        int lat = (int)(nd_ms() - t0);
        r.ok = true;
        r.latency_ms = lat < 1 ? 1 : lat;
        break;
    }
    close(sock);

done:
    nd_lock(&PB.mx);
    PB.res = r;
    PB.state = r.ok ? 2 : 3;
    PB.task = false;
    nd_unlock(&PB.mx);
}

int devos_probe_submit(const char *host, int timeout_ms)
{
    if (!host || !host[0]) return -1;
    nd_lock(&PB.mx);
    if (PB.task) { nd_unlock(&PB.mx); return -2; }
    memset(&PB.res, 0, sizeof(PB.res));
    snprintf(PB.host, sizeof(PB.host), "%s", host);
    PB.timeout_ms = timeout_ms < 200 ? 200 : timeout_ms > 30000 ? 30000 : timeout_ms;
    PB.stop = false;
    PB.state = 1;
    PB.gen++;
    if (PB.gen == 0) PB.gen++;
    PB.task = true;
    uint32_t handle = PB.gen;
    nd_unlock(&PB.mx);
    if (nd_spawn(probe_task, NULL, "probe", 4096) != 0) {
        nd_lock(&PB.mx);
        PB.task = false;
        PB.state = 3;
        snprintf(PB.res.error, sizeof(PB.res.error), "Couldn't start the probe task");
        nd_unlock(&PB.mx);
        return -1;
    }
    return (int)handle;
}

int devos_probe_poll(int handle, int *state, devos_probe_result_t *out)
{
    nd_lock(&PB.mx);
    if ((uint32_t)handle != PB.gen || handle <= 0) { nd_unlock(&PB.mx); return -1; }
    int st = PB.state == 1 ? 0 : PB.state == 2 ? 1 : PB.state == 3 ? 2 : 0;
    if (state) *state = st;
    if (out) *out = PB.res;
    nd_unlock(&PB.mx);
    return 0;
}

void devos_probe_cancel(int handle)
{
    nd_lock(&PB.mx);
    if ((uint32_t)handle == PB.gen) PB.stop = true;
    nd_unlock(&PB.mx);
}

void devos_probe_release(int handle)
{
    nd_lock(&PB.mx);
    if ((uint32_t)handle == PB.gen && !PB.task) PB.state = 0;
    nd_unlock(&PB.mx);
}

void nd_probe_init(void) { nd_mutex_create(&PB.mx); }
