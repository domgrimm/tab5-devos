#pragma once

/* devos_docker: Docker Engine API client (no LVGL) - straight to a Docker
 * daemon / socket proxy (http://host:2375) or through Portainer
 * (https://host:9443, X-API-Key, one of its environments).
 *
 * One Core 0 worker polls while the app is open (devos_docker_set_active):
 * the container list every few seconds, the selected container's stats, and
 * its logs while they are shown (tail, then only what's new). Actions
 * (start / stop / restart) are queued and followed by a refresh.
 * Requests go through devos_http, so VPN routing and .local names work.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_DOCKER_MAX 96

typedef enum { DEVOS_DOCKER_DIRECT = 0, DEVOS_DOCKER_PORTAINER = 1 } devos_docker_mode_t;

typedef struct {
    devos_docker_mode_t mode;
    char url[160];                  /* http://nas.local:2375  or  https://portainer.local:9443 */
    char api_key[128];              /* Portainer access token (kept in NVS) */
    int endpoint;                   /* Portainer environment id, 0 = the first one */
    bool insecure;                  /* accept a self-signed certificate */
    int interval_s;                 /* list refresh */
} devos_docker_config_t;

typedef struct {
    char id[16];                    /* short id (12 chars) */
    char name[64];
    char image[96];
    char state[16];                 /* running, exited, restarting, paused, created, dead */
    char status[64];                /* "Up 3 hours (healthy)" */
    char ports[96];                 /* "8080->80/tcp, 443/tcp" */
    char compose[48];               /* compose project, if any */
    int64_t created;
    uint8_t health;                 /* 0 none, 1 healthy, 2 unhealthy, 3 starting */
} devos_docker_ct_t;

typedef struct {
    bool valid;
    char id[16];
    float cpu_pct;
    uint64_t mem_used, mem_limit;
    uint64_t net_rx, net_tx;
    int pids;
} devos_docker_stats_t;

typedef struct {
    bool configured, active;
    bool busy;                      /* a request is in flight */
    char host[64];                  /* daemon host name */
    char version[24];
    char endpoint_name[48];         /* Portainer environment */
    int running, total;
    int64_t updated;                /* time() of the last good list */
    char error[112];                /* last failure, "" when fine */
    char note[112];                 /* last action result */
} devos_docker_status_t;

void devos_docker_init(void);
/* True once devos_docker_init() has run (the app owns it). A non-blocking
 * readiness query for Jobs, so availability never triggers init on the Core 0
 * scheduler. */
bool devos_docker_ready(void);
void devos_docker_get_config(devos_docker_config_t *out);
void devos_docker_set_config(const devos_docker_config_t *c);
bool devos_docker_configured(void);

/* Poll while true (the app is shown); refresh now. */
void devos_docker_set_active(bool active);
void devos_docker_refresh(void);

void devos_docker_status(devos_docker_status_t *out);
/* Sorted: running first, then by name. */
int devos_docker_list(devos_docker_ct_t *out, int max);
uint32_t devos_docker_generation(void);

/* "start", "stop" or "restart" the container (by id). Fire-and-forget UI
 * wrapper: it submits through the correlated queue below with a status note,
 * and returns 0 when queued, -1 on bad arguments or a full queue. */
int devos_docker_action(const char *id, const char *action);

/* ---- request-specific operations (Jobs + UI), PLAN.md 7.3 ----------------
 * Every command gets a ticket; the worker runs queued commands whether or not
 * the UI is being polled (devos_docker_set_active only controls the UI's list /
 * stats / logs refresh). Each request snapshots the config - and the Portainer
 * environment - at submit time, so a later settings change cannot send it to
 * the wrong host. Commands are serialised by the single worker and never
 * overwrite one another. Poll until the state leaves PENDING, then release
 * exactly once. */
#define DEVOS_DOCKER_REQS 8

typedef enum {
    DEVOS_DOCKER_REQ_PENDING = 1,
    DEVOS_DOCKER_REQ_DONE,          /* attempted; status != 0, or 0 on transport error */
    DEVOS_DOCKER_REQ_FAILED,        /* could not be attempted (not configured / cancelled) */
} devos_docker_req_state_t;

typedef struct {
    char id[16];                    /* short id (12 chars) */
    char name[64];
    char image[96];
    char state[16];                 /* running, exited, restarting, paused, ... */
    char health[16];                /* healthy / unhealthy / starting; "" if none */
    int64_t started;                /* unix seconds of State.StartedAt, 0 unknown */
    int64_t observed;               /* time() when the inspect was read */
} devos_docker_inspect_t;

typedef struct {
    devos_docker_req_state_t state;
    char action[12];                /* "start" / "stop" / "restart" / "inspect" */
    int status;                     /* HTTP status; 0 on transport error */
    char error[96];                 /* transport error / API message */
    devos_docker_inspect_t inspect; /* filled for a successful inspect */
} devos_docker_req_result_t;

/* Submit a command by container id or name ("start" / "stop" / "restart" /
 * "inspect"). timeout_ms bounds the HTTP exchange (<= 0 uses the default).
 * Returns a ticket > 0, or 0 when Docker isn't configured or the queue is
 * full. An inspect resolves its target independently of the UI's cached list. */
uint32_t devos_docker_request(const char *id, const char *action, int timeout_ms);
uint32_t devos_docker_inspect(const char *id, int timeout_ms);
/* Snapshot a ticket's state/result. False once released. Terminal states stay
 * readable until release. */
bool devos_docker_request_poll(uint32_t ticket, devos_docker_req_result_t *out);
/* Whether the request has been handed to the daemon already (the worker picked
 * it up). After this point a mutation's outcome may be unknown if it is
 * cancelled. Safe before cancel/release. */
bool devos_docker_request_started(uint32_t ticket);
/* Stop treating a request's (possibly in-flight) result as wanted. The worker
 * discards a running result it no longer owns; a mutation already sent keeps an
 * unknown outcome. Safe to call once before release. */
void devos_docker_request_cancel(uint32_t ticket);
/* Free a ticket slot (safe once; other tickets are unaffected). */
void devos_docker_request_release(uint32_t ticket);

/* The container whose stats (and logs, if logs is true) are kept fresh. */
void devos_docker_select(const char *id, bool logs);
void devos_docker_stats(devos_docker_stats_t *out);
/* Log text of the selected container (newest last, "HH:MM:SS " prefixes,
 * stderr lines start with "! "). Returns bytes copied. */
size_t devos_docker_logs(char *out, size_t cap, uint32_t *gen);

/* Deep links from a container's published ports, on the Docker host (the
 * server URL's host). The Docker app opens them in the Terminal / REST. */
/* The host port container port `priv`/tcp is published on, 0 = none. */
int devos_docker_published(const devos_docker_ct_t *c, int priv);
/* "host:port" for SSH: its port 22 (or 2222, linuxserver's openssh-server). */
bool devos_docker_ssh_target(const devos_docker_ct_t *c, char *out, size_t cap);
/* "http[s]://host:port/" of its first published web port (80, 443, 8080 ...). */
bool devos_docker_web_url(const devos_docker_ct_t *c, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
