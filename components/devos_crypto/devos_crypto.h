#pragma once

/* devos_crypto: small, dependency-free primitives for the authenticator's
 * vault and TOTP codes - identical on the device and in the simulator.
 *
 *   SHA-1 / SHA-256 / SHA-512, HMAC over each (TOTP: RFC 6238)
 *   PBKDF2-HMAC-SHA256 (vault key from the passphrase)
 *   ChaCha20-Poly1305 AEAD (RFC 8439) (vault encryption)
 *   base32 (RFC 4648, as authenticator secrets are written)
 *   random bytes, constant-time compare, wipe
 *
 * Checked against the RFC test vectors in tools/crypto_test.c.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { DEVOS_HASH_SHA1 = 1, DEVOS_HASH_SHA256 = 2, DEVOS_HASH_SHA512 = 3 } devos_hash_t;

/* Digest length in bytes (20, 32, 64). */
size_t devos_hash_len(devos_hash_t h);
void devos_hash(devos_hash_t h, const void *data, size_t len, uint8_t *out);
void devos_hmac(devos_hash_t h, const void *key, size_t key_len, const void *msg, size_t msg_len, uint8_t *out);

void devos_pbkdf2_sha256(const void *pw, size_t pw_len, const uint8_t *salt, size_t salt_len, uint32_t iterations,
                         uint8_t *out, size_t out_len);
/* The same, counting iterations done into *done (for a progress bar; read it
 * from another task). */
void devos_pbkdf2_sha256_progress(const void *pw, size_t pw_len, const uint8_t *salt, size_t salt_len,
                                  uint32_t iterations, uint8_t *out, size_t out_len, volatile uint32_t *done);

/* AEAD: out gets the ciphertext (same length as in) and tag the 16-byte tag. */
void devos_chachapoly_seal(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_len,
                           const void *in, size_t len, uint8_t *out, uint8_t tag[16]);
/* Returns false (and wipes out) if the tag doesn't match. */
bool devos_chachapoly_open(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_len,
                           const void *in, size_t len, const uint8_t tag[16], uint8_t *out);

/* Decode base32 (case-insensitive; spaces, dashes and '=' padding ignored).
 * Returns the byte count, or -1 on a bad character / overflow. */
int devos_base32_decode(const char *in, uint8_t *out, size_t cap);
/* Encode (no padding). Returns the length written. */
size_t devos_base32_encode(const uint8_t *in, size_t len, char *out, size_t cap);

/* TOTP / HOTP code (digits 6..8). */
uint32_t devos_hotp(devos_hash_t h, const uint8_t *secret, size_t secret_len, uint64_t counter, int digits);

void devos_random(void *out, size_t len);
bool devos_ct_equal(const void *a, const void *b, size_t len);
void devos_wipe(void *p, size_t len);

#ifdef __cplusplus
}
#endif
