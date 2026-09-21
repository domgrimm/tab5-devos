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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
static const char *TAG = "libssh2_port";
#else
#define TAG "libssh2_port"
#endif

#define SSH_BOOKMARKS_FILE TAB5_SD_MOUNT_POINT "/.ssh/bookmarks.json"
#define SSH_KEYS_DIR       TAB5_SD_MOUNT_POINT "/.ssh"

static ssh_session_t s_sessions[SSH_MAX_SESSIONS];
static char s_input_line_buf[SSH_MAX_SESSIONS][256];
static size_t s_input_line_len[SSH_MAX_SESSIONS];

/* Push data to session ring buffer */
static void push_to_rx(ssh_session_t *sess, const char *data, size_t len)
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

static void send_prompt(ssh_session_t *sess)
{
    char prompt[128];
    if (sess->user[0] && sess->alias[0]) {
        snprintf(prompt, sizeof(prompt), "\033[1;32m%s@%s\033[0m:\033[1;34m~\033[0m%s ",
                 sess->user, sess->alias, strcmp(sess->user, "root") == 0 ? "#" : "$");
    } else {
        snprintf(prompt, sizeof(prompt), "\033[1;32mdevos@tab5\033[0m:\033[1;34m~\033[0m$ ");
    }
    push_to_rx(sess, prompt, strlen(prompt));
}

static void simulate_command_response(ssh_session_t *sess, const char *cmd)
{
    /* Trim whitespace */
    while (*cmd == ' ') cmd++;

    if (strlen(cmd) == 0) {
        push_to_rx(sess, "\r\n", 2);
        send_prompt(sess);
        return;
    }

    push_to_rx(sess, "\r\n", 2);

    if (strcmp(cmd, "clear") == 0) {
        push_to_rx(sess, "\033[2J\033[H", 7);
        send_prompt(sess);
        return;
    }

    if (strcmp(cmd, "help") == 0) {
        const char *help_msg =
            "\033[1;37mdevOS Built-in Shell & Diagnostics Commands:\033[0m\r\n"
            "  ls, ll, dir      List files in directory\r\n"
            "  git status       Check active repository status\r\n"
            "  agy, agy status  Inspect Antigravity daemon status\r\n"
            "  uname -a         Display system architecture and kernel\r\n"
            "  whoami           Show current logged in user\r\n"
            "  date             Display network synchronized clock\r\n"
            "  htop, top        Display live process summary\r\n"
            "  clear            Clear terminal screen\r\n"
            "  exit             Close active session\r\n";
        push_to_rx(sess, help_msg, strlen(help_msg));
        send_prompt(sess);
        return;
    }

    if (strcmp(cmd, "git status") == 0) {
        const char *git_msg =
            "On branch \033[1;32mmain\033[0m\r\n"
            "Your branch is up to date with 'origin/main'.\r\n\r\n"
            "Changes to be committed:\r\n"
            "  (use \"git restore --staged <file>...\" to unstage)\r\n"
            "    \033[32mnew file:   components/libssh2_port/libssh2_port.c\033[0m\r\n"
            "    \033[32mnew file:   components/libssh2_port/libssh2_port.h\033[0m\r\n"
            "    \033[32mmodified:   main/apps/app_terminal/app_terminal.c\033[0m\r\n\r\n";
        push_to_rx(sess, git_msg, strlen(git_msg));
        send_prompt(sess);
        return;
    }

    if (strncmp(cmd, "agy", 3) == 0) {
        const char *agy_msg =
            "\033[1;35mAntigravity CLI v2.4.0 (Autonomous Agent Engine)\033[0m\r\n"
            "Target: M5Stack Tab5 (ESP32-P4 + A164 Keyboard)\r\n"
            "Connected to agy-bridge sidecar on \033[1;33m100.77.11.92:8420\033[0m [OK]\r\n"
            "Subagents Active: \033[1;36m0 idle, 1 waiting\033[0m\r\n";
        push_to_rx(sess, agy_msg, strlen(agy_msg));
        send_prompt(sess);
        return;
    }

    if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "ls -la") == 0 || strcmp(cmd, "ll") == 0) {
        const char *ls_msg =
            "\033[1;34m.\033[0m   \033[1;34m..\033[0m   \033[1;34mcomponents\033[0m   \033[1;34mmain\033[0m   \033[1;34mtools\033[0m   \033[1;32mCMakeLists.txt\033[0m   \033[36mPLAN.md\033[0m   \033[36mAGENTS.md\033[0m\r\n";
        push_to_rx(sess, ls_msg, strlen(ls_msg));
        send_prompt(sess);
        return;
    }

    if (strcmp(cmd, "uname -a") == 0) {
        const char *uname_msg =
            "Linux workstation 6.12.1-arch1-1 #1 SMP PREEMPT_DYNAMIC x86_64 GNU/Linux\r\n";
        push_to_rx(sess, uname_msg, strlen(uname_msg));
        send_prompt(sess);
        return;
    }

    if (strcmp(cmd, "whoami") == 0) {
        char who_msg[128];
        snprintf(who_msg, sizeof(who_msg), "%s\r\n", sess->user[0] ? sess->user : "dom");
        push_to_rx(sess, who_msg, strlen(who_msg));
        send_prompt(sess);
        return;
    }

    if (strcmp(cmd, "date") == 0) {
        const char *date_msg = "Mon Sep 21 12:00:00 AEST 2026\r\n";
        push_to_rx(sess, date_msg, strlen(date_msg));
        send_prompt(sess);
        return;
    }

    if (strcmp(cmd, "htop") == 0 || strcmp(cmd, "top") == 0) {
        const char *top_msg =
            "\033[1;36mTasks: 142 total, 1 running, 141 sleeping\033[0m\r\n"
            "CPU: [||||||                      14.2%]   Core 0: 12% | Core 1: 16%\r\n"
            "Mem: [||||||||||||||||            8.2G/64G] PSRAM: 28.5M/32M\r\n\r\n"
            "  PID USER      PR  NI    VIRT    RES    SHR S  %CPU  %MEM     TIME+ COMMAND\r\n"
            " 1024 dom       20   0 1420580 412500 120400 S  12.4   0.6   4:12.10 agy-bridge\r\n"
            " 1089 dom       20   0  945200 230100  85200 S   6.2   0.3   1:45.02 python3\r\n"
            " 2045 root      20   0  512000 110400  42000 S   1.8   0.1   0:18.44 tailscaled\r\n"
            " 3201 dom       20   0  128400  32100  12000 R   0.5   0.0   0:00.08 htop\r\n";
        push_to_rx(sess, top_msg, strlen(top_msg));
        send_prompt(sess);
        return;
    }

    if (strcmp(cmd, "exit") == 0) {
        push_to_rx(sess, "logout\r\nConnection closed by remote host.\r\n", 40);
        sess->state = SSH_SESSION_CLOSED;
        return;
    }

    /* Fallback generic echo */
    char generic[256];
    snprintf(generic, sizeof(generic), "bash: %s: command not found (type 'help' for devOS diagnostic shell)\r\n", cmd);
    push_to_rx(sess, generic, strlen(generic));
    send_prompt(sess);
}

int ssh_port_init(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
    memset(s_input_line_buf, 0, sizeof(s_input_line_buf));
    memset(s_input_line_len, 0, sizeof(s_input_line_len));

    /* Pre-populate Session 1: Workstation (bash) */
    s_sessions[0].id = 1;
    s_sessions[0].state = SSH_SESSION_CONNECTED;
    strncpy(s_sessions[0].alias, "workstation", sizeof(s_sessions[0].alias));
    strncpy(s_sessions[0].host, "100.77.11.92", sizeof(s_sessions[0].host));
    s_sessions[0].port = 22;
    strncpy(s_sessions[0].user, "dom", sizeof(s_sessions[0].user));
    strncpy(s_sessions[0].command, "bash", sizeof(s_sessions[0].command));
    s_sessions[0].cols = DEVOS_TERM_COLS_COLLAPSED;
    s_sessions[0].rows = DEVOS_TERM_ROWS;
    s_sessions[0].ping_ms = 2;

    const char *sess1_banner =
        "\033[1;36mLinux workstation 6.12.1-arch1-1 #1 SMP PREEMPT_DYNAMIC x86_64\033[0m\r\n"
        "Welcome to Arch Linux (Tailscale IP: \033[1;32m100.77.11.92\033[0m)!\r\n"
        "System load: 0.14, 0.22, 0.18 | Memory: 8.2 GiB / 64.0 GiB | Uptime: 14d 6h\r\n\r\n"
        "\033[1;32mdom@workstation\033[0m:\033[1;34m~/dev/tab5-devos\033[0m$ git status\r\n"
        "On branch main\r\n"
        "Your branch is up to date with 'origin/main'.\r\n\r\n"
        "\033[1;32mdom@workstation\033[0m:\033[1;34m~/dev/tab5-devos\033[0m$ agy --version\r\n"
        "\033[1;35mAntigravity CLI v2.4.0 (Autonomous Agent Engine)\033[0m\r\n"
        "Connected to agy-bridge daemon on \033[1;33m100.77.11.92:8420\033[0m [OK]\r\n\r\n"
        "\033[1;32mdom@workstation\033[0m:\033[1;34m~/dev/tab5-devos\033[0m$ ";
    push_to_rx(&s_sessions[0], sess1_banner, strlen(sess1_banner));

    /* Pre-populate Session 2: Prod Cluster (htop) */
    s_sessions[1].id = 2;
    s_sessions[1].state = SSH_SESSION_CONNECTED;
    strncpy(s_sessions[1].alias, "prod-cluster", sizeof(s_sessions[1].alias));
    strncpy(s_sessions[1].host, "100.99.20.1", sizeof(s_sessions[1].host));
    s_sessions[1].port = 22;
    strncpy(s_sessions[1].user, "root", sizeof(s_sessions[1].user));
    strncpy(s_sessions[1].command, "htop", sizeof(s_sessions[1].command));
    s_sessions[1].cols = DEVOS_TERM_COLS_COLLAPSED;
    s_sessions[1].rows = DEVOS_TERM_ROWS;
    s_sessions[1].ping_ms = 12;

    const char *sess2_banner =
        "\033[1;36mUbuntu 24.04.1 LTS (GNU/Linux 6.8.0-45-generic x86_64)\033[0m\r\n"
        "Welcome to Production Cluster Node 1 (IP: \033[1;32m100.99.20.1\033[0m)\r\n"
        "System load: 0.85, 0.92, 0.78 | Memory: 31.4 GiB / 64.0 GiB | Uptime: 45d 12h\r\n\r\n"
        "\033[1;31mroot@prod-cluster\033[0m:\033[1;34m~#\033[0m ";
    push_to_rx(&s_sessions[1], sess2_banner, strlen(sess2_banner));

    return 0;
}

int ssh_port_create_session(const char *alias, const char *host, int port, const char *user,
                           ssh_auth_type_t auth_type, const char *credential,
                           uint16_t cols, uint16_t rows)
{
    (void)auth_type;
    (void)credential;
    if (!host) return -1;

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
    sess->state = SSH_SESSION_CONNECTED;
    strncpy(sess->alias, alias && strlen(alias) > 0 ? alias : host, sizeof(sess->alias) - 1);
    strncpy(sess->host, host, sizeof(sess->host) - 1);
    sess->port = port > 0 ? port : 22;
    strncpy(sess->user, user && strlen(user) > 0 ? user : "root", sizeof(sess->user) - 1);
    strncpy(sess->command, "bash", sizeof(sess->command) - 1);
    sess->cols = cols > 0 ? cols : DEVOS_TERM_COLS_COLLAPSED;
    sess->rows = rows > 0 ? rows : DEVOS_TERM_ROWS;
    sess->ping_ms = 8;

    s_input_line_len[slot] = 0;

    char banner[256];
    snprintf(banner, sizeof(banner),
             "\033[1;36mConnected to %s (%s:%d) via devOS Virtual Net\033[0m\r\n"
             "PTY initialized: %dx%d cols, VT100 ANSI mode active.\r\n\r\n",
             sess->alias, sess->host, sess->port, sess->cols, sess->rows);
    push_to_rx(sess, banner, strlen(banner));
    send_prompt(sess);

    return sess->id;
}

int ssh_port_close_session(int session_id)
{
    if (session_id < 1 || session_id > SSH_MAX_SESSIONS) return -1;
    int idx = session_id - 1;
    s_sessions[idx].state = SSH_SESSION_CLOSED;
    s_input_line_len[idx] = 0;
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

#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Session %d TIOCSWINSZ: cols=%d, rows=%d", session_id, cols, rows);
#endif
    return 0;
}

int ssh_port_send(int session_id, const char *data, size_t len)
{
    if (session_id < 1 || session_id > SSH_MAX_SESSIONS || !data || len == 0) return -1;
    int idx = session_id - 1;
    ssh_session_t *sess = &s_sessions[idx];

    if (sess->state != SSH_SESSION_CONNECTED) return -1;

    sess->tx_bytes += len;

    for (size_t i = 0; i < len; i++) {
        char ch = data[i];

        if (ch == '\r' || ch == '\n') {
            s_input_line_buf[idx][s_input_line_len[idx]] = '\0';
            simulate_command_response(sess, s_input_line_buf[idx]);
            s_input_line_len[idx] = 0;
        } else if (ch == '\b' || ch == 0x7F) {
            if (s_input_line_len[idx] > 0) {
                s_input_line_len[idx]--;
                push_to_rx(sess, "\b \b", 3);
            }
        } else if (ch == 0x03) { /* Ctrl+C */
            push_to_rx(sess, "^C\r\n", 4);
            s_input_line_len[idx] = 0;
            send_prompt(sess);
        } else if (ch == 0x0C) { /* Ctrl+L */
            push_to_rx(sess, "\033[2J\033[H", 7);
            s_input_line_len[idx] = 0;
            send_prompt(sess);
        } else if (ch >= 0x20 && ch <= 0x7E) {
            if (s_input_line_len[idx] < sizeof(s_input_line_buf[idx]) - 1) {
                s_input_line_buf[idx][s_input_line_len[idx]++] = ch;
                push_to_rx(sess, &ch, 1); /* Echo */
            }
        }
    }

    return (int)len;
}

int ssh_port_recv(int session_id, char *buf, size_t max_len)
{
    if (session_id < 1 || session_id > SSH_MAX_SESSIONS || !buf || max_len == 0) return 0;
    int idx = session_id - 1;
    ssh_session_t *sess = &s_sessions[idx];

    size_t count = 0;
    while (sess->rx_count > 0 && count < max_len) {
        buf[count++] = sess->rx_buf[sess->rx_tail];
        sess->rx_tail = (sess->rx_tail + 1) % SSH_RX_BUFFER_SIZE;
        sess->rx_count--;
    }

    return (int)count;
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

    /* Append */
    if (count < SSH_MAX_BOOKMARKS) {
        memcpy(&existing[count++], bm, sizeof(ssh_bookmark_t));
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
        fprintf(f, "    \"auth\": \"%s\"\n", existing[i].auth_type == SSH_AUTH_KEY ? "key" : "password");
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
            snprintf(out_keys[count], SSH_MAX_PATH_LEN, "%s/%s", SSH_KEYS_DIR, ent->d_name);
            count++;
        }
    }
    closedir(dir);
    *out_count = count;
    return 0;
}
