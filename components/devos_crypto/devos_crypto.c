/* devos_crypto: see devos_crypto.h. Straightforward reference
 * implementations; tools/crypto_test.c checks them against the RFC vectors. */
#include "devos_crypto.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_random.h"
#endif

/* ------------------------------------------------------------------ util */
void devos_wipe(void *p, size_t len)
{
    volatile uint8_t *v = p;
    while (len--) *v++ = 0;
}

bool devos_ct_equal(const void *a, const void *b, size_t len)
{
    const uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < len; i++) d |= x[i] ^ y[i];
    return d == 0;
}

void devos_random(void *out, size_t len)
{
#ifdef ESP_PLATFORM
    esp_fill_random(out, len);
#else
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = f ? fread(out, 1, len, f) : 0;
    if (f) fclose(f);
    for (size_t i = got; i < len; i++) ((uint8_t *)out)[i] = (uint8_t)(i * 131 + 7);   /* never happens */
#endif
}

static uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint64_t ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put_be64(uint8_t *p, uint64_t v) { put_be32(p, (uint32_t)(v >> 32)); put_be32(p + 4, (uint32_t)v); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* ------------------------------------------------------------------ hash contexts */
typedef struct {
    devos_hash_t h;
    uint64_t len;                   /* bytes absorbed (SHA-512's 128-bit length: < 2^64 here) */
    size_t fill;
    union { uint32_t s32[8]; uint64_t s64[8]; } st;
    uint8_t buf[128];
} hctx_t;

static size_t block_len(devos_hash_t h) { return h == DEVOS_HASH_SHA512 ? 128 : 64; }
size_t devos_hash_len(devos_hash_t h) { return h == DEVOS_HASH_SHA1 ? 20 : h == DEVOS_HASH_SHA256 ? 32 : 64; }

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL };

static void sha1_block(uint32_t *s, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = be32(p + i * 4);
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e;
}

static void sha256_block(uint32_t *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = be32(p + i * 4);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

static void sha512_block(uint64_t *s, const uint8_t *p)
{
    uint64_t w[80];
    for (int i = 0; i < 16; i++) w[i] = be64(p + i * 8);
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K512[i] + w[i];
        uint64_t S0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
        uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

static void h_init(hctx_t *c, devos_hash_t h)
{
    memset(c, 0, sizeof(*c));
    c->h = h;
    if (h == DEVOS_HASH_SHA1) {
        static const uint32_t iv[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
        memcpy(c->st.s32, iv, sizeof(iv));
    } else if (h == DEVOS_HASH_SHA256) {
        static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
        memcpy(c->st.s32, iv, sizeof(iv));
    } else {
        static const uint64_t iv[8] = { 0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                                        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL };
        memcpy(c->st.s64, iv, sizeof(iv));
    }
}

static void h_block(hctx_t *c, const uint8_t *p)
{
    if (c->h == DEVOS_HASH_SHA1) sha1_block(c->st.s32, p);
    else if (c->h == DEVOS_HASH_SHA256) sha256_block(c->st.s32, p);
    else sha512_block(c->st.s64, p);
}

static void h_update(hctx_t *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t bl = block_len(c->h);
    c->len += len;
    if (c->fill) {
        size_t take = bl - c->fill < len ? bl - c->fill : len;
        memcpy(c->buf + c->fill, p, take);
        c->fill += take;
        p += take;
        len -= take;
        if (c->fill == bl) {
            h_block(c, c->buf);
            c->fill = 0;
        }
    }
    while (len >= bl) {
        h_block(c, p);
        p += bl;
        len -= bl;
    }
    if (len) {
        memcpy(c->buf, p, len);
        c->fill = len;
    }
}

static void h_final(hctx_t *c, uint8_t *out)
{
    size_t bl = block_len(c->h), lenb = c->h == DEVOS_HASH_SHA512 ? 16 : 8;
    uint64_t bits = c->len * 8;
    c->buf[c->fill++] = 0x80;
    if (c->fill > bl - lenb) {
        memset(c->buf + c->fill, 0, bl - c->fill);
        h_block(c, c->buf);
        c->fill = 0;
    }
    memset(c->buf + c->fill, 0, bl - c->fill);
    put_be64(c->buf + bl - 8, bits);
    h_block(c, c->buf);
    if (c->h == DEVOS_HASH_SHA512) for (int i = 0; i < 8; i++) put_be64(out + i * 8, c->st.s64[i]);
    else for (size_t i = 0; i < devos_hash_len(c->h) / 4; i++) put_be32(out + i * 4, c->st.s32[i]);
    devos_wipe(c, sizeof(*c));
}

void devos_hash(devos_hash_t h, const void *data, size_t len, uint8_t *out)
{
    hctx_t c;
    h_init(&c, h);
    h_update(&c, data, len);
    h_final(&c, out);
}

/* ------------------------------------------------------------------ HMAC */
typedef struct {
    hctx_t in, out;                 /* states after absorbing ipad / opad */
} hmac_t;

static void hmac_setup(hmac_t *m, devos_hash_t h, const void *key, size_t key_len)
{
    uint8_t k[128], pad[128];
    size_t bl = block_len(h);
    memset(k, 0, sizeof(k));
    if (key_len > bl) devos_hash(h, key, key_len, k);
    else memcpy(k, key, key_len);
    for (size_t i = 0; i < bl; i++) pad[i] = k[i] ^ 0x36;
    h_init(&m->in, h);
    h_update(&m->in, pad, bl);
    for (size_t i = 0; i < bl; i++) pad[i] = k[i] ^ 0x5c;
    h_init(&m->out, h);
    h_update(&m->out, pad, bl);
    devos_wipe(k, sizeof(k));
    devos_wipe(pad, sizeof(pad));
}

static void hmac_run(const hmac_t *m, const void *msg, size_t len, uint8_t *out)
{
    hctx_t c = m->in;
    uint8_t ih[64];
    h_update(&c, msg, len);
    h_final(&c, ih);
    c = m->out;
    h_update(&c, ih, devos_hash_len(c.h));
    h_final(&c, out);
    devos_wipe(ih, sizeof(ih));
}

void devos_hmac(devos_hash_t h, const void *key, size_t key_len, const void *msg, size_t msg_len, uint8_t *out)
{
    hmac_t m;
    hmac_setup(&m, h, key, key_len);
    hmac_run(&m, msg, msg_len, out);
    devos_wipe(&m, sizeof(m));
}

void devos_pbkdf2_sha256(const void *pw, size_t pw_len, const uint8_t *salt, size_t salt_len, uint32_t iterations,
                         uint8_t *out, size_t out_len)
{
    devos_pbkdf2_sha256_progress(pw, pw_len, salt, salt_len, iterations, out, out_len, NULL);
}

void devos_pbkdf2_sha256_progress(const void *pw, size_t pw_len, const uint8_t *salt, size_t salt_len,
                                  uint32_t iterations, uint8_t *out, size_t out_len, volatile uint32_t *done)
{
    hmac_t m;
    hmac_setup(&m, DEVOS_HASH_SHA256, pw, pw_len);
    uint8_t u[32], t[32], blk[128];
    for (uint32_t block = 1; out_len; block++) {
        size_t sl = salt_len < sizeof(blk) - 4 ? salt_len : sizeof(blk) - 4;
        memcpy(blk, salt, sl);
        put_be32(blk + sl, block);
        hmac_run(&m, blk, sl + 4, u);
        memcpy(t, u, 32);
        for (uint32_t i = 1; i < iterations; i++) {
            hmac_run(&m, u, 32, u);
            for (int k = 0; k < 32; k++) t[k] ^= u[k];
            if (done && (i & 1023) == 0) *done = i;
        }
        if (done) *done = iterations;
        size_t n = out_len < 32 ? out_len : 32;
        memcpy(out, t, n);
        out += n;
        out_len -= n;
    }
    devos_wipe(&m, sizeof(m));
    devos_wipe(u, sizeof(u));
    devos_wipe(t, sizeof(t));
}

uint32_t devos_hotp(devos_hash_t h, const uint8_t *secret, size_t secret_len, uint64_t counter, int digits)
{
    uint8_t msg[8], mac[64];
    put_be64(msg, counter);
    devos_hmac(h, secret, secret_len, msg, 8, mac);
    size_t ml = devos_hash_len(h);
    int off = mac[ml - 1] & 0x0f;
    uint32_t bin = (uint32_t)(mac[off] & 0x7f) << 24 | (uint32_t)mac[off + 1] << 16 | (uint32_t)mac[off + 2] << 8 | mac[off + 3];
    devos_wipe(mac, sizeof(mac));
    uint32_t mod = 1;
    if (digits < 6) digits = 6;
    if (digits > 8) digits = 8;
    for (int i = 0; i < digits; i++) mod *= 10;
    return bin % mod;
}

/* ------------------------------------------------------------------ ChaCha20 */
#define QR(a, b, c, d) a += b; d ^= a; d = rol32(d, 16); c += d; b ^= c; b = rol32(b, 12); \
                       a += b; d ^= a; d = rol32(d, 8); c += d; b ^= c; b = rol32(b, 7)

static void chacha_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64])
{
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = le32(key + i * 4);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = le32(nonce + i * 4);
    memcpy(x, s, sizeof(x));
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) put_le32(out + i * 4, x[i] + s[i]);
    devos_wipe(x, sizeof(x));
    devos_wipe(s, sizeof(s));
}

static void chacha_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t ks[64];
    while (len) {
        chacha_block(key, counter++, nonce, ks);
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++) out[i] = in[i] ^ ks[i];
        in += n;
        out += n;
        len -= n;
    }
    devos_wipe(ks, sizeof(ks));
}

/* ------------------------------------------------------------------ Poly1305 (26-bit limbs) */
typedef struct {
    uint32_t r[5], h[5], pad[4];
    uint8_t buf[16];
    size_t fill;
} poly_t;

static void poly_init(poly_t *p, const uint8_t key[32])
{
    memset(p, 0, sizeof(*p));
    p->r[0] = le32(key + 0) & 0x3ffffff;
    p->r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (le32(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 4; i++) p->pad[i] = le32(key + 16 + i * 4);
}

static void poly_blocks(poly_t *p, const uint8_t *m, size_t len, uint32_t hibit)
{
    uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    while (len >= 16) {
        h0 += le32(m + 0) & 0x3ffffff;
        h1 += (le32(m + 3) >> 2) & 0x3ffffff;
        h2 += (le32(m + 6) >> 4) & 0x3ffffff;
        h3 += (le32(m + 9) >> 6) & 0x3ffffff;
        h4 += (le32(m + 12) >> 8) | hibit;
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
        m += 16;
        len -= 16;
    }
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

static void poly_update(poly_t *p, const uint8_t *m, size_t len)
{
    if (p->fill) {
        size_t take = 16 - p->fill < len ? 16 - p->fill : len;
        memcpy(p->buf + p->fill, m, take);
        p->fill += take;
        m += take;
        len -= take;
        if (p->fill < 16) return;
        poly_blocks(p, p->buf, 16, 1u << 24);
        p->fill = 0;
    }
    size_t full = len & ~(size_t)15;
    if (full) poly_blocks(p, m, full, 1u << 24);
    m += full;
    len -= full;
    if (len) {
        memcpy(p->buf, m, len);
        p->fill = len;
    }
}

static void poly_final(poly_t *p, uint8_t tag[16])
{
    if (p->fill) {
        p->buf[p->fill++] = 1;
        memset(p->buf + p->fill, 0, 16 - p->fill);
        poly_blocks(p, p->buf, 16, 0);
    }
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    uint32_t c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;
    /* h - p */
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;             /* all ones if h >= p */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    /* to 32-bit words, + pad */
    uint32_t w0 = h0 | (h1 << 26), w1 = (h1 >> 6) | (h2 << 20), w2 = (h2 >> 12) | (h3 << 14), w3 = (h3 >> 18) | (h4 << 8);
    uint64_t f = (uint64_t)w0 + p->pad[0]; put_le32(tag + 0, (uint32_t)f);
    f = (uint64_t)w1 + p->pad[1] + (f >> 32); put_le32(tag + 4, (uint32_t)f);
    f = (uint64_t)w2 + p->pad[2] + (f >> 32); put_le32(tag + 8, (uint32_t)f);
    f = (uint64_t)w3 + p->pad[3] + (f >> 32); put_le32(tag + 12, (uint32_t)f);
    devos_wipe(p, sizeof(*p));
}

static void aead_tag(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_len, const uint8_t *ct,
                     size_t len, uint8_t tag[16])
{
    uint8_t block[64], zeros[16] = { 0 }, lens[16];
    chacha_block(key, 0, nonce, block);
    poly_t p;
    poly_init(&p, block);
    devos_wipe(block, sizeof(block));
    if (aad_len) poly_update(&p, aad, aad_len);
    if (aad_len % 16) poly_update(&p, zeros, 16 - aad_len % 16);
    if (len) poly_update(&p, ct, len);
    if (len % 16) poly_update(&p, zeros, 16 - len % 16);
    put_le32(lens + 0, (uint32_t)aad_len); put_le32(lens + 4, (uint32_t)((uint64_t)aad_len >> 32));
    put_le32(lens + 8, (uint32_t)len); put_le32(lens + 12, (uint32_t)((uint64_t)len >> 32));
    poly_update(&p, lens, 16);
    poly_final(&p, tag);
}

void devos_chachapoly_seal(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_len,
                           const void *in, size_t len, uint8_t *out, uint8_t tag[16])
{
    chacha_xor(key, 1, nonce, in, out, len);
    aead_tag(key, nonce, aad, aad_len, out, len, tag);
}

bool devos_chachapoly_open(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_len,
                           const void *in, size_t len, const uint8_t tag[16], uint8_t *out)
{
    uint8_t t[16];
    aead_tag(key, nonce, aad, aad_len, in, len, t);
    bool ok = devos_ct_equal(t, tag, 16);
    devos_wipe(t, sizeof(t));
    if (!ok) {
        devos_wipe(out, len);
        return false;
    }
    chacha_xor(key, 1, nonce, in, out, len);
    return true;
}

/* ------------------------------------------------------------------ base32 */
int devos_base32_decode(const char *in, uint8_t *out, size_t cap)
{
    uint32_t buf = 0;
    int bits = 0;
    size_t n = 0;
    for (; *in; in++) {
        char c = (char)toupper((unsigned char)*in);
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= '2' && c <= '7') v = c - '2' + 26;
        else if (c == ' ' || c == '-' || c == '=' || c == '\t') continue;
        else return -1;
        buf = (buf << 5) | (uint32_t)v;
        bits += 5;
        if (bits >= 8) {
            if (n >= cap) return -1;
            out[n++] = (uint8_t)(buf >> (bits - 8));
            bits -= 8;
        }
    }
    return (int)n;
}

size_t devos_base32_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    uint32_t buf = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        buf = (buf << 8) | in[i];
        bits += 8;
        while (bits >= 5 && o + 1 < cap) {
            out[o++] = a[(buf >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0 && o + 1 < cap) out[o++] = a[(buf << (5 - bits)) & 31];
    if (cap) out[o] = '\0';
    return o;
}
