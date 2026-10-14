/* nnmisc_posix.h - portable pieces of nnmisc.c for non-Windows hosts (header-only, all static; included by nnmisc.c under
 * #ifndef _WIN32 and by tests/port/misc): SHA-1 (RFC 3174), MD5 (RFC 1321), random bytes from getrandom(2), and the
 * mapping of a POSIX locale name ($LC_ALL / $LC_MESSAGES / $LANG) to the language codes oe_GetDesiredLanguage returns. */
#ifndef SSBU_NNMISC_POSIX_H
#define SSBU_NNMISC_POSIX_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/random.h>

/* ---------------------------------------------------------------- SHA-1 */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } misc_sha1;
static inline uint32_t misc_rol32(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
static void misc_sha1_block(misc_sha1 *c, const uint8_t *p) {
    uint32_t w[80], a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = misc_rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5a827999u; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ed9eba1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8f1bbcdcu; }
        else { f = b ^ cc ^ d; k = 0xca62c1d6u; }
        uint32_t t = misc_rol32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = misc_rol32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}
static void misc_sha1_init(misc_sha1 *c) {
    c->h[0] = 0x67452301u; c->h[1] = 0xefcdab89u; c->h[2] = 0x98badcfeu; c->h[3] = 0x10325476u; c->h[4] = 0xc3d2e1f0u;
    c->len = 0; c->n = 0;
}
static void misc_sha1_update(misc_sha1 *c, const void *data, size_t len) {
    const uint8_t *p = data;
    c->len += len;
    while (len) {
        size_t k = 64 - c->n < len ? 64 - c->n : len;
        memcpy(c->buf + c->n, p, k); c->n += k; p += k; len -= k;
        if (c->n == 64) { misc_sha1_block(c, c->buf); c->n = 0; }
    }
}
static void misc_sha1_final(misc_sha1 *c, uint8_t out[20]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, z = 0, l[8];
    misc_sha1_update(c, &pad, 1);
    while (c->n != 56) misc_sha1_update(c, &z, 1);
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    misc_sha1_update(c, l, 8);
    for (int i = 0; i < 5; i++) { out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16); out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i]; }
}

/* ---------------------------------------------------------------- MD5 */
typedef struct { uint32_t h[4]; uint64_t len; uint8_t buf[64]; size_t n; } misc_md5;
static void misc_md5_block(misc_md5 *c, const uint8_t *p) {
    static const uint8_t S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    uint32_t m[16], a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    for (int i = 0; i < 16; i++) m[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16) { f = (b & cc) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ cc ^ d; g = (3 * i + 5) & 15; }
        else { f = cc ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t t = d; d = cc; cc = b;
        b = b + misc_rol32(a + f + K[i] + m[g], S[i]);
        a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
}
static void misc_md5_init(misc_md5 *c) { c->h[0] = 0x67452301u; c->h[1] = 0xefcdab89u; c->h[2] = 0x98badcfeu; c->h[3] = 0x10325476u; c->len = 0; c->n = 0; }
static void misc_md5_update(misc_md5 *c, const void *data, size_t len) {
    const uint8_t *p = data;
    c->len += len;
    while (len) {
        size_t k = 64 - c->n < len ? 64 - c->n : len;
        memcpy(c->buf + c->n, p, k); c->n += k; p += k; len -= k;
        if (c->n == 64) { misc_md5_block(c, c->buf); c->n = 0; }
    }
}
/* Finalises a COPY: the guest may call GetHash and then Update again (CryptGetHashParam did not end the hash either). */
static void misc_md5_final(const misc_md5 *src, uint8_t out[16]) {
    misc_md5 t = *src, *c = &t;
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, z = 0, l[8];
    misc_md5_update(c, &pad, 1);
    while (c->n != 56) misc_md5_update(c, &z, 1);
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (8 * i));
    misc_md5_update(c, l, 8);
    for (int i = 0; i < 4; i++) { out[4 * i] = (uint8_t)c->h[i]; out[4 * i + 1] = (uint8_t)(c->h[i] >> 8); out[4 * i + 2] = (uint8_t)(c->h[i] >> 16); out[4 * i + 3] = (uint8_t)(c->h[i] >> 24); }
}

/* ---------------------------------------------------------------- random */
/* 1 when n bytes came from getrandom(2) (GRND_NONBLOCK is not used: the pool is initialised long before a game runs). */
static int misc_getrandom(void *p, size_t n) {
    uint8_t *b = p;
    while (n) {
        ssize_t r = getrandom(b, n, 0);
        if (r < 0) { if (errno == EINTR) continue; return 0; }
        b += r; n -= (size_t)r;
    }
    return 1;
}

/* ---------------------------------------------------------------- locale
 * Same mapping as the Windows branch of desired_language: `name` is a BCP-47-like tag ("en-GB", "zh-Hant-TW", "es-MX").
 * misc_locale_to_tag turns a POSIX locale name ("de_DE.UTF-8@euro", "C", "") into such a tag; "C" / "POSIX" / "" -> "" (-> en-US). */
static void misc_locale_to_tag(const char *loc, char *out, size_t cap) {
    size_t n = 0;
    if (!loc || !loc[0] || !strcmp(loc, "C") || !strcmp(loc, "POSIX") || !strncmp(loc, "C.", 2)) { out[0] = 0; return; }
    for (; loc[n] && loc[n] != '.' && loc[n] != '@' && n + 1 < cap; n++) out[n] = loc[n] == '_' ? '-' : loc[n];
    out[n] = 0;
}
static void misc_map_language(const char *name, char pick[16]) {
    static const char *const supported[] = {"ja", "en-US", "en-GB", "fr", "fr-CA", "de", "it", "es", "es-419", "zh-Hans", "zh-Hant", "ko", "nl", "ru"};
    const size_t ns = sizeof supported / sizeof supported[0];
    int exact = 0;
    strcpy(pick, "en-US");
    for (size_t i = 0; i < ns; i++) if (!strcasecmp(name, supported[i])) { strcpy(pick, supported[i]); exact = 1; }
    if (exact) return;
    if (!strncasecmp(name, "zh", 2)) strcpy(pick, strstr(name, "TW") || strstr(name, "HK") || strstr(name, "MO") || strstr(name, "Hant") ? "zh-Hant" : "zh-Hans");
    else if (!strncasecmp(name, "en", 2)) strcpy(pick, !strcasecmp(name, "en-US") || !strcasecmp(name, "en-CA") ? "en-US" : "en-GB");
    else if (!strncasecmp(name, "fr", 2)) strcpy(pick, !strcasecmp(name, "fr-CA") ? "fr-CA" : "fr");
    else if (!strncasecmp(name, "es", 2)) strcpy(pick, !strcasecmp(name, "es-ES") || !strcasecmp(name, "es") ? "es" : "es-419");
    else if (name[0] && name[1]) {
        char two[3] = {name[0], name[1], 0};
        for (size_t i = 0; i < ns; i++) if (!strcasecmp(two, supported[i])) strcpy(pick, supported[i]);
    }
}
/* The system language of a Linux user: $LC_ALL, else $LC_MESSAGES, else $LANG (the order setlocale uses for messages). */
static const char *misc_env_locale(void) {
    const char *v;
    if ((v = getenv("LC_ALL")) && v[0]) return v;
    if ((v = getenv("LC_MESSAGES")) && v[0]) return v;
    if ((v = getenv("LANG")) && v[0]) return v;
    return "";
}
#endif
