/* tests/port/io/stubs.c - the runtime pieces nnaudio.c / nnhid.c / nnpad_sdl.c call, for the stand-alone io tests. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "plat.h"
#include "runtime.h"
#include "hostrt.h"

const DynSym dyn_syms[1];
const int dyn_sym_count = 0;
volatile long io_events_signalled;
uint64_t io_last_event;

void os_init_event(uint64_t key, int autoclear) { (void)key; (void)autoclear; }
void os_signal_event(uint64_t key) { __atomic_add_fetch(&io_events_signalled, 1, __ATOMIC_SEQ_CST); io_last_event = key; }
void register_imports(const ImportDef *defs, int count) { (void)defs; (void)count; }
static const char *g_names[64];
static int g_nnames;
uint64_t hostfn_address(const char *name, ImportFn fn) { (void)fn; if (g_nnames < 64) g_names[g_nnames++] = name; return 0x7400000000ULL + (uint64_t)g_nnames * 16; }
const char *hostfn_current_name(void) { return "?"; }
void trace_printf(const char *fmt, ...) { (void)fmt; }
void guest_trap(const char *what, uint64_t addr) { printf("guest_trap: %s at %llx\n", what, (unsigned long long)addr); exit(3); }
void call_guest(Ctx *c, uint64_t addr) { (void)c; (void)addr; }
int crash_thread_init(void) { return 0; }
int fight_seq_ticks(void) { return 1; }
void nvn_front_shot_step(int step) { (void)step; }
