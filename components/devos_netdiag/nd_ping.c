/* devos_netdiag: ICMP echo (ping). */
#include "nd_int.h"
#include "devos_net.h"

#include <math.h>

/* ------------------------------------------------------------------ ICMP */
static uint16_t csum(const void *data, int len)
{
    const uint8_t *p = data;
    uint32_t sum = 0;
    for (; len > 1; len -= 2, p += 2) sum += (uint32_t)(p[0] << 8 | p[1]);
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

int nd_icmp_socket(void)
{
    int s;
#ifdef ESP_PLATFORM
    s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
#else
    /* Linux "ping sockets" (no root needed, net.ipv4.ping_group_range) */
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (s < 0) s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
#endif
    return s;
}

int nd_icmp_send(int sock, uint32_t ip, uint16_t id, uint16_t seq, int size)
{
    uint8_t pkt[1480];
    if (size < 0) size = 0;
    if (size > (int)sizeof(pkt) - 8) size = (int)sizeof(pkt) - 8;
    memset(pkt, 0, (size_t)size + 8);
    pkt[0] = 8;                             /* echo request */
    pkt[4] = (uint8_t)(id >> 8);
    pkt[5] = (uint8_t)id;
    pkt[6] = (uint8_t)(seq >> 8);
    pkt[7] = (uint8_t)seq;
    for (int i = 0; i < size; i++) pkt[8 + i] = (uint8_t)(0x20 + i % 64);
    uint16_t c = csum(pkt, size + 8);
    pkt[2] = (uint8_t)(c >> 8);
    pkt[3] = (uint8_t)c;
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = ip;
    return sendto(sock, pkt, (size_t)size + 8, 0, (struct sockaddr *)&to, sizeof(to)) < 0 ? -1 : 0;
}

int nd_icmp_recv(int sock, int timeout_ms, uint32_t *ip, uint16_t *seq, uint16_t *id, int *ttl)
{
    int64_t end = nd_ms() + timeout_ms;
    for (;;) {
        int left = (int)(end - nd_ms());
        if (left <= 0) return 0;
        int r = nd_wait_readable(sock, left);
        if (r <= 0) return r;
        uint8_t buf[1600];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = (int)recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 8) continue;
        const uint8_t *icmp = buf;
        int t = -1;
        if ((buf[0] >> 4) == 4 && n >= 28) {    /* raw socket: IP header first */
            int ihl = (buf[0] & 15) * 4;
            t = buf[8];
            icmp = buf + ihl;
            n -= ihl;
            if (n < 8) continue;
        }
        if (icmp[0] != 0) continue;             /* not an echo reply */
        if (ip) *ip = from.sin_addr.s_addr;
        if (id) *id = (uint16_t)(icmp[4] << 8 | icmp[5]);
        if (seq) *seq = (uint16_t)(icmp[6] << 8 | icmp[7]);
        if (ttl) *ttl = t;
        return 1;
    }
}

/* ------------------------------------------------------------------ engine */
static struct {
    nd_mutex_t mx;
    devos_ping_stats_t st;
    float hist[DEVOS_PING_HIST];
    int hist_n, hist_head;
    uint32_t gen;
    volatile bool stop, task;
    int count, interval, size;
    double sum, jsum;
    int jn;
    float prev;
} P = { .mx = ND_MUTEX_INIT };

static void hist_push(float v)
{
    P.hist[P.hist_head] = v;
    P.hist_head = (P.hist_head + 1) % DEVOS_PING_HIST;
    if (P.hist_n < DEVOS_PING_HIST) P.hist_n++;
}

static void ping_task(void *arg)
{
    (void)arg;
    char ip[48];
    devos_ping_stats_t *st = &P.st;
    if (devos_net_resolve(st->target, ip, sizeof(ip)) != 0) {
        nd_lock(&P.mx);
        snprintf(st->error, sizeof(st->error), "Couldn't resolve %.60s", st->target);
        st->running = false;
        P.gen++;
        nd_unlock(&P.mx);
        P.task = false;
        return;
    }
    struct in_addr a;
    inet_aton(ip, &a);
    int sock = nd_icmp_socket();
    if (sock < 0) {
        nd_lock(&P.mx);
        snprintf(st->error, sizeof(st->error), "Can't open an ICMP socket (errno %d)", errno);
        st->running = false;
        P.gen++;
        nd_unlock(&P.mx);
        P.task = false;
        return;
    }
    devos_net_socket_route(sock, a.s_addr);
    nd_lock(&P.mx);
    snprintf(st->ip, sizeof(st->ip), "%.15s", ip);
    P.gen++;
    nd_unlock(&P.mx);
    uint16_t id = (uint16_t)(nd_ms() ^ 0x5a17);
    int wait = P.interval < 1000 ? 1000 : P.interval > 2000 ? 2000 : P.interval;
    for (uint16_t seq = 1; !P.stop && (!P.count || seq <= P.count); seq++) {
        int64_t t0 = nd_ms();
        float rtt = -1;
        int ttl = -1;
        if (nd_icmp_send(sock, a.s_addr, id, seq, P.size) == 0) {
            int64_t end = t0 + wait;
            while (!P.stop) {
                uint32_t from;
                uint16_t rs, rid;
                int left = (int)(end - nd_ms());
                if (left <= 0) break;
                int r = nd_icmp_recv(sock, left < 200 ? left : 200, &from, &rs, &rid, &ttl);
                if (r < 0) break;
                if (r == 0) continue;
                /* raw sockets see every reply: match ours (ping sockets rewrite the id) */
                if (from != a.s_addr || rs != seq) continue;
#ifdef ESP_PLATFORM
                if (rid != id) continue;
#endif
                rtt = (float)(nd_ms() - t0);
                if (rtt < 0.1f) rtt = 0.1f;
                break;
            }
        } else {
            nd_lock(&P.mx);
            snprintf(st->error, sizeof(st->error), "Send failed (errno %d) - no route?", errno);
            nd_unlock(&P.mx);
        }
        if (P.stop) break;
        nd_lock(&P.mx);
        st->sent++;
        st->last_ms = rtt;
        if (rtt >= 0) {
            st->received++;
            st->ttl = ttl;
            P.sum += rtt;
            if (st->received == 1 || rtt < st->min_ms) st->min_ms = rtt;
            if (rtt > st->max_ms) st->max_ms = rtt;
            st->avg_ms = (float)(P.sum / st->received);
            if (P.prev >= 0) {
                P.jsum += fabs(rtt - P.prev);
                P.jn++;
                st->jitter_ms = (float)(P.jsum / P.jn);
            }
            P.prev = rtt;
            if (st->error[0] && !strncmp(st->error, "Send", 4)) st->error[0] = '\0';
        }
        hist_push(rtt);
        P.gen++;
        nd_unlock(&P.mx);
        int64_t next = t0 + P.interval;
        while (!P.stop && nd_ms() < next) nd_sleep_ms((int)(next - nd_ms()) < 100 ? (int)(next - nd_ms()) : 100);
    }
    close(sock);
    nd_lock(&P.mx);
    st->running = false;
    P.gen++;
    nd_unlock(&P.mx);
    P.task = false;
}

int devos_ping_start(const char *host, int count, int interval_ms, int size)
{
    if (!host || !host[0]) return -1;
    devos_ping_stop();
    for (int i = 0; i < 100 && P.task; i++) nd_sleep_ms(20);
    if (P.task) return -1;
    nd_lock(&P.mx);
    memset(&P.st, 0, sizeof(P.st));
    snprintf(P.st.target, sizeof(P.st.target), "%s", host);
    P.st.size = size < 0 ? 0 : size > 1400 ? 1400 : size;
    P.st.running = true;
    P.st.last_ms = -1;
    P.st.ttl = -1;
    P.hist_n = P.hist_head = 0;
    P.count = count < 0 ? 0 : count;
    P.interval = interval_ms < 200 ? 200 : interval_ms > 10000 ? 10000 : interval_ms;
    P.size = P.st.size;
    P.sum = P.jsum = 0;
    P.jn = 0;
    P.prev = -1;
    P.stop = false;
    P.task = true;
    P.gen++;
    nd_unlock(&P.mx);
    if (nd_spawn(ping_task, NULL, "ping", 4096) != 0) {
        P.task = false;
        P.st.running = false;
        snprintf(P.st.error, sizeof(P.st.error), "Couldn't start the ping task");
        return -1;
    }
    return 0;
}

void devos_ping_stop(void) { P.stop = true; }

void devos_ping_stats(devos_ping_stats_t *out)
{
    nd_lock(&P.mx);
    *out = P.st;
    nd_unlock(&P.mx);
}

int devos_ping_history(float *out, int max)
{
    nd_lock(&P.mx);
    int n = P.hist_n < max ? P.hist_n : max;
    int start = (P.hist_head - n + DEVOS_PING_HIST) % DEVOS_PING_HIST;
    for (int i = 0; i < n; i++) out[i] = P.hist[(start + i) % DEVOS_PING_HIST];
    nd_unlock(&P.mx);
    return n;
}

uint32_t devos_ping_generation(void)
{
    nd_lock(&P.mx);
    uint32_t g = P.gen;
    nd_unlock(&P.mx);
    return g;
}

void nd_ping_init(void) { nd_mutex_create(&P.mx); }
