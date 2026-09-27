/* devos_netdiag: TCP connect port scanner with an optional ping sweep. */
#include "nd_int.h"
#include "devos_net.h"

#include <ctype.h>

#ifndef ESP_PLATFORM
#include <fcntl.h>
#endif

#ifdef ESP_PLATFORM
#define POOL 6                  /* lwIP sockets are shared with Wi-Fi, SSH, MQTT, VPNs */
#else
#define POOL 48
#endif
#define MAX_PROBES 70000

static const struct { uint16_t port; const char *name; } SERVICES[] = {
    {21, "ftp"}, {22, "ssh"}, {23, "telnet"}, {25, "smtp"}, {53, "dns"}, {80, "http"}, {81, "http-alt"},
    {88, "kerberos"}, {110, "pop3"}, {111, "rpcbind"}, {135, "msrpc"}, {139, "netbios"}, {143, "imap"},
    {443, "https"}, {445, "smb"}, {465, "smtps"}, {548, "afp"}, {554, "rtsp"}, {587, "submission"},
    {631, "ipp"}, {873, "rsync"}, {993, "imaps"}, {995, "pop3s"}, {1080, "socks"}, {1433, "mssql"},
    {1883, "mqtt"}, {1900, "upnp"}, {2049, "nfs"}, {2375, "docker"}, {2376, "docker-tls"}, {3000, "web-dev"},
    {3306, "mysql"}, {3389, "rdp"}, {4000, "web"}, {5000, "upnp/web"}, {5001, "web-tls"}, {5353, "mdns"},
    {5432, "postgres"}, {5601, "kibana"}, {5900, "vnc"}, {6053, "esphome"}, {6379, "redis"}, {6443, "k8s-api"},
    {7878, "radarr"}, {8000, "http-alt"}, {8008, "http-alt"}, {8080, "http-proxy"}, {8081, "http-alt"},
    {8086, "influxdb"}, {8096, "jellyfin"}, {8123, "home-asst"}, {8384, "syncthing"}, {8443, "https-alt"},
    {8554, "rtsp-alt"}, {8883, "mqtts"}, {8888, "http-alt"}, {8989, "sonarr"}, {9000, "portainer"},
    {9090, "prometheus"}, {9091, "transmission"}, {9100, "printer"}, {9443, "portainer-tls"},
    {11434, "ollama"}, {19999, "netdata"}, {32400, "plex"}, {51820, "wireguard"},
};

const char *devos_scan_service(int port)
{
    for (unsigned i = 0; i < sizeof(SERVICES) / sizeof(SERVICES[0]); i++) {
        if (SERVICES[i].port == port) return SERVICES[i].name;
    }
    return NULL;
}

/* ------------------------------------------------------------------ parsing */
typedef void (*host_fn)(void *ud, uint32_t ip_host_order, const char *name);

static int parse_ip(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return -1;
    *out = a << 24 | b << 16 | c << 8 | d;
    return 0;
}

/* Walks the target list; returns the host count or -1 (err). With fn == NULL
 * nothing is resolved (names count as one host each). */
static int walk_targets(const char *spec, host_fn fn, void *ud, char *err, size_t errcap)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", spec ? spec : "");
    int total = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ", \t", &save); tok; tok = strtok_r(NULL, ", \t", &save)) {
        char *slash = strchr(tok, '/'), *dash = strchr(tok, '-');
        uint32_t ip;
        if (slash) {
            *slash = '\0';
            int bits = atoi(slash + 1);
            if (parse_ip(tok, &ip) || bits < 0 || bits > 32) {
                snprintf(err, errcap, "Bad subnet: %.40s", tok);
                return -1;
            }
            if (bits < 22) {
                snprintf(err, errcap, "/%d is too big: scan at most a /22 (1024 addresses)", bits);
                return -1;
            }
            uint32_t mask = bits ? 0xffffffffu << (32 - bits) : 0, net = ip & mask, size = ~mask + 1u;
            uint32_t first = net, last = net + size - 1;
            if (bits <= 30) { first++; last--; }           /* skip network + broadcast */
            for (uint32_t h = first; h <= last && total <= DEVOS_SCAN_MAX_HOSTS; h++) {
                if (fn) fn(ud, h, NULL);
                total++;
            }
        } else if (dash && (*dash = '\0', !parse_ip(tok, &ip))) {
            const char *rest = dash + 1;
            uint32_t last;
            if (strchr(rest, '.')) {
                if (parse_ip(rest, &last)) { snprintf(err, errcap, "Bad range end: %.40s", rest); return -1; }
            } else {
                int x = atoi(rest);
                if (x < 0 || x > 255) { snprintf(err, errcap, "Bad range end: %.40s", rest); return -1; }
                last = (ip & 0xffffff00u) | (uint32_t)x;
            }
            if (last < ip) { snprintf(err, errcap, "Range runs backwards"); return -1; }
            if (last - ip >= DEVOS_SCAN_MAX_HOSTS) { snprintf(err, errcap, "Range too big (max 1024)"); return -1; }
            for (uint32_t h = ip; h <= last; h++) {
                if (fn) fn(ud, h, NULL);
                total++;
            }
        } else if (dash && (*dash = '-', 0)) {
        } else if (!parse_ip(tok, &ip)) {
            if (fn) fn(ud, ip, NULL);
            total++;
        } else {
            for (const char *c = tok; *c; c++) {
                if (!isalnum((unsigned char)*c) && *c != '.' && *c != '-' && *c != '_') {
                    snprintf(err, errcap, "Not a host, IP, range or subnet: %.40s", tok);
                    return -1;
                }
            }
            if (fn) {
                char ips[48];
                if (devos_net_resolve(tok, ips, sizeof(ips)) == 0 && !parse_ip(ips, &ip)) fn(ud, ip, tok);
            }
            total++;
        }
        if (total > DEVOS_SCAN_MAX_HOSTS) {
            snprintf(err, errcap, "Too many hosts (max %d)", DEVOS_SCAN_MAX_HOSTS);
            return -1;
        }
    }
    if (!total) {
        snprintf(err, errcap, "Enter a host, IP, range (10.0.0.1-50) or subnet (10.0.0.0/24)");
        return -1;
    }
    return total;
}

int devos_scan_count_hosts(const char *targets, char *err, size_t errcap)
{
    char e[96] = "";
    int n = walk_targets(targets, NULL, NULL, e, sizeof(e));
    if (err && errcap) snprintf(err, errcap, "%s", e);
    return n;
}

/* Fills out (may be NULL) and returns the count, or -1. */
static int walk_ports(const char *spec, uint16_t *out, int max, char *err, size_t errcap)
{
    char buf[512];
    const char *s = spec ? spec : "";
    while (*s == ' ') s++;
    if (!*s || !strncasecmp(s, "common", 6)) s = DEVOS_SCAN_COMMON_PORTS;
    snprintf(buf, sizeof(buf), "%s", s);
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ", \t", &save); tok; tok = strtok_r(NULL, ", \t", &save)) {
        char *dash = strchr(tok, '-');
        int a = atoi(tok), b = dash ? atoi(dash + 1) : a;
        if (a < 1 || b > 65535 || b < a || !isdigit((unsigned char)tok[0])) {
            snprintf(err, errcap, "Bad port: %.30s", tok);
            return -1;
        }
        for (int p = a; p <= b; p++) {
            if (out && n < max) out[n] = (uint16_t)p;
            n++;
        }
    }
    if (!n) {
        snprintf(err, errcap, "Enter ports, e.g. 22,80,443 or 8000-8100 or common");
        return -1;
    }
    return n;
}

int devos_scan_count_ports(const char *ports, char *err, size_t errcap)
{
    char e[96] = "";
    int n = walk_ports(ports, NULL, 0, e, sizeof(e));
    if (err && errcap) snprintf(err, errcap, "%s", e);
    return n;
}

/* ------------------------------------------------------------------ engine */
static struct {
    nd_mutex_t mx;
    volatile bool stop, task;
    uint32_t gen;
    devos_scan_status_t st;
    devos_scan_host_t *hosts;           /* DEVOS_SCAN_MAX_HOSTS, PSRAM */
    int nhosts;
    uint16_t *ports;
    int nports;
    int timeout;
    bool ping_first;
    int64_t t0;
    char targets[256], portspec[512];
} S = { .mx = ND_MUTEX_INIT };

static void add_host(void *ud, uint32_t ip, const char *name)
{
    (void)ud;
    if (S.nhosts >= DEVOS_SCAN_MAX_HOSTS) return;
    for (int i = 0; i < S.nhosts; i++) {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255), (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255));
        if (!strcmp(S.hosts[i].ip, tmp)) return;
    }
    devos_scan_host_t *h = &S.hosts[S.nhosts++];
    memset(h, 0, sizeof(*h));
    snprintf(h->ip, sizeof(h->ip), "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255), (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255));
    if (name) snprintf(h->name, sizeof(h->name), "%s", name);
    h->rtt_ms = -1;
}

static void phase(const char *fmt, int n)
{
    nd_lock(&S.mx);
    snprintf(S.st.phase, sizeof(S.st.phase), fmt, n);
    S.st.elapsed_ms = (int)(nd_ms() - S.t0);
    S.gen++;
    nd_unlock(&S.mx);
}

static void ping_sweep(void)
{
    int sock = nd_icmp_socket();
    if (sock < 0) return;
    uint16_t id = (uint16_t)(nd_ms() ^ 0x3c5a);
    int64_t *sent_at = nd_alloc(sizeof(int64_t) * (size_t)S.nhosts);
    if (!sent_at) { close(sock); return; }
    int next = 0;
    int64_t last_send = 0;
    while (!S.stop) {
        /* send a few at a time so replies can drain */
        for (int k = 0; k < 8 && next < S.nhosts; k++, next++) {
            struct in_addr a;
            inet_aton(S.hosts[next].ip, &a);
            if (next == 0) devos_net_socket_route(sock, a.s_addr);
            sent_at[next] = nd_ms();
            nd_icmp_send(sock, a.s_addr, id, (uint16_t)next, 16);
            last_send = sent_at[next];
        }
        if (next >= S.nhosts && nd_ms() - last_send > 1200) break;
        uint32_t from;
        uint16_t seq, rid;
        int ttl;
        while (nd_icmp_recv(sock, next < S.nhosts ? 4 : 50, &from, &seq, &rid, &ttl) == 1) {
#ifdef ESP_PLATFORM
            if (rid != id) continue;
#endif
            if (seq >= S.nhosts) continue;
            struct in_addr a;
            inet_aton(S.hosts[seq].ip, &a);
            if (a.s_addr != from) continue;
            nd_lock(&S.mx);
            if (!S.hosts[seq].alive) {
                S.hosts[seq].alive = true;
                S.hosts[seq].rtt_ms = (int)(nd_ms() - sent_at[seq]);
                S.st.hosts_alive++;
                S.gen++;
            }
            nd_unlock(&S.mx);
        }
    }
    free(sent_at);
    close(sock);
}

typedef struct {
    int fd, host;
    uint16_t port;
    int64_t t0;
} slot_t;

static int probe_start(int host, uint16_t port)
{
    struct in_addr a;
    inet_aton(S.hosts[host].ip, &a);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    devos_net_socket_route(fd, a.s_addr);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = a };
    int r = connect(fd, (struct sockaddr *)&to, sizeof(to));
    if (r == 0 || errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN) return fd;
    /* immediate refusal still tells us the host is there */
    if (errno == ECONNREFUSED) {
        nd_lock(&S.mx);
        if (!S.hosts[host].alive) { S.hosts[host].alive = true; S.st.hosts_alive++; }
        nd_unlock(&S.mx);
    }
    close(fd);
    return -2;
}

static void record(int host, uint16_t port, int result)
{
    nd_lock(&S.mx);
    devos_scan_host_t *h = &S.hosts[host];
    if (result == 0 || result == ECONNREFUSED) {
        if (!h->alive) { h->alive = true; S.st.hosts_alive++; }
    }
    if (result == 0) {
        if (h->n_open < DEVOS_SCAN_MAX_OPEN) h->open[h->n_open++] = port;
        S.st.open_ports++;
    }
    S.st.probes_done++;
    S.gen++;
    nd_unlock(&S.mx);
}

static void tcp_scan(void)
{
    int nh = 0;
    int *order = nd_alloc(sizeof(int) * (size_t)S.nhosts);
    if (!order) return;
    for (int i = 0; i < S.nhosts; i++) if (!S.ping_first || S.hosts[i].alive) order[nh++] = i;
    nd_lock(&S.mx);
    S.st.probes_total = nh * S.nports;
    S.st.probes_done = 0;
    nd_unlock(&S.mx);
    phase(nh == 1 ? "Scanning %d host" : "Scanning %d hosts", nh);
    slot_t slots[POOL];
    for (int i = 0; i < POOL; i++) slots[i].fd = -1;
    long total = (long)nh * S.nports, next = 0;
    int active = 0, pool = POOL;
    while (!S.stop && (next < total || active)) {
        /* fill free slots (port-major so one host isn't hammered) */
        for (int i = 0; i < pool && next < total; i++) {
            if (slots[i].fd >= 0) continue;
            int hi = order[next % nh];
            uint16_t port = S.ports[next / nh];
            int fd = probe_start(hi, port);
            if (fd == -1) {                     /* out of sockets: use fewer */
                if (pool > 1 && active) pool = active;
                break;
            }
            next++;
            if (fd == -2) {
                record(hi, port, ECONNREFUSED);
                continue;
            }
            slots[i] = (slot_t){ fd, hi, port, nd_ms() };
            active++;
        }
        if (!active) continue;
        fd_set w;
        FD_ZERO(&w);
        int maxfd = -1;
        for (int i = 0; i < POOL; i++) {
            if (slots[i].fd < 0) continue;
            FD_SET(slots[i].fd, &w);
            if (slots[i].fd > maxfd) maxfd = slots[i].fd;
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
        int r = select(maxfd + 1, NULL, &w, NULL, &tv);
        int64_t now = nd_ms();
        for (int i = 0; i < POOL; i++) {
            slot_t *s = &slots[i];
            if (s->fd < 0) continue;
            int res = -1;
            if (r > 0 && FD_ISSET(s->fd, &w)) {
                int err = 0;
                socklen_t el = sizeof(err);
                getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &el);
                res = err;
            } else if (now - s->t0 < S.timeout) {
                continue;
            } else {
                res = ETIMEDOUT;
            }
            close(s->fd);
            s->fd = -1;
            active--;
            record(s->host, s->port, res);
        }
        if (pool < POOL && active < pool) pool++;   /* sockets freed up: grow back slowly */
    }
    for (int i = 0; i < POOL; i++) if (slots[i].fd >= 0) close(slots[i].fd);
    free(order);
}

/* PTR names for the hosts we found (one UDP socket, parallel queries) */
static void reverse_names(void)
{
    char server[48];
    devos_dns_default_server(server, sizeof(server));
    struct in_addr sa;
    if (!inet_aton(server, &sa)) return;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    devos_net_socket_route(s, sa.s_addr);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr = sa };
    int asked = 0;
    for (int i = 0; i < S.nhosts && asked < 128; i++) {
        devos_scan_host_t *h = &S.hosts[i];
        if (h->name[0] || (!h->alive && !h->n_open)) continue;
        unsigned a, b, c, d;
        if (sscanf(h->ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) continue;
        char q[64];
        snprintf(q, sizeof(q), "%u.%u.%u.%u.in-addr.arpa", d, c, b, a);
        const char *names[1] = { q };
        uint16_t types[1] = { DEVOS_DNS_PTR };
        uint8_t buf[128];
        int len = nd_dns_build_query(buf, sizeof(buf), (uint16_t)i, names, types, 1, true, false);
        if (len > 0) sendto(s, buf, (size_t)len, 0, (struct sockaddr *)&to, sizeof(to));
        asked++;
    }
    int64_t end = nd_ms() + 1500;
    uint8_t *msg = nd_alloc(1024);
    while (asked && msg && !S.stop) {
        int left = (int)(end - nd_ms());
        if (left <= 0 || nd_wait_readable(s, left) <= 0) break;
        int n = (int)recv(s, msg, 1024, 0);
        if (n < 12) continue;
        int id = msg[0] << 8 | msg[1];
        int an = msg[6] << 8 | msg[7];
        if (id >= S.nhosts || !an) continue;
        int off = 12;
        char name[256];
        if (nd_dns_read_name(msg, n, &off, name, sizeof(name)) < 0) continue;
        off += 4;
        for (int k = 0; k < an && off + 10 < n; k++) {
            if (nd_dns_read_name(msg, n, &off, name, sizeof(name)) < 0) break;
            int type = msg[off] << 8 | msg[off + 1], rdlen = msg[off + 8] << 8 | msg[off + 9];
            off += 10;
            if (type == DEVOS_DNS_PTR) {
                int o = off;
                if (nd_dns_read_name(msg, n, &o, name, sizeof(name)) == 0) {
                    nd_lock(&S.mx);
                    snprintf(S.hosts[id].name, sizeof(S.hosts[id].name), "%.63s", name);
                    S.gen++;
                    nd_unlock(&S.mx);
                }
                break;
            }
            off += rdlen;
        }
    }
    free(msg);
    close(s);
}

static void scan_task(void *arg)
{
    (void)arg;
    char err[96] = "";
    phase("Resolving targets%.0d", 0);
    if (walk_targets(S.targets, add_host, NULL, err, sizeof(err)) < 0 || !S.nhosts) {
        nd_lock(&S.mx);
        snprintf(S.st.error, sizeof(S.st.error), "%s", err[0] ? err : "No target resolved");
        nd_unlock(&S.mx);
        goto done;
    }
    nd_lock(&S.mx);
    S.st.hosts_total = S.nhosts;
    nd_unlock(&S.mx);
    if (S.ping_first && S.nhosts > 0) {
        phase(S.nhosts == 1 ? "Pinging %d host" : "Pinging %d hosts", S.nhosts);
        ping_sweep();
    }
    if (!S.stop) tcp_scan();
    if (!S.stop) {
        phase("Looking up names%.0d", 0);
        reverse_names();
    }
done:
    nd_lock(&S.mx);
    snprintf(S.st.phase, sizeof(S.st.phase), "%s", S.stop ? "Stopped" : S.st.error[0] ? "Failed" : "Done");
    S.st.elapsed_ms = (int)(nd_ms() - S.t0);
    S.st.running = false;
    S.gen++;
    nd_unlock(&S.mx);
    S.task = false;
}

int devos_scan_start(const char *targets, const char *ports, int timeout_ms, bool ping_first)
{
    char err[96];
    if (S.task) return -1;
    int nh = devos_scan_count_hosts(targets, err, sizeof(err));
    int np = nh > 0 ? devos_scan_count_ports(ports, err, sizeof(err)) : -1;
    nd_lock(&S.mx);
    memset(&S.st, 0, sizeof(S.st));
    if (nh < 0 || np < 0 || (long)nh * np > MAX_PROBES) {
        snprintf(S.st.error, sizeof(S.st.error), "%s",
                 nh < 0 || np < 0 ? err : "Too many probes: narrow the hosts or ports");
        snprintf(S.st.phase, sizeof(S.st.phase), "Failed");
        S.gen++;
        nd_unlock(&S.mx);
        return -1;
    }
    if (!S.hosts) S.hosts = nd_alloc(sizeof(devos_scan_host_t) * DEVOS_SCAN_MAX_HOSTS);
    free(S.ports);
    S.ports = nd_alloc(sizeof(uint16_t) * (size_t)np);
    if (!S.hosts || !S.ports) {
        snprintf(S.st.error, sizeof(S.st.error), "Out of memory");
        nd_unlock(&S.mx);
        return -1;
    }
    S.nports = walk_ports(ports, S.ports, np, err, sizeof(err));
    S.nhosts = 0;
    snprintf(S.targets, sizeof(S.targets), "%s", targets);
    S.timeout = timeout_ms < 100 ? 100 : timeout_ms > 5000 ? 5000 : timeout_ms;
    S.ping_first = ping_first;
    S.stop = false;
    S.st.running = true;
    S.t0 = nd_ms();
    S.task = true;
    S.gen++;
    nd_unlock(&S.mx);
    if (nd_spawn(scan_task, NULL, "portscan", 6144) != 0) {
        S.task = false;
        S.st.running = false;
        return -1;
    }
    return 0;
}

void devos_scan_stop(void) { S.stop = true; }

void devos_scan_status(devos_scan_status_t *out)
{
    nd_lock(&S.mx);
    *out = S.st;
    if (S.st.running) out->elapsed_ms = (int)(nd_ms() - S.t0);
    nd_unlock(&S.mx);
}

int devos_scan_results(devos_scan_host_t *out, int max)
{
    int n = 0;
    nd_lock(&S.mx);
    for (int i = 0; i < S.nhosts && n < max; i++) {
        if (S.hosts[i].alive || S.hosts[i].n_open) out[n++] = S.hosts[i];
    }
    nd_unlock(&S.mx);
    return n;
}

uint32_t devos_scan_generation(void)
{
    nd_lock(&S.mx);
    uint32_t g = S.gen;
    nd_unlock(&S.mx);
    return g;
}

void nd_scan_init(void) { nd_mutex_create(&S.mx); }

/* ------------------------------------------------------------------ init */
void nd_ping_init(void);
void nd_dns_init(void);

void devos_netdiag_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    nd_ping_init();
    nd_dns_init();
    nd_scan_init();
}
