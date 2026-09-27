#pragma once

/* devos_totp: the authenticator's encrypted vault (no LVGL).
 *
 * Accounts are kept as otpauth:// URIs, encrypted with ChaCha20-Poly1305
 * under a key derived from your passphrase (PBKDF2-HMAC-SHA256, 100 000
 * rounds, random salt) and stored in the Tab5's flash (NVS) - never in the
 * clear, never on the SD card unless you export an (encrypted) backup.
 * Unlocking runs on a Core 0 task (the key derivation takes a second or
 * two); after 5 wrong tries each further try waits longer (kept across
 * restarts). Locking wipes the key and the accounts from memory.
 *
 * Imports otpauth://totp/... URIs and Google Authenticator's export QR codes
 * (otpauth-migration://offline?data=...).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_TOTP_MAX       64
#define DEVOS_TOTP_BACKUP    "/totp/vault-backup.bin"     /* on the SD card */

typedef struct {
    char issuer[48];
    char label[64];                 /* account, e.g. me@example.com */
    uint8_t secret[64];
    uint8_t secret_len;
    uint8_t algo;                   /* devos_hash_t: 1 SHA-1, 2 SHA-256, 3 SHA-512 */
    uint8_t digits;                 /* 6..8 */
    uint16_t period;                /* seconds, usually 30 */
} devos_totp_acct_t;

typedef enum {
    DEVOS_TOTP_NO_VAULT = 0,        /* first run: create one */
    DEVOS_TOTP_LOCKED,
    DEVOS_TOTP_BUSY,                /* deriving the key */
    DEVOS_TOTP_UNLOCKED,
} devos_totp_state_t;

void devos_totp_init(void);
devos_totp_state_t devos_totp_state(void);
const char *devos_totp_error(void);         /* why the last create / unlock / save failed */
int devos_totp_lockout_s(void);             /* seconds to wait before the next try */

int devos_totp_create(const char *passphrase);     /* async; min 6 characters */
int devos_totp_unlock(const char *passphrase);     /* async */
void devos_totp_lock(void);
/* Re-encrypt under a new passphrase (unlocked only; async). */
int devos_totp_change_passphrase(const char *passphrase);

int devos_totp_count(void);
bool devos_totp_get(int i, devos_totp_acct_t *out);
/* Changes are saved (encrypted) immediately. Return 0, or -1 (see error). */
int devos_totp_add(const devos_totp_acct_t *a);
int devos_totp_update(int i, const devos_totp_acct_t *a);
int devos_totp_delete(int i);
int devos_totp_move(int i, int dir);

/* Current code; remaining_s gets the seconds left in its period. */
uint32_t devos_totp_code(const devos_totp_acct_t *a, int64_t unix_time, int *remaining_s);

/* otpauth://totp/... or otpauth-migration://...: adds the accounts in it.
 * Returns how many were added (0 = duplicates only), or -1 (see error). */
int devos_totp_import_uri(const char *uri);
/* Parse one otpauth://totp URI without adding it. */
int devos_totp_parse_uri(const char *uri, devos_totp_acct_t *out, char *err, size_t errcap);
/* Write the account as an otpauth URI (for export / show). */
size_t devos_totp_to_uri(const devos_totp_acct_t *a, char *out, size_t cap);

/* Encrypted backup on the SD card (same passphrase); import merges one back. */
int devos_totp_export_backup(void);
int devos_totp_import_backup(const char *passphrase);   /* async, unlocked only */
/* Forget the vault entirely (needs no passphrase: for a forgotten one). */
void devos_totp_erase(void);

#ifdef __cplusplus
}
#endif
