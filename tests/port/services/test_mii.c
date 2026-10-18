/* test_mii.c - nnmii.c on Linux: GetRegionCode from SSBU_REGION / $LC_ALL / $LC_MESSAGES / $LANG. nnmii.c is compiled INTO this test. */
#include "nnmii.c"
#include "stubs.h"
static int trapped;
static int region(const char *region_env, const char *lc_all, const char *lc_msg, const char *lang, char *iso) {
    const char *e[4][2] = {{"SSBU_REGION", region_env}, {"LC_ALL", lc_all}, {"LC_MESSAGES", lc_msg}, {"LANG", lang}};
    for (int i = 0; i < 4; i++) { if (e[i][1]) setenv(e[i][0], e[i][1], 1); else unsetenv(e[i][0]); }
    return host_region_code(iso);
}
int main(void) {
    char iso[4];
    SECTION("locale territory -> region code");
    CHECK(region(NULL, NULL, NULL, "en_GB.UTF-8", iso) == 2 && !strcmp(iso, "GB"));
    CHECK(region(NULL, NULL, NULL, "en_US.UTF-8", iso) == 1 && !strcmp(iso, "US"));
    CHECK(region(NULL, NULL, NULL, "ja_JP.UTF-8", iso) == 0);
    CHECK(region(NULL, NULL, NULL, "en_AU", iso) == 3);
    CHECK(region(NULL, NULL, NULL, "ko_KR.eucKR", iso) == 4);
    CHECK(region(NULL, NULL, NULL, "zh_CN.UTF-8", iso) == 5);
    CHECK(region(NULL, NULL, NULL, "de_DE@euro", iso) == 2);
    CHECK(region(NULL, "fr_CA.UTF-8", NULL, "en_GB.UTF-8", iso) == 1);          /* LC_ALL wins */
    CHECK(region(NULL, NULL, "es_MX.UTF-8", "en_GB.UTF-8", iso) == 1);          /* then LC_MESSAGES */
    SECTION("SSBU_REGION overrides; unknown stays unknown (the game stops, as on Windows)");
    CHECK(region("3", NULL, NULL, "en_GB.UTF-8", iso) == 3);
    CHECK(region("9", NULL, NULL, "en_GB.UTF-8", iso) == 2);                   /* invalid override ignored */
    CHECK(region(NULL, NULL, NULL, "C", iso) == -1 && !strcmp(iso, "??"));
    CHECK(region(NULL, NULL, NULL, "POSIX", iso) == -1);
    CHECK(region(NULL, NULL, NULL, "en", iso) == -1);
    CHECK(region(NULL, NULL, NULL, NULL, iso) == -1);
    CHECK(region(NULL, NULL, NULL, "xx_ZZ.UTF-8", iso) == -1 && !strcmp(iso, "ZZ"));
    CHECK(region("4", NULL, NULL, "C", iso) == 4);
    return mt_done("nnmii");
}
