/* fiber.h - user-mode context switch for aarch64 Linux (fiber_aarch64.S), the base of the Win32 fiber shims in
 * plat.h (CreateFiberEx / ConvertThreadToFiber / SwitchToFiber / DeleteFiber / GetCurrentFiber / GetFiberData).
 *
 * What a switch saves: x19-x28, x29 (fp), x30 (lr), sp, d8-d15 (low 64 bits of v8-v15: all AAPCS64 asks), and FPCR.
 *   - FPCR is per fiber: saved on every switch, written on resume only if it differs (an msr is the costly part).
 *     The runtime never changes FPCR today; if the lifter ever maps guest FPCR writes to the host register, the
 *     guest's rounding / flush-to-zero mode follows the guest fiber instead of leaking into whatever runs next on
 *     that host thread. FPSR (cumulative exception flags) is not saved: nothing reads it.
 *   - x18 is not saved: on Linux it is an ordinary temporary (no shadow call stack in this build).
 *   - No SVE/SME state: callee-saved state is only d8-d15 under AAPCS64 for non-SVE-ABI functions.
 * BTI/PAC: the assembly has `bti c` landing pads (as hint #34, a NOP on older cores), never signs LR itself (the
 * switch is a leaf: callers' own paciasp/autiasp pairs stay balanced because sp is restored with lr), and the object
 * carries the GNU property note for BTI + PAC so -mbranch-protection=standard / -z force-bti links keep BTI on.
 *
 * TLS HAZARD (fiber migration). A fiber suspended on thread A may be resumed by thread B. Compilers treat the thread
 * pointer (tpidr_el0) as constant inside one function, so in
 *       t_x = 1; SwitchToFiber(f); t_x = 2;        // or a loop around call_guest()
 * the second store may go to A's t_x. Any function that can switch fibers (directly or through guest code) must not
 * touch _Thread_local variables itself after the switch point: use noinline accessors (PLAT_TLS_ACCESSORS below),
 * or move the TLS access into a separate noinline function. The plat.h functions (GetCurrentThreadId, GetLastError,
 * Enter/LeaveCriticalSection, the fiber shims) are all out of line and safe. Also: a CRITICAL_SECTION entered on A
 * and left on B works; an SRWLOCK must not be held across a switch (see plat.h); errno after a switch is B's.
 */
#ifndef SSBU_FIBER_H
#define SSBU_FIBER_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct plat_fiber plat_fiber;
typedef void (*plat_fiber_fn)(void *arg);

/* Saved register file; offsets are used by fiber_aarch64.S (keep in sync). */
enum { PLAT_FIBER_X19 = 0, PLAT_FIBER_FP = 10, PLAT_FIBER_LR = 11, PLAT_FIBER_SP = 12, PLAT_FIBER_D8 = 13,
       PLAT_FIBER_FPCR = 21, PLAT_FIBER_NREGS = 22 };

struct plat_fiber {
    uint64_t regs[PLAT_FIBER_NREGS];   /* must stay first */
    plat_fiber_fn fn;
    void *arg;                         /* GetFiberData */
    void *stack_map;                   /* mmap base (guard page included), NULL for a converted thread */
    size_t stack_map_size;
    int is_thread;                     /* made by plat_fiber_convert_thread */
    void *tsan;                        /* ThreadSanitizer fiber, when built with -fsanitize=thread */
    struct plat_fls *fls;              /* fiber-local storage values (plat_posix.c), NULL until the first FlsSetValue */
};

/* A fiber that will run fn(arg) on a stack of stack_size bytes (rounded up to pages, min 64 KiB) plus a 64 KiB
 * PROT_NONE guard below it. fn must never return (it switches away instead); returning is fatal. NULL on failure. */
plat_fiber *plat_fiber_create(size_t stack_size, plat_fiber_fn fn, void *arg);
/* A fiber object for the calling thread itself (no stack of its own): the target of the first save. It starts without
 * FLS values; ConvertThreadToFiber (plat.h) is the one that hands the thread's values over and makes it current. */
plat_fiber *plat_fiber_convert_thread(void *arg);
/* Saves the running context in `from`, resumes `to`. Returns when something switches back to `from` (possibly on
 * another thread). `to` must not be running anywhere. It does not change the current fiber (GetCurrentFiber and so
 * FLS): a caller that wants those right calls plat_fiber_set_current(to) first, on the switching thread; whoever later
 * switches back to `from` sets it for its own thread the same way. */
void plat_fiber_switch(plat_fiber *from, plat_fiber *to);
/* The fiber GetCurrentFiber / FlsGetValue / FlsSetValue see on the calling thread (NULL: the thread itself). */
void plat_fiber_set_current(plat_fiber *f);
/* Runs f's FLS callbacks, frees the stack and the object; f must not be running. */
void plat_fiber_delete(plat_fiber *f);
static inline void *plat_fiber_arg(const plat_fiber *f) { return f->arg; }
/* Live FLS value sets (one per fiber or thread that has set a value and not ended): for tests and leak checks. */
size_t plat_fls_sets(void);

/* The raw assembly switch (no sanitizer bookkeeping) and the entry trampoline. */
void plat_fiber_switch_raw(plat_fiber *from, plat_fiber *to);
void plat_fiber_trampoline(void);

/* Noinline TLS accessors safe across fiber migration:
 *     static _Thread_local uint64_t t_fiber;
 *     PLAT_TLS_ACCESSORS(uint64_t, t_fiber)       // defines t_fiber_get() and t_fiber_set(v)
 * Each call recomputes the thread pointer; the empty asm with a memory clobber keeps the calls from being merged or
 * treated as pure. */
#if defined(__clang__)
#define PLAT_TLS_NOINLINE __attribute__((noinline))
#else
#define PLAT_TLS_NOINLINE __attribute__((noinline, noipa))
#endif
#define PLAT_TLS_ACCESSORS(type, var) \
    static PLAT_TLS_NOINLINE __attribute__((unused)) type var##_get(void) { __asm__ __volatile__("" ::: "memory"); return var; } \
    static PLAT_TLS_NOINLINE __attribute__((unused)) void var##_set(type v) { __asm__ __volatile__("" ::: "memory"); var = v; }

#ifdef __cplusplus
}
#endif
#endif /* SSBU_FIBER_H */
