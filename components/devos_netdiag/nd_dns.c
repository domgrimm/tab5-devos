/* devos_netdiag: DNS lookups and mDNS / DNS-SD browsing (one DNS codec). */
#include "nd_int.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif
#include "devos_net.h"

#include <ctype.h>
#include <strings.h>

/* ------------------------------------------------------------------ codec */
static int put_name(uint8_t *buf, int cap, int o, const char *name)
{
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        int l = dot ? (int)(dot - p) : (int)strlen(p);
        if (l <= 0 || l > 63 || o + l + 2 > cap) return -1;
        buf[o++] = (uint8_t)l;
        memcpy(buf + o, p, (size_t)l);
        o += l;
        p += l;
        if (*p == '.') p++;
    }
    if (o + 1 > cap) return -1;
    buf[o++] = 0;
    return o;
}

int nd_dns_build_query(uint8_t *buf, int cap, uint16_t id, const char *const *names, const uint16_t *types, int n,
                       bool rd, bool qu)
{
    if (cap < 12) return -1;
    memset(buf, 0, 12);
    buf[0] = (uint8_t)(id >> 8);
    buf[1] = (uint8_t)id;
    if (rd) buf[2] = 0x01;
    buf[4] = (uint8_t)(n >> 8);
    buf[5] = (uint8_t)n;
    int o = 12;
    for (int i = 0; i < n; i++) {
        o = put_name(buf, cap, o, names[i]);
        if (o < 0 || o + 4 > cap) return -1;
        buf[o++] = (uint8_t)(types[i] >> 8);
        buf[o++] = (uint8_t)types[i];
        buf[o++] = qu ? 0x80 : 0x00;           /* class IN (+ unicast-response bit) */
        buf[o++] = 0x01;
    }
    return o;
}

int nd_dns_read_name(const uint8_t *msg, int len, int *off, char *out, size_t cap)
{
    int o = *off, jumps = 0;
    bool jumped = false;
    size_t w = 0;
    out[0] = '\0';
    for (;;) {
        if (o >= len) return -1;
        uint8_t l = msg[o];
        if ((l & 0xc0) == 0xc0) {
            if (o + 1 >= len || ++jumps > 20) return -1;
            int ptr = ((l & 0x3f) << 8) | msg[o + 1];
            if (!jumped) *off = o + 2;
            jumped = true;
            o = ptr;
            continue;
        }
        if (l & 0xc0) return -1;
        o++;
        if (!l) break;
        if (o + l > len) return -1;
        if (w && w + 1 < cap) out[w++] = '.';
        for (int i = 0; i < l; i++) {
            char c = (char)msg[o + i];
            if (w + 1 < cap) out[w++] = (c >= 32 && c < 127) ? c : '?';
        }
        o += l;
    }
    out[w < cap ? w : cap - 1] = '\0';
    if (!jumped) *off = o;
    return 0;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void fmt_ip6(const uint8_t *a, char *out, size_t cap)
{
    uint16_t g[8];
    for (int i = 0; i < 8; i++) g[i] = (uint16_t)(a[i * 2] << 8 | a[i * 2 + 1]);
    int best = -1, bestn = 0;                  /* longest run of zero groups becomes :: */
    for (int i = 0; i < 8;) {
        if (g[i]) { i++; continue; }
        int j = i;
        while (j < 8 && !g[j]) j++;
        if (j - i > bestn) { best = i; bestn = j - i; }
        i = j;
    }
    if (bestn < 2) best = -1;
    size_t o = 0;
    out[0] = '\0';
    for (int i = 0; i < 8 && o + 6 < cap; i++) {
        if (i == best) {
            o += (size_t)snprintf(out + o, cap - o, "::");
            i += bestn - 1;
            continue;
        }
        bool colon = i > 0 && !(best >= 0 && i == best + bestn);
        o += (size_t)snprintf(out + o, cap - o, "%s%x", colon ? ":" : "", g[i]);
    }
}

typedef void (*rr_fn)(void *ud, int section, const char *name, uint16_t type, uint32_t ttl, const uint8_t *msg,
                      int len, int rdoff, int rdlen);

/* walk every record of a reply; returns the header flags, or -1 */
static int parse_msg(const uint8_t *msg, int len, rr_fn fn, void *ud)
{
    if (len < 12) return -1;
    int qd = rd16(msg + 4), counts[3] = { rd16(msg + 6), rd16(msg + 8), rd16(msg + 10) };
    int off = 12;
    char name[256];
    for (int i = 0; i < qd; i++) {
        if (nd_dns_read_name(msg, len, &off, name, sizeof(name)) < 0 || off + 4 > len) return -1;
        off += 4;
    }
    for (int s = 0; s < 3; s++) {
        for (int i = 0; i < counts[s]; i++) {
            if (nd_dns_read_name(msg, len, &off, name, sizeof(name)) < 0 || off + 10 > len) return -1;
            uint16_t type = rd16(msg + off);
            uint32_t ttl = rd32(msg + off + 4);
            int rdlen = rd16(msg + off + 8);
            off += 10;
            if (off + rdlen > len) return -1;
            fn(ud, s, name, type, ttl, msg, len, off, rdlen);
            off += rdlen;
        }
    }
    return rd16(msg + 2);
}

static void fmt_rdata(uint16_t type, const uint8_t *msg, int len, int off, int rdlen, char *out, size_t cap)
{
    const uint8_t *d = msg + off;
    char n1[256], n2[256];
    int o;
    out[0] = '\0';
    switch (type) {
    case DEVOS_DNS_A:
        if (rdlen == 4) snprintf(out, cap, "%u.%u.%u.%u", d[0], d[1], d[2], d[3]);
        return;
    case DEVOS_DNS_AAAA:
        if (rdlen == 16) fmt_ip6(d, out, cap);
        return;
    case DEVOS_DNS_CNAME:
    case DEVOS_DNS_NS:
    case DEVOS_DNS_PTR:
        o = off;
        if (nd_dns_read_name(msg, len, &o, n1, sizeof(n1)) == 0) snprintf(out, cap, "%.240s.", n1);
        return;
    case DEVOS_DNS_MX:
        o = off + 2;
        if (rdlen > 2 && nd_dns_read_name(msg, len, &o, n1, sizeof(n1)) == 0) snprintf(out, cap, "%u %.230s.", rd16(d), n1);
        return;
    case DEVOS_DNS_SRV:
        o = off + 6;
        if (rdlen > 6 && nd_dns_read_name(msg, len, &o, n1, sizeof(n1)) == 0)
            snprintf(out, cap, "%u %u %u %.220s.", rd16(d), rd16(d + 2), rd16(d + 4), n1);
        return;
    case DEVOS_DNS_SOA:
        o = off;
        if (nd_dns_read_name(msg, len, &o, n1, sizeof(n1)) == 0 && nd_dns_read_name(msg, len, &o, n2, sizeof(n2)) == 0 &&
            o + 20 <= off + rdlen)
            snprintf(out, cap, "%.100s. %.100s. %u %u %u %u %u", n1, n2, (unsigned)rd32(msg + o), (unsigned)rd32(msg + o + 4),
                     (unsigned)rd32(msg + o + 8), (unsigned)rd32(msg + o + 12), (unsigned)rd32(msg + o + 16));
        return;
    case DEVOS_DNS_TXT: {
        size_t w = 0;
        for (int i = 0; i < rdlen && w + 4 < cap;) {
            int l = d[i++];
            if (w) out[w++] = ' ';
            out[w++] = '"';
            for (int k = 0; k < l && i < rdlen && w + 2 < cap; k++, i++) {
                char c = (char)d[i];
                out[w++] = (c >= 32 && c < 127) ? c : '.';
            }
            if (w + 1 < cap) out[w++] = '"';
        }
        out[w < cap ? w : cap - 1] = '\0';
        return;
    }
    default:
        snprintf(out, cap, "(%d bytes)", rdlen);
        return;
    }
}

const char *devos_dns_type_name(uint16_t t)
{
    switch (t) {
    case DEVOS_DNS_A: return "A";
    case DEVOS_DNS_NS: return "NS";
    case DEVOS_DNS_CNAME: return "CNAME";
    case DEVOS_DNS_SOA: return "SOA";
    case DEVOS_DNS_PTR: return "PTR";
    case DEVOS_DNS_MX: return "MX";
    case DEVOS_DNS_TXT: return "TXT";
    case DEVOS_DNS_AAAA: return "AAAA";
    case DEVOS_DNS_SRV: return "SRV";
    case 41: return "OPT";
    case 46: return "RRSIG";
    case 47: return "NSEC";
    case 65: return "HTTPS";
    case DEVOS_DNS_ANY: return "ANY";
    default: return "?";
    }
}

const char *devos_dns_rcode_name(int rc)
{
    static const char *n[] = { "NOERROR", "FORMERR", "SERVFAIL", "NXDOMAIN", "NOTIMP", "REFUSED" };
    return rc >= 0 && rc < 6 ? n[rc] : "ERROR";
}

/* ------------------------------------------------------------------ unicast lookup */
/* The big engine state lives in PSRAM (task-only data): internal RAM is for
 * stacks, DMA, TLS and Wi-Fi. */
static EXT_RAM_BSS_ATTR struct {
    nd_mutex_t mx;
    devos_dns_result_t res;
    uint32_t gen;
    volatile bool task;
    char server[48], name[128];
    uint16_t type;
    uint16_t port;
} D = { .mx = ND_MUTEX_INIT };

/* Split "host" or "host:port" (port 1..65535); host is copied to hostcap. */
static void split_host_port(const char *in, char *host, size_t hostcap, uint16_t *port)
{
    snprintf(host, hostcap, "%s", in ? in : "");
    *port = 53;
    char *colon = strrchr(host, ':');
    if (colon && colon[1]) {
        int p = atoi(colon + 1);
        if (p > 0 && p < 65536) {
            *port = (uint16_t)p;
            *colon = '\0';
        }
    }
}

void devos_dns_default_server(char *out, size_t cap)
{
    out[0] = '\0';
#ifdef ESP_PLATFORM
    devos_wifi_status_t st;
    if (devos_net_wifi_get_status(&st) == 0) {
        snprintf(out, cap, "%s", st.dns[0] && strcmp(st.dns, "0.0.0.0") ? st.dns : st.gateway);
    }
#else
    FILE *f = fopen("/etc/resolv.conf", "r");
    char line[160];
    while (f && fgets(line, sizeof(line), f)) {
        char ip[64];
        struct in_addr a;
        if (sscanf(line, " nameserver %63s", ip) == 1 && inet_aton(ip, &a)) {
            snprintf(out, cap, "%s", ip);
            break;
        }
    }
    if (f) fclose(f);
#endif
    if (!out[0]) snprintf(out, cap, "1.1.1.1");
}

typedef struct {
    devos_dns_result_t *r;
} lk_ud_t;

static void lookup_rr(void *ud, int section, const char *name, uint16_t type, uint32_t ttl, const uint8_t *msg, int len,
                      int rdoff, int rdlen)
{
    devos_dns_result_t *r = ((lk_ud_t *)ud)->r;
    if (type == 41 || r->n >= DEVOS_DNS_MAX_RR) return;     /* EDNS OPT */
    devos_dns_rr_t *rr = &r->rr[r->n++];
    snprintf(rr->name, sizeof(rr->name), "%s.", name);
    rr->type = type;
    rr->section = (uint8_t)section;
    rr->ttl = ttl;
    fmt_rdata(type, msg, len, rdoff, rdlen, rr->data, sizeof(rr->data));
}

static int dns_tcp(const char *server, uint16_t port, const uint8_t *q, int qlen, uint8_t *buf, int cap)
{
    int fd = devos_net_socket_connect(server, port, 3000);
    if (fd < 0) return -1;
    uint8_t lenb[2] = { (uint8_t)(qlen >> 8), (uint8_t)qlen };
    int got = -1;
    if (devos_net_socket_send_all(fd, lenb, 2) == 0 && devos_net_socket_send_all(fd, q, (size_t)qlen) == 0) {
        int have = 0, want = -1;
        while (have < (want < 0 ? 2 : want + 2)) {
            int n = devos_net_socket_recv(fd, buf + have, (size_t)(cap - have), 3000);
            if (n <= 0) break;
            have += n;
            if (want < 0 && have >= 2) {
                want = rd16(buf);
                if (want + 2 > cap) want = cap - 2;
            }
        }
        if (want >= 0 && have >= want + 2) {
            memmove(buf, buf + 2, (size_t)want);
            got = want;
        }
    }
    devos_net_socket_close(fd);
    return got;
}

/* The lookup core shared by the Network UI's singleton and Jobs' per-call
 * contexts: one query, one result, an optional stop flag checked while waiting.
 * No shared state is read or written here. */
static void dns_lookup_core(const char *server, uint16_t port, const char *name, uint16_t type,
                            devos_dns_result_t *r, volatile bool *stop)
{
    memset(r, 0, sizeof(*r));
    snprintf(r->server, sizeof(r->server), "%s", server);
    snprintf(r->query, sizeof(r->query), "%s", name);
    r->qtype = type;
    uint8_t *q = nd_alloc(512), *buf = nd_alloc(8192);
    if (!q || !buf) {
        snprintf(r->error, sizeof(r->error), "Out of memory");
        free(q);
        free(buf);
        return;
    }
    uint16_t qid = (uint16_t)(nd_ms() * 7919);
    const char *names[1] = { name };
    uint16_t types[1] = { type };
    int qlen = nd_dns_build_query(q, 512, qid, names, types, 1, true, false);
    int64_t t0 = nd_ms();
    int n = -1;
    struct in_addr sa;
    if (qlen < 0) snprintf(r->error, sizeof(r->error), "That name isn't valid");
    else if (!inet_aton(server, &sa)) snprintf(r->error, sizeof(r->error), "The server must be an IP address");
    else {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) snprintf(r->error, sizeof(r->error), "No socket (errno %d)", errno);
        else {
            devos_net_socket_route(s, sa.s_addr);
            struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = sa };
            for (int attempt = 0; attempt < 2 && n < 0 && !(stop && *stop); attempt++) {
                sendto(s, q, (size_t)qlen, 0, (struct sockaddr *)&to, sizeof(to));
                int64_t end = nd_ms() + 1500;
                while (n < 0) {
                    if (stop && *stop) break;
                    int left = (int)(end - nd_ms());
                    if (left <= 0 || nd_wait_readable(s, left) <= 0) break;
                    int k = (int)recv(s, buf, 8192, 0);
                    if (k >= 12 && rd16(buf) == qid) n = k;
                }
            }
            close(s);
            if (n < 0 && !(stop && *stop)) snprintf(r->error, sizeof(r->error), "No answer from %s", server);
            else if (n >= 12 && (buf[2] & 0x02)) {           /* truncated: ask over TCP */
                int k = dns_tcp(server, port, q, qlen, buf, 8192);
                if (k >= 12) n = k;
                else r->truncated = true;
            }
        }
    }
    r->ms = (int)(nd_ms() - t0);
    if (n >= 12) {
        lk_ud_t ud = { r };
        int flags = parse_msg(buf, n, lookup_rr, &ud);
        if (flags < 0) snprintf(r->error, sizeof(r->error), "Couldn't parse the reply");
        else {
            r->rcode = flags & 0x0f;
            r->authoritative = (flags & 0x0400) != 0;
            r->recursion = (flags & 0x0080) != 0;
            if (flags & 0x0200) r->truncated = true;
        }
    }
    free(q);
    free(buf);
}

static void lookup_task(void *arg)
{
    (void)arg;
    devos_dns_result_t *r = nd_alloc(sizeof(*r));
    if (!r) {
        nd_lock(&D.mx);
        snprintf(D.res.error, sizeof(D.res.error), "Out of memory");
        D.res.busy = false;
        D.gen++;
        nd_unlock(&D.mx);
        D.task = false;
        return;
    }
    dns_lookup_core(D.server, D.port, D.name, D.type, r, NULL);
    nd_lock(&D.mx);
    D.res = *r;
    D.res.busy = false;
    D.gen++;
    nd_unlock(&D.mx);
    free(r);
    D.task = false;
}

/* ---- request-specific lookups (Jobs network.dns) ----
 * A small ticket pool of independent lookups. Each owns its own task, socket,
 * deadline and result, so a Jobs lookup never clobbers an in-progress Network
 * UI lookup (PLAN.md 7.3). Tickets are slot + monotonic id; a released or
 * cancelled ticket's result is discarded. */
#define DEVOS_DNS_CTX_MAX 2
static EXT_RAM_BSS_ATTR struct {
    nd_mutex_t mx;
    bool used, busy, stop;
    uint32_t id;
    char server[48], name[128];
    uint16_t type;
    uint16_t port;
    devos_dns_result_t res;
} C[DEVOS_DNS_CTX_MAX];
static uint32_t C_seq;

static void ctx_task(void *arg)
{
    int slot = (int)(intptr_t)arg;
    char server[48], name[128];
    uint16_t type, port;
    uint32_t id;
    nd_lock(&C[slot].mx);
    snprintf(server, sizeof(server), "%s", C[slot].server);
    snprintf(name, sizeof(name), "%s", C[slot].name);
    type = C[slot].type;
    port = C[slot].port;
    id = C[slot].id;
    nd_unlock(&C[slot].mx);

    devos_dns_result_t *r = nd_alloc(sizeof(*r));
    if (r) dns_lookup_core(server, port, name, type, r, &C[slot].stop);
    nd_lock(&C[slot].mx);
    if (C[slot].used && C[slot].id == id) {
        if (r) C[slot].res = *r;
        C[slot].busy = false;
    }
    nd_unlock(&C[slot].mx);
    free(r);
}

int devos_dns_ctx_start(const char *server, const char *name, uint16_t type)
{
    if (!name || !name[0]) return 0;
    if (strlen(name) >= 128) return 0;                  /* would truncate to the wrong name */
    if (server && strlen(server) >= 48) return 0;
    char nm[128];
    snprintf(nm, sizeof(nm), "%s", name);
    size_t l = strlen(nm);
    while (l && (nm[l - 1] == '.' || nm[l - 1] == ' ')) nm[--l] = '\0';
    const char *sp = nm;
    while (*sp == ' ') sp++;
    if (!*sp) return 0;

    int slot = -1;
    for (int i = 0; i < DEVOS_DNS_CTX_MAX; i++) {
        nd_lock(&C[i].mx);
        bool free_slot = !C[i].used;
        nd_unlock(&C[i].mx);
        if (free_slot) { slot = i; break; }
    }
    if (slot < 0) return 0;
    nd_lock(&C[slot].mx);
    C[slot].used = true;
    C[slot].busy = true;
    C[slot].stop = false;
    C[slot].id = ++C_seq;
    if (!C[slot].id) C[slot].id = ++C_seq;
    struct in_addr a;
    if (inet_aton(sp, &a)) {                                  /* IP -> reverse lookup */
        uint8_t *b = (uint8_t *)&a.s_addr;
        snprintf(C[slot].name, sizeof(C[slot].name), "%u.%u.%u.%u.in-addr.arpa", b[3], b[2], b[1], b[0]);
        C[slot].type = DEVOS_DNS_PTR;
    } else {
        snprintf(C[slot].name, sizeof(C[slot].name), "%s", sp);
        C[slot].type = type ? type : DEVOS_DNS_A;
    }
    char sv[48];
    if (server && server[0]) snprintf(sv, sizeof(sv), "%s", server);
    else devos_dns_default_server(sv, sizeof(sv));
    split_host_port(sv, C[slot].server, sizeof(C[slot].server), &C[slot].port);
    memset(&C[slot].res, 0, sizeof(C[slot].res));
    char nmbuf[128], svbuf[48];
    snprintf(nmbuf, sizeof(nmbuf), "%s", C[slot].name);
    snprintf(svbuf, sizeof(svbuf), "%s", C[slot].server);
    snprintf(C[slot].res.query, sizeof(C[slot].res.query), "%s", nmbuf);
    snprintf(C[slot].res.server, sizeof(C[slot].res.server), "%s", svbuf);
    C[slot].res.qtype = C[slot].type;
    C[slot].res.busy = true;
    uint32_t id = C[slot].id;
    nd_unlock(&C[slot].mx);

    if (nd_spawn(ctx_task, (void *)(intptr_t)slot, "jobsdns", 6144) != 0) {
        nd_lock(&C[slot].mx);
        if (C[slot].used && C[slot].id == id) {
            C[slot].busy = false;
            snprintf(C[slot].res.error, sizeof(C[slot].res.error), "Couldn't start the lookup");
        }
        nd_unlock(&C[slot].mx);
        return 0;
    }
    return (int)id;
}

/* 0 running, 1 done (out filled), -1 unknown ticket. */
int devos_dns_ctx_poll(int ticket, devos_dns_result_t *out)
{
    if (ticket <= 0) return -1;
    for (int i = 0; i < DEVOS_DNS_CTX_MAX; i++) {
        nd_lock(&C[i].mx);
        if (C[i].used && C[i].id == (uint32_t)ticket) {
            bool busy = C[i].busy;
            if (!busy && out) *out = C[i].res;
            nd_unlock(&C[i].mx);
            return busy ? 0 : 1;
        }
        nd_unlock(&C[i].mx);
    }
    return -1;
}

void devos_dns_ctx_cancel(int ticket)
{
    for (int i = 0; i < DEVOS_DNS_CTX_MAX; i++) {
        nd_lock(&C[i].mx);
        if (C[i].used && C[i].id == (uint32_t)ticket) { C[i].stop = true; nd_unlock(&C[i].mx); return; }
        nd_unlock(&C[i].mx);
    }
}

void devos_dns_ctx_release(int ticket)
{
    for (int i = 0; i < DEVOS_DNS_CTX_MAX; i++) {
        nd_lock(&C[i].mx);
        if (C[i].used && C[i].id == (uint32_t)ticket) { C[i].used = false; nd_unlock(&C[i].mx); return; }
        nd_unlock(&C[i].mx);
    }
}

int devos_dns_lookup_start(const char *server, const char *name, uint16_t type)
{
    if (!name || !name[0] || D.task) return -1;
    if (strlen(name) >= 128) return -1;
    if (server && strlen(server) >= 48) return -1;
    char nm[128];
    snprintf(nm, sizeof(nm), "%s", name);
    size_t l = strlen(nm);
    while (l && (nm[l - 1] == '.' || nm[l - 1] == ' ')) nm[--l] = '\0';
    const char *s = nm;
    while (*s == ' ') s++;
    if (!*s) return -1;
    struct in_addr a;
    nd_lock(&D.mx);
    if (inet_aton(s, &a)) {                                  /* IP -> reverse lookup */
        uint8_t *b = (uint8_t *)&a.s_addr;
        snprintf(D.name, sizeof(D.name), "%u.%u.%u.%u.in-addr.arpa", b[3], b[2], b[1], b[0]);
        D.type = DEVOS_DNS_PTR;
    } else {
        snprintf(D.name, sizeof(D.name), "%s", s);
        D.type = type ? type : DEVOS_DNS_A;
    }
    char sv[48];
    if (server && server[0]) snprintf(sv, sizeof(sv), "%s", server);
    else devos_dns_default_server(sv, sizeof(sv));
    split_host_port(sv, D.server, sizeof(D.server), &D.port);
    memset(&D.res, 0, sizeof(D.res));
    D.res.busy = true;
    snprintf(D.res.query, sizeof(D.res.query), "%s", D.name);
    snprintf(D.res.server, sizeof(D.res.server), "%s%s", D.server, D.port != 53 ? " (custom port)" : "");
    D.res.qtype = D.type;
    D.task = true;
    D.gen++;
    nd_unlock(&D.mx);
    if (nd_spawn(lookup_task, NULL, "dns", 6144) != 0) {
        D.task = false;
        nd_lock(&D.mx);
        D.res.busy = false;
        snprintf(D.res.error, sizeof(D.res.error), "Couldn't start the lookup");
        nd_unlock(&D.mx);
        return -1;
    }
    return 0;
}

void devos_dns_lookup_result(devos_dns_result_t *out)
{
    nd_lock(&D.mx);
    *out = D.res;
    nd_unlock(&D.mx);
}

uint32_t devos_dns_generation(void)
{
    nd_lock(&D.mx);
    uint32_t g = D.gen;
    nd_unlock(&D.mx);
    return g;
}

/* ------------------------------------------------------------------ mDNS / DNS-SD */
static const struct { const char *type, *label; } KNOWN[] = {
    { "_http._tcp", "Web" }, { "_https._tcp", "Web (HTTPS)" }, { "_ssh._tcp", "SSH" },
    { "_sftp-ssh._tcp", "SFTP" }, { "_esphomelib._tcp", "ESPHome" }, { "_home-assistant._tcp", "Home Assistant" },
    { "_hap._tcp", "HomeKit" }, { "_mqtt._tcp", "MQTT" }, { "_ipp._tcp", "Printer" }, { "_ipps._tcp", "Printer" },
    { "_printer._tcp", "Printer (LPD)" }, { "_pdl-datastream._tcp", "Printer" }, { "_smb._tcp", "File share (SMB)" },
    { "_afpovertcp._tcp", "File share (AFP)" }, { "_nfs._tcp", "NFS" }, { "_googlecast._tcp", "Chromecast" },
    { "_airplay._tcp", "AirPlay" }, { "_raop._tcp", "AirPlay audio" }, { "_spotify-connect._tcp", "Spotify Connect" },
    { "_workstation._tcp", "Computer" }, { "_device-info._tcp", "Device info" }, { "_octoprint._tcp", "OctoPrint" },
    { "_rfb._tcp", "VNC" }, { "_companion-link._tcp", "Apple device" }, { "_matter._tcp", "Matter" },
    { "_arduino._tcp", "Arduino OTA" }, { "_wled._tcp", "WLED" }, { "_shelly._tcp", "Shelly" },
    { "_sonos._tcp", "Sonos" }, { "_adisk._tcp", "Time Machine" }, { "_ftp._tcp", "FTP" },
    { "_scanner._tcp", "Scanner" }, { "_uscan._tcp", "Scanner" }, { "_plexmediasvr._tcp", "Plex" },
    { "_elg._tcp", "Elgato" }, { "_sleep-proxy._udp", "Sleep proxy" }, { "_matterc._udp", "Matter setup" },
};
#define N_KNOWN ((int)(sizeof(KNOWN) / sizeof(KNOWN[0])))
#define MAX_TYPES 64
#define MAX_HOSTS 64

const char *devos_mdns_type_label(const char *type)
{
    for (int i = 0; i < N_KNOWN; i++) if (!strcasecmp(KNOWN[i].type, type)) return KNOWN[i].label;
    return NULL;
}

static EXT_RAM_BSS_ATTR struct {
    nd_mutex_t mx;
    devos_mdns_svc_t *svc;              /* DEVOS_MDNS_MAX, PSRAM */
    int n;
    uint32_t gen;
    volatile bool stop, task;
    char err[96];
    int duration;
    /* browse state (task only) */
    char types[MAX_TYPES][40];
    uint8_t type_asked[MAX_TYPES];
    int ntypes;
    struct { char name[64]; char ip[16]; } hosts[MAX_HOSTS];
    int nhosts;
} M = { .mx = ND_MUTEX_INIT };

static bool ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}

/* "Kitchen._http._tcp.local" -> instance "Kitchen", type "_http._tcp" */
static bool split_instance(const char *full, char *inst, size_t icap, char *type, size_t tcap)
{
    if (!ends_with(full, ".local")) return false;
    size_t l = strlen(full) - 6;
    /* the type is the last two labels before .local: _svc._proto */
    int dots = 0;
    size_t i = l;
    while (i > 0) {
        i--;
        if (full[i] == '.') {
            if (++dots == 2) break;
        }
    }
    if (dots < 2 || full[i + 1] != '_') return false;
    snprintf(type, tcap, "%.*s", (int)(l - i - 1), full + i + 1);
    snprintf(inst, icap, "%.*s", (int)i, full);
    return inst[0] != '\0';
}

static int add_type(const char *t)
{
    for (int i = 0; i < M.ntypes; i++) if (!strcasecmp(M.types[i], t)) return i;
    if (M.ntypes >= MAX_TYPES || t[0] != '_') return -1;
    snprintf(M.types[M.ntypes], sizeof(M.types[0]), "%s", t);
    M.type_asked[M.ntypes] = 0;
    return M.ntypes++;
}

static devos_mdns_svc_t *find_svc(const char *inst, const char *type, bool create)
{
    for (int i = 0; i < M.n; i++) {
        if (!strcasecmp(M.svc[i].instance, inst) && !strcasecmp(M.svc[i].type, type)) return &M.svc[i];
    }
    if (!create || M.n >= DEVOS_MDNS_MAX) return NULL;
    devos_mdns_svc_t *s = &M.svc[M.n++];
    memset(s, 0, sizeof(*s));
    snprintf(s->instance, sizeof(s->instance), "%s", inst);
    snprintf(s->type, sizeof(s->type), "%s", type);
    add_type(type);
    return s;
}

static void set_host_ip(const char *host, const char *ip)
{
    int i;
    for (i = 0; i < M.nhosts && strcasecmp(M.hosts[i].name, host); i++) {}
    if (i == M.nhosts) {
        if (M.nhosts >= MAX_HOSTS) return;
        M.nhosts++;
        snprintf(M.hosts[i].name, sizeof(M.hosts[i].name), "%s", host);
    }
    snprintf(M.hosts[i].ip, sizeof(M.hosts[i].ip), "%s", ip);
    for (int k = 0; k < M.n; k++) {
        if (!strcasecmp(M.svc[k].host, host)) snprintf(M.svc[k].ip, sizeof(M.svc[k].ip), "%s", ip);
    }
}

static const char *host_ip(const char *host)
{
    for (int i = 0; i < M.nhosts; i++) if (!strcasecmp(M.hosts[i].name, host)) return M.hosts[i].ip;
    return NULL;
}

static void mdns_rr(void *ud, int section, const char *name, uint16_t type, uint32_t ttl, const uint8_t *msg, int len,
                    int rdoff, int rdlen)
{
    (void)ud;
    (void)section;
    if (ttl == 0) return;                       /* goodbye packet */
    char inst[64], stype[40], target[256];
    nd_lock(&M.mx);
    if (type == DEVOS_DNS_PTR) {
        int o = rdoff;
        if (nd_dns_read_name(msg, len, &o, target, sizeof(target)) == 0) {
            if (!strcasecmp(name, "_services._dns-sd._udp.local")) {
                if (ends_with(target, ".local")) {
                    target[strlen(target) - 6] = '\0';
                    add_type(target);
                }
            } else if (split_instance(target, inst, sizeof(inst), stype, sizeof(stype))) {
                find_svc(inst, stype, true);
                M.gen++;
            }
        }
    } else if (type == DEVOS_DNS_SRV && rdlen > 6) {
        int o = rdoff + 6;
        if (split_instance(name, inst, sizeof(inst), stype, sizeof(stype)) &&
            nd_dns_read_name(msg, len, &o, target, sizeof(target)) == 0) {
            devos_mdns_svc_t *s = find_svc(inst, stype, true);
            if (s) {
                s->port = rd16(msg + rdoff + 4);
                snprintf(s->host, sizeof(s->host), "%.63s", target);
                const char *ip = host_ip(target);
                if (ip) snprintf(s->ip, sizeof(s->ip), "%s", ip);
                M.gen++;
            }
        }
    } else if (type == DEVOS_DNS_TXT) {
        if (split_instance(name, inst, sizeof(inst), stype, sizeof(stype))) {
            devos_mdns_svc_t *s = find_svc(inst, stype, true);
            if (s) {
                const uint8_t *d = msg + rdoff;
                size_t w = 0;
                for (int i = 0; i < rdlen && w + 3 < sizeof(s->txt);) {
                    int l = d[i++];
                    if (!l) continue;
                    if (w) { s->txt[w++] = ' '; s->txt[w++] = ' '; }
                    for (int k = 0; k < l && i < rdlen && w + 1 < sizeof(s->txt); k++, i++) {
                        char c = (char)d[i];
                        s->txt[w++] = (c >= 32 && c < 127) ? c : '.';
                    }
                }
                s->txt[w < sizeof(s->txt) ? w : sizeof(s->txt) - 1] = '\0';
                M.gen++;
            }
        }
    } else if (type == DEVOS_DNS_A && rdlen == 4) {
        char ip[16];
        const uint8_t *d = msg + rdoff;
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", d[0], d[1], d[2], d[3]);
        set_host_ip(name, ip);
        M.gen++;
    }
    nd_unlock(&M.mx);
}

static void send_q(int s, const char *const *names, const uint16_t *types, int n, bool qu)
{
    uint8_t buf[512];
    int len = nd_dns_build_query(buf, sizeof(buf), 0, names, types, n, false, qu);
    if (len < 0) return;
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(5353) };
    to.sin_addr.s_addr = inet_addr("224.0.0.251");
    sendto(s, buf, (size_t)len, 0, (struct sockaddr *)&to, sizeof(to));
}

/* ask in batches of 8 questions */
static void ask(int *socks, int ns, const char *const *names, const uint16_t *types, int n, bool qu)
{
    for (int i = 0; i < n; i += 8) {
        int k = n - i < 8 ? n - i : 8;
        for (int s = 0; s < ns; s++) if (socks[s] >= 0) send_q(socks[s], names + i, types + i, k, qu);
    }
}

/* The interface mDNS uses: the Wi-Fi address on the device (a VPN may own
 * the default route); in the simulator DEVOS_SIM_MDNS_IF=<ip> picks one. */
static uint32_t mcast_if_addr(void)
{
    struct in_addr a = { 0 };
#ifdef ESP_PLATFORM
    devos_wifi_status_t st;
    if (devos_net_wifi_get_status(&st) == 0 && st.connected) inet_aton(st.ip, &a);
#else
    const char *e = getenv("DEVOS_SIM_MDNS_IF");
    if (e) inet_aton(e, &a);
#endif
    return a.s_addr;
}

static void mk_mcast_if(int s)
{
    struct in_addr a = { .s_addr = mcast_if_addr() };
    if (a.s_addr) setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof(a));
}

static void mdns_task(void *arg)
{
    (void)arg;
    int socks[2] = { -1, -1 };             /* [0] legacy unicast (ephemeral port), [1] 5353 member */
    socks[0] = socket(AF_INET, SOCK_DGRAM, 0);
    if (socks[0] >= 0) {
        uint8_t ttl = 255;
        setsockopt(socks[0], IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        mk_mcast_if(socks[0]);
    }
    socks[1] = socket(AF_INET, SOCK_DGRAM, 0);
    if (socks[1] >= 0) {
        int one = 1;
        setsockopt(socks[1], SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
        setsockopt(socks[1], SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
        struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(5353) };
        me.sin_addr.s_addr = htonl(INADDR_ANY);
        struct ip_mreq mr;
        mr.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
        mr.imr_interface.s_addr = mcast_if_addr() ? mcast_if_addr() : htonl(INADDR_ANY);
        if (bind(socks[1], (struct sockaddr *)&me, sizeof(me)) != 0 ||
            setsockopt(socks[1], IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) != 0) {
            close(socks[1]);
            socks[1] = -1;
        } else {
            uint8_t ttl = 255;
            setsockopt(socks[1], IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
            mk_mcast_if(socks[1]);
        }
    }
    if (socks[0] < 0 && socks[1] < 0) {
        nd_lock(&M.mx);
        snprintf(M.err, sizeof(M.err), "Couldn't open a UDP socket");
        M.gen++;
        nd_unlock(&M.mx);
        M.task = false;
        return;
    }
    nd_lock(&M.mx);
    M.ntypes = 0;
    M.nhosts = 0;
    for (int i = 0; i < N_KNOWN; i++) add_type(KNOWN[i].type);
    nd_unlock(&M.mx);

    static const int rounds_at[] = { 0, 700, 1600, 2800, 4200 };
    int round = 0;
    int64_t t0 = nd_ms(), end = t0 + M.duration;
    uint8_t *buf = nd_alloc(1600);
    while (!M.stop && nd_ms() < end && buf) {
        int64_t now = nd_ms();
        if (round < (int)(sizeof(rounds_at) / sizeof(rounds_at[0])) && now - t0 >= rounds_at[round]) {
            /* questions: service enumeration, types not asked twice yet, then details
             * for instances missing their SRV / TXT and hosts missing an address */
            const char *names[96];
            uint16_t types[96];
            static EXT_RAM_BSS_ATTR char fulls[48][112];
            int n = 0, nf = 0;
            nd_lock(&M.mx);
            names[n] = "_services._dns-sd._udp.local";
            types[n++] = DEVOS_DNS_PTR;
            static EXT_RAM_BSS_ATTR char tq[MAX_TYPES][48];
            for (int i = 0; i < M.ntypes && n < 60; i++) {
                if (M.type_asked[i] >= 2) continue;
                M.type_asked[i]++;
                snprintf(tq[i], sizeof(tq[i]), "%s.local", M.types[i]);
                names[n] = tq[i];
                types[n++] = DEVOS_DNS_PTR;
            }
            for (int i = 0; i < M.n && nf < 46 && n < 94; i++) {
                devos_mdns_svc_t *s = &M.svc[i];
                if (!s->port) {
                    snprintf(fulls[nf], sizeof(fulls[nf]), "%.60s.%.40s.local", s->instance, s->type);
                    names[n] = fulls[nf++];
                    types[n++] = DEVOS_DNS_SRV;
                    if (n < 94) {
                        names[n] = fulls[nf - 1];
                        types[n++] = DEVOS_DNS_TXT;
                    }
                } else if (s->host[0] && !s->ip[0] && nf < 46) {
                    snprintf(fulls[nf], sizeof(fulls[nf]), "%s", s->host);
                    names[n] = fulls[nf++];
                    types[n++] = DEVOS_DNS_A;
                }
            }
            nd_unlock(&M.mx);
            ask(socks, 2, names, types, n, round == 0);
            round++;
        }
        /* collect replies */
        int wait = 60;
        for (int s = 0; s < 2; s++) {
            if (socks[s] < 0) continue;
            if (nd_wait_readable(socks[s], wait) <= 0) continue;
            wait = 0;
            for (;;) {
                int k = (int)recv(socks[s], buf, 1600, MSG_DONTWAIT);
                if (k < 12) break;
                if (buf[2] & 0x80) parse_msg(buf, k, mdns_rr, NULL);   /* responses only */
            }
        }
    }
    free(buf);
    for (int s = 0; s < 2; s++) if (socks[s] >= 0) close(socks[s]);
    nd_lock(&M.mx);
    M.gen++;
    nd_unlock(&M.mx);
    M.task = false;
}

int devos_mdns_browse_start(int duration_ms)
{
    if (M.task) return -1;
    nd_lock(&M.mx);
    if (!M.svc) M.svc = nd_alloc(sizeof(devos_mdns_svc_t) * DEVOS_MDNS_MAX);
    if (!M.svc) {
        nd_unlock(&M.mx);
        return -1;
    }
    M.n = 0;
    M.err[0] = '\0';
    M.duration = duration_ms < 1000 ? 1000 : duration_ms > 20000 ? 20000 : duration_ms;
    M.stop = false;
    M.task = true;
    M.gen++;
    nd_unlock(&M.mx);
    if (nd_spawn(mdns_task, NULL, "mdns", 6144) != 0) {
        M.task = false;
        snprintf(M.err, sizeof(M.err), "Couldn't start the browser");
        return -1;
    }
    return 0;
}

void devos_mdns_stop(void) { M.stop = true; }
bool devos_mdns_busy(void) { return M.task; }
const char *devos_mdns_error(void) { return M.err; }

uint32_t devos_mdns_generation(void)
{
    nd_lock(&M.mx);
    uint32_t g = M.gen;
    nd_unlock(&M.mx);
    return g;
}

static int svc_cmp(const void *a, const void *b)
{
    const devos_mdns_svc_t *x = a, *y = b;
    int c = strcasecmp(x->type, y->type);
    return c ? c : strcasecmp(x->instance, y->instance);
}

int devos_mdns_results(devos_mdns_svc_t *out, int max)
{
    nd_lock(&M.mx);
    int n = M.svc ? (M.n < max ? M.n : max) : 0;
    if (n) memcpy(out, M.svc, sizeof(*out) * (size_t)n);
    nd_unlock(&M.mx);
    qsort(out, (size_t)n, sizeof(*out), svc_cmp);
    return n;
}

void nd_dns_init(void)
{
    nd_mutex_create(&D.mx);
    nd_mutex_create(&M.mx);
    for (int i = 0; i < DEVOS_DNS_CTX_MAX; i++) nd_mutex_create(&C[i].mx);
}
