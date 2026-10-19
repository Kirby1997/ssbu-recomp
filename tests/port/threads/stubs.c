/* stubs.c - what os_threads.c needs from the rest of the runtime, for tests/port/threads.
 * "Guest" addresses are host functions void f(Ctx *) here: call_guest calls them directly. The import table that
 * register_thread_imports passes is kept so the tests can call nn::os handlers by their mangled names. */
#include "plat.h"
#include "runtime.h"
#include "hostrt.h"
#include <stdarg.h>

void call_guest(Ctx *c, uint64_t addr) { ((void (*)(Ctx *))(uintptr_t)addr)(c); }
void guest_trap(const char *what, uint64_t addr) { fprintf(stderr, "guest_trap: %s (at %llx)\n", what, (unsigned long long)addr); abort(); }
void hitch_cur_ctx(Ctx *c) { (void)c; }
long long nvn_front_frame_next(void) { return 0; }
void trace_printf(const char *fmt, ...) { (void)fmt; }
static const ImportDef *g_defs;
static int g_ndefs;
void register_imports(const ImportDef *defs, int count) { g_defs = defs; g_ndefs = count; }
void register_thread_imports(void);
ImportFn tst_import(const char *name) {
    if (!g_defs) register_thread_imports();
    for (int i = 0; i < g_ndefs; i++) if (!strcmp(g_defs[i].name, name)) return g_defs[i].fn;
    fprintf(stderr, "no import %s\n", name);
    abort();
}
/* display.c / nvn_gl.c (after the upstream 938e66b merge): L1 tracing (off), performance cores (nothing), the present thread */
int display_l1_trace(void) { return 0; }
double display_l1_ms(void) { return 0; }
void display_perf_thread(void) { }
unsigned long nvn_front_present_tid(void) { return 0; }
