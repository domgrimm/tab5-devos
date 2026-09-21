#pragma once

/* opendev_client: OpenCode / OpenChamber HTTP+SSE engine (no LVGL).
 *
 * Transport is devos_net BSD sockets, so this compiles unchanged for the
 * simulator and ESP-IDF/lwIP. All poll paths are non-blocking; the only
 * blocking calls are short-timeout REST requests issued from UI actions.
 *
 * OpenCode v1 API subset: GET /session, POST /session,
 * GET /session/:id/message?limit=N, POST /session/:id/prompt_async,
 * POST /session/:id/abort, POST /session/:id/permissions/:pid,
 * GET /session/:id/diff, GET /event (SSE).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OPENDEV_MODE_CODE = 0,    /* opencode serve */
    OPENDEV_MODE_CHAMBER      /* OpenChamber (token auth) */
} opendev_mode_t;

typedef enum {
    OPENDEV_DOWN = 0,         /* no SSE link */
    OPENDEV_CONNECTING,       /* TCP handshake in flight */
    OPENDEV_UP,               /* SSE live */
} opendev_status_t;

typedef enum {
    OPENDEV_ROLE_USER = 0,
    OPENDEV_ROLE_ASST
} opendev_role_t;

typedef enum {
    OPENDEV_KIND_TEXT = 0,
    OPENDEV_KIND_THINK,
    OPENDEV_KIND_TOOL
} opendev_kind_t;

#define OPENDEV_MAX_SESSIONS 8
#define OPENDEV_MAX_BLOCKS 64
#define OPENDEV_ID_MAX 64
#define OPENDEV_TITLE_MAX 96
#define OPENDEV_BLOCK_MAX 1536
#define OPENDEV_HOST_MAX 64
#define OPENDEV_TOKEN_MAX 128
#define OPENDEV_DIFF_MAX 8192

typedef struct {
    char id[OPENDEV_ID_MAX];
    char title[OPENDEV_TITLE_MAX];
    char model[OPENDEV_TITLE_MAX];
    bool busy;
} opendev_session_t;

typedef struct {
    uint8_t role;   /* opendev_role_t */
    uint8_t kind;   /* opendev_kind_t */
    char text[OPENDEV_BLOCK_MAX];
} opendev_block_t;

typedef struct {
    bool active;
    char id[OPENDEV_ID_MAX];
    char session_id[OPENDEV_ID_MAX];
    char text[512];
} opendev_permission_t;

/* Lifecycle */
void opendev_client_init(void);
void opendev_client_poll(void);             /* call ~10 Hz, never blocks UI */
int opendev_client_reconnect(void);         /* drop link, retry immediately */
opendev_status_t opendev_client_status(void);
const char *opendev_client_status_text(void);
uint32_t opendev_client_generation(void);   /* bumps on any store mutation */

/* Server config (persisted: NVS on target, JSON file in sim) */
void opendev_client_get_config(char *host, size_t host_len, int *port,
                               opendev_mode_t *mode, char *token, size_t token_len);
int opendev_client_set_server(const char *host, int port);
int opendev_client_set_chamber(const char *host, int port, const char *token);
int opendev_client_set_mode(opendev_mode_t mode);
/* openchamber://connect?host=H&port=P&token=T (also accepts p= as token) */
int opendev_client_pair(const char *uri);

/* Sessions (short-timeout REST, safe from UI actions) */
int opendev_client_session_count(void);
const opendev_session_t *opendev_client_session(int idx);
int opendev_client_active(void);
int opendev_client_select(int idx);         /* loads messages */
int opendev_client_refresh_sessions(void);
int opendev_client_new_session(void);
int opendev_client_send(const char *text);  /* prompt_async + optimistic block */
int opendev_client_abort(void);

/* Message blocks of the active session */
int opendev_client_block_count(void);
const opendev_block_t *opendev_client_block(int idx);

/* Tool permission */
bool opendev_client_permission_pending(opendev_permission_t *out);
int opendev_client_answer_permission(bool allow, bool always);

/* Unified diff of the active session (raw body, rendered mono) */
int opendev_client_fetch_diff(void);
const char *opendev_client_diff_text(void);

#ifdef __cplusplus
}
#endif
