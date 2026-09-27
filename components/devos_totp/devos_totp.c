/* devos_totp: see devos_totp.h.
 *
 * Vault blob: "DVT1" | kdf rounds (u32 LE) | salt[16] | nonce[12] | ciphertext | tag[16]
 * The magic, rounds and salt are the AEAD's associated data. Plaintext: one
 * otpauth://totp URI per line.
 */
#include "devos_totp.h"
#include "devos_config.h"
#include "devos_crypto.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
static SemaphoreHandle_t s_mx;
#define LOCK()   do { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_mx) xSemaphoreGive(s_mx); } while (0)
static int64_t uptime_s(void) { return esp_timer_get_time() / 1000000; }
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
static pthread_mutex_t s_mx_sim = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&s_mx_sim)
#define UNLOCK() pthread_mutex_unlock(&s_mx_sim)
static int64_t uptime_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}
#define SIM_VAULT TAB5_SD_MOUNT_POINT "/.devos/totp_vault.bin"
#define SIM_FAILS TAB5_SD_MOUNT_POINT "/.devos/totp_fails.txt"
#endif

#define MAGIC      "DVT1"
#define ROUNDS     100000
#define HDR_LEN    (4 + 4 + 16 + 12)
#define PLAIN_MAX  (DEVOS_TOTP_MAX * 400)

static devos_totp_state_t s_state;
static devos_totp_acct_t *s_acct;           /* DEVOS_TOTP_MAX, wiped on lock */
static int s_n;
static uint8_t s_key[32], s_salt[16];
static uint32_t s_rounds = ROUNDS;
static char s_err[96];
static uint32_t s_fails;
static int64_t s_wait_until;                /* uptime seconds */
static bool s_inited;

/* ------------------------------------------------------------------ storage */
static uint8_t *load_blob(size_t *len)
{
    *len = 0;
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("totp", NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t n = 0;
    uint8_t *b = NULL;
    if (nvs_get_blob(h, "vault", NULL, &n) == ESP_OK && n >= HDR_LEN + 16 && (b = malloc(n)) &&
        nvs_get_blob(h, "vault", b, &n) == ESP_OK) {
        *len = n;
    } else {
        free(b);
        b = NULL;
    }
    nvs_close(h);
    return b;
#else
    FILE *f = fopen(SIM_VAULT, "rb");
    if (!f) return NULL;
    uint8_t *b = malloc(PLAIN_MAX + HDR_LEN + 16);
    size_t n = b ? fread(b, 1, PLAIN_MAX + HDR_LEN + 16, f) : 0;
    fclose(f);
    if (n < HDR_LEN + 16) {                      /* an empty vault is header + tag */
        free(b);
        return NULL;
    }
    *len = n;
    return b;
#endif
}

static bool store_blob(const uint8_t *b, size_t len)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("totp", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = (b ? nvs_set_blob(h, "vault", b, len) : nvs_erase_key(h, "vault")) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok || !b;
#else
    if (!b) {
        remove(SIM_VAULT);
        return true;
    }
    mkdir(TAB5_SD_MOUNT_POINT "/.devos", 0755);
    FILE *f = fopen(SIM_VAULT, "wb");
    if (!f) return false;
    bool ok = fwrite(b, 1, len, f) == len;
    return fclose(f) == 0 && ok;
#endif
}

static void save_fails(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("totp", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "fails", s_fails);
        nvs_commit(h);
        nvs_close(h);
    }
#else
    FILE *f = fopen(SIM_FAILS, "w");
    if (f) {
        fprintf(f, "%u\n", (unsigned)s_fails);
        fclose(f);
    }
#endif
}

static void load_fails(void)
{
    s_fails = 0;
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("totp", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "fails", &s_fails);
        nvs_close(h);
    }
#else
    FILE *f = fopen(SIM_FAILS, "r");
    if (f) {
        unsigned v = 0;
        if (fscanf(f, "%u", &v) == 1) s_fails = v;
        fclose(f);
    }
#endif
}

static int wait_for(uint32_t fails)
{
    if (fails < 5) return 0;
    int w = 30 << (fails - 5 > 7 ? 7 : fails - 5);   /* 30 s, 1 min, 2 min ... up to ~1 h */
    return w > 3600 ? 3600 : w;
}

/* ------------------------------------------------------------------ uri helpers */
static void pct_decode(const char *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        if (in[i] == '%' && i + 2 < n && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            char h[3] = { in[i + 1], in[i + 2], 0 };
            out[o++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else if (in[i] == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

static size_t pct_encode(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '@') out[o++] = (char)c;
        else o += (size_t)snprintf(out + o, cap - o, "%%%02X", c);
    }
    out[o] = '\0';
    return o;
}

/* value of ?key=... / &key=... */
static bool query_get(const char *q, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = q; p && *p;) {
        if (!strncasecmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t vl = strcspn(v, "&#");
            pct_decode(v, vl, out, cap);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

int devos_totp_parse_uri(const char *uri, devos_totp_acct_t *a, char *err, size_t errcap)
{
    memset(a, 0, sizeof(*a));
    while (*uri == ' ') uri++;
    if (!strncasecmp(uri, "otpauth://hotp/", 15)) {
        snprintf(err, errcap, "Counter-based (HOTP) codes aren't supported");
        return -1;
    }
    if (strncasecmp(uri, "otpauth://totp/", 15)) {
        snprintf(err, errcap, "Not an authenticator QR code (otpauth://)");
        return -1;
    }
    const char *lab = uri + 15, *q = strchr(lab, '?');
    if (!q) {
        snprintf(err, errcap, "The code has no secret");
        return -1;
    }
    char label[160];
    pct_decode(lab, (size_t)(q - lab), label, sizeof(label));
    char *colon = strchr(label, ':');
    if (colon) {
        *colon = '\0';
        snprintf(a->issuer, sizeof(a->issuer), "%s", label);
        const char *acc = colon + 1;
        while (*acc == ' ') acc++;
        snprintf(a->label, sizeof(a->label), "%s", acc);
    } else {
        snprintf(a->label, sizeof(a->label), "%s", label);
    }
    q++;
    char v[160];
    if (query_get(q, "issuer", v, sizeof(v)) && v[0]) snprintf(a->issuer, sizeof(a->issuer), "%s", v);
    if (!query_get(q, "secret", v, sizeof(v))) {
        snprintf(err, errcap, "The code has no secret");
        return -1;
    }
    int n = devos_base32_decode(v, a->secret, sizeof(a->secret));
    if (n <= 0) {
        snprintf(err, errcap, "The secret isn't valid base32");
        return -1;
    }
    a->secret_len = (uint8_t)n;
    a->algo = 1;
    if (query_get(q, "algorithm", v, sizeof(v))) {
        if (!strcasecmp(v, "SHA256")) a->algo = 2;
        else if (!strcasecmp(v, "SHA512")) a->algo = 3;
        else if (strcasecmp(v, "SHA1")) {
            snprintf(err, errcap, "Unsupported algorithm %.20s", v);
            return -1;
        }
    }
    a->digits = 6;
    if (query_get(q, "digits", v, sizeof(v))) a->digits = (uint8_t)atoi(v);
    if (a->digits < 6 || a->digits > 8) {
        snprintf(err, errcap, "Unsupported number of digits");
        return -1;
    }
    a->period = 30;
    if (query_get(q, "period", v, sizeof(v)) && atoi(v) > 0) a->period = (uint16_t)atoi(v);
    if (!a->issuer[0] && !a->label[0]) snprintf(a->label, sizeof(a->label), "Account");
    return 0;
}

size_t devos_totp_to_uri(const devos_totp_acct_t *a, char *out, size_t cap)
{
    char iss[160], lab[200], sec[120];
    pct_encode(a->issuer, iss, sizeof(iss));
    pct_encode(a->label, lab, sizeof(lab));
    devos_base32_encode(a->secret, a->secret_len, sec, sizeof(sec));
    static const char *alg[] = { "SHA1", "SHA1", "SHA256", "SHA512" };
    int n = snprintf(out, cap, "otpauth://totp/%s%s%s?secret=%s%s%s&algorithm=%s&digits=%d&period=%d", iss,
                     a->issuer[0] ? ":" : "", lab, sec, a->issuer[0] ? "&issuer=" : "", iss, alg[a->algo & 3], a->digits,
                     a->period);
    devos_wipe(sec, sizeof(sec));
    return n < 0 ? 0 : (size_t)n;
}

/* ------------------------------------------------------------------ Google Authenticator export */
static size_t b64_decode(const char *in, uint8_t *out, size_t cap)
{
    uint32_t buf = 0;
    int bits = 0;
    size_t n = 0;
    for (; *in; in++) {
        int c = (unsigned char)*in, v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == ' ') v = 62;                      /* a '+' a URL decoder turned into a space */
        else continue;                                  /* '=' padding, newlines */
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8 && n < cap) {
            out[n++] = (uint8_t)(buf >> (bits - 8));
            bits -= 8;
        }
    }
    return n;
}

static bool pb_varint(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
    *v = 0;
    for (int shift = 0; *p < end && shift < 64; shift += 7) {
        uint8_t b = *(*p)++;
        *v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

/* one OtpParameters message */
static bool pb_otp(const uint8_t *p, const uint8_t *end, devos_totp_acct_t *a, int *type)
{
    memset(a, 0, sizeof(*a));
    a->algo = 1;
    a->digits = 6;
    a->period = 30;
    *type = 2;
    while (p < end) {
        uint64_t key, v;
        if (!pb_varint(&p, end, &key)) return false;
        int field = (int)(key >> 3), wt = (int)(key & 7);
        if (wt == 0) {
            if (!pb_varint(&p, end, &v)) return false;
            if (field == 4) a->algo = v == 2 ? 2 : v == 3 ? 3 : 1;
            else if (field == 5) a->digits = v == 2 ? 8 : 6;
            else if (field == 6) *type = (int)v;
        } else if (wt == 2) {
            if (!pb_varint(&p, end, &v) || v > (uint64_t)(end - p)) return false;
            size_t l = (size_t)v;
            if (field == 1) {
                a->secret_len = (uint8_t)(l < sizeof(a->secret) ? l : sizeof(a->secret));
                memcpy(a->secret, p, a->secret_len);
            } else if (field == 2) {
                char nm[160];
                size_t k = l < sizeof(nm) - 1 ? l : sizeof(nm) - 1;
                memcpy(nm, p, k);
                nm[k] = '\0';
                char *c = strchr(nm, ':');                  /* "Issuer:account" */
                snprintf(a->label, sizeof(a->label), "%s", c ? c + 1 : nm);
                if (c && !a->issuer[0]) {
                    *c = '\0';
                    snprintf(a->issuer, sizeof(a->issuer), "%s", nm);
                }
            } else if (field == 3) {
                size_t k = l < sizeof(a->issuer) - 1 ? l : sizeof(a->issuer) - 1;
                memcpy(a->issuer, p, k);
                a->issuer[k] = '\0';
            }
            p += l;
        } else if (wt == 1) {
            p += 8;
        } else if (wt == 5) {
            p += 4;
        } else {
            return false;
        }
    }
    return a->secret_len > 0;
}

/* ------------------------------------------------------------------ vault crypto */
static void wipe_accounts(void)
{
    if (s_acct) devos_wipe(s_acct, sizeof(devos_totp_acct_t) * DEVOS_TOTP_MAX);
    s_n = 0;
}

/* encrypt the accounts under s_key / s_salt and store (lock held) */
static bool save_locked(void)
{
    char *plain = malloc(PLAIN_MAX);
    uint8_t *blob = malloc(PLAIN_MAX + HDR_LEN + 16);
    if (!plain || !blob) {
        free(plain);
        free(blob);
        snprintf(s_err, sizeof(s_err), "Out of memory");
        return false;
    }
    size_t o = 0;
    for (int i = 0; i < s_n && o + 400 < PLAIN_MAX; i++) {
        o += devos_totp_to_uri(&s_acct[i], plain + o, PLAIN_MAX - o);
        plain[o++] = '\n';
    }
    memcpy(blob, MAGIC, 4);
    blob[4] = (uint8_t)s_rounds; blob[5] = (uint8_t)(s_rounds >> 8); blob[6] = (uint8_t)(s_rounds >> 16); blob[7] = (uint8_t)(s_rounds >> 24);
    memcpy(blob + 8, s_salt, 16);
    devos_random(blob + 24, 12);
    devos_chachapoly_seal(s_key, blob + 24, blob, 24, plain, o, blob + HDR_LEN, blob + HDR_LEN + o);
    devos_wipe(plain, PLAIN_MAX);
    free(plain);
    bool ok = store_blob(blob, HDR_LEN + o + 16);
    free(blob);
    if (!ok) snprintf(s_err, sizeof(s_err), "Couldn't save the vault");
    return ok;
}

/* parse decrypted text into accounts (lock held) */
static void load_plain(const char *plain, size_t n, bool merge)
{
    if (!merge) s_n = 0;
    const char *p = plain, *end = plain + n;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t l = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char line[400], err[64];
        if (l && l < sizeof(line)) {
            memcpy(line, p, l);
            line[l] = '\0';
            devos_totp_acct_t a;
            if (devos_totp_parse_uri(line, &a, err, sizeof(err)) == 0 && s_n < DEVOS_TOTP_MAX) {
                bool dup = false;
                for (int i = 0; i < s_n && !dup; i++)
                    dup = s_acct[i].secret_len == a.secret_len && !memcmp(s_acct[i].secret, a.secret, a.secret_len);
                if (!dup) s_acct[s_n++] = a;
            }
            devos_wipe(line, sizeof(line));
        }
        p += l + 1;
    }
}

/* ------------------------------------------------------------------ async jobs */
typedef enum { JOB_CREATE, JOB_UNLOCK, JOB_REKEY, JOB_IMPORT } job_t;
static struct {
    job_t kind;
    char pass[128];
} s_job;
static volatile uint32_t s_kdf_done, s_kdf_total;   /* the running job's PBKDF2 rounds */

#ifdef ESP_PLATFORM
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
#else
static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#endif

static void job_run(void)
{
    char pass[128];
    LOCK();
    job_t kind = s_job.kind;
    memcpy(pass, s_job.pass, sizeof(pass));
    devos_wipe(s_job.pass, sizeof(s_job.pass));
    UNLOCK();
    size_t pl = strlen(pass);
    if (kind == JOB_CREATE || kind == JOB_REKEY) {
        uint8_t salt[16], key[32];
        devos_random(salt, sizeof(salt));
        s_kdf_total = ROUNDS;
        uint32_t t0 = now_ms();
        devos_pbkdf2_sha256_progress(pass, pl, salt, sizeof(salt), ROUNDS, key, sizeof(key), &s_kdf_done);
        printf("[totp] key derived in %u ms (%u rounds)\n", (unsigned)(now_ms() - t0), (unsigned)ROUNDS);
        LOCK();
        memcpy(s_salt, salt, 16);
        memcpy(s_key, key, 32);
        s_rounds = ROUNDS;
        if (kind == JOB_CREATE) s_n = 0;
        bool ok = save_locked();
        s_state = ok ? DEVOS_TOTP_UNLOCKED : (kind == JOB_CREATE ? DEVOS_TOTP_NO_VAULT : DEVOS_TOTP_UNLOCKED);
        if (ok) s_err[0] = '\0';
        UNLOCK();
        devos_wipe(key, sizeof(key));
    } else {
        size_t bl = 0;
        uint8_t *blob = NULL;
        if (kind == JOB_UNLOCK) {
            blob = load_blob(&bl);
        } else {                                        /* JOB_IMPORT: the SD backup */
            FILE *f = fopen(TAB5_SD_MOUNT_POINT DEVOS_TOTP_BACKUP, "rb");
            if (f) {
                blob = malloc(PLAIN_MAX + HDR_LEN + 16);
                bl = blob ? fread(blob, 1, PLAIN_MAX + HDR_LEN + 16, f) : 0;
                fclose(f);
            }
        }
        bool ok = false;
        if (blob && bl >= HDR_LEN + 16 && !memcmp(blob, MAGIC, 4)) {
            uint32_t rounds = blob[4] | (uint32_t)blob[5] << 8 | (uint32_t)blob[6] << 16 | (uint32_t)blob[7] << 24;
            if (rounds < 1000 || rounds > 10000000) rounds = ROUNDS;
            uint8_t key[32];
            s_kdf_total = rounds;
            uint32_t t0 = now_ms();
            devos_pbkdf2_sha256_progress(pass, pl, blob + 8, 16, rounds, key, sizeof(key), &s_kdf_done);
            printf("[totp] key derived in %u ms (%u rounds)\n", (unsigned)(now_ms() - t0), (unsigned)rounds);
            size_t cl = bl - HDR_LEN - 16;
            char *plain = malloc(cl + 1);
            if (plain && devos_chachapoly_open(key, blob + 24, blob, 24, blob + HDR_LEN, cl, blob + bl - 16, (uint8_t *)plain)) {
                plain[cl] = '\0';
                ok = true;
                LOCK();
                if (kind == JOB_UNLOCK) {
                    memcpy(s_key, key, 32);
                    memcpy(s_salt, blob + 8, 16);
                    s_rounds = rounds;
                    load_plain(plain, cl, false);
                    s_fails = 0;
                    save_fails();
                    s_state = DEVOS_TOTP_UNLOCKED;
                } else {
                    int before = s_n;
                    load_plain(plain, cl, true);
                    snprintf(s_err, sizeof(s_err), "Imported %d account%s from the backup", s_n - before, s_n - before == 1 ? "" : "s");
                    save_locked();
                    s_state = DEVOS_TOTP_UNLOCKED;
                }
                UNLOCK();
            }
            if (plain) {
                devos_wipe(plain, cl + 1);
                free(plain);
            }
            devos_wipe(key, sizeof(key));
        }
        if (!ok) {
            LOCK();
            if (kind == JOB_UNLOCK) {
                if (!blob) {
                    snprintf(s_err, sizeof(s_err), "No vault found");
                    s_state = DEVOS_TOTP_NO_VAULT;
                } else {
                    s_fails++;
                    save_fails();
                    int w = wait_for(s_fails);
                    s_wait_until = uptime_s() + w;
                    if (w) snprintf(s_err, sizeof(s_err), "Wrong passphrase - wait %d s before the next try", w);
                    else snprintf(s_err, sizeof(s_err), "Wrong passphrase (%u of 5 before a wait)", (unsigned)s_fails);
                    s_state = DEVOS_TOTP_LOCKED;
                }
            } else {
                snprintf(s_err, sizeof(s_err), blob ? "The backup's passphrase doesn't match" : "No backup at %s on the SD card",
                         DEVOS_TOTP_BACKUP);
                s_state = DEVOS_TOTP_UNLOCKED;
            }
            UNLOCK();
        }
        free(blob);
    }
    devos_wipe(pass, sizeof(pass));
}

#ifdef ESP_PLATFORM
static void job_task(void *arg)
{
    (void)arg;
    job_run();
    vTaskDelete(NULL);
}
#else
static void *job_thread(void *arg)
{
    (void)arg;
    job_run();
    return NULL;
}
#endif

static int job_start(job_t kind, const char *pass)
{
    LOCK();
    if (s_state == DEVOS_TOTP_BUSY) {
        UNLOCK();
        return -1;
    }
    s_job.kind = kind;
    snprintf(s_job.pass, sizeof(s_job.pass), "%s", pass);
    s_kdf_done = 0;
    s_kdf_total = ROUNDS;
    devos_totp_state_t prev = s_state;
    s_state = DEVOS_TOTP_BUSY;
    s_err[0] = '\0';
    UNLOCK();
#ifdef ESP_PLATFORM
    /* crypto on the network / crypto core; SHA rounds need little stack */
    if (xTaskCreatePinnedToCore(job_task, "totp_kdf", 6144, NULL, 2, NULL, DEVOS_CORE_NET_CRYPTO) != pdPASS) {
#else
    pthread_t t;
    if (pthread_create(&t, NULL, job_thread, NULL) != 0) {
#endif
        LOCK();
        s_state = prev;
        devos_wipe(s_job.pass, sizeof(s_job.pass));
        snprintf(s_err, sizeof(s_err), "Couldn't start");
        UNLOCK();
        return -1;
    }
#ifndef ESP_PLATFORM
    pthread_detach(t);
#endif
    return 0;
}

/* ------------------------------------------------------------------ api */
void devos_totp_init(void)
{
    if (s_inited) return;
    s_inited = true;
#ifdef ESP_PLATFORM
    s_mx = xSemaphoreCreateMutex();
    s_acct = heap_caps_calloc(DEVOS_TOTP_MAX, sizeof(devos_totp_acct_t), MALLOC_CAP_INTERNAL);   /* secrets stay on-chip */
    if (!s_acct) s_acct = calloc(DEVOS_TOTP_MAX, sizeof(devos_totp_acct_t));
#else
    s_acct = calloc(DEVOS_TOTP_MAX, sizeof(devos_totp_acct_t));
#endif
    size_t l;
    uint8_t *b = load_blob(&l);
    s_state = b ? DEVOS_TOTP_LOCKED : DEVOS_TOTP_NO_VAULT;
    free(b);
    load_fails();
    s_wait_until = uptime_s() + wait_for(s_fails);          /* a restart doesn't skip the wait */
}

devos_totp_state_t devos_totp_state(void) { return s_state; }

const char *devos_totp_busy_text(void)
{
    switch (s_job.kind) {
    case JOB_CREATE: return "Creating the vault";
    case JOB_UNLOCK: return "Unlocking";
    case JOB_REKEY:  return "Changing the passphrase";
    default:         return "Opening the backup";
    }
}

int devos_totp_progress(void)
{
    uint32_t total = s_kdf_total, done = s_kdf_done;
    if (s_state != DEVOS_TOTP_BUSY || !total) return 0;
    return done >= total ? 100 : (int)((uint64_t)done * 100 / total);
}
const char *devos_totp_error(void) { return s_err; }

int devos_totp_lockout_s(void)
{
    int64_t w = s_wait_until - uptime_s();
    return w > 0 ? (int)w : 0;
}

int devos_totp_create(const char *pass)
{
    if (!pass || strlen(pass) < 6) {
        snprintf(s_err, sizeof(s_err), "Use at least 6 characters");
        return -1;
    }
    return job_start(JOB_CREATE, pass);
}

int devos_totp_unlock(const char *pass)
{
    if (s_state != DEVOS_TOTP_LOCKED || !pass) return -1;
    int w = devos_totp_lockout_s();
    if (w) {
        snprintf(s_err, sizeof(s_err), "Too many wrong tries - wait %d s", w);
        return -1;
    }
    return job_start(JOB_UNLOCK, pass);
}

int devos_totp_change_passphrase(const char *pass)
{
    if (s_state != DEVOS_TOTP_UNLOCKED) return -1;
    if (!pass || strlen(pass) < 6) {
        snprintf(s_err, sizeof(s_err), "Use at least 6 characters");
        return -1;
    }
    return job_start(JOB_REKEY, pass);
}

int devos_totp_import_backup(const char *pass)
{
    if (s_state != DEVOS_TOTP_UNLOCKED || !pass) return -1;
    return job_start(JOB_IMPORT, pass);
}

void devos_totp_lock(void)
{
    LOCK();
    if (s_state == DEVOS_TOTP_UNLOCKED) {
        wipe_accounts();
        devos_wipe(s_key, sizeof(s_key));
        s_state = DEVOS_TOTP_LOCKED;
    }
    UNLOCK();
}

void devos_totp_erase(void)
{
    LOCK();
    wipe_accounts();
    devos_wipe(s_key, sizeof(s_key));
    store_blob(NULL, 0);
    s_fails = 0;
    save_fails();
    s_wait_until = 0;
    s_state = DEVOS_TOTP_NO_VAULT;
    UNLOCK();
}

int devos_totp_count(void) { return s_state == DEVOS_TOTP_UNLOCKED ? s_n : 0; }

bool devos_totp_get(int i, devos_totp_acct_t *out)
{
    LOCK();
    bool ok = s_state == DEVOS_TOTP_UNLOCKED && i >= 0 && i < s_n;
    if (ok) *out = s_acct[i];
    UNLOCK();
    return ok;
}

int devos_totp_add(const devos_totp_acct_t *a)
{
    LOCK();
    int rc = -1;
    if (s_state != DEVOS_TOTP_UNLOCKED) snprintf(s_err, sizeof(s_err), "Locked");
    else if (s_n >= DEVOS_TOTP_MAX) snprintf(s_err, sizeof(s_err), "The vault is full (%d)", DEVOS_TOTP_MAX);
    else if (!a->secret_len) snprintf(s_err, sizeof(s_err), "No secret");
    else {
        bool dup = false;
        for (int i = 0; i < s_n && !dup; i++)
            dup = s_acct[i].secret_len == a->secret_len && !memcmp(s_acct[i].secret, a->secret, a->secret_len);
        if (dup) snprintf(s_err, sizeof(s_err), "That account is already in the vault");
        else {
            s_acct[s_n++] = *a;
            rc = save_locked() ? 0 : -1;
        }
    }
    UNLOCK();
    return rc;
}

int devos_totp_update(int i, const devos_totp_acct_t *a)
{
    LOCK();
    int rc = -1;
    if (s_state == DEVOS_TOTP_UNLOCKED && i >= 0 && i < s_n) {
        s_acct[i] = *a;
        rc = save_locked() ? 0 : -1;
    }
    UNLOCK();
    return rc;
}

int devos_totp_delete(int i)
{
    LOCK();
    int rc = -1;
    if (s_state == DEVOS_TOTP_UNLOCKED && i >= 0 && i < s_n) {
        devos_wipe(&s_acct[i], sizeof(s_acct[i]));
        memmove(&s_acct[i], &s_acct[i + 1], sizeof(s_acct[0]) * (size_t)(s_n - i - 1));
        s_n--;
        devos_wipe(&s_acct[s_n], sizeof(s_acct[0]));
        rc = save_locked() ? 0 : -1;
    }
    UNLOCK();
    return rc;
}

int devos_totp_move(int i, int dir)
{
    LOCK();
    int rc = -1, j = i + dir;
    if (s_state == DEVOS_TOTP_UNLOCKED && i >= 0 && i < s_n && j >= 0 && j < s_n) {
        devos_totp_acct_t t = s_acct[i];
        s_acct[i] = s_acct[j];
        s_acct[j] = t;
        devos_wipe(&t, sizeof(t));
        rc = save_locked() ? 0 : -1;
    }
    UNLOCK();
    return rc;
}

uint32_t devos_totp_code(const devos_totp_acct_t *a, int64_t now, int *remaining_s)
{
    int period = a->period ? a->period : 30;
    if (remaining_s) *remaining_s = period - (int)(now % period);
    return devos_hotp((devos_hash_t)(a->algo ? a->algo : 1), a->secret, a->secret_len, (uint64_t)(now / period), a->digits);
}

int devos_totp_import_uri(const char *uri)
{
    if (s_state != DEVOS_TOTP_UNLOCKED) return -1;
    while (*uri == ' ') uri++;
    if (!strncasecmp(uri, "otpauth-migration://", 20)) {
        const char *q = strchr(uri, '?');
        char data[3072];
        if (!q || !query_get(q + 1, "data", data, sizeof(data))) {
            snprintf(s_err, sizeof(s_err), "That export code has no data");
            return -1;
        }
        uint8_t *pb = malloc(sizeof(data));
        if (!pb) return -1;
        size_t n = b64_decode(data, pb, sizeof(data));
        const uint8_t *p = pb, *end = pb + n;
        int added = 0, skipped = 0;
        while (p < end) {
            uint64_t key, len;
            if (!pb_varint(&p, end, &key)) break;
            if ((key & 7) == 2) {
                if (!pb_varint(&p, end, &len) || len > (uint64_t)(end - p)) break;
                if ((key >> 3) == 1) {                          /* otp_parameters */
                    devos_totp_acct_t a;
                    int type;
                    if (pb_otp(p, p + len, &a, &type)) {
                        if (type == 1) skipped++;                /* HOTP */
                        else if (devos_totp_add(&a) == 0) added++;
                    }
                    devos_wipe(&a, sizeof(a));
                }
                p += len;
            } else if ((key & 7) == 0) {
                uint64_t v;
                if (!pb_varint(&p, end, &v)) break;
            } else {
                break;
            }
        }
        devos_wipe(pb, sizeof(data));
        devos_wipe(data, sizeof(data));
        free(pb);
        if (!added && !skipped) {
            snprintf(s_err, sizeof(s_err), "No new accounts in that code (already added?)");
            return 0;
        }
        snprintf(s_err, sizeof(s_err), "Imported %d account%s%s", added, added == 1 ? "" : "s",
                 skipped ? " (skipped counter-based ones)" : "");
        return added;
    }
    devos_totp_acct_t a;
    char err[96];
    if (devos_totp_parse_uri(uri, &a, err, sizeof(err)) != 0) {
        snprintf(s_err, sizeof(s_err), "%s", err);
        return -1;
    }
    int rc = devos_totp_add(&a);
    devos_wipe(&a, sizeof(a));
    return rc == 0 ? 1 : (strstr(s_err, "already") ? 0 : -1);
}

int devos_totp_export_backup(void)
{
    size_t l;
    uint8_t *b = load_blob(&l);
    if (!b) {
        snprintf(s_err, sizeof(s_err), "No vault to back up");
        return -1;
    }
    mkdir(TAB5_SD_MOUNT_POINT "/totp", 0755);
    FILE *f = fopen(TAB5_SD_MOUNT_POINT DEVOS_TOTP_BACKUP, "wb");
    bool ok = f && fwrite(b, 1, l, f) == l;
    if (f) ok = fclose(f) == 0 && ok;
    free(b);
    snprintf(s_err, sizeof(s_err), ok ? "Encrypted backup written to %s" : "Couldn't write %s (SD card?)", DEVOS_TOTP_BACKUP);
    return ok ? 0 : -1;
}
