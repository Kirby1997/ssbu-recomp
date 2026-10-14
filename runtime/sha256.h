/* sha256.h - SHA-256 (FIPS 180-4), portable C, header-only (every function static). Used where Windows had CNG
 * (bcrypt.dll): nnro.c's NRR check of every loaded module (correctness: a wrong hash refuses the module).
 * Byte order independent (big-endian loads/stores by hand); no alignment requirement on the input.
 *   uint8_t h[32]; sha256_digest(data, size, h);
 *   or sha256_init(&s); sha256_update(&s, p, n) ...; sha256_final(&s, h);
 * Tests: tests/port/modules/test_sha256.c (FIPS 180 / NIST CSRC example vectors). */
#ifndef SSBU_SHA256_H
#define SSBU_SHA256_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct { uint32_t h[8]; uint64_t bytes; uint8_t buf[64]; size_t used; } sha256_state;

static const uint32_t sha256_k_[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

#define SHA256_ROR_(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block_(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = SHA256_ROR_(w[i - 15], 7) ^ SHA256_ROR_(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = SHA256_ROR_(w[i - 2], 17) ^ SHA256_ROR_(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (SHA256_ROR_(e, 6) ^ SHA256_ROR_(e, 11) ^ SHA256_ROR_(e, 25)) + ((e & f) ^ (~e & g)) + sha256_k_[i] + w[i];
        uint32_t t2 = (SHA256_ROR_(a, 2) ^ SHA256_ROR_(a, 13) ^ SHA256_ROR_(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
#undef SHA256_ROR_

static void sha256_init(sha256_state *s) {
    static const uint32_t iv[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memcpy(s->h, iv, sizeof iv);
    s->bytes = 0; s->used = 0;
}
static void sha256_update(sha256_state *s, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    s->bytes += n;
    if (s->used) {
        size_t take = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->buf + s->used, p, take); s->used += take; p += take; n -= take;
        if (s->used < 64) return;
        sha256_block_(s->h, s->buf); s->used = 0;
    }
    for (; n >= 64; p += 64, n -= 64) sha256_block_(s->h, p);
    memcpy(s->buf, p, n); s->used = n;
}
static void sha256_final(sha256_state *s, uint8_t out[32]) {
    uint64_t bits = s->bytes * 8;
    s->buf[s->used++] = 0x80;
    if (s->used > 56) { memset(s->buf + s->used, 0, 64 - s->used); sha256_block_(s->h, s->buf); s->used = 0; }
    memset(s->buf + s->used, 0, 56 - s->used);
    for (int i = 0; i < 8; i++) s->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_block_(s->h, s->buf);
    for (int i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16); out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i]; }
}
static void sha256_digest(const void *data, size_t n, uint8_t out[32]) {
    sha256_state s;
    sha256_init(&s); sha256_update(&s, data, n); sha256_final(&s, out);
}
#endif /* SSBU_SHA256_H */
