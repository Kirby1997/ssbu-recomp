/* tests/port/window: the Linux window path of display.c + the top of nvn_vk.c on the real device.
 *   test_window [frames|close|sigterm] [n]
 * display_init (SDL window on its own thread, nvn_vk_init through the nvn_gl_start stub, the 60 Hz vsync thread), then n
 * frames (default 120, after 30 not measured) presented exactly as the game presents (display_before_present, present,
 * display_pace, display_wait_refresh); prints frame-interval and vsync-signal p50/p99, window state reads, CPU time.
 * close: a WINDOW_CLOSE_REQUESTED event must reach oe_exit_request -> report_and_exit; sigterm: the same through SIGTERM.
 * Then nvn_vk_shutdown. Exit 0 = all checks passed. SSBU_VK_SELFTEST=<n>: display_init runs nvn_vk.c's own self-test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include "plat.h"
#include "runtime.h"
#include "hostrt.h"
#include <SDL3/SDL.h>
#include "display_sdl.h"

void display_init(void);
void display_pace(void);
int display_wait_refresh(void);
void display_before_present(void);
void register_display_imports(void);
void nvn_vk_present_black(void);
void nvn_vk_shutdown(void);
void nvn_vk_report(FILE *out);
void nvn_render_run(void (*fn)(void *), void *arg);
void *nvn_front_hwnd(void);
int test_wd_iconic(void);
extern LONGLONG g_sig_t[];
extern volatile LONG g_sig_n, g_exit_requests, g_report_exit;
extern const ImportDef *g_vi_defs;
extern int g_vi_count;

static int fails;
#define CHECK(cond, ...) do { int ok_ = (cond); printf("  %s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double ms_of(LONGLONG t) { return (double)t / 1e6; }
static void job_present(void *arg) { (void)arg; nvn_vk_present_black(); }
static double cpu_s(void) { struct rusage u; getrusage(RUSAGE_SELF, &u); return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6; }

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *mode = argc > 1 ? argv[1] : "frames";
    int n = argc > 2 ? atoi(argv[2]) : 120;
    if (n < 10) n = 10;
    printf("test: mode %s, %d frames; WAYLAND_DISPLAY=%s DISPLAY=%s SDL_VIDEODRIVER=%s SSBU_VK_PRESENT=%s SSBU_SHOW=%s\n", mode, n, getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "-",
           getenv("DISPLAY") ? getenv("DISPLAY") : "-", getenv("SDL_VIDEODRIVER") ? getenv("SDL_VIDEODRIVER") : "-", getenv("SSBU_VK_PRESENT") ? getenv("SSBU_VK_PRESENT") : "-",
           getenv("SSBU_SHOW") ? getenv("SSBU_SHOW") : "-");
    register_display_imports();                             /* the vsync event as the game gets it (nn::vi::GetDisplayVsyncEvent) */
    static Ctx c;
    static uint8_t event_obj[64];
    for (int i = 0; i < g_vi_count; i++) if (strstr(g_vi_defs[i].name, "GetDisplayVsyncEvent")) { c.x[0] = (uint64_t)(uintptr_t)event_obj; g_vi_defs[i].fn(&c); }
    CHECK(display_sdl_window() == NULL, "display_sdl_window() is NULL before display_init");
    { int f = -1, m = -1; display_window_state(&f, &m); CHECK(f == 1 && m == 0, "display_window_state before the window: focused %d minimised %d (1, 0)", f, m); }

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    display_init();
    QueryPerformanceCounter(&t1);
    SDL_Window *w = display_sdl_window();
    int pw = 0, ph = 0, f = -1, m = -1;
    display_window_pixels(&pw, &ph);
    display_window_state(&f, &m);
    printf("test: display_init %.1f ms; SDL video driver %s; window %dx%d pixels; focused %d minimised %d\n", ms_of(t1.QuadPart - t0.QuadPart), SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?", pw, ph, f, m);
    CHECK(w != NULL, "display_sdl_window() after display_init");
    CHECK(nvn_front_hwnd() == (void *)w, "nvn_gl_start got the SDL_Window * (nvn_front_hwnd)");
    CHECK(pw > 0 && ph > 0, "window pixel size known (%dx%d)", pw, ph);

    double *dt = malloc(sizeof *dt * (size_t)n), sum = 0;
    int nf = 0, nm = 0, reads = 0, iconic = 0;
    LARGE_INTEGER prev, now, a;
    LONG sig0 = 0;
    double cpu0 = 0;
    for (int i = -30; i < n; i++) {
        if (i == 0) { QueryPerformanceCounter(&a); prev = a; sig0 = g_sig_n; cpu0 = cpu_s(); }
        display_before_present();
        nvn_render_run(job_present, NULL);
        display_pace();
        display_wait_refresh();
        if (i >= 0) { QueryPerformanceCounter(&now); dt[i] = ms_of(now.QuadPart - prev.QuadPart); sum += dt[i]; prev = now; }
        if (i % 10 == 0) { display_window_state(&f, &m); nf += f; nm += m; reads++; iconic += test_wd_iconic(); }
    }
    double cpu = cpu_s() - cpu0;
    LONG sig1 = g_sig_n;
    qsort(dt, (size_t)n, sizeof *dt, cmpd);
    int late = 0;
    for (int i = 0; i < n; i++) late += dt[i] > 25.0;
    printf("test: %d frames in %.3f s = %.2f fps; frame interval ms: min %.2f p50 %.2f p99 %.2f max %.2f; %d over 25 ms; process CPU %.1f%% of one core\n", n, sum / 1000.0, n * 1000.0 / sum, dt[0], dt[n / 2],
           dt[(int)((n - 1) * 0.99)], dt[n - 1], late, cpu * 100000.0 / sum);
    int ns = (int)(sig1 - sig0) - 1;
    if (sig1 > 4096) ns = 0;
    double *sd = malloc(sizeof *sd * (size_t)(ns > 0 ? ns : 1)), worst = 0;
    for (int i = 0; i < ns; i++) { sd[i] = ms_of(g_sig_t[sig0 + i + 1] - g_sig_t[sig0 + i]); double e = sd[i] - 1000.0 / 60.0; if (e < 0) e = -e; if (e > worst) worst = e; }
    if (ns > 0) {
        qsort(sd, (size_t)ns, sizeof *sd, cmpd);
        printf("test: vsync thread: %d signals; interval ms p50 %.3f p99 %.3f min %.3f max %.3f; largest error vs 16.667: %.3f ms\n", ns + 1, sd[ns / 2], sd[(int)((ns - 1) * 0.99)], sd[0], sd[ns - 1], worst);
    }
    printf("test: window state over %d reads: focused %d, minimised %d, wd_iconic %d\n", reads, nf, nm, iconic);
    nvn_vk_report(stdout);
    CHECK(dt[n / 2] > 16.0 && dt[n / 2] < 17.4, "frame interval p50 %.2f ms is one 60 Hz refresh", dt[n / 2]);
    CHECK(dt[(int)((n - 1) * 0.99)] < 25.0, "frame interval p99 %.2f ms < 25 ms", dt[(int)((n - 1) * 0.99)]);
    CHECK(ns > 0 && sd[ns / 2] > 16.5 && sd[ns / 2] < 16.85, "vsync signal p50 %.3f ms", ns > 0 ? sd[ns / 2] : 0.0);
    CHECK(nm == 0 && iconic == 0, "not minimised while presenting");

    if (!strcmp(mode, "close") || !strcmp(mode, "sigterm")) {
        if (!strcmp(mode, "close")) {
            SDL_Event e;
            memset(&e, 0, sizeof e);
            e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED; e.window.windowID = SDL_GetWindowID(w);
            CHECK(SDL_PushEvent(&e), "pushed WINDOW_CLOSE_REQUESTED");
        } else kill(getpid(), SIGTERM);                     /* SDL turns SIGTERM into SDL_EVENT_QUIT */
        for (int k = 0; k < 200 && !g_report_exit; k++) Sleep(10);
        CHECK(g_report_exit && g_exit_requests == 1, "%s reached oe_exit_request once (%d) and report_and_exit", mode, (int)g_exit_requests);
    }
    nvn_vk_shutdown();
    printf("test: %s (%d failed checks)\n", fails ? "FAIL" : "PASS", fails);
    fflush(stdout);
    _exit(fails ? 1 : 0);
}
