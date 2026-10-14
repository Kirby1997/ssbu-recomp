/* tests/port/misc/test_misc.c - nnmisc_posix.h: SHA-1 (RFC 3174) / MD5 (RFC 1321) vectors, getrandom, locale mapping. */
#include "tp.h"
#include <stdarg.h>
#include "nnmisc_posix.h"

static void hex(const uint8_t *d, size_t n, char *out) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]); }
static void sha1_check(const char *msg, size_t rep, const char *want) {
    misc_sha1 h; uint8_t d[20]; char s[48];
    misc_sha1_init(&h);
    for (size_t i = 0; i < rep; i++) misc_sha1_update(&h, msg, strlen(msg));
    misc_sha1_final(&h, d); hex(d, 20, s);
    CHECKF(!strcmp(s, want), "sha1(\"%.20s\" x%zu) = %s, want %s", msg, rep, s, want);
}
static void md5_check(const char *msg, size_t rep, const char *want) {
    misc_md5 h; uint8_t d[16]; char s[48];
    misc_md5_init(&h);
    for (size_t i = 0; i < rep; i++) misc_md5_update(&h, msg, strlen(msg));
    misc_md5_final(&h, d); hex(d, 16, s);
    CHECKF(!strcmp(s, want), "md5(\"%.20s\" x%zu) = %s, want %s", msg, rep, s, want);
}
static void lang(const char *loc, const char *want) {
    char tag[128], pick[16];
    misc_locale_to_tag(loc, tag, sizeof tag);
    misc_map_language(tag, pick);
    CHECKF(!strcmp(pick, want), "locale \"%s\" -> tag \"%s\" -> \"%s\", want \"%s\"", loc, tag, pick, want);
}

int main(void) {
    SECTION("SHA-1, RFC 3174 test vectors + FIPS 180 vectors");
    sha1_check("abc", 1, "a9993e364706816aba3e25717850c26c9cd0d89d");
    sha1_check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    sha1_check("a", 1000000, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    sha1_check("0123456701234567012345670123456701234567012345670123456701234567", 10, "dea356a2cddd90c7a7ecedc5ebb563934f460452");
    sha1_check("", 1, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    SECTION("SHA-1 incremental == one shot (all split points of a 200-byte message)");
    {   uint8_t m[200], want[20], got[20];
        for (int i = 0; i < 200; i++) m[i] = (uint8_t)(i * 7 + 3);
        misc_sha1 h; misc_sha1_init(&h); misc_sha1_update(&h, m, 200); misc_sha1_final(&h, want);
        for (int sp = 0; sp <= 200; sp++) { misc_sha1_init(&h); misc_sha1_update(&h, m, sp); misc_sha1_update(&h, m + sp, 200 - sp); misc_sha1_final(&h, got); CHECKF(!memcmp(want, got, 20), "split %d", sp); }
    }
    SECTION("MD5, RFC 1321 test suite");
    md5_check("", 1, "d41d8cd98f00b204e9800998ecf8427e");
    md5_check("a", 1, "0cc175b9c0f1b6a831c399e269772661");
    md5_check("abc", 1, "900150983cd24fb0d6963f7d28e17f72");
    md5_check("message digest", 1, "f96b697d7cb7938d525a2f31aaf161d0");
    md5_check("abcdefghijklmnopqrstuvwxyz", 1, "c3fcd3d76192e4007dfb496cca67e13b");
    md5_check("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 1, "d174ab98d277d9f5a5611c2c9f419d9f");
    md5_check("1234567890", 8, "57edf4a22be3c955ac49da2e2107b67a");
    SECTION("MD5: GetHash does not end the hash (update after final continues)");
    {   misc_md5 h; uint8_t d[16], e[16]; char s[40];
        misc_md5_init(&h); misc_md5_update(&h, "a", 1); misc_md5_final(&h, d); misc_md5_update(&h, "bc", 2); misc_md5_final(&h, e); hex(e, 16, s);
        CHECK(!strcmp(s, "900150983cd24fb0d6963f7d28e17f72"));
        hex(d, 16, s); CHECK(!strcmp(s, "0cc175b9c0f1b6a831c399e269772661"));
    }
    SECTION("getrandom");
    {   uint8_t a[4096] = {0}, b[4096] = {0}; int zeros = 0, same = 0;
        CHECK(misc_getrandom(a, sizeof a)); CHECK(misc_getrandom(b, sizeof b));
        for (int i = 0; i < 4096; i++) { zeros += a[i] == 0; same += a[i] == b[i]; }
        CHECKF(zeros < 60, "%d zero bytes in 4096 (expect ~16)", zeros);
        CHECKF(same < 60, "%d equal bytes between two draws (expect ~16)", same);
        CHECK(misc_getrandom(a, 0));
        uint8_t big[1 << 20]; CHECK(misc_getrandom(big, sizeof big));      /* > 256 B per call limit of GRND_RANDOM-less getrandom is 33 MB; loop handles short reads */
        /* UUID bits as util_GenerateUuid sets them */
        uint8_t u[16]; misc_getrandom(u, 16); u[6] = (uint8_t)((u[6] & 0x0f) | 0x40); u[8] = (uint8_t)((u[8] & 0x3f) | 0x80);
        CHECK((u[6] >> 4) == 4 && (u[8] >> 6) == 2);
    }
    SECTION("locale mapping ($LANG values -> oe language codes)");
    lang("en_US.UTF-8", "en-US"); lang("en_GB.UTF-8", "en-GB"); lang("en_AU.UTF-8", "en-GB"); lang("en_CA.UTF-8", "en-US");
    lang("de_DE.UTF-8", "de"); lang("de_DE@euro", "de"); lang("fr_FR.UTF-8", "fr"); lang("fr_CA.UTF-8", "fr-CA"); lang("fr_BE", "fr");
    lang("es_ES.UTF-8", "es"); lang("es_MX.UTF-8", "es-419"); lang("es_AR", "es-419"); lang("it_IT.UTF-8", "it"); lang("ja_JP.UTF-8", "ja");
    lang("ko_KR.UTF-8", "ko"); lang("nl_NL.UTF-8", "nl"); lang("ru_RU.UTF-8", "ru");
    lang("zh_CN.UTF-8", "zh-Hans"); lang("zh_TW.UTF-8", "zh-Hant"); lang("zh_HK.UTF-8", "zh-Hant"); lang("zh_SG", "zh-Hans");
    lang("pt_BR.UTF-8", "en-US"); lang("C", "en-US"); lang("POSIX", "en-US"); lang("C.UTF-8", "en-US"); lang("", "en-US"); lang("x", "en-US");
    SECTION("locale env precedence: LC_ALL > LC_MESSAGES > LANG");
    setenv("LANG", "de_DE.UTF-8", 1); unsetenv("LC_ALL"); unsetenv("LC_MESSAGES");
    CHECK(!strcmp(misc_env_locale(), "de_DE.UTF-8"));
    setenv("LC_MESSAGES", "ja_JP.UTF-8", 1); CHECK(!strcmp(misc_env_locale(), "ja_JP.UTF-8"));
    setenv("LC_ALL", "ko_KR.UTF-8", 1); CHECK(!strcmp(misc_env_locale(), "ko_KR.UTF-8"));
    setenv("LC_ALL", "", 1); CHECK(!strcmp(misc_env_locale(), "ja_JP.UTF-8"));
    unsetenv("LC_ALL"); unsetenv("LC_MESSAGES"); unsetenv("LANG"); CHECK(!strcmp(misc_env_locale(), ""));
    return tp_done("test_misc");
}
