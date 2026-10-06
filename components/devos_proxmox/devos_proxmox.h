#pragma once

/* devos_proxmox: Proxmox VE API client (no LVGL) - node and guest control for
 * a Proxmox VE host over its HTTPS API (/api2/json).
 *
 * One Core 0 worker polls while the app is open (devos_proxmox_set_active):
 * the cluster's guests and nodes. Actions (start / stop / shutdown / reboot)
 * are queued as correlated tickets and followed by a refresh. Requests go
 * through devos_http, so VPN routing and .local names work.
 *
 * Auth is an API token (Datacenter > Permissions > API Tokens): a token id
 * "user@realm!tokenid" and its secret, sent as
 *   Authorization: PVEAPIToken=user@realm!tokenid=secret
 * so there is no session to renew. Proxmox serves a self-signed certificate by
 * default, so `insecure` is normally on.
 *
 * A guest is addressed by its vmid alone: the node and the guest type come from
 * the cluster resources, so a job never has to know which node a VM is on.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_PROXMOX_MAX        96     /* guests kept */
#define DEVOS_PROXMOX_MAX_NODES  16

typedef struct {
    char url[160];                  /* https://pve.local:8006 (no /api2/json) */
    char token_id[96];              /* user@realm!tokenid */
    char secret[96];                /* the token secret (kept in NVS) */
    bool insecure;                  /* accept Proxmox's self-signed certificate */
    int interval_s;                 /* list refresh */
} devos_proxmox_config_t;

typedef enum { DEVOS_PROXMOX_QEMU = 0, DEVOS_PROXMOX_LXC = 1 } devos_proxmox_kind_t;

typedef struct {
    int vmid;
    uint8_t kind;                   /* devos_proxmox_kind_t */
    char name[64];
    char node[32];
    char status[16];                /* running, stopped, paused, suspended */
    bool template_guest;
    float cpu;                      /* 0..1 of one core */
    uint64_t mem, maxmem;           /* bytes */
    uint64_t disk, maxdisk;         /* bytes */
    int64_t uptime_s;
} devos_proxmox_guest_t;

typedef struct {
    char node[32];
    char status[16];                /* online, offline, unknown */
    float cpu;
    uint64_t mem, maxmem;
    int64_t uptime_s;
    int guests;                     /* guests the node reports */
} devos_proxmox_node_t;

typedef struct {
    bool configured, active;
    bool busy;                      /* a request is in flight */
    char host[64];                  /* from /version */
    char version[24];               /* "8.2.4" */
    int running, total;             /* guests */
    int nodes;
    int64_t updated;                /* time() of the last good list */
    char error[112];                /* last failure, "" when fine */
    char note[112];                 /* last action result */
} devos_proxmox_status_t;

void devos_proxmox_init(void);
/* True once devos_proxmox_init() has run (the app owns it). A non-blocking
 * readiness query for Jobs, so availability never triggers init on the Core 0
 * scheduler. */
bool devos_proxmox_ready(void);
void devos_proxmox_get_config(devos_proxmox_config_t *out);
void devos_proxmox_set_config(const devos_proxmox_config_t *c);
bool devos_proxmox_configured(void);

/* Poll while true (the app is shown); refresh now. */
void devos_proxmox_set_active(bool active);
void devos_proxmox_refresh(void);

void devos_proxmox_status(devos_proxmox_status_t *out);
/* Sorted: running first, then by name. */
int devos_proxmox_list(devos_proxmox_guest_t *out, int max);
int devos_proxmox_nodes(devos_proxmox_node_t *out, int max);
uint32_t devos_proxmox_generation(void);

/* "start" / "stop" / "shutdown" / "reboot" the guest (by vmid). Fire-and-forget
 * UI wrapper: it submits through the correlated queue below with a status note,
 * and returns 0 when queued, -1 on bad arguments or a full queue. */
int devos_proxmox_action(int vmid, const char *action);

/* ---- request-specific operations (Jobs + UI) -----------------------------
 * Every command gets a ticket; the worker resolves the guest's node and type
 * from the cluster resources and runs it whether or not the UI is being polled
 * (devos_proxmox_set_active only controls the UI's refresh). Each request
 * snapshots the config at submit time, so a later settings change cannot send
 * it to the wrong host. Poll until the state leaves PENDING, then release
 * exactly once. */
#define DEVOS_PROXMOX_REQS 8

typedef enum {
    DEVOS_PROXMOX_REQ_PENDING = 1,
    DEVOS_PROXMOX_REQ_DONE,         /* attempted; status != 0, or 0 on transport error */
    DEVOS_PROXMOX_REQ_FAILED,       /* could not be attempted (not configured / no such guest) */
} devos_proxmox_req_state_t;

typedef struct {
    int vmid;
    uint8_t kind;
    char name[64];
    char node[32];
    char status[16];
    float cpu;
    uint64_t mem, maxmem;
    int64_t uptime_s;
    int64_t observed;               /* time() when the status was read */
} devos_proxmox_inspect_t;

typedef struct {
    devos_proxmox_req_state_t state;
    char action[12];                /* "start" / "stop" / "shutdown" / "reboot" / "status" */
    int status;                     /* HTTP status; 0 on transport error */
    char error[96];                 /* transport error / API message */
    char task[80];                  /* the UPID Proxmox returned, "" when none */
    devos_proxmox_inspect_t inspect;/* filled for a successful status read */
} devos_proxmox_req_result_t;

/* Submit a command for a guest ("start" / "stop" / "shutdown" / "reboot" /
 * "status"). timeout_ms bounds the HTTP exchange (<= 0 uses the default).
 * Returns a ticket > 0, or 0 when Proxmox isn't configured or the queue is
 * full. */
uint32_t devos_proxmox_request(int vmid, const char *action, int timeout_ms);
/* Snapshot a ticket's state/result. False once released. Terminal states stay
 * readable until release. */
bool devos_proxmox_request_poll(uint32_t ticket, devos_proxmox_req_result_t *out);
/* Whether the request has been handed to Proxmox already (the worker picked it
 * up). After this point a mutation's outcome may be unknown if it is cancelled.
 * Safe before cancel/release. */
bool devos_proxmox_request_started(uint32_t ticket);
/* Stop treating a request's (possibly in-flight) result as wanted. The worker
 * discards a running result it no longer owns; a mutation already sent keeps an
 * unknown outcome. Safe to call once before release. */
void devos_proxmox_request_cancel(uint32_t ticket);
/* Free a ticket slot (safe once; other tickets are unaffected). */
void devos_proxmox_request_release(uint32_t ticket);

/* Deep links, on the Proxmox host (the server URL's host). The Proxmox app
 * opens the web UI in REST / the SSH host in the Terminal. */
/* "https://host:8006/?console=kvm&novnc=1&vmid=100&node=pve" for a guest. */
bool devos_proxmox_console_url(const devos_proxmox_guest_t *g, char *out, size_t cap);
/* "user@host" SSH target derived from the URL host and the token's user. */
bool devos_proxmox_ssh_target(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
