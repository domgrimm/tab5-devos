#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MICROLINK_MAX_PEERS         16
#define MICROLINK_MAX_NAME_LEN      64
#define MICROLINK_MAX_IP_LEN        46
#define MICROLINK_MAX_KEY_LEN       128

typedef enum {
    MICROLINK_STATE_DISCONNECTED = 0,
    MICROLINK_STATE_CONNECTING,
    MICROLINK_STATE_AUTHENTICATING,
    MICROLINK_STATE_CONNECTED,
    MICROLINK_STATE_ERROR
} microlink_state_t;

typedef enum {
    MICROLINK_PEER_OS_UNKNOWN = 0,
    MICROLINK_PEER_OS_LINUX,
    MICROLINK_PEER_OS_MACOS,
    MICROLINK_PEER_OS_WINDOWS,
    MICROLINK_PEER_OS_BSD,
    MICROLINK_PEER_OS_MOBILE
} microlink_peer_os_t;

typedef struct {
    char name[MICROLINK_MAX_NAME_LEN];
    char fqdn[MICROLINK_MAX_NAME_LEN];
    char ip[MICROLINK_MAX_IP_LEN];
    microlink_peer_os_t os_type;
    char os_desc[32];
    bool is_direct;                 /* Direct WireGuard P2P vs DERP relay */
    int ping_ms;
    bool is_online;
    uint32_t last_seen_sec;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
} microlink_peer_t;

typedef struct {
    microlink_state_t state;
    char node_name[MICROLINK_MAX_NAME_LEN];
    char tailnet_domain[MICROLINK_MAX_NAME_LEN];
    char assigned_ip[MICROLINK_MAX_IP_LEN];
    char derp_relay_name[32];
    int derp_ping_ms;
    uint16_t mtu;
    bool is_wireguard_hw;
    uint64_t total_rx_bytes;
    uint64_t total_tx_bytes;
    int peer_count;
    microlink_peer_t peers[MICROLINK_MAX_PEERS];
} microlink_status_t;

typedef struct {
    char auth_key[MICROLINK_MAX_KEY_LEN];
    char hostname[32];
    bool auto_connect;
    int derp_region_pref;
} microlink_config_t;

typedef void (*microlink_event_cb_t)(microlink_state_t new_state, void *user_data);

/* Core API */
int microlink_init(const microlink_config_t *config);
int microlink_connect(void);
int microlink_disconnect(void);
microlink_state_t microlink_get_state(void);
int microlink_get_status(microlink_status_t *out_status);
/* MagicDNS-style lookup of a tailnet host name (node or peer) without copying
 * the ~4 KB status struct. Returns 0 and fills out_ip when connected and found. */
int microlink_resolve(const char *name, char *out_ip, size_t out_len);

/* Diagnostics & Controls */
int microlink_ping_derp(int *out_ping_ms);
int microlink_ping_peer(int peer_idx, int *out_ping_ms);
int microlink_refresh_peers(void);
int microlink_set_auth_key(const char *auth_key);
int microlink_get_auth_key(char *out_buf, size_t buf_len);

/* NVS Storage */
int microlink_nvs_save(void);
int microlink_nvs_load(void);

/* Event Listener */
void microlink_add_state_listener(microlink_event_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif
