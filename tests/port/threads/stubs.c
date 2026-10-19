/* stubs.c - what os_threads.c needs from the rest of the runtime, for tests/port/threads.
 * "Guest" addresses are host functions void f(Ctx *) here: call_guest calls them directly. Every import table passed to
 * register_imports is kept (a later registration of a name wins, as hostrt.c's replace), so the tests can call the
 * nn::os handlers (and, in test_cxxrt, the C++ runtime's) by their mangled names. */
#include "plat.h"
#include "runtime.h"
#include "hostrt.h"
#include <stdarg.h>

void call_guest(Ctx *c, uint64_t addr) { ((void (*)(Ctx *))(uintptr_t)addr)(c); }
void guest_trap(const char *what, uint64_t addr) { fprintf(stderr, "guest_trap: %s (at %llx)\n", what, (unsigned long long)addr); abort(); }
void hitch_cur_ctx(Ctx *c) { (void)c; }
long long nvn_front_frame_next(void) { return 0; }
void trace_printf(const char *fmt, ...) { (void)fmt; }
static struct { const ImportDef *defs; int count; } g_tables[4096];
static int g_ntables;
static pthread_mutex_t g_tables_mu = PTHREAD_MUTEX_INITIALIZER;
void register_imports(const ImportDef *defs, int count) {
    pthread_mutex_lock(&g_tables_mu);
    if (g_ntables == 4096) { fprintf(stderr, "stubs.c: too many import tables\n"); abort(); }
    g_tables[g_ntables].defs = defs;
    g_tables[g_ntables].count = count;
    g_ntables++;
    pthread_mutex_unlock(&g_tables_mu);
}
ImportFn import_lookup(const char *name) {
    ImportFn fn = NULL;
    pthread_mutex_lock(&g_tables_mu);
    for (int t = g_ntables - 1; t >= 0 && !fn; t--)
        for (int i = 0; i < g_tables[t].count; i++) if (!strcmp(g_tables[t].defs[i].name, name)) { fn = g_tables[t].defs[i].fn; break; }
    pthread_mutex_unlock(&g_tables_mu);
    return fn;
}
void register_thread_imports(void);
ImportFn tst_import(const char *name) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, register_thread_imports);
    ImportFn fn = import_lookup(name);
    if (fn) return fn;
    fprintf(stderr, "no import %s\n", name);
    abort();
}
/* display.c / nvn_gl.c (after the upstream 938e66b merge): L1 tracing (off), performance cores (nothing), the present thread */
int display_l1_trace(void) { return 0; }
double display_l1_ms(void) { return 0; }
void display_perf_thread(void) { }
unsigned long nvn_front_present_tid(void) { return 0; }
