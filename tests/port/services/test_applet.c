/* test_applet.c - nnapplet.c on Linux: nn::swkbd::ShowKeyboard answers (SSBU_SWKBD=cancel / text:<utf-8> / default), UTF-16 <-> UTF-32
 * handling of the guest's char16 text (surrogate pairs), the UTF-8 result form, buffer truncation, MessageBoxW stand-in.
 * nnapplet.c is compiled INTO this test. */
#include "nnapplet.c"
#include "stubs.h"
ImportFn import_lookup(const char *name) { (void)name; return NULL; }
void call_guest(Ctx *c, uint64_t addr) { (void)c; (void)addr; abort(); }
void report_and_exit(int code) { exit(code); }
void trace_printf(const char *fmt, ...) { (void)fmt; }

static Ctx C;
static uint8_t arg[0x600];
static uint8_t outbuf[256];
static uint64_t string[2];
static uint32_t show(const char *env, int utf8, uint64_t cap) {
    if (env) setenv("SSBU_SWKBD", env, 1); else unsetenv("SSBU_SWKBD");
    unsetenv("SSBU_SEQ"); unsetenv("SSBU_PRESS"); unsetenv("SSBU_SCRIPT");
    memset(arg, 0, sizeof arg); memset(outbuf, 0xee, sizeof outbuf);
    arg[KC_UTF8] = (uint8_t)utf8;
    *(uint32_t *)(arg + KC_MAXLEN) = 100;
    string[0] = (uint64_t)(uintptr_t)outbuf; string[1] = cap;
    memset(&C, 0, sizeof C);
    C.x[0] = (uint64_t)(uintptr_t)string; C.x[1] = (uint64_t)(uintptr_t)arg;
    swkbd_ShowKeyboard(&C);
    return (uint32_t)C.x[0];
}
int main(void) {
    SECTION("SSBU_SWKBD=cancel and the default on Linux (no keyboard yet): 0x29f");
    CHECK(show("cancel", 0, 128) == SWKBD_RESULT_CANCELED && outbuf[0] == 0xee);
    CHECK(show(NULL, 0, 128) == SWKBD_RESULT_CANCELED);
    CHECK(show("dialog", 0, 128) == SWKBD_RESULT_CANCELED);
    CHECK(show("bogus", 0, 128) == SWKBD_RESULT_CANCELED);
    SECTION("SSBU_SWKBD=text:<utf-8> -> UTF-16 in the caller's String (BMP, surrogate pair)");
    CHECK(show("text:Kirby", 0, 128) == 0 && *(uint16_t *)outbuf == 'K' && *(uint16_t *)(outbuf + 8) == 'y' && *(uint16_t *)(outbuf + 10) == 0);
    CHECK(show("text:A\xf0\x9f\x98\x80\xc3\xa9", 0, 128) == 0);             /* A, U+1F600, e-acute */
    { uint16_t *u = (uint16_t *)outbuf; CHECKF(u[0] == 'A' && u[1] == 0xd83d && u[2] == 0xde00 && u[3] == 0xe9 && u[4] == 0, "%04x %04x %04x %04x %04x", u[0], u[1], u[2], u[3], u[4]); }
    SECTION("UTF-8 result form (isUseUtf8) and truncation on character boundaries");
    CHECK(show("text:A\xf0\x9f\x98\x80\xc3\xa9", 1, 128) == 0 && !strcmp((char *)outbuf, "A\xf0\x9f\x98\x80\xc3\xa9"));
    CHECK(show("text:Kirby", 0, 8) == 0 && *(uint16_t *)(outbuf + 6) == 0 && *(uint16_t *)(outbuf + 4) == 'r' && outbuf[8] == 0xee);        /* 3 chars + NUL in 8 bytes */
    CHECK(show("text:A\xf0\x9f\x98\x80", 0, 6) == 0 && *(uint16_t *)outbuf == 'A' && *(uint16_t *)(outbuf + 2) == 0);                      /* pair does not fit: dropped whole */
    CHECK(show("text:A\xf0\x9f\x98\x80", 0, 8) == 0 && *(uint16_t *)(outbuf + 2) == 0xd83d && *(uint16_t *)(outbuf + 4) == 0xde00 && *(uint16_t *)(outbuf + 6) == 0);
    SECTION("copy_w: guest char16 text with a surrogate pair -> one wide character");
    { uint16_t g[] = {'x', 0xd83d, 0xde00, 'y', 0}; wchar_t w[8]; copy_w(w, (uint64_t)(uintptr_t)g, 8); CHECK(w[0] == 'x' && w[1] == 0x1f600 && w[2] == 'y' && w[3] == 0);
      uint16_t lone[] = {0xd83d, 'z', 0}; copy_w(w, (uint64_t)(uintptr_t)lone, 8); CHECK(w[0] == 0xd83d && w[1] == 'z'); }
    SECTION("MessageBoxW stand-in: message to stderr, a question is answered no");
    CHECK(MessageBoxW(NULL, L"Really?", L"Software keyboard", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDNO);
    CHECK(MessageBoxW(NULL, L"Too short", L"Software keyboard", MB_OK | MB_ICONWARNING) == IDOK);
    return mt_done("nnapplet");
}
