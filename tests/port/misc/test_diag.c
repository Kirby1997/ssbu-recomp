/* tests/port/misc/test_diag.c - the diagnostics on Linux, standalone: the freeze watchdog (nvn_vk_wd.c, included here with
 * stubbed renderer state), the hitch log's Linux helpers (hitch.c), debug_mode.c's safe_read. */
#include "tp.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include "runtime.h"
#include "hostrt.h"
long long nvn_front_frame_next(void) { return 0; }

/* ---- stubs for what nvn_vk_draw.c / nvn_vk_m4.c / os_threads.c provide */
struct { int alive; } V;
static DWORD g_render_tid; static int g_pw_nthread; static HANDLE g_pw_thread[8], g_prep_thread;
static LONG64 g_vk_frames, g_vk_skipped, g_vk_errors, g_vk_recreated, g_jq_queued, g_jq_done, g_bq_queued, g_bq_done, g_pq_queued, g_pq_done, g_pq_running, g_bar_n, g_bar_timeout;
static int g_wd_where, g_in_fight, g_prep_state, g_prep_marked, g_prep_read, g_prep_nf, g_prep_nfiles, g_offline;
static int fight_recent(void) { return 0; }
static CRITICAL_SECTION g_sh_lock, g_gpl_lock;
static LONG64 tk(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
typedef struct { volatile LONG tid; const char *fn; int line; LONG64 since; } WdWait;
static WdWait g_wd_wait[64];
static volatile LONG64 g_wd_beat, g_wd_try;

const char *guest_thread_name_of(DWORD tid) { (void)tid; return NULL; }
/* after the upstream 938e66b merge: the hold counters (nvn_vk_m4.c), the window state (display.c), the present thread (the
 * front end), the report path and the minidump (hostrt.c, Linux: next to the exe, no dumps) */
enum { H_N = 4 };
static LONG64 g_hold_n[H_N];
void display_window_state(int *focused, int *minimised) { *focused = 1; *minimised = 0; }
unsigned long nvn_front_present_tid(void) { return 0; }
int diag_report_path(char *out, size_t size, const char *suffix) {
    char exe[400];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    int k = n ? snprintf(out, size, "%s%s", exe, suffix) : 0;
    return k > 0 && (size_t)k < size;
}
int diag_minidump(void *ep, const char *why) { (void)ep; (void)why; return 0; }
#include "nvn_vk_posix.h"          /* the nvn_vk TU's POSIX shims (the stall log's GetFileAttributesExA) */
#include "nvn_vk_wd.c"

static char g_freeze[512];
static int freeze_has(const char *needle) {
    FILE *f = fopen(g_freeze, "rb"); static char buf[1 << 16]; size_t n;
    if (!f) return 0;
    n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    return strstr(buf, needle) != NULL;
}
static volatile int g_beat_run;
static DWORD WINAPI beater(LPVOID a) { (void)a; while (g_beat_run) { g_wd_beat = tk(); g_wd_try++; Sleep(100); } return 0; }
static DWORD WINAPI idler(LPVOID a) { (void)a; while (g_beat_run) Sleep(20); return 0; }

int main(int argc, char **argv) {
    char exe[400]; (void)argc; (void)argv;
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe); CHECK(n > 0);
    snprintf(g_freeze, sizeof g_freeze, "%s_freeze.txt", exe);
    unlink(g_freeze);
    setenv("SSBU_VK_WATCHDOG", "1", 1);               /* limit 1 s so the test is short */
    V.alive = 1;
    SECTION("watchdog: frames arrive every 100 ms for 5 s -> no report");
    g_beat_run = 1;
    HANDLE b = CreateThread(NULL, 0, beater, NULL, 0, NULL), id = CreateThread(NULL, 0, idler, NULL, 0, NULL);
    wd_start();
    Sleep(5000);
    CHECK(!freeze_has("NO FRAME PRESENTED"));
    CHECK(access(g_freeze, F_OK) != 0);
    SECTION("watchdog: frames stop for ~2.7 s -> one report with a thread list, then 'frames reach the window again'");
    g_beat_run = 0; WaitForSingleObject(b, 2000);
    g_beat_run = 1; id = CreateThread(NULL, 0, idler, NULL, 0, NULL);       /* keep one live thread to list */
    Sleep(2700);
    CHECK(freeze_has("NO FRAME PRESENTED FOR"));
    CHECK(freeze_has("threads: "));
    CHECK(freeze_has("lock g_sh_lock"));
    SECTION("watchdog: minimised window never reported on Linux (wd_iconic stub is 0)");
    CHECK(wd_iconic() == 0);
    g_beat_run = 1; b = CreateThread(NULL, 0, beater, NULL, 0, NULL);
    Sleep(1500);
    wd_stop();
    g_beat_run = 0; (void)id;
    printf("--- freeze file ---\n"); { char cmd[600]; snprintf(cmd, sizeof cmd, "cat '%s'", g_freeze); if (system(cmd)) {} }
    unlink(g_freeze);

    SECTION("debug_mode.c safe_read idiom: process_vm_readv on own memory");
    {   int x = 12345, y = 0; struct iovec l = {&y, sizeof y}, r = {&x, sizeof x};
        CHECK(process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)sizeof x && y == 12345);
        struct iovec r2 = {(void *)0x1000, 4}; errno = 0;
        CHECK(process_vm_readv(getpid(), &l, 1, &r2, 1, 0) < 0 && errno == EFAULT);
    }
    return tp_done("test_diag");
}
