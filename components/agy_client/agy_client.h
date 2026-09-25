#pragma once

/* agy_client: Antigravity bridge WebSocket engine (no LVGL).
 *
 * Minimal RFC6455 client over devos_net BSD sockets: same code on the
 * simulator and ESP-IDF/lwIP. Poll-driven, never blocks the UI task.
 * Frames are parsed as a stream (any size; messages larger than
 * AGY_MSG_MAX are skipped whole), pings are answered with their payload,
 * and a link that stays silent past the server's ping interval is
 * reconnected. The handshake Accept key is not checked (the bridge PSK
 * authenticates the session instead).
 *
 * Protocol: see tools/agy_bridge/bridge_server.py docstring.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AGY_DOWN = 0,
    AGY_CONNECTING,
    AGY_UP,
} agy_status_t;

typedef enum {
    AGY_ROLE_USER = 0,
    AGY_ROLE_AGY
} agy_role_t;

typedef enum {
    AGY_KIND_TEXT = 0,
    AGY_KIND_THINK,
    AGY_KIND_TOOL,
    AGY_KIND_ERROR,
} agy_kind_t;

typedef enum {
    AGY_TOOL_RUNNING = 0,
    AGY_TOOL_DONE,
    AGY_TOOL_ERROR,
} agy_tool_state_t;

#define AGY_MAX_AGENTS 8
#define AGY_MAX_ARTIFACTS 8
#define AGY_MAX_BLOCKS 80
#define AGY_NAME_MAX 64
#define AGY_STATE_MAX 24
#define AGY_BLOCK_MAX 6144
#define AGY_HOST_MAX 64
#define AGY_TOKEN_MAX 128
#define AGY_ARTIFACT_MAX 16384
#define AGY_DIFF_MAX (48 * 1024)
#define AGY_PROMPT_MAX 12288
#define AGY_PATH_MAX 160
#define AGY_PREVIEW_MAX 8192

typedef struct {
    char name[AGY_NAME_MAX];
    char state[AGY_STATE_MAX];   /* running | idle | waiting_for_input */
} agy_agent_t;

typedef struct {
    char name[AGY_NAME_MAX];
    char kind[AGY_STATE_MAX];
    char text[AGY_ARTIFACT_MAX];
} agy_artifact_t;

typedef struct {
    uint8_t role;    /* agy_role_t */
    uint8_t kind;    /* agy_kind_t */
    uint8_t state;   /* agy_tool_state_t (tool blocks) */
    uint32_t rev;    /* changes whenever the block does */
    char id[40];     /* tool call id (tool blocks) */
    char name[48];   /* tool name (tool blocks) */
    char text[AGY_BLOCK_MAX];
} agy_block_t;

typedef struct {
    bool active;
    char id[AGY_NAME_MAX];
    char text[512];
} agy_permission_t;

typedef struct {
    bool active;
    char id[AGY_NAME_MAX];
    char prompt[512];
    char choices[4][128];
    int choice_count;
} agy_question_t;

void agy_client_init(void);
void agy_client_poll(void);                 /* call ~10 Hz, never blocks UI */
int agy_client_reconnect(void);
agy_status_t agy_client_status(void);
const char *agy_client_status_text(void);
uint32_t agy_client_generation(void);

/* Server config (persisted: NVS on target, JSON file in sim) */
void agy_client_get_config(char *host, size_t host_len, int *port,
                           char *token, size_t token_len);
int agy_client_set_server(const char *host, int port);
int agy_client_set_token(const char *token);

/* Conversation */
int agy_client_send(const char *text, const char *command);
int agy_client_new(void);                   /* start a fresh conversation */
int agy_client_abort(void);                 /* stop the running turn */
const char *agy_client_conversation_id(void);
const char *agy_client_model(void);
const char *agy_client_workspace(void);
bool agy_client_busy(void);
/* Token usage of the last finished turn (0 until one finishes). */
void agy_client_usage(int *input, int *output, int *total);

/* Subagents / artifacts / blocks */
int agy_client_agent_count(void);
const agy_agent_t *agy_client_agent(int idx);
int agy_client_artifact_count(void);
const agy_artifact_t *agy_client_artifact(int idx);
int agy_client_block_count(void);
const agy_block_t *agy_client_block(int idx);
/* Unified diff of every file edited this conversation (newest last). */
const char *agy_client_diff_text(void);
uint32_t agy_client_diff_rev(void);
int agy_client_diff_file_count(void);

/* Permission + question modals */
bool agy_client_permission_pending(agy_permission_t *out);
/* Diff of the edit waiting for approval ("" when not an edit). */
const char *agy_client_permission_preview(void);
int agy_client_answer_permission(bool allow, bool always);
bool agy_client_question_pending(agy_question_t *out);
int agy_client_answer_question(int choice);          /* -1 = skip */
int agy_client_answer_question_text(const char *text); /* typed answer */

#ifdef __cplusplus
}
#endif
