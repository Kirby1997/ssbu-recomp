/* runtime/sha256.h against FIPS 180-4 / NIST CSRC example vectors, plus every length 0..300 one-shot vs. chunked
 * (reference: a SHA-256 over the 301 digests, computed with Python's hashlib). */
#include <stdint.h>
#include <string.h>
#include "sha256.h"
#include "mt.h"

static void hex(const uint8_t h[32], char out[65]) { for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", h[i]); }
static int is(const uint8_t h[32], const char *want) { char s[65]; hex(h, s); if (strcmp(s, want)) fprintf(stderr, "    got  %s\n    want %s\n", s, want); return strcmp(s, want) == 0; }

int main(void) {
    uint8_t h[32];
    SECTION("FIPS 180 examples: empty, abc, 448-bit (2 blocks), 896-bit (2 blocks), 1,000,000 x 'a'");
    sha256_digest("", 0, h);
    CHECK(is(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    sha256_digest("abc", 3, h);
    CHECK(is(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_digest(m448, strlen(m448), h);
    CHECK(is(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    const char *m896 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    sha256_digest(m896, strlen(m896), h);
    CHECK(is(h, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
    char *a = malloc(1000000); memset(a, 'a', 1000000);
    sha256_digest(a, 1000000, h);
    CHECK(is(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    {   sha256_state s; sha256_init(&s);                           /* the same million in odd-sized pieces */
        for (size_t at = 0, k = 1; at < 1000000; at += k, k = k * 7 % 997 + 1) sha256_update(&s, a + at, at + k > 1000000 ? 1000000 - at : k);
        sha256_final(&s, h);
        CHECK(is(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")); }
    {   uint8_t buf[200]; memcpy(buf + 3, "abc", 3);                 /* unaligned input */
        sha256_digest(buf + 3, 3, h);
        CHECK(is(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")); }
    free(a);
    SECTION("every length 0..300 (padding edges 55/56/63/64/119/120 ...), one-shot == byte-at-a-time == 13-byte pieces");
    {   uint8_t msg[300], d1[32], d2[32], d3[32]; sha256_state all, s; int same = 1;
        for (int i = 0; i < 300; i++) msg[i] = (uint8_t)((i * 7 + 3) & 0xff);
        sha256_init(&all);
        for (size_t n = 0; n <= 300; n++) {
            sha256_digest(msg, n, d1);
            sha256_init(&s); for (size_t i = 0; i < n; i++) sha256_update(&s, msg + i, 1); sha256_final(&s, d2);
            sha256_init(&s); for (size_t i = 0; i < n; i += 13) sha256_update(&s, msg + i, n - i < 13 ? n - i : 13); sha256_final(&s, d3);
            if (memcmp(d1, d2, 32) || memcmp(d1, d3, 32)) { same = 0; fprintf(stderr, "    length %zu: piecewise differs\n", n); }
            sha256_update(&all, d1, 32);
        }
        sha256_final(&all, h);
        CHECK(same);
        CHECK(is(h, "7d917fbd2cf49ddff9ad0a8706bba32d204e92e71d2e369c5a03d6af29278c9f")); }
    return mt_done("test_sha256");
}
