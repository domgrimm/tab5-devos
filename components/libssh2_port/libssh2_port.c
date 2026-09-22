#include "libssh2_port.h"
#include "devos_config.h"
#include "devos_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "libssh2.h"
static const char *TAG = "libssh2_port";
#else
#define TAG "libssh2_port"
#include <pty.h>
#include <utmp.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <errno.h>
#endif

#define SSH_BOOKMARKS_FILE TAB5_SD_MOUNT_POINT "/.ssh/bookmarks.json"
#define SSH_KEYS_DIR       TAB5_SD_MOUNT_POINT "/.ssh"

static EXT_RAM_BSS_ATTR ssh_session_t s_sessions[SSH_MAX_SESSIONS];

#ifdef ESP_PLATFORM
/* =========================================================================
 * On-device SSH client (libssh2, mbedTLS backend) over the real lwIP socket
 * layer (devos_net_socket_*). Each session runs its own task on the network/
 * crypto core; the UI task exchanges bytes through lock-free stream buffers.
 * ========================================================================= */
#define SSH_TX_BUFFER_SIZE  8192

typedef struct {
    TaskHandle_t         task;
    StreamBufferHandle_t rx;        /* remote -> UI */
    StreamBufferHandle_t tx;        /* UI -> remote */
    uint8_t             *rx_store;  /* PSRAM backing for rx stream buffer */
    uint8_t             *tx_store;  /* PSRAM backing for tx stream buffer */
    void                *rx_ctrl;   /* StaticStreamBuffer_t */
    void                *tx_ctrl;   /* StaticStreamBuffer_t */
    ssh_auth_type_t      auth;
    char                 cred[256]; /* password, or private-key path */
    volatile bool        close_req;
    volatile bool        resize_req;
    volatile uint16_t    rcols, rrows;
} ssh_aux_t;

static ssh_aux_t s_aux[SSH_MAX_SESSIONS];

static void ssh_aux_free(ssh_aux_t *a)
{
    if (a->rx) { vStreamBufferDelete(a->rx); a->rx = NULL; }
    if (a->tx) { vStreamBufferDelete(a->tx); a->tx = NULL; }
    free(a->rx_store); a->rx_store = NULL;
    free(a->tx_store); a->tx_store = NULL;
    free(a->rx_ctrl);  a->rx_ctrl  = NULL;
    free(a->tx_ctrl);  a->tx_ctrl  = NULL;
}

static void ssh_log_err(LIBSSH2_SESSION *session, int id, const char *stage)
{
    char *msg = NULL;
    int len = 0;
    if (session) libssh2_session_last_error(session, &msg, &len, 0);
    ESP_LOGE(TAG, "ssh %d: %s failed: %s", id, stage, msg ? msg : "?");
}

static void ssh_session_task(void *arg)
{
    int slot = (int)(intptr_t)arg;
    ssh_session_t *s = &s_sessions[slot];
    ssh_aux_t     *a = &s_aux[slot];
    LIBSSH2_SESSION *session = NULL;
    LIBSSH2_CHANNEL *chan = NULL;
    char buf[1024];

    s->state = SSH_SESSION_CONNECTING;
    int fd = devos_net_socket_connect(s->host, s->port, 10000);
    if (fd < 0) { ESP_LOGE(TAG, "ssh %d: TCP connect to %s:%d failed", s->id, s->host, s->port); goto cleanup; }
    s->sock_fd = fd;

    session = libssh2_session_init();
    if (!session) { ESP_LOGE(TAG, "ssh %d: session_init failed", s->id); goto cleanup; }
    s->session_ctx = session;
    libssh2_session_set_blocking(session, 1);

    s->state = SSH_SESSION_AUTHENTICATING;
    if (libssh2_session_handshake(session, fd) != 0) { ssh_log_err(session, s->id, "handshake"); goto cleanup; }

    int rc;
    if (a->auth == SSH_AUTH_KEY && a->cred[0]) {
        char pub[300];
        snprintf(pub, sizeof(pub), "%s.pub", a->cred);
        rc = libssh2_userauth_publickey_fromfile(session, s->user, pub, a->cred, NULL);
        if (rc) rc = libssh2_userauth_publickey_fromfile(session, s->user, NULL, a->cred, NULL);
    } else {
        rc = libssh2_userauth_password(session, s->user, a->cred);
    }
    if (rc) { ssh_log_err(session, s->id, "auth"); goto cleanup; }

    chan = libssh2_channel_open_session(session);
    if (!chan) { ssh_log_err(session, s->id, "channel_open"); goto cleanup; }
    s->channel_ctx = chan;

    libssh2_channel_request_pty_ex(chan, "xterm-256color", 14, NULL, 0, s->cols, s->rows, 0, 0);
    if (libssh2_channel_shell(chan) != 0) { ssh_log_err(session, s->id, "shell"); goto cleanup; }

    libssh2_session_set_blocking(session, 0);
    libssh2_keepalive_config(session, 1, 30);
    s->state = SSH_SESSION_CONNECTED;
    ESP_LOGI(TAG, "ssh %d: connected %s@%s:%d", s->id, s->user, s->host, s->port);

    while (!a->close_req) {
        if (a->resize_req) {
            libssh2_channel_request_pty_size(chan, a->rcols, a->rrows);
            a->resize_req = false;
        }

        /* UI -> remote */
        size_t tn = xStreamBufferReceive(a->tx, buf, sizeof(buf), 0);
        size_t off = 0;
        while (off < tn && !a->close_req) {
            ssize_t w = libssh2_channel_write(chan, buf + off, tn - off);
            if (w == LIBSSH2_ERROR_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            if (w < 0) { a->close_req = true; break; }
            off += (size_t)w;
            s->tx_bytes += (uint32_t)w;
        }

        /* remote -> UI */
        ssize_t r = libssh2_channel_read(chan, buf, sizeof(buf));
        if (r > 0) {
            xStreamBufferSend(a->rx, buf, (size_t)r, 0);
            s->rx_bytes += (uint32_t)r;
        } else if (r == LIBSSH2_ERROR_EAGAIN) {
            vTaskDelay(pdMS_TO_TICKS(10));
        } else if (r < 0) {
            break;
        } else { /* 0 bytes */
            if (libssh2_channel_eof(chan)) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

cleanup:
    if (session) libssh2_session_set_blocking(session, 1);
    if (chan) { libssh2_channel_close(chan); libssh2_channel_free(chan); }
    if (session) { libssh2_session_disconnect(session, "devOS session closed"); libssh2_session_free(session); }
    if (s->sock_fd >= 0) { devos_net_socket_close(s->sock_fd); s->sock_fd = -1; }
    s->channel_ctx = NULL;
    s->session_ctx = NULL;
    s->state = (s->state == SSH_SESSION_CONNECTED || a->close_req) ? SSH_SESSION_CLOSED : SSH_SESSION_ERROR;
    ssh_aux_free(a);
    a->task = NULL;
    vTaskDelete(NULL);
}
#endif /* ESP_PLATFORM */

/* Push data to session ring buffer */
static void __attribute__((unused)) push_to_rx(ssh_session_t *sess, const char *data, size_t len)
{
    if (!sess || !data || len == 0) return;

    for (size_t i = 0; i < len; i++) {
        sess->rx_buf[sess->rx_head] = data[i];
        sess->rx_head = (sess->rx_head + 1) % SSH_RX_BUFFER_SIZE;
        if (sess->rx_count < SSH_RX_BUFFER_SIZE) {
            sess->rx_count++;
        } else {
            /* Overwrite oldest byte */
            sess->rx_tail = (sess->rx_tail + 1) % SSH_RX_BUFFER_SIZE;
        }
    }
    sess->rx_bytes += len;
}

int ssh_port_init(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        s_sessions[i].sock_fd = -1;
    }

#ifdef ESP_PLATFORM
    memset(s_aux, 0, sizeof(s_aux));
    libssh2_init(0);
#endif

#ifndef ESP_PLATFORM
    /* Automatically launch Session 1: Real SSH to Workstation (root@10.2.132.54) directly over LAN */
    ssh_port_create_session("workstation", "10.2.132.54", 22, "root", SSH_AUTH_KEY,
                            "./sim_sdcard/.ssh/id_ed25519",
                            DEVOS_TERM_COLS_COLLAPSED, DEVOS_TERM_ROWS);
#endif

    return 0;
}

int ssh_port_create_session(const char *alias, const char *host, int port, const char *user,
                           ssh_auth_type_t auth_type, const char *credential,
                           uint16_t cols, uint16_t rows)
{
    (void)auth_type;
    if (!host || strlen(host) == 0) return -1;

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (s_sessions[i].state == SSH_SESSION_DISCONNECTED || s_sessions[i].state == SSH_SESSION_CLOSED) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        return -1; /* All sessions busy */
    }

    ssh_session_t *sess = &s_sessions[slot];
    memset(sess, 0, sizeof(ssh_session_t));
    sess->id = slot + 1;
    sess->sock_fd = -1;
    strncpy(sess->alias, alias && strlen(alias) > 0 ? alias : host, sizeof(sess->alias) - 1);
    strncpy(sess->host, host, sizeof(sess->host) - 1);
    sess->port = port > 0 ? port : 22;
    strncpy(sess->user, user && strlen(user) > 0 ? user : "root", sizeof(sess->user) - 1);
    strncpy(sess->command, "ssh", sizeof(sess->command) - 1);
    sess->cols = cols > 0 ? cols : DEVOS_TERM_COLS_COLLAPSED;
    sess->rows = rows > 0 ? rows : DEVOS_TERM_ROWS;
    sess->ping_ms = 1;

#ifndef ESP_PLATFORM
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = sess->cols;
    ws.ws_row = sess->rows;

    int master_fd = -1;
    pid_t pid = forkpty(&master_fd, NULL, NULL, &ws);
    if (pid < 0) {
        sess->state = SSH_SESSION_ERROR;
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "\r\n[forkpty error: %s]\r\n", strerror(errno));
        push_to_rx(sess, err_msg, strlen(err_msg));
        return -1;
    }

    if (pid == 0) {
        /* Child process: execute ssh client */
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", sess->port);

        char user_host[128];
        snprintf(user_host, sizeof(user_host), "%s@%s", sess->user, sess->host);

        /* Resolve SSH private key */
        char key_path[256] = {0};
        if (credential && strlen(credential) > 0) {
            if (strncmp(credential, "/sdcard/", 8) == 0) {
                snprintf(key_path, sizeof(key_path), "./sim_sdcard/%s", credential + 8);
            } else {
                strncpy(key_path, credential, sizeof(key_path) - 1);
            }
        }
        if (key_path[0] == '\0' || access(key_path, R_OK) != 0) {
            if (access("./sim_sdcard/.ssh/id_ed25519", R_OK) == 0) {
                strncpy(key_path, "./sim_sdcard/.ssh/id_ed25519", sizeof(key_path) - 1);
            } else if (access("/home/dom/.ssh/id_ed25519", R_OK) == 0) {
                strncpy(key_path, "/home/dom/.ssh/id_ed25519", sizeof(key_path) - 1);
            }
        }

        char *argv[32];
        int argc = 0;
        argv[argc++] = "ssh";
        argv[argc++] = "-tt";
        argv[argc++] = "-p";
        argv[argc++] = port_str;
        argv[argc++] = "-o";
        argv[argc++] = "StrictHostKeyChecking=no";
        argv[argc++] = "-o";
        argv[argc++] = "UserKnownHostsFile=/dev/null";
        argv[argc++] = "-o";
        argv[argc++] = "LogLevel=ERROR";
        argv[argc++] = "-o";
        argv[argc++] = "ConnectTimeout=8";

        if (key_path[0] && access(key_path, R_OK) == 0) {
            argv[argc++] = "-i";
            argv[argc++] = key_path;
        }

        argv[argc++] = user_host;
        argv[argc] = NULL;

        execvp("ssh", argv);
        fprintf(stderr, "Failed to exec ssh: %s\n", strerror(errno));
        _exit(127);
    }

    /* Parent process */
    int flags = fcntl(master_fd, F_GETFL, 0);
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    sess->sock_fd = master_fd;
    sess->session_ctx = (void *)(intptr_t)pid;
    sess->state = SSH_SESSION_CONNECTED;
#else
    /* Spin up a per-session libssh2 client task on the network/crypto core. */
    ssh_aux_t *a = &s_aux[slot];
    memset(a, 0, sizeof(*a));
    a->auth = auth_type;
    if (credential) strncpy(a->cred, credential, sizeof(a->cred) - 1);

    a->rx_store = heap_caps_malloc(SSH_RX_BUFFER_SIZE + 1, MALLOC_CAP_SPIRAM);
    a->tx_store = heap_caps_malloc(SSH_TX_BUFFER_SIZE + 1, MALLOC_CAP_SPIRAM);
    a->rx_ctrl  = malloc(sizeof(StaticStreamBuffer_t));
    a->tx_ctrl  = malloc(sizeof(StaticStreamBuffer_t));
    if (!a->rx_store || !a->tx_store || !a->rx_ctrl || !a->tx_ctrl) {
        ssh_aux_free(a);
        sess->state = SSH_SESSION_ERROR;
        return -1;
    }
    a->rx = xStreamBufferCreateStatic(SSH_RX_BUFFER_SIZE, 1, a->rx_store, (StaticStreamBuffer_t *)a->rx_ctrl);
    a->tx = xStreamBufferCreateStatic(SSH_TX_BUFFER_SIZE, 1, a->tx_store, (StaticStreamBuffer_t *)a->tx_ctrl);

    sess->state = SSH_SESSION_CONNECTING;
    char taskname[16];
    snprintf(taskname, sizeof(taskname), "ssh%d", sess->id);
    /* 12 KB stack: the mbedTLS key exchange during handshake is stack-hungry. */
    if (xTaskCreatePinnedToCore(ssh_session_task, taskname, 12288,
                                (void *)(intptr_t)slot, 6, &a->task,
                                DEVOS_CORE_NET_CRYPTO) != pdPASS) {
        ssh_aux_free(a);
        sess->state = SSH_SESSION_ERROR;
        return -1;
    }
#endif

    return sess->id;
}

int ssh_port_close_session(int session_id)
{
    if (session_id < 1 || session_id > SSH_MAX_SESSIONS) return -1;
    int idx = session_id - 1;
    ssh_session_t *sess = &s_sessions[idx];

#ifndef ESP_PLATFORM
    if (sess->sock_fd >= 0) {
        close(sess->sock_fd);
        sess->sock_fd = -1;
    }
    if (sess->session_ctx) {
        pid_t pid = (pid_t)(intptr_t)sess->session_ctx;
        kill(pid, SIGTERM);
        waitpid(pid, NULL, WNOHANG);
        sess->session_ctx = NULL;
    }
    sess->state = SSH_SESSION_CLOSED;
#else
    /* Signal the session task to tear down; it frees resources and marks CLOSED. */
    if (s_aux[idx].task) {
        s_aux[idx].close_req = true;
    } else {
        sess->state = SSH_SESSION_CLOSED;
    }
#endif
    return 0;
}

ssh_session_t *ssh_port_get_session(int session_id)
{
    if (session_id < 1 || session_id > SSH_MAX_SESSIONS) return NULL;
    return &s_sessions[session_id - 1];
}

int ssh_port_get_active_sessions(int *out_ids, int max_ids)
{
    if (!out_ids || max_ids <= 0) return 0;
    int count = 0;
    for (int i = 0; i < SSH_MAX_SESSIONS && count < max_ids; i++) {
        if (s_sessions[i].state == SSH_SESSION_CONNECTED) {
            out_ids[count++] = s_sessions[i].id;
        }
    }
    return count;
}

int ssh_port_resize_pty(int session_id, uint16_t cols, uint16_t rows)
{
    if (session_id < 1 || session_id > SSH_MAX_SESSIONS) return -1;
    int idx = session_id - 1;
    s_sessions[idx].cols = cols;
    s_sessions[idx].rows = rows;

#ifndef ESP_PLATFORM
    if (s_sessions[idx].sock_fd >= 0) {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_col = cols;
        ws.ws_row = rows;
        ioctl(s_sessions[idx].sock_fd, TIOCSWINSZ, &ws);
    }
#endif

#ifdef ESP_PLATFORM
    /* Applied by the session task via libssh2_channel_request_pty_size(). */
    s_aux[idx].rcols = cols;
    s_aux[idx].rrows = rows;
    s_aux[idx].resize_req = true;
    ESP_LOGI(TAG, "Session %d PTY resize queued: cols=%d, rows=%d", session_id, cols, rows);
#endif
    return 0;
}

int ssh_port_send(int session_id, const char *data, size_t len)
{
    if (session_id < 1 || session_id > SSH_MAX_SESSIONS || !data || len == 0) return -1;
    int idx = session_id - 1;
    ssh_session_t *sess = &s_sessions[idx];

    if (sess->state != SSH_SESSION_CONNECTED) return -1;

#ifndef ESP_PLATFORM
    if (sess->sock_fd >= 0) {
        ssize_t w = write(sess->sock_fd, data, len);
        if (w > 0) {
            sess->tx_bytes += (uint32_t)w;
            return (int)w;
        }
        return -1;
    }
    return (int)len;
#else
    if (s_aux[idx].tx) {
        return (int)xStreamBufferSend(s_aux[idx].tx, data, len, 0);
    }
    return -1;
#endif
}

int ssh_port_recv(int session_id, char *buf, size_t max_len)
{
    if (session_id < 1 || session_id > SSH_MAX_SESSIONS || !buf || max_len == 0) return 0;
    int idx = session_id - 1;
    ssh_session_t *sess = &s_sessions[idx];

#ifdef ESP_PLATFORM
    (void)sess;
    ssh_aux_t *a = &s_aux[idx];
    if (!a->rx) return 0;
    return (int)xStreamBufferReceive(a->rx, buf, max_len, 0);
#else
    if (sess->sock_fd >= 0) {
        char temp[2048];
        ssize_t n = read(sess->sock_fd, temp, sizeof(temp));
        if (n > 0) {
            push_to_rx(sess, temp, (size_t)n);
        } else if (n == 0 || (n < 0 && (errno == EIO || errno == EBADF))) {
            /* Child shell closed */
            push_to_rx(sess, "\r\n[Session disconnected]\r\n", 25);
            close(sess->sock_fd);
            sess->sock_fd = -1;
            sess->state = SSH_SESSION_CLOSED;
        }
    }

    size_t count = 0;
    while (sess->rx_count > 0 && count < max_len) {
        buf[count++] = sess->rx_buf[sess->rx_tail];
        sess->rx_tail = (sess->rx_tail + 1) % SSH_RX_BUFFER_SIZE;
        sess->rx_count--;
    }

    return (int)count;
#endif
}

int ssh_port_load_bookmarks(ssh_bookmark_t *out_bookmarks, int max_count, int *out_count)
{
    if (!out_bookmarks || max_count <= 0 || !out_count) return -1;

    *out_count = 0;
    FILE *f = fopen(SSH_BOOKMARKS_FILE, "r");
    if (!f) return -1;

    char line[256];
    ssh_bookmark_t current;
    memset(&current, 0, sizeof(current));
    current.port = 22;
    int count = 0;

    while (fgets(line, sizeof(line), f) && count < max_count) {
        char val[128];
        int num = 0;

        if (sscanf(line, " \"alias\": \"%127[^\"]\"", val) == 1) {
            strncpy(current.alias, val, sizeof(current.alias) - 1);
        } else if (sscanf(line, " \"host\": \"%127[^\"]\"", val) == 1) {
            strncpy(current.host, val, sizeof(current.host) - 1);
        } else if (sscanf(line, " \"port\": %d", &num) == 1) {
            current.port = num;
        } else if (sscanf(line, " \"user\": \"%127[^\"]\"", val) == 1) {
            strncpy(current.user, val, sizeof(current.user) - 1);
        } else if (sscanf(line, " \"auth\": \"%127[^\"]\"", val) == 1) {
            if (strcasecmp(val, "key") == 0) current.auth_type = SSH_AUTH_KEY;
            else current.auth_type = SSH_AUTH_PASSWORD;
        } else if (sscanf(line, " \"key_path\": \"%127[^\"]\"", val) == 1) {
            strncpy(current.key_path, val, sizeof(current.key_path) - 1);
        } else if (strstr(line, "}") != NULL) {
            if (current.host[0] != '\0') {
                memcpy(&out_bookmarks[count++], &current, sizeof(ssh_bookmark_t));
                memset(&current, 0, sizeof(current));
                current.port = 22;
            }
        }
    }

    fclose(f);
    *out_count = count;
    return 0;
}

int ssh_port_save_bookmark(const ssh_bookmark_t *bm)
{
    if (!bm || strlen(bm->host) == 0) return -1;

    ssh_bookmark_t existing[SSH_MAX_BOOKMARKS];
    int count = 0;
    ssh_port_load_bookmarks(existing, SSH_MAX_BOOKMARKS - 1, &count);

    /* Update existing if matching alias or (host + user + port), else append */
    int target_idx = -1;
    for (int i = 0; i < count; i++) {
        if ((strlen(bm->alias) > 0 && strcmp(existing[i].alias, bm->alias) == 0) ||
            (strcmp(existing[i].host, bm->host) == 0 && strcmp(existing[i].user, bm->user) == 0 && existing[i].port == bm->port)) {
            target_idx = i;
            break;
        }
    }

    if (target_idx >= 0) {
        memcpy(&existing[target_idx], bm, sizeof(ssh_bookmark_t));
    } else if (count < SSH_MAX_BOOKMARKS) {
        memcpy(&existing[count++], bm, sizeof(ssh_bookmark_t));
    } else {
        return -1;
    }

    FILE *f = fopen(SSH_BOOKMARKS_FILE, "w");
    if (!f) return -1;

    fprintf(f, "[\n");
    for (int i = 0; i < count; i++) {
        fprintf(f, "  {\n");
        fprintf(f, "    \"alias\": \"%s\",\n", existing[i].alias);
        fprintf(f, "    \"host\": \"%s\",\n", existing[i].host);
        fprintf(f, "    \"port\": %d,\n", existing[i].port);
        fprintf(f, "    \"user\": \"%s\",\n", existing[i].user);
        fprintf(f, "    \"auth\": \"%s\"", existing[i].auth_type == SSH_AUTH_KEY ? "key" : "password");
        if (existing[i].key_path[0]) {
            fprintf(f, ",\n    \"key_path\": \"%s\"\n", existing[i].key_path);
        } else {
            fprintf(f, "\n");
        }
        fprintf(f, "  }%s\n", i < count - 1 ? "," : "");
    }
    fprintf(f, "]\n");
    fclose(f);

    return 0;
}

int ssh_port_delete_bookmark(int index)
{
    ssh_bookmark_t existing[SSH_MAX_BOOKMARKS];
    int count = 0;
    ssh_port_load_bookmarks(existing, SSH_MAX_BOOKMARKS, &count);

    if (index < 0 || index >= count) return -1;

    FILE *f = fopen(SSH_BOOKMARKS_FILE, "w");
    if (!f) return -1;

    fprintf(f, "[\n");
    int written = 0;
    for (int i = 0; i < count; i++) {
        if (i == index) continue;
        fprintf(f, "  {\n");
        fprintf(f, "    \"alias\": \"%s\",\n", existing[i].alias);
        fprintf(f, "    \"host\": \"%s\",\n", existing[i].host);
        fprintf(f, "    \"port\": %d,\n", existing[i].port);
        fprintf(f, "    \"user\": \"%s\",\n", existing[i].user);
        fprintf(f, "    \"auth\": \"%s\"", existing[i].auth_type == SSH_AUTH_KEY ? "key" : "password");
        if (existing[i].key_path[0]) {
            fprintf(f, ",\n    \"key_path\": \"%s\"\n", existing[i].key_path);
        } else {
            fprintf(f, "\n");
        }
        fprintf(f, "  }%s\n", written < count - 2 ? "," : "");
        written++;
    }
    fprintf(f, "]\n");
    fclose(f);

    return 0;
}

int ssh_port_scan_keys(char out_keys[][SSH_MAX_PATH_LEN], int max_keys, int *out_count)
{
    if (!out_keys || max_keys <= 0 || !out_count) return -1;
    *out_count = 0;

    DIR *dir = opendir(SSH_KEYS_DIR);
    if (!dir) return -1;

    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(dir)) != NULL && count < max_keys) {
        if (strncmp(ent->d_name, "id_", 3) == 0 && strstr(ent->d_name, ".pub") == NULL) {
            snprintf(out_keys[count], SSH_MAX_PATH_LEN, "%.32s/%.64s", SSH_KEYS_DIR, ent->d_name);
            count++;
        }
    }
    closedir(dir);
    *out_count = count;
    return 0;
}
