/* plat_crash.h - crash reporting and A64 code inspection for POSIX aarch64 hosts (the Win32 build uses
 * SetUnhandledExceptionFilter / RtlVirtualUnwind in hostrt.c and nncxxrt.c instead; nothing here exists on _WIN32).
 *
 * Crash reports: crash_install() puts one handler on SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGTRAP (SA_SIGINFO |
 * SA_ONSTACK | SA_NODEFER). The handler prints, on stdout as hostrt.c's Windows filter did:
 *     HOST EXCEPTION SIGSEGV (signal 11, SEGV_MAPERR: address not mapped) at host address 0x...
 *       writing address 0x...
 *     host registers: pc lr sp fp
 *     host call stack (frame pointers, innermost first): one line per frame with module+offset and symbol (dladdr)
 * then calls the report callback (hostrt.c: guest registers, guest call stack, the run's summaries, ExitProcess(4)).
 * If there is no callback or it returns, the process ends with _exit(exit_code). One report per process: a second
 * thread that crashes meanwhile waits for the first to end the process; a fault inside the report itself ends the
 * process at once; a report that hangs (a lock held by the crashed code) is cut after SSBU_CRASH_TIMEOUT seconds
 * (default 20) with the same exit code.
 *
 * Stack overflow: the handler runs on a per-thread alternate stack, which every thread that may crash must have:
 * crash_thread_init() (idempotent, cheap after the first call on a thread; the stack is freed at thread exit).
 * crash_install() does it for the calling thread. A thread without one dies of an overflow without a report.
 *
 * Frame-pointer walks: every runtime and lifted file is built -fno-omit-frame-pointer, so x29 chains the frame
 * records {previous x29, return address} up to a thread or fiber entry (fiber_aarch64.S starts fibers with x29 = 0).
 * Reads of the chain never fault (process_vm_readv on our own pid, else vmem_readable). PAC bits are stripped.
 */
#ifndef SSBU_PLAT_CRASH_H
#define SSBU_PLAT_CRASH_H
#ifndef _WIN32
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CRASH_MAX_FRAMES 128
typedef struct crash_info {
    int sig, code;               /* signal and si_code; sig 0: a software exception from crash_report_here() */
    uint32_t exc_code;           /* crash_report_here()'s code (hostrt.c: 0xE0000001 guest trap, 0xE0000002 untranslated) */
    const char *what;            /* "SIGSEGV" ... or crash_report_here()'s text */
    uint64_t addr;               /* si_addr (fault address for SEGV/BUS, faulting pc for ILL/TRAP/FPE) */
    int access;                  /* 'r', 'w', 'x' (from ESR) or 0 when unknown */
    uint64_t pc, lr, sp, fp;     /* crash_report_here: pc = the call instruction in its caller */
    const uint64_t *x;           /* x0..x30 of the interrupted code; NULL for crash_report_here() */
    uint64_t pcs[CRASH_MAX_FRAMES];   /* pcs[0] = pc; then return addresses, innermost first */
    int npcs;
    int lr_frame;                /* index of the entry taken from lr (fault in a leaf before its frame record), else -1 */
} crash_info;
typedef void (*crash_report_fn)(const crash_info *ci);

/* Install the handlers (process-wide, once; later calls only replace callback / exit code) and give the calling
 * thread its alternate stack. report may be NULL. */
void crash_install(crash_report_fn report, int exit_code);
/* Alternate signal stack for the calling thread (256 KiB + a guard page, NORESERVE). 0 or -errno. Call it first thing in
 * every thread entry (os_threads.c / plat_posix.c thread trampolines, boot.c's main guest thread). */
int crash_thread_init(void);
/* Software exception: the same report as a signal (header "HOST EXCEPTION 0x<code> (<what>)", host call stack of the
 * caller, callback), then _exit(exit_code) if the callback returns. Replaces RaiseException in hostrt.c. */
__attribute__((noreturn)) void crash_report_here(uint32_t code, const char *what);

/* Never-faulting read of n bytes at addr: 1 if all could be read. */
int crash_read(uint64_t addr, void *out, size_t n);
/* Return addresses along the frame-record chain starting at fp (not including the frame's own pc). Stops at a null,
 * unreadable, misaligned or non-increasing link, or after max entries. */
int crash_walk_fp(uint64_t fp, uint64_t *out, int max);
/* Return addresses of the caller's frames, innermost first (out[0] = return address into the caller of the function
 * that called crash_backtrace). */
int crash_backtrace(uint64_t *out, int max);
/* One line per pc: "  #i 0x... module+0xoff (symbol+0xoff)". Names are looked up at pc - 4 for return addresses (all
 * entries but the first; the first too when !first_is_pc), so a call at the very end of a function is not named after
 * the next one. Only exported symbols are known (link with -rdynamic for more); module+offset is always exact. */
void crash_print_frames(FILE *f, const uint64_t *pcs, int n, int lr_frame, int first_is_pc);
uint64_t crash_strip_pac(uint64_t a);

/* A64 "trivial function" check (hostrt.c audit_trivial, plan\FAKE_AUDIT.md): 0 = real code; 1 = empty body (ret);
 * 2 = only stores constants into the context x0 points to, then returns (*x0v := the value stored at [x0, #0] when
 * *sets_x0). Follows up to 4 unconditional branches (tail calls / thunks); hints (nop, bti, paciasp/autiasp) are
 * ignored. Accepts mov/movz/movn/movk/orr-immediate/mov-register into scratch registers, movi #0 / fmov from zr into
 * vector registers, and str/stur/stp (integer and vector, signed offset) with base x0 of a known value. */
int a64_trivial(const void *code, long long *x0v, int *sets_x0);

#ifdef __cplusplus
}
#endif
#endif /* !_WIN32 */
#endif /* SSBU_PLAT_CRASH_H */
