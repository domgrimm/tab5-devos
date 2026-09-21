#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSH_MAX_SESSIONS            8
#define SSH_MAX_BOOKMARKS           16
#define SSH_MAX_KEYS                8
#define SSH_RX_BUFFER_SIZE          32768   /* 32 KB ring buffer per session */
#define SSH_MAX_ALIAS_LEN           64
#define SSH_MAX_HOST_LEN            128
#define SSH_MAX_USER_LEN            64
#define SSH_MAX_PATH_LEN            128

typedef enum {
    SSH_AUTH_PASSWORD = 0,
    SSH_AUTH_KEY,
    SSH_AUTH_AGENT,
    SSH_AUTH_NONE
} ssh_auth_type_t;

typedef enum {
    SSH_SESSION_DISCONNECTED = 0,
    SSH_SESSION_CONNECTING,
    SSH_SESSION_AUTHENTICATING,
    SSH_SESSION_CONNECTED,
    SSH_SESSION_CLOSED,
    SSH_SESSION_ERROR
} ssh_session_state_t;

typedef struct {
    char alias[SSH_MAX_ALIAS_LEN];
    char host[SSH_MAX_HOST_LEN];
    int  port;
    char user[SSH_MAX_USER_LEN];
    ssh_auth_type_t auth_type;
    char key_path[SSH_MAX_PATH_LEN];
} ssh_bookmark_t;

typedef struct {
    int id;                                 /* 1-based session ID */
    ssh_session_state_t state;
    char alias[SSH_MAX_ALIAS_LEN];
    char host[SSH_MAX_HOST_LEN];
    int  port;
    char user[SSH_MAX_USER_LEN];
    char command[SSH_MAX_ALIAS_LEN];        /* e.g. "bash", "htop", "tmux" */
    uint16_t cols;
    uint16_t rows;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    int ping_ms;

    /* Ring buffer for output stream */
    char rx_buf[SSH_RX_BUFFER_SIZE];
    uint32_t rx_head;
    uint32_t rx_tail;
    uint32_t rx_count;

    /* Internal context handle */
    int sock_fd;
    void *session_ctx;
    void *channel_ctx;
} ssh_session_t;

/* API */
int ssh_port_init(void);

/* Session Management */
int ssh_port_create_session(const char *alias, const char *host, int port, const char *user,
                           ssh_auth_type_t auth_type, const char *credential,
                           uint16_t cols, uint16_t rows);
int ssh_port_close_session(int session_id);
ssh_session_t *ssh_port_get_session(int session_id);
int ssh_port_get_active_sessions(int *out_ids, int max_ids);
int ssh_port_resize_pty(int session_id, uint16_t cols, uint16_t rows);

/* Data I/O */
int ssh_port_send(int session_id, const char *data, size_t len);
int ssh_port_recv(int session_id, char *buf, size_t max_len);

/* Bookmarks & Storage */
int ssh_port_load_bookmarks(ssh_bookmark_t *out_bookmarks, int max_count, int *out_count);
int ssh_port_save_bookmark(const ssh_bookmark_t *bm);
int ssh_port_delete_bookmark(int index);
int ssh_port_scan_keys(char out_keys[][SSH_MAX_PATH_LEN], int max_keys, int *out_count);

#ifdef __cplusplus
}
#endif
