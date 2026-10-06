#pragma once

/* devos_netdiag: the network diagnostics engines behind the Network app (no
 * LVGL). Every long operation runs on its own Core 0 task; the UI starts it
 * and polls a snapshot (plus a generation counter so it only redraws on
 * change). All sockets go through the VPN routing (devos_net_socket_route).
 *
 *   ping   ICMP echo to one host, with RTT history and loss / jitter stats
 *   dns    one query to a DNS server (A, AAAA, CNAME, MX, TXT, NS, PTR, SRV,
 *          SOA), answers with TTLs; an IP address becomes a PTR lookup
 *   scan   TCP connect scan of hosts (a.b.c.d, a.b.c.d/24, a.b.c.1-50,
 *          host names) x ports ("22,80,8000-8100" or "common")
 *   mdns   DNS-SD browse on the local network (_services._dns-sd enumeration
 *          plus the usual service types), resolved to host, IP, port and TXT
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void devos_netdiag_init(void);
/* True once devos_netdiag_init() has run. The Network app owns initialisation;
 * Jobs asks before using the request-specific engines so a switched-off app
 * never leaves one half-built (AGENTS.md invariant 10). */
bool devos_netdiag_ready(void);

/* ------------------------------------------------------------------ ping */
#define DEVOS_PING_HIST 240

typedef struct {
    bool running;
    char target[64];
    char ip[16];
    int size;
    int sent, received;
    float last_ms;                  /* < 0: the last one was lost */
    float min_ms, avg_ms, max_ms;
    float jitter_ms;                /* mean |difference| between consecutive replies */
    int ttl;                        /* of the last reply */
    char error[80];
} devos_ping_stats_t;

/* count 0 = until stopped. interval 200..10000 ms, size 0..1400 bytes. */
int devos_ping_start(const char *host, int count, int interval_ms, int size);
void devos_ping_stop(void);
void devos_ping_stats(devos_ping_stats_t *out);
/* Oldest first; each entry an RTT in ms, or -1 for a lost packet. */
int devos_ping_history(float *out, int max);
uint32_t devos_ping_generation(void);

/* ------------------------------------------------------------------ probe
 * A request-specific one-shot ICMP echo for Jobs. Unlike the ping engine it
 * owns its socket, id and deadline, so it never disturbs a running ping in
 * the Network app. One probe at a time (a second submit is refused). */
typedef struct {
    bool ok;
    char ip[48];
    int latency_ms;                 /* -1 when lost */
    char error[64];                 /* "" on success */
} devos_probe_result_t;

/* Start a probe; returns a handle > 0, -1 on bad input, -2 when one is already
 * running. */
int devos_probe_submit(const char *host, int timeout_ms);
/* state: 0 running, 1 done, 2 failed; -1 on a stale handle. */
int devos_probe_poll(int handle, int *state, devos_probe_result_t *out);
void devos_probe_cancel(int handle);
void devos_probe_release(int handle);

/* ------------------------------------------------------------------ dns */
enum {
    DEVOS_DNS_A = 1, DEVOS_DNS_NS = 2, DEVOS_DNS_CNAME = 5, DEVOS_DNS_SOA = 6, DEVOS_DNS_PTR = 12,
    DEVOS_DNS_MX = 15, DEVOS_DNS_TXT = 16, DEVOS_DNS_AAAA = 28, DEVOS_DNS_SRV = 33, DEVOS_DNS_ANY = 255
};

#define DEVOS_DNS_MAX_RR 40

typedef struct {
    char name[128];
    uint16_t type;
    uint8_t section;                /* 0 answer, 1 authority, 2 additional */
    uint32_t ttl;
    char data[256];                 /* formatted: "192.0.2.1", "10 mail.example.com." ... */
} devos_dns_rr_t;

typedef struct {
    bool busy;                      /* async lookup still running */
    char query[128];                /* what was asked (PTR name for an IP) */
    uint16_t qtype;
    char server[48];
    int rcode;                      /* 0 NOERROR, 3 NXDOMAIN ... */
    bool authoritative, truncated, recursion;
    int ms;
    int n;
    devos_dns_rr_t rr[DEVOS_DNS_MAX_RR];
    char error[96];                 /* transport error; "" when a reply came */
} devos_dns_result_t;

/* The DNS server lookups use by default (DHCP's on the device). */
void devos_dns_default_server(char *out, size_t cap);
/* Start a lookup on a worker (server NULL / "" = default). */
int devos_dns_lookup_start(const char *server, const char *name, uint16_t type);
/* Copies the latest result (busy while running). */
void devos_dns_lookup_result(devos_dns_result_t *out);
uint32_t devos_dns_generation(void);
const char *devos_dns_type_name(uint16_t type);
const char *devos_dns_rcode_name(int rcode);

/* Request-specific lookups for Jobs (PLAN.md 7.3): an independent task, socket
 * and result per ticket, so a job never disturbs an in-progress UI lookup.
 * Returns a ticket > 0, or 0 (bad name / pool full). */
int devos_dns_ctx_start(const char *server, const char *name, uint16_t type);
/* 0 running, 1 done (out filled), -1 unknown ticket. */
int devos_dns_ctx_poll(int ticket, devos_dns_result_t *out);
void devos_dns_ctx_cancel(int ticket);
void devos_dns_ctx_release(int ticket);

/* ------------------------------------------------------------------ scan */
#define DEVOS_SCAN_MAX_HOSTS 1024
#define DEVOS_SCAN_MAX_OPEN  24         /* open ports kept per host */

typedef struct {
    char ip[16];
    char name[64];                  /* reverse DNS / the name you typed, may be "" */
    bool alive;                     /* answered ping or refused a port */
    int rtt_ms;                     /* ping RTT, -1 unknown */
    int n_open;
    uint16_t open[DEVOS_SCAN_MAX_OPEN];
} devos_scan_host_t;

typedef struct {
    bool running;
    char phase[40];                 /* "Pinging 254 hosts", "Scanning ports", "Done" */
    int hosts_total, hosts_alive;
    int probes_total, probes_done;
    int open_ports;
    int elapsed_ms;
    char error[96];
} devos_scan_status_t;

/* Validate targets / ports without starting (for the form); returns host /
 * port count or -1 with the reason in err. */
int devos_scan_count_hosts(const char *targets, char *err, size_t errcap);
int devos_scan_count_ports(const char *ports, char *err, size_t errcap);
int devos_scan_start(const char *targets, const char *ports, int timeout_ms, bool ping_first);
void devos_scan_stop(void);
void devos_scan_status(devos_scan_status_t *out);
/* Hosts that are alive or have open ports, in address order. */
int devos_scan_results(devos_scan_host_t *out, int max);
uint32_t devos_scan_generation(void);
/* "ssh", "http", ... or NULL */
const char *devos_scan_service(int port);
#define DEVOS_SCAN_COMMON_PORTS "21,22,23,25,53,80,81,88,110,139,143,443,445,554,631,1883,1900,2049,3000,3306," \
                                "3389,5000,5001,5353,5432,5900,6053,6379,8000,8008,8080,8081,8086,8123,8443,8883," \
                                "8888,9000,9090,9100,9443,32400,51820"

/* ------------------------------------------------------------------ mdns */
#define DEVOS_MDNS_MAX 96

typedef struct {
    char instance[64];              /* "Kitchen Speaker" */
    char type[40];                  /* "_http._tcp" */
    char host[64];                  /* "kitchen.local" */
    char ip[16];
    uint16_t port;
    char txt[200];                  /* "version=2024.6.1  mac=24:0a:..." */
} devos_mdns_svc_t;

/* Browse for duration_ms (queries repeat, answers accumulate). */
int devos_mdns_browse_start(int duration_ms);
void devos_mdns_stop(void);
bool devos_mdns_busy(void);
/* Sorted by type, then instance. */
int devos_mdns_results(devos_mdns_svc_t *out, int max);
uint32_t devos_mdns_generation(void);
const char *devos_mdns_error(void);
/* Friendly name for a service type ("_ssh._tcp" -> "SSH"), or NULL. */
const char *devos_mdns_type_label(const char *type);

/* ------------------------------------------------------------------ wol */
/* Wake-on-LAN: send a magic packet to a sleeping machine's NIC. The MAC is
 * accepted as "aa:bb:cc:dd:ee:ff", "aa-bb-cc-dd-ee-ff", "aabb.ccdd.eeff" or
 * plain "aabbccddeeff". With no address the packet goes to the subnet
 * broadcast (and 255.255.255.255); typing a host or IP sends it there
 * directly, which also reaches a machine across a routed network (or a
 * WireGuard / Tailscale subnet - the socket is routed like every other). */

/* Parse into 6 bytes; returns 0, or -1 with the reason in err. */
int devos_wol_parse_mac(const char *text, uint8_t mac[6], char *err, size_t errcap);
/* Format 6 bytes back to "aa:bb:cc:dd:ee:ff". */
void devos_wol_format_mac(const uint8_t mac[6], char *out, size_t cap);
/* Send one magic packet. addr "" / NULL = broadcast. Returns 0 on success, -1
 * with the reason in devos_wol_error(). */
int devos_wol_send(const uint8_t mac[6], const char *addr);
const char *devos_wol_error(void);
/* Where the last packet went, for the result line ("255.255.255.255:9"). */
const char *devos_wol_last_target(void);

/* Request-specific send for Jobs: caller-owned error/target buffers, so a job
 * never reads the Network app's shared last-error (PLAN.md 7.3). */
int devos_wol_send_ex(const uint8_t mac[6], const char *addr,
                      char *target_out, size_t target_cap,
                      char *err_out, size_t err_cap);

/* Async WoL for Jobs network.wol: the target is resolved on a worker, so a
 * hostname never blocks the scheduler task. Returns a ticket > 0, or 0 (no
 * free slot). */
int devos_wol_submit(const uint8_t mac[6], const char *addr);
/* 0 running, 1 done (out filled), -1 unknown ticket. */
int devos_wol_poll(int ticket, bool *ok, char *target, size_t target_cap,
                   char *error, size_t error_cap);
void devos_wol_cancel(int ticket);
void devos_wol_release(int ticket);

#ifdef __cplusplus
}
#endif
