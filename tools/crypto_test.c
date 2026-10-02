/* Host test for components/devos_crypto against the RFC test vectors.
 *
 *   gcc -O2 -Icomponents/devos_crypto tools/crypto_test.c components/devos_crypto/devos_crypto.c -o /tmp/crypto_test && /tmp/crypto_test
 */
#include "devos_crypto.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int fails;

static void hex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + i * 2, "%02x", b[i]);
    out[n * 2] = '\0';
}

static void check_hex(const char *name, const uint8_t *b, size_t n, const char *want)
{
    char got[300];
    hex(b, n, got);
    char w[300];
    size_t o = 0;
    for (const char *p = want; *p && o < sizeof(w) - 1; p++) if (*p != ' ' && *p != ':') w[o++] = *p;
    w[o] = '\0';
    if (strcmp(got, w)) {
        printf("FAIL %s\n  got  %s\n  want %s\n", name, got, w);
        fails++;
    } else {
        printf("ok   %s\n", name);
    }
}

static void check_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("FAIL %s: got %08u want %08u\n", name, got, want);
        fails++;
    } else {
        printf("ok   %s\n", name);
    }
}

static void unhex(const char *s, uint8_t *out, size_t *n)
{
    size_t o = 0;
    while (*s) {
        while (*s == ' ' || *s == ':') s++;
        if (!s[0] || !s[1]) break;
        unsigned v;
        sscanf(s, "%2x", &v);
        out[o++] = (uint8_t)v;
        s += 2;
    }
    *n = o;
}

int main(void)
{
    uint8_t d[64];
    devos_hash(DEVOS_HASH_SHA1, "abc", 3, d);
    check_hex("SHA-1 abc", d, 20, "a9993e364706816aba3e25717850c26c9cd0d89d");
    devos_hash(DEVOS_HASH_SHA256, "abc", 3, d);
    check_hex("SHA-256 abc", d, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *m56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    devos_hash(DEVOS_HASH_SHA256, m56, strlen(m56), d);
    check_hex("SHA-256 56 bytes", d, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    devos_hash(DEVOS_HASH_SHA1, m56, strlen(m56), d);
    check_hex("SHA-1 56 bytes", d, 20, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    devos_hash(DEVOS_HASH_SHA512, "abc", 3, d);
    check_hex("SHA-512 abc", d, 64,
              "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    const char *m112 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    devos_hash(DEVOS_HASH_SHA512, m112, strlen(m112), d);
    check_hex("SHA-512 112 bytes", d, 64,
              "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");

    uint8_t k20[20];
    memset(k20, 0x0b, 20);
    devos_hmac(DEVOS_HASH_SHA1, k20, 20, "Hi There", 8, d);
    check_hex("HMAC-SHA1 RFC 2202 #1", d, 20, "b617318655057264e28bc0b6fb378c8ef146be00");
    devos_hmac(DEVOS_HASH_SHA256, k20, 20, "Hi There", 8, d);
    check_hex("HMAC-SHA256 RFC 4231 #1", d, 32, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    devos_hmac(DEVOS_HASH_SHA512, k20, 20, "Hi There", 8, d);
    check_hex("HMAC-SHA512 RFC 4231 #1", d, 64,
              "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854");
    uint8_t k131[131];
    memset(k131, 0xaa, sizeof(k131));
    const char *big = "Test Using Larger Than Block-Size Key - Hash Key First";
    devos_hmac(DEVOS_HASH_SHA256, k131, 131, big, strlen(big), d);
    check_hex("HMAC-SHA256 RFC 4231 #6 (long key)", d, 32, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    /* RFC 6238 appendix B */
    const char *s1 = "12345678901234567890", *s256 = "12345678901234567890123456789012",
               *s512 = "1234567890123456789012345678901234567890123456789012345678901234";
    struct { uint64_t t; uint32_t c1, c256, c512; } tv[] = {
        { 59, 94287082, 46119246, 90693936 }, { 1111111109, 7081804, 68084774, 25091201 },
        { 1234567890, 89005924, 91819424, 93441116 }, { 2000000000, 69279037, 90698825, 38618901 },
        { 20000000000ULL, 65353130, 77737706, 47863826 },
    };
    for (unsigned i = 0; i < sizeof(tv) / sizeof(tv[0]); i++) {
        char n[64];
        snprintf(n, sizeof(n), "TOTP SHA1   T=%llu", (unsigned long long)tv[i].t);
        check_u32(n, devos_hotp(DEVOS_HASH_SHA1, (const uint8_t *)s1, 20, tv[i].t / 30, 8), tv[i].c1);
        snprintf(n, sizeof(n), "TOTP SHA256 T=%llu", (unsigned long long)tv[i].t);
        check_u32(n, devos_hotp(DEVOS_HASH_SHA256, (const uint8_t *)s256, 32, tv[i].t / 30, 8), tv[i].c256);
        snprintf(n, sizeof(n), "TOTP SHA512 T=%llu", (unsigned long long)tv[i].t);
        check_u32(n, devos_hotp(DEVOS_HASH_SHA512, (const uint8_t *)s512, 64, tv[i].t / 30, 8), tv[i].c512);
    }

    /* RFC 7914 section 11 */
    uint8_t dk[64];
    devos_pbkdf2_sha256("passwd", 6, (const uint8_t *)"salt", 4, 1, dk, 64);
    check_hex("PBKDF2-SHA256 passwd/salt/1", dk, 64,
              "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
    devos_pbkdf2_sha256("Password", 8, (const uint8_t *)"NaCl", 4, 80000, dk, 64);
    check_hex("PBKDF2-SHA256 Password/NaCl/80000", dk, 64,
              "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d");

    /* RFC 8439 section 2.8.2 */
    uint8_t key[32], nonce[12], aad[12], ct[200], pt[200], tag[16];
    size_t n;
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x80 + i);
    unhex("07000000 40414243 44454647", nonce, &n);
    unhex("50515253c0c1c2c3c4c5c6c7", aad, &n);
    const char *msg = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    size_t ml = strlen(msg);
    devos_chachapoly_seal(key, nonce, aad, 12, msg, ml, ct, tag);
    check_hex("ChaCha20-Poly1305 ciphertext", ct, ml,
              "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116");
    check_hex("ChaCha20-Poly1305 tag", tag, 16, "1ae10b594f09e26a7e902ecbd0600691");
    bool ok = devos_chachapoly_open(key, nonce, aad, 12, ct, ml, tag, pt);
    if (!ok || memcmp(pt, msg, ml)) { printf("FAIL ChaCha20-Poly1305 open\n"); fails++; }
    else printf("ok   ChaCha20-Poly1305 open\n");
    ct[5] ^= 1;
    if (devos_chachapoly_open(key, nonce, aad, 12, ct, ml, tag, pt)) { printf("FAIL tampered ciphertext accepted\n"); fails++; }
    else printf("ok   tampered ciphertext rejected\n");

    uint8_t b[32];
    int bn = devos_base32_decode("JBSWY3DPEHPK3PXP", b, sizeof(b));
    check_hex("base32 decode", b, (size_t)(bn > 0 ? bn : 0), "48656c6c6f21deadbeef");
    char e[40];
    devos_base32_encode(b, (size_t)bn, e, sizeof(e));
    if (strcmp(e, "JBSWY3DPEHPK3PXP")) { printf("FAIL base32 encode: %s\n", e); fails++; }
    else printf("ok   base32 encode\n");
    if (devos_base32_decode("jbsw y3dp-ehpk 3pxp==", b, sizeof(b)) != 10) { printf("FAIL base32 lenient\n"); fails++; }
    else printf("ok   base32 lenient\n");
    if (devos_base32_decode("ABC1", b, sizeof(b)) != -1) { printf("FAIL base32 bad char accepted\n"); fails++; }
    else printf("ok   base32 bad char rejected\n");

    /* base64 / base64url (RFC 4648 section 10) */
    {
        static const struct { const char *in, *b64, *url; } bv[] = {
            { "",       "",         "" },
            { "f",      "Zg==",     "Zg" },
            { "fo",     "Zm8=",     "Zm8" },
            { "foo",    "Zm9v",     "Zm9v" },
            { "foob",   "Zm9vYg==", "Zm9vYg" },
            { "fooba",  "Zm9vYmE=", "Zm9vYmE" },
            { "foobar", "Zm9vYmFy", "Zm9vYmFy" },
        };
        for (unsigned i = 0; i < sizeof(bv) / sizeof(bv[0]); i++) {
            char e[64];
            uint8_t d2[64];
            char name[64];
            devos_base64_encode((const uint8_t *)bv[i].in, strlen(bv[i].in), e, sizeof(e));
            snprintf(name, sizeof(name), "base64 encode \"%s\"", bv[i].in);
            if (strcmp(e, bv[i].b64)) { printf("FAIL %s: got %s want %s\n", name, e, bv[i].b64); fails++; }
            else printf("ok   %s\n", name);
            int dn = devos_base64_decode(bv[i].b64, d2, sizeof(d2));
            d2[dn < 0 ? 0 : dn] = '\0';
            snprintf(name, sizeof(name), "base64 decode \"%s\"", bv[i].b64);
            if (dn != (int)strlen(bv[i].in) || memcmp(d2, bv[i].in, (size_t)dn)) {
                printf("FAIL %s: got %s\n", name, d2);
                fails++;
            } else printf("ok   %s\n", name);

            devos_base64url_encode((const uint8_t *)bv[i].in, strlen(bv[i].in), e, sizeof(e));
            snprintf(name, sizeof(name), "base64url encode \"%s\"", bv[i].in);
            if (strcmp(e, bv[i].url)) { printf("FAIL %s: got %s want %s\n", name, e, bv[i].url); fails++; }
            else printf("ok   %s\n", name);
            dn = devos_base64url_decode(bv[i].url, d2, sizeof(d2));
            d2[dn < 0 ? 0 : dn] = '\0';
            snprintf(name, sizeof(name), "base64url decode \"%s\"", bv[i].url);
            if (dn != (int)strlen(bv[i].in) || memcmp(d2, bv[i].in, (size_t)dn)) { printf("FAIL %s\n", name); fails++; }
            else printf("ok   %s\n", name);
        }
        uint8_t d2[4];
        if (devos_base64_decode("***", d2, sizeof(d2)) != -1) { printf("FAIL base64 bad char accepted\n"); fails++; }
        else printf("ok   base64 bad char rejected\n");
        if (devos_base64_decode("Zm9v", d2, 2) != -1) { printf("FAIL base64 overflow accepted\n"); fails++; }
        else printf("ok   base64 overflow rejected\n");
    }

    /* hex encode (decode is exercised via the separators below) */
    {
        char e[40];
        uint8_t d2[16];
        devos_hex_encode((const uint8_t *)"ABC", 3, e, sizeof(e));
        if (strcmp(e, "414243")) { printf("FAIL hex encode: %s\n", e); fails++; }
        else printf("ok   hex encode\n");
        int dn = devos_hex_decode("41:42-43 44", d2, sizeof(d2));
        if (dn != 4 || memcmp(d2, "ABCD", 4)) { printf("FAIL hex decode separators\n"); fails++; }
        else printf("ok   hex decode separators\n");
        dn = devos_hex_decode("4", d2, sizeof(d2));
        if (dn != -1) { printf("FAIL hex odd length accepted\n"); fails++; }
        else printf("ok   hex odd length rejected\n");
    }

    /* percent-encoding (RFC 3986) */
    {
        char e[80];
        uint8_t d2[80];
        devos_url_encode((const uint8_t *)"a b/c?d=e&f", 11, e, sizeof(e));
        if (strcmp(e, "a%20b%2Fc%3Fd%3De%26f")) { printf("FAIL url encode: %s\n", e); fails++; }
        else printf("ok   url encode\n");
        int dn = devos_url_decode(e, d2, sizeof(d2));
        if (dn != 11 || memcmp(d2, "a b/c?d=e&f", 11)) { printf("FAIL url decode\n"); fails++; }
        else printf("ok   url decode\n");
        dn = devos_url_decode("a+b", d2, sizeof(d2));
        if (dn != 3 || memcmp(d2, "a b", 3)) { printf("FAIL url decode plus\n"); fails++; }
        else printf("ok   url decode plus\n");
        if (devos_url_decode("a%2", d2, sizeof(d2)) != -1) { printf("FAIL url bad escape accepted\n"); fails++; }
        else printf("ok   url bad escape rejected\n");
    }

    /* base58 (Bitcoin alphabet): known vectors */
    {
        static const struct { const char *hex_in, *b58; } bv[] = {
            { "00",                 "1" },
            { "",                   "" },
            { "0000000000000000000000000000000000", "11111111111111111" },
            { "61",                 "2g" },
            { "626262",             "a3gV" },
            { "636363",             "aPEr" },
            { "516b6fcd0f",         "ABnLTmg" },
            { "bf4f89001e670274dd", "3SEo3LWLoPntC" },
            { "572e4794",           "3EFU7m" },
            { "ecac89cad93923c02321", "EJDM8drfXA6uyA" },
            { "10c8511e",           "Rt5zm" },
            { "00000000000000000000", "1111111111" },
        };
        for (unsigned i = 0; i < sizeof(bv) / sizeof(bv[0]); i++) {
            uint8_t raw[64];
            size_t rl = 0;
            for (const char *p = bv[i].hex_in; p[0] && p[1]; p += 2) {
                unsigned v; sscanf(p, "%2x", &v); raw[rl++] = (uint8_t)v;
            }
            char e[128];
            devos_base58_encode(raw, rl, e, sizeof(e));
            char name[80];
            snprintf(name, sizeof(name), "base58 encode \"%s\"", bv[i].hex_in);
            if (strcmp(e, bv[i].b58)) { printf("FAIL %s: got %s want %s\n", name, e, bv[i].b58); fails++; }
            else printf("ok   %s\n", name);
            uint8_t back[64];
            int bn2 = devos_base58_decode(bv[i].b58, back, sizeof(back));
            snprintf(name, sizeof(name), "base58 decode \"%s\"", bv[i].b58);
            if (bn2 != (int)rl || memcmp(back, raw, rl)) { printf("FAIL %s\n", name); fails++; }
            else printf("ok   %s\n", name);
        }
        uint8_t d2[8];
        if (devos_base58_decode("0OIl", d2, sizeof(d2)) != -1) { printf("FAIL base58 bad char accepted\n"); fails++; }
        else printf("ok   base58 bad char rejected\n");
    }

    /* CRC-32 (IEEE): "123456789" -> 0xCBF43926 */
    {
        uint32_t c = devos_crc32("123456789", 9);
        if (c != 0xCBF43926u) { printf("FAIL crc32: got %08x\n", c); fails++; }
        else printf("ok   crc32 \"123456789\"\n");
        if (devos_crc32("", 0) != 0) { printf("FAIL crc32 empty\n"); fails++; }
        else printf("ok   crc32 empty\n");
    }

    /* streaming hash equals the one-shot result, fed in awkward chunk sizes */
    {
        const char *msg = "The quick brown fox jumps over the lazy dog";
        size_t ml = strlen(msg);
        for (size_t chunk = 1; chunk <= 17; chunk *= 2) {
            uint8_t one[32], two[32];
            devos_hash(DEVOS_HASH_SHA256, msg, ml, one);
            union { max_align_t a; char b[512]; } store;
            devos_hash_ctx_t *ctx = devos_hash_begin(DEVOS_HASH_SHA256, (devos_hash_ctx_t *)store.b);
            for (size_t off = 0; off < ml; off += chunk) {
                size_t k = ml - off < chunk ? ml - off : chunk;
                devos_hash_update(ctx, msg + off, k);
            }
            devos_hash_end(ctx, two);
            char name[64];
            snprintf(name, sizeof(name), "stream sha256 chunk=%zu", chunk);
            if (memcmp(one, two, 32)) { printf("FAIL %s\n", name); fails++; }
            else printf("ok   %s\n", name);
        }
    }

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
