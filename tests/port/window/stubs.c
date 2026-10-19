/* tests/port/window: what display.c and nvn_vk.c call in the rest of the runtime, reduced to what the window test needs.
 * nvn_gl_start (nvn_gl.c's render thread) = nvn_vk_init on the calling thread; nvn_render_run runs the job in place;
 * os_signal_event records the time of every vsync signal; report_and_exit (the close path) sets a flag. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "runtime.h"
#include "hostrt.h"
#include "gfx_options.h"

const GfxOptions *gfx_options(void) {
    static GfxOptions o;
    static int init;
    if (!init) { init = 1; o.width = 1280; o.height = 720; o.scale = 1.0f; }     /* graphics.ini defaults: windowed, clock pacing */
    return &o;
}

#define MAX_SIG 4096
LONGLONG g_sig_t[MAX_SIG];
volatile LONG g_sig_n;
void os_init_event(uint64_t key, int autoclear) { (void)key; (void)autoclear; }
void os_signal_event(uint64_t key) {
    (void)key;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    LONG i = InterlockedIncrement(&g_sig_n) - 1;
    if (i < MAX_SIG) g_sig_t[i] = t.QuadPart;
}

int nvn_vk_init(void *hwnd);
void *g_test_hwnd;
void nvn_gl_start(void *hwnd) { g_test_hwnd = hwnd; nvn_vk_init(hwnd); }
void *nvn_front_hwnd(void) { return g_test_hwnd; }
void nvn_gl_report(FILE *out) { (void)out; }
void nvn_render_run(void (*fn)(void *), void *arg) { fn(arg); }
void hitch_init(void) {}
void hitch_pacing(int on) { (void)on; }
void nvn_pf_pace(LONG64 ticks) { (void)ticks; }
void debug_mode_frame(long long frame) { (void)frame; }

volatile LONG g_exit_requests, g_report_exit;
int oe_exit_request(void) { InterlockedIncrement(&g_exit_requests); return 0; }      /* 0: "end now" -> report_and_exit(0) */
void report_and_exit(int code) { printf("test: report_and_exit(%d) called by the close path\n", code); fflush(stdout); InterlockedExchange(&g_report_exit, 1); }

const ImportDef *g_vi_defs;
int g_vi_count;
void register_imports(const ImportDef *defs, int count) { g_vi_defs = defs; g_vi_count = count; }
/* after the upstream 938e66b merge: graphics.ini latency (normal), the kept present and the L1 report of the front end */
int gfx_latency(void) { return 0; }
void nvn_front_post_present_flush(void) { }
unsigned long nvn_front_present_tid(void) { return 0; }
void nvn_l1_report(FILE *out) { (void)out; }
