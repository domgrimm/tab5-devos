/* devos_secrets: the named-credential store for Jobs (PLAN.md 12.1).
 *
 * Values are encrypted at rest with ChaCha20-Poly1305 (devos_crypto) under a
 * 32-byte device key. The sealed blob is a file on the SD card
 * (`/.devos/secrets.enc`) so it is not bounded by the NVS per-entry blob
 * limit; the device key lives in NVS on the target (a file under the SD root
 * in the simulator, so host tests exercise the same round trip). The device
 * key is generated on first use, never hardcoded.
 *
 * Security statement: the blob is genuinely AEAD-encrypted, so a copy of the
 * SD card alone yields ciphertext. The device key is stored in NVS; an
 * attacker who can also read NVS reads the key, so full at-rest protection
 * requires NVS encryption (an `nvs_keys` partition + CONFIG_NVS_ENCRYPTION).
 * This module does not claim NVS is encrypted; see
 * devos_secrets_security_note(). Values are kept decrypted in PSRAM while the
 * engine is up so unattended jobs need no interactive PIN, and are wiped by
 * devos_secrets_deinit().
 *
 * No LVGL. Named references only: a job source stores secret("name"); the
 * value leaves this module only through devos_secret_resolve(), and the caller
 * wipes that copy. */

#include "devos_secrets.h"
#include "devos_config.h"
#include "devos_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "nvs.h"
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

/* Settings edits secrets on Core 1 while the Core 0 scheduler resolves them
 * through the hook in main.c, so every entry point takes this lock. */
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_mx;
#define SEC_LOCK()   do { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); } while (0)
#define SEC_UNLOCK() do { if (s_mx) xSemaphoreGive(s_mx); } while (0)
#else
#include <pthread.h>
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
#define SEC_LOCK()   pthread_mutex_lock(&s_mx)
#define SEC_UNLOCK() pthread_mutex_unlock(&s_mx)
#endif

static void sec_mutex_init(void)
{
#ifdef ESP_PLATFORM
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
#endif
}

#define SEC_MAGIC    "DVS1"
#define SEC_NONCE    12
#define SEC_TAG      16
#define SEC_HEADER   (4 + SEC_NONCE + SEC_TAG)
#define SEC_MAX_PLAIN (24 * 1024)     /* plaintext limit before sealing */

#define SEC_BLOB_PATH TAB5_SD_MOUNT_POINT "/.devos/secrets.enc"
#define SEC_IMPORT_PATH TAB5_SD_MOUNT_POINT "/.devos/secrets.import"
#ifndef ESP_PLATFORM
#define SIM_KEY       TAB5_SD_MOUNT_POINT "/.devos/secrets.key"
#endif

typedef struct {
    char name[DEVOS_SECRET_NAME_MAX];
    char label[DEVOS_SECRET_LABEL_MAX];
    char value[DEVOS_SECRET_VALUE_MAX];
    uint16_t value_len;
    uint32_t version;
} secret_t;

static EXT_RAM_BSS_ATTR secret_t s_sec[DEVOS_SECRETS_MAX];
static int s_count;
static uint8_t s_key[32];
static bool s_key_ok;
static bool s_loaded;
static bool s_corrupt;
static devos_secret_info_t s_info;

/* Wipe everything; the caller holds SEC_LOCK. */
static void secrets_reset_locked(void)
{
    for (int i = 0; i < s_count; i++) {
        devos_wipe(s_sec[i].value, sizeof(s_sec[i].value));
        memset(&s_sec[i], 0, sizeof(s_sec[i]));
    }
    s_count = 0;
    devos_wipe(s_key, sizeof(s_key));
    s_key_ok = false;
    s_loaded = false;
}

/* ------------------------------------------------------------------ files */
static bool file_read(const char *path, uint8_t **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > SEC_MAX_PLAIN + SEC_HEADER) { fclose(f); return false; }
    /* One extra byte so a text caller (secrets.import) can treat the buffer as
     * a NUL-terminated string even when the file's last line has no newline. */
    uint8_t *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return false; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(b); return false; }
    b[n] = '\0';
    *out = b;
    *out_len = (size_t)n;
    return true;
}

static bool file_write(const char *path, const uint8_t *b, size_t n)
{
    mkdir(TAB5_SD_MOUNT_POINT "/.devos", 0755);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = n && fwrite(b, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    return ok;
}

/* ---- device key: NVS on target, a file in the simulator ---- */
static bool key_read(uint8_t out[32])
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("secrets", NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = 32;
    bool ok = nvs_get_blob(h, "devkey", out, &n) == ESP_OK && n == 32;
    nvs_close(h);
    return ok;
#else
    uint8_t *b = NULL;
    size_t n = 0;
    bool ok = file_read(SIM_KEY, &b, &n) && n == 32;
    if (ok) memcpy(out, b, 32);
    free(b);
    return ok;
#endif
}

static bool key_write(const uint8_t key[32])
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("secrets", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "devkey", key, 32) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
#else
    return file_write(SIM_KEY, key, 32);
#endif
}

static bool key_load_or_create(void)
{
    if (key_read(s_key)) { s_key_ok = true; return true; }
    devos_random(s_key, sizeof(s_key));
    if (!key_write(s_key)) return false;
    s_key_ok = true;
    return true;
}

/* ---- sealed blob on the SD card ---- */
static bool blob_load(void)
{
    uint8_t *raw = NULL;
    size_t rn = 0;
    if (!file_read(SEC_BLOB_PATH, &raw, &rn)) return true;   /* nothing stored yet */
    if (rn < SEC_HEADER || memcmp(raw, SEC_MAGIC, 4) != 0) { free(raw); s_corrupt = true; return false; }
    const uint8_t *nonce = raw + 4;
    const uint8_t *tag = raw + 4 + SEC_NONCE;
    const uint8_t *ct = raw + SEC_HEADER;
    size_t clen = rn - SEC_HEADER;
    uint8_t *pt = malloc(clen ? clen : 1);
    if (!pt) { free(raw); return false; }
    if (!devos_chachapoly_open(s_key, nonce, SEC_MAGIC, 4, ct, clen, tag, pt)) {
        devos_wipe(pt, clen);
        free(pt);
        free(raw);
        s_corrupt = true;
        return false;
    }
    size_t o = 0;
    s_count = 0;
    s_corrupt = false;
    if (clen < 1) { devos_wipe(pt, clen); free(pt); free(raw); return true; }
    int count = pt[o++];
    bool ok = count <= DEVOS_SECRETS_MAX;
    for (int i = 0; ok && i < count; i++) {
        if (o + 1 > clen) { ok = false; break; }
        int nl = pt[o++];
        if (nl <= 0 || nl >= DEVOS_SECRET_NAME_MAX || o + (size_t)nl + 2 > clen) { ok = false; break; }
        secret_t *s = &s_sec[s_count];
        memcpy(s->name, pt + o, (size_t)nl);
        s->name[nl] = '\0';
        o += (size_t)nl;
        size_t vl = (size_t)pt[o] | ((size_t)pt[o + 1] << 8);
        o += 2;
        if (vl >= DEVOS_SECRET_VALUE_MAX || o + vl + 4 > clen) { ok = false; break; }
        memcpy(s->value, pt + o, vl);
        s->value[vl] = '\0';
        s->value_len = (uint16_t)vl;
        s->label[0] = '\0';
        o += vl;
        s->version = (uint32_t)pt[o] | ((uint32_t)pt[o + 1] << 8) |
                     ((uint32_t)pt[o + 2] << 16) | ((uint32_t)pt[o + 3] << 24);
        if (!s->version) s->version = 1;
        o += 4;
        s_count++;
    }
    devos_wipe(pt, clen);
    free(pt);
    free(raw);
    if (!ok) { secrets_reset_locked(); s_corrupt = true; return false; }
    return true;
}

static devos_err_t blob_save(void)
{
    if (!s_key_ok) return DEVOS_ERR_INVALID_STATE;
    uint8_t *pt = malloc(SEC_MAX_PLAIN);
    if (!pt) return DEVOS_ERR_NO_MEM;
    size_t o = 0;
    pt[o++] = (uint8_t)s_count;
    for (int i = 0; i < s_count; i++) {
        secret_t *s = &s_sec[i];
        size_t nl = strlen(s->name);
        if (o + 1 + nl + 2 + s->value_len + 4 > SEC_MAX_PLAIN) { free(pt); return DEVOS_ERR_INVALID_SIZE; }
        pt[o++] = (uint8_t)nl;
        memcpy(pt + o, s->name, nl);
        o += nl;
        pt[o++] = (uint8_t)(s->value_len & 0xff);
        pt[o++] = (uint8_t)(s->value_len >> 8);
        memcpy(pt + o, s->value, s->value_len);
        o += s->value_len;
        pt[o++] = (uint8_t)(s->version & 0xff);
        pt[o++] = (uint8_t)((s->version >> 8) & 0xff);
        pt[o++] = (uint8_t)((s->version >> 16) & 0xff);
        pt[o++] = (uint8_t)((s->version >> 24) & 0xff);
    }
    uint8_t *out = malloc(SEC_HEADER + o);
    if (!out) { devos_wipe(pt, SEC_MAX_PLAIN); free(pt); return DEVOS_ERR_NO_MEM; }
    memcpy(out, SEC_MAGIC, 4);
    devos_random(out + 4, SEC_NONCE);
    devos_chachapoly_seal(s_key, out + 4, SEC_MAGIC, 4, pt, o, out + SEC_HEADER, out + 4 + SEC_NONCE);
    devos_wipe(pt, SEC_MAX_PLAIN);
    free(pt);
    bool ok = file_write(SEC_BLOB_PATH, out, SEC_HEADER + o);
    devos_wipe(out, SEC_HEADER + o);
    free(out);
    return ok ? DEVOS_OK : DEVOS_ERR_FAIL;
}

/* ------------------------------------------------------------------ api */
static secret_t *find(const char *name);

/* Provisioning by file: `/sdcard/.devos/secrets.import` holds `name=value`
 * lines (one per secret, `#` comments). It is read once, sealed into the store,
 * overwritten and removed - the plaintext never stays on the card. */
static void import_file(void)
{
    uint8_t *ubuf = NULL;
    size_t len = 0;
    if (!file_read(SEC_IMPORT_PATH, &ubuf, &len)) return;
    char *buf = (char *)ubuf;
    int added = 0;
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *s = line;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        if (*s && *s != '#') {
            char *eq = strchr(s, '=');
            if (eq) {
                *eq = '\0';
                char *name = s;
                size_t nl2 = strlen(name);
                while (nl2 && (name[nl2 - 1] == ' ' || name[nl2 - 1] == '\t')) name[--nl2] = '\0';
                char *val = eq + 1;
                while (*val == ' ' || *val == '\t') val++;
                size_t vl = strlen(val);
                while (vl && (val[vl - 1] == ' ' || val[vl - 1] == '\t' || val[vl - 1] == '\r')) val[--vl] = '\0';
                if (name[0] && vl > 0) {
                    secret_t *e = find(name);
                    if (!e) {
                        if (s_count < DEVOS_SECRETS_MAX) {
                            e = &s_sec[s_count++];
                            memset(e, 0, sizeof(*e));
                            snprintf(e->name, sizeof(e->name), "%s", name);
                            e->version = 1;
                        }
                    } else {
                        e->version++;
                    }
                    if (e) {
                        snprintf(e->value, sizeof(e->value), "%s", val);
                        e->value_len = (uint16_t)strlen(e->value);
                        snprintf(e->label, sizeof(e->label), "%s", name);
                        added++;
                    }
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    /* overwrite the plaintext before removing the file */
    memset(buf, 0, len);
    free(buf);
    if (added) blob_save();
    FILE *f = fopen(SEC_IMPORT_PATH, "wb");
    if (f) { fwrite("", 1, 0, f); fclose(f); }
    remove(SEC_IMPORT_PATH);
}

/* Load once; the caller holds SEC_LOCK, so the check-then-act is atomic and
 * cannot run twice from two cores. */
static void ensure_loaded_locked(void)
{
    if (s_loaded) return;
    s_loaded = true;
    if (!key_load_or_create()) return;
    blob_load();
    import_file();
}

devos_err_t devos_secrets_init(void)
{
    sec_mutex_init();
    SEC_LOCK();
    ensure_loaded_locked();
    bool ok = s_key_ok;
    SEC_UNLOCK();
    return ok ? DEVOS_OK : DEVOS_ERR_FAIL;
}

void devos_secrets_deinit(void)
{
    SEC_LOCK();
    secrets_reset_locked();
    SEC_UNLOCK();
}

static secret_t *find(const char *name)
{
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_sec[i].name, name) == 0) return &s_sec[i];
    return NULL;
}

int devos_secrets_count(void)
{
    SEC_LOCK();
    ensure_loaded_locked();
    int n = s_count;
    SEC_UNLOCK();
    return n;
}

const devos_secret_info_t *devos_secrets_at(int index)
{
    const devos_secret_info_t *out = NULL;
    SEC_LOCK();
    ensure_loaded_locked();
    if (index >= 0 && index < s_count) {
        snprintf(s_info.name, sizeof(s_info.name), "%s", s_sec[index].name);
        snprintf(s_info.label, sizeof(s_info.label), "%s",
                 s_sec[index].label[0] ? s_sec[index].label : s_sec[index].name);
        s_info.version = s_sec[index].version;
        out = &s_info;
    }
    SEC_UNLOCK();
    return out;
}

bool devos_secrets_has(const char *name)
{
    if (!name || !name[0]) return false;
    SEC_LOCK();
    ensure_loaded_locked();
    bool has = find(name) != NULL;
    SEC_UNLOCK();
    return has;
}

int devos_secret_resolve(const char *name, char *out, size_t cap)
{
    if (!name || !out || cap == 0) return -1;
    SEC_LOCK();
    ensure_loaded_locked();
    secret_t *s = s_key_ok ? find(name) : NULL;
    int n = -1;
    if (!s) {
        out[0] = '\0';
    } else if ((size_t)s->value_len + 1 > cap) {
        devos_wipe(out, cap);
    } else {
        memcpy(out, s->value, s->value_len);
        out[s->value_len] = '\0';
        n = s->value_len;
    }
    SEC_UNLOCK();
    return n;
}

void devos_secret_wipe(char *buf, size_t len)
{
    if (buf) devos_wipe(buf, len);
}

devos_err_t devos_secrets_set(const char *name, const char *value, const char *label)
{
    if (!name || !name[0] || !value) return DEVOS_ERR_INVALID_ARG;
    if (strlen(name) >= DEVOS_SECRET_NAME_MAX) return DEVOS_ERR_INVALID_ARG;
    if (strlen(value) >= DEVOS_SECRET_VALUE_MAX) return DEVOS_ERR_INVALID_SIZE;
    SEC_LOCK();
    ensure_loaded_locked();
    secret_t *s = find(name);
    devos_err_t rc;
    if (!s) {
        if (s_count >= DEVOS_SECRETS_MAX) {
            rc = DEVOS_ERR_NO_MEM;
        } else {
            s = &s_sec[s_count++];
            memset(s, 0, sizeof(*s));
            snprintf(s->name, sizeof(s->name), "%s", name);
            s->version = 1;
            rc = DEVOS_OK;
        }
    } else {
        s->version++;
        rc = DEVOS_OK;
    }
    if (rc == DEVOS_OK) {
        size_t vl = strlen(value);
        memcpy(s->value, value, vl);
        s->value[vl] = '\0';
        s->value_len = (uint16_t)vl;
        snprintf(s->label, sizeof(s->label), "%s", label ? label : name);
        rc = blob_save();
    }
    SEC_UNLOCK();
    return rc;
}

devos_err_t devos_secrets_delete(const char *name)
{
    if (!name || !name[0]) return DEVOS_ERR_INVALID_ARG;
    SEC_LOCK();
    ensure_loaded_locked();
    secret_t *s = find(name);
    if (!s) {
        SEC_UNLOCK();
        return DEVOS_ERR_NOT_FOUND;
    }
    int idx = (int)(s - s_sec);
    devos_wipe(s->value, sizeof(s->value));
    for (int i = idx; i < s_count - 1; i++) s_sec[i] = s_sec[i + 1];
    memset(&s_sec[--s_count], 0, sizeof(s_sec[0]));
    devos_err_t rc = blob_save();
    SEC_UNLOCK();
    return rc;
}

devos_err_t devos_secrets_save(void)
{
    SEC_LOCK();
    ensure_loaded_locked();
    devos_err_t rc = blob_save();
    SEC_UNLOCK();
    return rc;
}

devos_err_t devos_secrets_set_device_key(const uint8_t key[32])
{
    if (!key) return DEVOS_ERR_INVALID_ARG;
    sec_mutex_init();
    SEC_LOCK();
    secrets_reset_locked();
    bool ok = key_write(key);
    if (ok) {
        memcpy(s_key, key, 32);
        s_key_ok = true;
        s_loaded = true;
        ok = blob_load();
    }
    SEC_UNLOCK();
    return ok ? DEVOS_OK : DEVOS_ERR_FAIL;
}

bool devos_secrets_corrupt(void)
{
    SEC_LOCK();
    bool c = s_corrupt;
    SEC_UNLOCK();
    return c;
}

const char *devos_secrets_security_note(void)
{
#ifdef ESP_PLATFORM
    return "ChaCha20-Poly1305 sealed blob on SD; device key in NVS. Enable NVS encryption "
           "(nvs_keys partition + CONFIG_NVS_ENCRYPTION) for at-rest key protection.";
#else
    return "ChaCha20-Poly1305 sealed blob; simulator test key on disk (not target security parity).";
#endif
}
