#include "libssh2_port.h"
#include <stdarg.h>
#include "devos_config.h"
#include "devos_core.h"
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
#include "freertos/stream_buffer.h"
#include "nvs.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/base64.h"
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

#define SSH_BOOKMARKS_FILE   TAB5_SD_MOUNT_POINT "/.ssh/bookmarks.json"
#define SSH_KEYS_DIR         TAB5_SD_MOUNT_POINT "/.ssh"
#define SSH_KNOWN_HOSTS_FILE TAB5_SD_MOUNT_POINT "/.ssh/known_hosts"

static EXT_RAM_BSS_ATTR ssh_session_t s_sessions[SSH_MAX_SESSIONS];

/* Format a failure reason into s->last_error. Goes through a local buffer
 * because the arguments are often other fields of the same session. */
static void set_error(ssh_session_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_error(ssh_session_t *s, const char *fmt, ...)
{
    char buf[sizeof(s->last_error)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    memcpy(s->last_error, buf, sizeof(buf));
}

static const char *auth_name(ssh_auth_type_t t)
{
    switch (t) {
    case SSH_AUTH_KEY:        return "key";
    case SSH_AUTH_DEVICE_KEY: return "device";
    default:                  return "password";
    }
}

#ifdef ESP_PLATFORM
/* =========================================================================
 * On-device SSH client (libssh2, mbedTLS backend) over the lwIP socket layer
 * (devos_net_socket_*). Each session runs its own task on the network/crypto
 * core; the UI exchanges bytes through per-slot stream buffers that live for
 * the whole firmware run (never freed while the UI might be reading them).
 * ========================================================================= */
#define SSH_TX_BUFFER_SIZE  8192
#define SSH_NVS_NS          "ssh"
#define SSH_HOSTKEY_WAIT_MS 120000

typedef struct {
    TaskHandle_t         task;
    StreamBufferHandle_t rx;                /* remote -> UI */
    StreamBufferHandle_t tx;                /* UI -> remote */
    ssh_auth_type_t      auth;
    char                 password[SSH_MAX_PASSWORD_LEN];
    char                 keypath[SSH_MAX_PATH_LEN];
    volatile bool        close_req;
    volatile bool        resize_req;
    volatile int         hk_answer;         /* 0 pending, 1 trust, -1 reject */
    volatile uint16_t    rcols, rrows;
} ssh_aux_t;

static ssh_aux_t s_aux[SSH_MAX_SESSIONS];

static bool ensure_buffers(ssh_aux_t *a)
{
    if (!a->rx) {
        uint8_t *store = heap_caps_malloc(SSH_RX_BUFFER_SIZE + 1, MALLOC_CAP_SPIRAM);
        StaticStreamBuffer_t *ctrl = malloc(sizeof(StaticStreamBuffer_t));
        if (!store || !ctrl) { free(store); free(ctrl); return false; }
        a->rx = xStreamBufferCreateStatic(SSH_RX_BUFFER_SIZE, 1, store, ctrl);
    }
    if (!a->tx) {
        uint8_t *store = heap_caps_malloc(SSH_TX_BUFFER_SIZE + 1, MALLOC_CAP_SPIRAM);
        StaticStreamBuffer_t *ctrl = malloc(sizeof(StaticStreamBuffer_t));
        if (!store || !ctrl) { free(store); free(ctrl); return false; }
        a->tx = xStreamBufferCreateStatic(SSH_TX_BUFFER_SIZE, 1, store, ctrl);
    }
    xStreamBufferReset(a->rx);
    xStreamBufferReset(a->tx);
    return true;
}

static void set_err(ssh_session_t *s, LIBSSH2_SESSION *session, const char *stage)
{
    char *msg = NULL;
    int len = 0;
    if (session) libssh2_session_last_error(session, &msg, &len, 0);
    if (msg && msg[0]) {
        set_error(s, "%s: %s", stage, msg);
    } else {
        set_error(s, "%s", stage);
    }
    ESP_LOGE(TAG, "ssh %d: %s", s->id, s->last_error);
}

/* Push a status line into the session's output so it shows in the terminal. */
static void notice(ssh_aux_t *a, const char *fmt, const char *arg)
{
    char line[200];
    snprintf(line, sizeof(line), fmt, arg ? arg : "");
    xStreamBufferSend(a->rx, line, strlen(line), 0);
}

static LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC(kbd_cb)
{
    (void)name; (void)name_len; (void)instruction; (void)instruction_len; (void)prompts;
    ssh_aux_t *a = (ssh_aux_t *)(*abstract);
    for (int i = 0; i < num_prompts; i++) {
        responses[i].text = strdup(a->password);
        responses[i].length = responses[i].text ? (unsigned int)strlen(a->password) : 0;
    }
}

/* Load the device key pair from NVS into heap buffers (caller frees). */
static bool devkey_load(char **priv, size_t *priv_len, char **pub, size_t *pub_len)
{
    nvs_handle_t h;
    *priv = *pub = NULL;
    if (nvs_open(SSH_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t pl = 0, ul = 0;
    bool ok = nvs_get_str(h, "devkey", NULL, &pl) == ESP_OK && nvs_get_str(h, "devpub", NULL, &ul) == ESP_OK;
    if (ok) {
        *priv = malloc(pl);
        *pub = malloc(ul);
        ok = *priv && *pub && nvs_get_str(h, "devkey", *priv, &pl) == ESP_OK &&
             nvs_get_str(h, "devpub", *pub, &ul) == ESP_OK;
        *priv_len = pl ? pl - 1 : 0;
        *pub_len = ul ? ul - 1 : 0;
    }
    nvs_close(h);
    if (!ok) { free(*priv); free(*pub); *priv = *pub = NULL; }
    return ok;
}

static bool authenticate(ssh_session_t *s, ssh_aux_t *a, LIBSSH2_SESSION *session)
{
    size_t ulen = strlen(s->user);
    const char *methods = libssh2_userauth_list(session, s->user, ulen);
    if (!methods) {
        if (libssh2_userauth_authenticated(session)) return true;
        set_err(s, session, "Could not get authentication methods");
        return false;
    }
    char why[96] = "";

    if (strstr(methods, "publickey")) {
        if (a->auth == SSH_AUTH_DEVICE_KEY) {
            char *priv, *pub;
            size_t priv_len, pub_len;
            if (devkey_load(&priv, &priv_len, &pub, &pub_len)) {
                int rc = libssh2_userauth_publickey_frommemory(session, s->user, ulen, pub, pub_len,
                                                                priv, priv_len, NULL);
                free(priv);
                free(pub);
                if (rc == 0) return true;
                snprintf(why, sizeof(why), "device key not accepted (is it in ~/.ssh/authorized_keys?)");
            } else {
                snprintf(why, sizeof(why), "no device key yet (create one in the sidebar)");
            }
        } else if (a->auth == SSH_AUTH_KEY && a->keypath[0]) {
            char pub[SSH_MAX_PATH_LEN + 8];
            snprintf(pub, sizeof(pub), "%s.pub", a->keypath);
            struct stat st;
            int rc = libssh2_userauth_publickey_fromfile(session, s->user,
                                                         stat(pub, &st) == 0 ? pub : NULL,
                                                         a->keypath, NULL);
            if (rc == 0) return true;
            char *msg = NULL;
            int ml = 0;
            libssh2_session_last_error(session, &msg, &ml, 0);
            snprintf(why, sizeof(why), "key %s rejected (%.40s)", a->keypath, msg ? msg : "?");
        }
    }

    if (a->password[0]) {
        if (strstr(methods, "password") && libssh2_userauth_password(session, s->user, a->password) == 0) {
            return true;
        }
        if (strstr(methods, "keyboard-interactive") &&
            libssh2_userauth_keyboard_interactive(session, s->user, kbd_cb) == 0) {
            return true;
        }
        snprintf(why, sizeof(why), "wrong password?");
    } else if (!why[0]) {
        snprintf(why, sizeof(why), "no password given");
    }

    set_error(s, "Authentication failed: %s (server accepts: %.40s)",
             why, methods);
    ESP_LOGE(TAG, "ssh %d: %s", s->id, s->last_error);
    return false;
}

static int knownhost_keybit(int type)
{
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:       return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_DSS:       return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
    case LIBSSH2_HOSTKEY_TYPE_ED25519:   return LIBSSH2_KNOWNHOST_KEY_ED25519;
    default:                             return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}

static const char *hostkey_type_name(int type)
{
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:       return "RSA";
    case LIBSSH2_HOSTKEY_TYPE_DSS:       return "DSA";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return "ECDSA-256";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return "ECDSA-384";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return "ECDSA-521";
    case LIBSSH2_HOSTKEY_TYPE_ED25519:   return "ED25519";
    default:                             return "unknown";
    }
}

/* Trust-on-first-use against /sdcard/.ssh/known_hosts (OpenSSH format). */
static bool verify_host_key(ssh_session_t *s, ssh_aux_t *a, LIBSSH2_SESSION *session)
{
    size_t klen = 0;
    int ktype = 0;
    const char *key = libssh2_session_hostkey(session, &klen, &ktype);
    if (!key) {
        set_err(s, session, "Server sent no host key");
        return false;
    }
    const unsigned char *hash = (const unsigned char *)libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256);
    char b64[64] = "";
    size_t olen = 0;
    if (hash && mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &olen, hash, 32) == 0) {
        while (olen > 0 && b64[olen - 1] == '=') b64[--olen] = '\0';
    }
    snprintf(s->hostkey_fp, sizeof(s->hostkey_fp), "SHA256:%s", b64);
    snprintf(s->hostkey_type, sizeof(s->hostkey_type), "%s", hostkey_type_name(ktype));

    int typemask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownhost_keybit(ktype);
    LIBSSH2_KNOWNHOSTS *kh = libssh2_knownhost_init(session);
    int check = LIBSSH2_KNOWNHOST_CHECK_NOTFOUND;
    if (kh) {
        libssh2_knownhost_readfile(kh, SSH_KNOWN_HOSTS_FILE, LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        struct libssh2_knownhost *found = NULL;
        check = libssh2_knownhost_checkp(kh, s->host, s->port, key, klen, typemask, &found);
    }
    if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH) {
        libssh2_knownhost_free(kh);
        return true;
    }
    if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
        s->hostkey_changed = true;
        set_error(s, "Host key CHANGED for %.40s (%s %.50s): possible attack. If the server was "
                     "reinstalled, delete its line from /sdcard/.ssh/known_hosts.",
                  s->host, s->hostkey_type, s->hostkey_fp);
        ESP_LOGE(TAG, "ssh %d: %s", s->id, s->last_error);
        libssh2_knownhost_free(kh);
        return false;
    }

    /* Unknown host: ask the user (the UI shows type + fingerprint). */
    a->hk_answer = 0;
    s->state = SSH_SESSION_HOSTKEY_PROMPT;
    for (int waited = 0; a->hk_answer == 0 && !a->close_req && waited < SSH_HOSTKEY_WAIT_MS; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (a->hk_answer != 1) {
        set_error(s, "Host key for %.60s was not trusted", s->host);
        libssh2_knownhost_free(kh);
        return false;
    }
    s->state = SSH_SESSION_AUTHENTICATING;
    if (kh) {
        char name[SSH_MAX_HOST_LEN + 10];
        if (s->port == 22) snprintf(name, sizeof(name), "%s", s->host);
        else snprintf(name, sizeof(name), "[%s]:%d", s->host, s->port);
        if (libssh2_knownhost_addc(kh, name, NULL, key, klen, "devOS", 5, typemask, NULL) == 0 &&
            libssh2_knownhost_writefile(kh, SSH_KNOWN_HOSTS_FILE, LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0) {
            ESP_LOGI(TAG, "ssh %d: %s added to known_hosts", s->id, name);
        } else {
            ESP_LOGW(TAG, "ssh %d: could not save known_hosts (SD card missing?)", s->id);
        }
        libssh2_knownhost_free(kh);
    }
    return true;
}

static void ssh_session_task(void *arg)
{
    int slot = (int)(intptr_t)arg;
    ssh_session_t *s = &s_sessions[slot];
    ssh_aux_t     *a = &s_aux[slot];
    LIBSSH2_SESSION *session = NULL;
    LIBSSH2_CHANNEL *chan = NULL;
    bool was_connected = false;
    char buf[1024];

    s->state = SSH_SESSION_CONNECTING;
    int fd = devos_net_socket_connect(s->host, s->port, 10000);
    if (fd < 0) {
        if (devos_net_is_tailnet_target(s->host) && !devos_telemetry_get()->tailscale_online) {
            set_error(s, "%.60s is a tailnet address, but Tailscale is not connected", s->host);
        } else {
            set_error(s, "Could not reach %.60s:%d (network or host down?)", s->host, s->port);
        }
        ESP_LOGE(TAG, "ssh %d: %s", s->id, s->last_error);
        goto cleanup;
    }
    s->sock_fd = fd;

    session = libssh2_session_init_ex(NULL, NULL, NULL, a);
    if (!session) { set_error(s, "Out of memory"); goto cleanup; }
    s->session_ctx = session;
    libssh2_session_set_blocking(session, 1);
    libssh2_session_set_timeout(session, 20000);

    s->state = SSH_SESSION_AUTHENTICATING;
    if (libssh2_session_handshake(session, fd) != 0) { set_err(s, session, "SSH handshake failed"); goto cleanup; }
    if (!verify_host_key(s, a, session)) goto cleanup;
    if (!authenticate(s, a, session)) goto cleanup;
    memset(a->password, 0, sizeof(a->password));

    chan = libssh2_channel_open_session(session);
    if (!chan) { set_err(s, session, "Could not open a channel"); goto cleanup; }
    s->channel_ctx = chan;

    if (libssh2_channel_request_pty_ex(chan, "xterm-256color", 14, NULL, 0, s->cols, s->rows, 0, 0) != 0) {
        set_err(s, session, "PTY request refused");
        goto cleanup;
    }
    if (libssh2_channel_shell(chan) != 0) { set_err(s, session, "Shell request refused"); goto cleanup; }

    libssh2_session_set_timeout(session, 0);
    libssh2_session_set_blocking(session, 0);
    libssh2_keepalive_config(session, 1, 30);
    s->last_error[0] = '\0';
    s->state = SSH_SESSION_CONNECTED;
    was_connected = true;
    ESP_LOGI(TAG, "ssh %d: connected %s@%s:%d", s->id, s->user, s->host, s->port);

    while (!a->close_req) {
        if (a->resize_req) {
            a->resize_req = false;
            libssh2_channel_request_pty_size(chan, a->rcols, a->rrows);
        }

        /* UI -> remote */
        size_t tn = xStreamBufferReceive(a->tx, buf, sizeof(buf), 0);
        size_t off = 0;
        while (off < tn && !a->close_req) {
            ssize_t w = libssh2_channel_write(chan, buf + off, tn - off);
            if (w == LIBSSH2_ERROR_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            if (w < 0) { set_err(s, session, "Write failed"); a->close_req = true; break; }
            off += (size_t)w;
            s->tx_bytes += (uint32_t)w;
        }

        /* remote -> UI: wait for room rather than dropping output */
        ssize_t r = libssh2_channel_read(chan, buf, sizeof(buf));
        if (r > 0) {
            size_t sent = 0;
            while (sent < (size_t)r && !a->close_req) {
                sent += xStreamBufferSend(a->rx, buf + sent, (size_t)r - sent, pdMS_TO_TICKS(50));
            }
            s->rx_bytes += (uint32_t)r;
        } else if (r == LIBSSH2_ERROR_EAGAIN || r == 0) {
            if (libssh2_channel_eof(chan)) {
                set_error(s, "Session ended");
                break;
            }
            int next = 0;
            libssh2_keepalive_send(session, &next);
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            set_err(s, session, "Connection lost");
            break;
        }
    }

cleanup:
    if (session) libssh2_session_set_blocking(session, 1);
    if (session) libssh2_session_set_timeout(session, 3000);
    if (chan) { libssh2_channel_close(chan); libssh2_channel_free(chan); }
    if (session) { libssh2_session_disconnect(session, "devOS session closed"); libssh2_session_free(session); }
    if (s->sock_fd >= 0) { devos_net_socket_close(s->sock_fd); s->sock_fd = -1; }
    s->channel_ctx = NULL;
    s->session_ctx = NULL;
    memset(a->password, 0, sizeof(a->password));
    if (was_connected || a->close_req) {
        if (was_connected) notice(a, "\r\n\033[0;33m[%s]\033[0m\r\n", s->last_error[0] ? s->last_error : "Session closed");
        s->state = SSH_SESSION_CLOSED;
    } else {
        if (!s->last_error[0]) set_error(s, "Connection failed");
        s->state = SSH_SESSION_ERROR;
    }
    a->task = NULL;
    vTaskDelete(NULL);
}

/* ---- device key (ECDSA P-256) ---- */
static size_t put_ssh_string(unsigned char *dst, const void *src, uint32_t len)
{
    dst[0] = (unsigned char)(len >> 24); dst[1] = (unsigned char)(len >> 16);
    dst[2] = (unsigned char)(len >> 8);  dst[3] = (unsigned char)len;
    memcpy(dst + 4, src, len);
    return 4 + len;
}

bool ssh_port_devkey_exists(void)
{
    nvs_handle_t h;
    if (nvs_open(SSH_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t l = 0;
    bool ok = nvs_get_str(h, "devpub", NULL, &l) == ESP_OK && l > 1;
    nvs_close(h);
    return ok;
}

int ssh_port_devkey_generate(void)
{
    int ret = -1;
    mbedtls_pk_context pk;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    unsigned char *pem = calloc(1, 800);
    unsigned char *blob = calloc(1, 160);
    char *line = calloc(1, 300);
    if (!pem || !blob || !line) goto out;

    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char *)"devos-ssh", 9) != 0 ||
        mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), mbedtls_ctr_drbg_random, &drbg) != 0 ||
        mbedtls_pk_write_key_pem(&pk, pem, 800) != 0) {
        ESP_LOGE(TAG, "device key generation failed");
        goto out;
    }
    unsigned char q[65];
    size_t qlen = 0;
    if (mbedtls_ecp_write_public_key(mbedtls_pk_ec(pk), MBEDTLS_ECP_PF_UNCOMPRESSED, &qlen, q, sizeof(q)) != 0) {
        goto out;
    }
    size_t bl = 0;
    bl += put_ssh_string(blob + bl, "ecdsa-sha2-nistp256", 19);
    bl += put_ssh_string(blob + bl, "nistp256", 8);
    bl += put_ssh_string(blob + bl, q, (uint32_t)qlen);
    char b64[200];
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &olen, blob, bl) != 0) goto out;
    snprintf(line, 300, "ecdsa-sha2-nistp256 %s devos@tab5", b64);

    nvs_handle_t h;
    if (nvs_open(SSH_NVS_NS, NVS_READWRITE, &h) != ESP_OK) goto out;
    if (nvs_set_str(h, "devkey", (const char *)pem) == ESP_OK && nvs_set_str(h, "devpub", line) == ESP_OK &&
        nvs_commit(h) == ESP_OK) {
        ret = 0;
        ESP_LOGI(TAG, "device key created: %s", line);
    }
    nvs_close(h);
out:
    if (pem) { memset(pem, 0, 800); free(pem); }
    free(blob);
    free(line);
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return ret;
}

int ssh_port_devkey_public(char *out, size_t len)
{
    if (!out || len == 0) return -1;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(SSH_NVS_NS, NVS_READONLY, &h) != ESP_OK) return -1;
    size_t l = len;
    esp_err_t e = nvs_get_str(h, "devpub", out, &l);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

#else  /* !ESP_PLATFORM: simulator device key via ssh-keygen */
#define SIM_DEVKEY TAB5_SD_MOUNT_POINT "/.ssh/id_devos"

bool ssh_port_devkey_exists(void)
{
    return access(SIM_DEVKEY ".pub", R_OK) == 0;
}

int ssh_port_devkey_generate(void)
{
    mkdir(SSH_KEYS_DIR, 0700);
    unlink(SIM_DEVKEY);
    unlink(SIM_DEVKEY ".pub");
    int rc = system("ssh-keygen -q -t ecdsa -b 256 -m PEM -N '' -C devos@tab5 -f " SIM_DEVKEY " >/dev/null 2>&1");
    return rc == 0 ? 0 : -1;
}

int ssh_port_devkey_public(char *out, size_t len)
{
    if (!out || len == 0) return -1;
    out[0] = '\0';
    FILE *f = fopen(SIM_DEVKEY ".pub", "r");
    if (!f) return -1;
    if (!fgets(out, (int)len, f)) out[0] = '\0';
    fclose(f);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    return out[0] ? 0 : -1;
}
#endif /* ESP_PLATFORM */

#ifndef ESP_PLATFORM
/* Push data to the simulator session ring buffer */
static void push_to_rx(ssh_session_t *sess, const char *data, size_t len)
{
    if (!sess || !data || len == 0) return;

    for (size_t i = 0; i < len; i++) {
        sess->rx_buf[sess->rx_head] = data[i];
        sess->rx_head = (sess->rx_head + 1) % SSH_RX_BUFFER_SIZE;
        if (sess->rx_count < SSH_RX_BUFFER_SIZE) {
            sess->rx_count++;
        } else {
            sess->rx_tail = (sess->rx_tail + 1) % SSH_RX_BUFFER_SIZE;
        }
    }
    sess->rx_bytes += len;
}
#endif

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
    return 0;
}

static bool slot_free(int i)
{
    ssh_session_state_t st = s_sessions[i].state;
    bool idle = st == SSH_SESSION_DISCONNECTED || st == SSH_SESSION_CLOSED || st == SSH_SESSION_ERROR;
#ifdef ESP_PLATFORM
    return idle && s_aux[i].task == NULL;
#else
    return idle;
#endif
}

int ssh_port_create_session(const char *alias, const char *host, int port, const char *user,
                           ssh_auth_type_t auth_type, const char *credential,
                           uint16_t cols, uint16_t rows)
{
    if (!host || strlen(host) == 0) return -1;

    /* Prefer a never-used/freed slot so finished sessions stay visible. */
    int slot = -1;
    for (int pass = 0; pass < 2 && slot < 0; pass++) {
        for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
            if ((pass == 0 && s_sessions[i].state == SSH_SESSION_DISCONNECTED) || (pass == 1 && slot_free(i))) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) return -1;                /* all sessions busy */

    ssh_session_t *sess = &s_sessions[slot];
    memset(sess, 0, sizeof(ssh_session_t));
    sess->id = slot + 1;
    sess->sock_fd = -1;
    snprintf(sess->alias, sizeof(sess->alias), "%s", alias && strlen(alias) > 0 ? alias : host);
    snprintf(sess->host, sizeof(sess->host), "%s", host);
    sess->port = port > 0 ? port : 22;
    snprintf(sess->user, sizeof(sess->user), "%s", user && strlen(user) > 0 ? user : "root");
    sess->cols = cols > 0 ? cols : DEVOS_TERM_COLS_COLLAPSED;
    sess->rows = rows > 0 ? rows : DEVOS_TERM_ROWS;

#ifndef ESP_PLATFORM
    (void)auth_type;
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = sess->cols;
    ws.ws_row = sess->rows;

    int master_fd = -1;
    pid_t pid = forkpty(&master_fd, NULL, NULL, &ws);
    if (pid < 0) {
        sess->state = SSH_SESSION_ERROR;
        set_error(sess, "forkpty: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child process: host ssh client (it prompts for passwords/host keys itself) */
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", sess->port);
        char user_host[200];
        snprintf(user_host, sizeof(user_host), "%s@%s", sess->user, sess->host);
        char key_path[256] = {0};
        if (auth_type == SSH_AUTH_KEY && credential && credential[0]) {
            if (strncmp(credential, "/sdcard/", 8) == 0) {
                snprintf(key_path, sizeof(key_path), "./sim_sdcard/%s", credential + 8);
            } else {
                snprintf(key_path, sizeof(key_path), "%s", credential);
            }
        } else if (auth_type == SSH_AUTH_DEVICE_KEY) {
            snprintf(key_path, sizeof(key_path), "%s", TAB5_SD_MOUNT_POINT "/.ssh/id_devos");
        }
        char *argv[16];
        int argc = 0;
        argv[argc++] = "ssh";
        argv[argc++] = "-tt";
        argv[argc++] = "-p";
        argv[argc++] = port_str;
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

    int flags = fcntl(master_fd, F_GETFL, 0);
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
    sess->sock_fd = master_fd;
    sess->session_ctx = (void *)(intptr_t)pid;
    sess->state = SSH_SESSION_CONNECTED;
#else
    ssh_aux_t *a = &s_aux[slot];
    StreamBufferHandle_t rx = a->rx, tx = a->tx;    /* keep the slot's buffers */
    memset(a, 0, sizeof(*a));
    a->rx = rx;
    a->tx = tx;
    a->auth = auth_type;
    if (credential) {
        if (auth_type == SSH_AUTH_KEY) snprintf(a->keypath, sizeof(a->keypath), "%s", credential);
        else snprintf(a->password, sizeof(a->password), "%s", credential);
    }
    if (!ensure_buffers(a)) {
        sess->state = SSH_SESSION_ERROR;
        set_error(sess, "Out of memory");
        return -1;
    }

    sess->state = SSH_SESSION_CONNECTING;
    char taskname[16];
    snprintf(taskname, sizeof(taskname), "ssh%d", sess->id);
    /* 12 KB stack: the mbedTLS key exchange during handshake is stack-hungry. */
    if (xTaskCreatePinnedToCore(ssh_session_task, taskname, 12288,
                                (void *)(intptr_t)slot, 6, &a->task,
                                DEVOS_CORE_NET_CRYPTO) != pdPASS) {
        sess->state = SSH_SESSION_ERROR;
        set_error(sess, "Could not start the session task");
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
    sess->state = SSH_SESSION_DISCONNECTED;
#else
    if (s_aux[idx].task) {
        s_aux[idx].close_req = true;        /* the task tears down and marks CLOSED */
        s_aux[idx].hk_answer = -1;
    } else {
        sess->state = SSH_SESSION_DISCONNECTED;
    }
#endif
    return 0;
}

int ssh_port_hostkey_answer(int session_id, bool trust)
{
    if (session_id < 1 || session_id > SSH_MAX_SESSIONS) return -1;
#ifdef ESP_PLATFORM
    s_aux[session_id - 1].hk_answer = trust ? 1 : -1;
#else
    (void)trust;
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
#else
    s_aux[idx].rcols = cols;
    s_aux[idx].rrows = rows;
    s_aux[idx].resize_req = true;
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
    return s_aux[idx].tx ? (int)xStreamBufferSend(s_aux[idx].tx, data, len, 0) : -1;
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
    return a->rx ? (int)xStreamBufferReceive(a->rx, buf, max_len, 0) : 0;
#else
    if (sess->sock_fd >= 0) {
        char temp[2048];
        ssize_t n = read(sess->sock_fd, temp, sizeof(temp));
        if (n > 0) {
            push_to_rx(sess, temp, (size_t)n);
        } else if (n == 0 || (n < 0 && (errno == EIO || errno == EBADF))) {
            push_to_rx(sess, "\r\n\033[0;33m[Session closed]\033[0m\r\n", 31);
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
        char auth_val[16];
        int num = 0;

        /* scanf directly into each field at its exact capacity-1 width: no
         * truncation of 128-byte host/key_path, and no snprintf so GCC's
         * -Wformat-truncation has nothing to flag. */
        if (sscanf(line, " \"alias\": \"%63[^\"]\"", current.alias) == 1) {
        } else if (sscanf(line, " \"host\": \"%127[^\"]\"", current.host) == 1) {
        } else if (sscanf(line, " \"port\": %d", &num) == 1) {
            current.port = num;
        } else if (sscanf(line, " \"user\": \"%63[^\"]\"", current.user) == 1) {
        } else if (sscanf(line, " \"auth\": \"%15[^\"]\"", auth_val) == 1) {
            if (strcasecmp(auth_val, "key") == 0) current.auth_type = SSH_AUTH_KEY;
            else if (strcasecmp(auth_val, "device") == 0) current.auth_type = SSH_AUTH_DEVICE_KEY;
            else current.auth_type = SSH_AUTH_PASSWORD;
        } else if (sscanf(line, " \"key_path\": \"%127[^\"]\"", current.key_path) == 1) {
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
        fprintf(f, "    \"auth\": \"%s\"", auth_name(existing[i].auth_type));
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
        fprintf(f, "    \"auth\": \"%s\"", auth_name(existing[i].auth_type));
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
