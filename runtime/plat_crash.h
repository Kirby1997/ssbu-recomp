/* plat_crash.h - crash reporting and A64 code inspection for POSIX aarch64 hosts (the Win32 build uses
 * SetUnhandledExceptionFilter / RtlVirtualUnwind in hostrt.c and nncxxrt.c instead; nothing here exists on _WIN32).
 *
 * Crash reports: crash_install() puts one handler on SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP and SIGABRT (SA_SIGINFO |
 * SA_ONSTACK | SA_NODEFER), keeping the handlers that were there before. The report has two steps:
 *  1. Async-signal-safe (no stdio, no malloc, no locks: static buffers, open(2)/write(2), memory read with crash_read): the same
 *     text goes to the report file (crash_set_report_file; hostrt.c: <exe>_crash.txt via diag_report_path; created and
 *     appended to by the handler, so a run that does not crash leaves no file) and then to fd 1 (the console):
 *         === <UTC time>  HOST EXCEPTION SIGSEGV (signal 11, SEGV_MAPERR: address not mapped) at host address 0x... (module+0x...)
 *           writing address 0x...                     (SIGSEGV/SIGBUS; the kind of access from the ESR record)
 *           instruction at pc: ...
 *         thread <tid> (<name>), on logical processor <n>; process <pid>, up <s> s
 *         host registers: pc lr sp fp pstate, then x0..x30; host stack from sp-0x40 (48 words)
 *         host call stack (frame pointers, innermost first): "#i pc module+0xvaddr (exported symbol+0xoff)"
 *         the report section (crash_set_section; hostrt.c: guest thread, guest registers, guest call stack)
 *         build: <exe>, size, time, build-id
 *     The file's copy then lists the modules (ranges, build-ids) and ends with "=== end of crash report". Module names and
 *     symbols come from a snapshot taken outside the handler (dl_iterate_phdr at crash_install, crash_thread_init and
 *     crash_modules_refresh); an address outside it is placed with /proc/self/maps (open/read only). A fault while writing a
 *     part of the report marks that part and the report goes on.
 *  2. The report callback (hostrt.c: the run's summaries with stdio, then report_and_exit -> ExitProcess). Not async-signal-
 *     safe, and bounded by SSBU_CRASH_TIMEOUT seconds (default 20, read at crash_install); a fault inside it ends it.
 * Then the process ends: the handler that was installed before crash_install (if any) gets the signal (a fault re-executes
 * into it; a sent signal is raised again); else, with SSBU_CORE=1 (read at crash_install) and core dumps on (RLIMIT_CORE != 0;
 * SteamOS: unlimited, the core goes to steamos-log-submitter -> systemd-coredump, see coredumpctl), the signal is raised again
 * with its default action, so a core is written and the exit status names the signal; else (the default) _exit(exit_code) (the
 * callback's ExitProcess code, normally 4) as the Windows build. crash_report_here() always ends with _exit. One report per process: a second thread that crashes meanwhile
 * waits for the first to end the process.
 *
 * Stack overflow: the handler runs on a per-thread alternate stack, which every thread that may crash must have:
 * crash_thread_init() (idempotent, cheap after the first call on a thread; the stack is freed at thread exit). crash_install()
 * does it for the calling thread, plat_posix.c's CreateThread and os_threads.c's host threads for theirs. A thread without one
 * dies of an overflow without a report.
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
    uint64_t pstate;             /* signal only */
    const uint64_t *x;           /* x0..x30 of the interrupted code; NULL for crash_report_here() */
    uint64_t pcs[CRASH_MAX_FRAMES];   /* pcs[0] = pc; then return addresses, innermost first */
    int npcs;
    int lr_frame;                /* index of the entry taken from lr (fault in a leaf before its frame record), else -1 */
} crash_info;
typedef void (*crash_report_fn)(const crash_info *ci);

/* Report text sink of the async-signal-safe step (write(2) of a static buffer). Only for use inside a crash_section_fn. */
typedef struct crash_out crash_out;
void crash_put(crash_out *o, const char *s);
void crash_putn(crash_out *o, const char *s, size_t n);
void crash_put_hex(crash_out *o, uint64_t v, int width);    /* lowercase, no "0x"; zero-padded to width digits (0: as needed) */
void crash_put_dec(crash_out *o, long long v);
/* A part of the report written by someone else (hostrt.c: the guest side). Runs inside the signal handler, twice (file, then
 * console): it must be async-signal-safe and give the same text both times. A fault inside it is caught and marked. */
typedef void (*crash_section_fn)(crash_out *o, const crash_info *ci);

/* Install the handlers (process-wide, once; later calls only replace callback / exit code) and give the calling
 * thread its alternate stack. report may be NULL. */
void crash_install(crash_report_fn report, int exit_code);
void crash_set_section(crash_section_fn fn);
/* The file the report is also written to: the handler creates it (O_APPEND; a descriptor is kept spare for it). Checks now that
 * it can be created: 0 or -errno. */
int crash_set_report_file(const char *path);
/* Take the module snapshot again if modules were loaded or unloaded since (outside any handler; cheap when none were). */
void crash_modules_refresh(void);
/* plat_posix.c ExitProcess: on the reporting thread inside the callback, returns to the end of the report (does not return);
 * otherwise nothing. */
void crash_on_exit(int code);
/* Alternate signal stack for the calling thread (256 KiB + a guard page, NORESERVE). 0 or -errno. Call it first thing in
 * every thread entry (os_threads.c / plat_posix.c thread trampolines, boot.c's main guest thread). */
int crash_thread_init(void);
/* Software exception: the same report as a signal (header "HOST EXCEPTION 0x<code> (<what>)", host call stack of the
 * caller, callback), then _exit(exit_code) if the callback returns. Replaces RaiseException in hostrt.c. */
__attribute__((noreturn)) void crash_report_here(uint32_t code, const char *what);

/* Never-faulting read of n bytes at addr: 1 if all could be read. Async-signal-safe. */
int crash_read(uint64_t addr, void *out, size_t n);
/* Return addresses along the frame-record chain starting at fp (not including the frame's own pc). Stops at a null,
 * unreadable, misaligned or non-increasing link, or after max entries. */
int crash_walk_fp(uint64_t fp, uint64_t *out, int max);
/* Return addresses of the caller's frames, innermost first (out[0] = return address into the caller of the function
 * that called crash_backtrace). */
int crash_backtrace(uint64_t *out, int max);
/* One line per pc: "  #i 0x... module+0xoff (symbol+0xoff)". Names are looked up at pc - 4 for return addresses (all
 * entries but the first; the first too when !first_is_pc), so a call at the very end of a function is not named after
 * the next one. Only exported symbols are known (link with -rdynamic for more); module+offset is always exact. Uses stdio
 * and dladdr: not for a signal handler (the crash report has its own). */
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
