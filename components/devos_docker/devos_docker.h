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

/* "start", "stop" or "restart" the container (by id). */
int devos_docker_action(const char *id, const char *action);

/* The container whose stats (and logs, if logs is true) are kept fresh. */
void devos_docker_select(const char *id, bool logs);
void devos_docker_stats(devos_docker_stats_t *out);
/* Log text of the selected container (newest last, "HH:MM:SS " prefixes,
 * stderr lines start with "! "). Returns bytes copied. */
size_t devos_docker_logs(char *out, size_t cap, uint32_t *gen);

#ifdef __cplusplus
}
#endif
