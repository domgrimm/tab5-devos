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
#define SSH_RX_BUFFER_SIZE          32768   /* 32 KB buffer per session */
#define SSH_MAX_ALIAS_LEN           64
#define SSH_MAX_HOST_LEN            128
#define SSH_MAX_USER_LEN            64
#define SSH_MAX_PATH_LEN            128
#define SSH_MAX_PASSWORD_LEN        128

typedef enum {
    SSH_AUTH_PASSWORD = 0,      /* credential = password */
    SSH_AUTH_KEY,               /* credential = private key path (PEM, or OpenSSH + .pub) */
    SSH_AUTH_AGENT,             /* unused */
    SSH_AUTH_NONE,
    SSH_AUTH_DEVICE_KEY,        /* this device's ECDSA key; credential = optional password fallback */
} ssh_auth_type_t;

typedef enum {
    SSH_SESSION_DISCONNECTED = 0,   /* slot free */
    SSH_SESSION_CONNECTING,
    SSH_SESSION_AUTHENTICATING,
    SSH_SESSION_CONNECTED,
    SSH_SESSION_CLOSED,             /* ended normally (remote exit / user close) */
    SSH_SESSION_ERROR,              /* failed; see last_error */
    SSH_SESSION_HOSTKEY_PROMPT,     /* unknown host key: waiting for ssh_port_hostkey_answer() */
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
    volatile ssh_session_state_t state;
    char alias[SSH_MAX_ALIAS_LEN];
    char host[SSH_MAX_HOST_LEN];
    int  port;
    char user[SSH_MAX_USER_LEN];
    uint16_t cols;
    uint16_t rows;
    uint32_t rx_bytes;
    uint32_t tx_bytes;

    char last_error[224];                   /* human-readable failure reason */
    char hostkey_type[24];                  /* for the trust prompt, e.g. "ECDSA-256" */
    char hostkey_fp[64];                    /* "SHA256:..." */
    bool hostkey_changed;                   /* known_hosts mismatch (refused) */

    /* Simulator only: output ring for the forkpty backend */
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

/* Session Management. Returns the session id (1..SSH_MAX_SESSIONS) or -1.
 * Connection happens in the background: watch ssh_port_get_session()->state. */
int ssh_port_create_session(const char *alias, const char *host, int port, const char *user,
                           ssh_auth_type_t auth_type, const char *credential,
                           uint16_t cols, uint16_t rows);
/* Live session: disconnect. Finished/failed session: free the slot. */
int ssh_port_close_session(int session_id);
ssh_session_t *ssh_port_get_session(int session_id);
int ssh_port_get_active_sessions(int *out_ids, int max_ids);
int ssh_port_resize_pty(int session_id, uint16_t cols, uint16_t rows);
/* Answer an SSH_SESSION_HOSTKEY_PROMPT (trust = add to known_hosts). */
int ssh_port_hostkey_answer(int session_id, bool trust);

/* Data I/O */
int ssh_port_send(int session_id, const char *data, size_t len);
int ssh_port_recv(int session_id, char *buf, size_t max_len);

/* Device key: an ECDSA P-256 key generated on this device (private key in NVS),
 * usable as SSH_AUTH_DEVICE_KEY once its public key is in the server's
 * ~/.ssh/authorized_keys. */
bool ssh_port_devkey_exists(void);
int  ssh_port_devkey_generate(void);
int  ssh_port_devkey_public(char *out, size_t len);    /* OpenSSH "ecdsa-sha2-nistp256 ..." line */

/* Bookmarks & Storage */
int ssh_port_load_bookmarks(ssh_bookmark_t *out_bookmarks, int max_count, int *out_count);
int ssh_port_save_bookmark(const ssh_bookmark_t *bm);
int ssh_port_delete_bookmark(int index);
int ssh_port_scan_keys(char out_keys[][SSH_MAX_PATH_LEN], int max_keys, int *out_count);

#ifdef __cplusplus
}
#endif
