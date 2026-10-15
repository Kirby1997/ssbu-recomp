/* plat_crash.c - crash reports (signals + frame-pointer walks) and A64 code inspection for POSIX aarch64 hosts.
 * Interface and behaviour: plat_crash.h. Replaces hostrt.c's SetUnhandledExceptionFilter / RtlVirtualUnwind and
 * nncxxrt.c's RtlCaptureContext walk on Linux; the Windows build does not compile anything here. */
#ifndef _WIN32
#include "plat.h"
#include "plat_crash.h"
#include <signal.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#ifndef AT_MINSIGSTKSZ
#define AT_MINSIGSTKSZ 51
#endif

/* ------------------------------------------------------------------------------------------------ memory reads */
uint64_t crash_strip_pac(uint64_t a) {
#if defined(__aarch64__)
    register uint64_t x30 __asm__("x30") = a;            /* xpaclri (hint #7): a no-op on cores without PAC */
    __asm__("hint #7" : "+r"(x30));
    return x30;
#else
    return a;
#endif
}
int crash_read(uint64_t addr, void *out, size_t n) {
    static int no_pvr;
    if (!n) return 1;
    if (addr + n < addr) return 0;
    if (!__atomic_load_n(&no_pvr, __ATOMIC_RELAXED)) {
        int e = errno;
        struct iovec l = {out, n}, r = {(void *)(uintptr_t)addr, n};
        ssize_t k = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
        int err = errno;
        errno = e;
        if (k == (ssize_t)n) return 1;
        if (k >= 0 || (err != ENOSYS && err != EPERM)) return 0;
        __atomic_store_n(&no_pvr, 1, __ATOMIC_RELAXED);   /* seccomp sandbox: the region table / pipe probe of vmem.c */
    }
    if (!vmem_readable((const void *)(uintptr_t)addr, n)) return 0;
    memcpy(out, (const void *)(uintptr_t)addr, n);
    return 1;
}
int crash_walk_fp(uint64_t fp, uint64_t *out, int max) {
    int n = 0;
    while (n < max && fp && !(fp & 7)) {
        uint64_t rec[2];
        if (!crash_read(fp, rec, sizeof rec)) break;
        uint64_t ra = crash_strip_pac(rec[1]);
        if (!ra) break;
        out[n++] = ra;
        if (rec[0] <= fp || rec[0] - fp > (256ull << 20)) break;    /* the chain only goes up the stack, by sane steps */
        fp = rec[0];
    }
    return n;
}
__attribute__((noinline)) int crash_backtrace(uint64_t *out, int max) {
    return crash_walk_fp((uint64_t)(uintptr_t)__builtin_frame_address(0), out, max);
}

void crash_print_frames(FILE *f, const uint64_t *pcs, int n, int lr_frame, int first_is_pc) {
    for (int i = 0; i < n; i++) {
        Dl_info di;
        uint64_t pc = pcs[i], at = i || !first_is_pc ? pc - 4 : pc;   /* a return address names the instruction after the call */
        fprintf(f, "  #%-2d %016llx", i, (unsigned long long)pc);
        if (dladdr((void *)(uintptr_t)at, &di) && di.dli_fname) {
            const char *b = strrchr(di.dli_fname, '/');
            fprintf(f, "  %s+0x%llx", b ? b + 1 : di.dli_fname, (unsigned long long)(pc - (uint64_t)(uintptr_t)di.dli_fbase));
            if (di.dli_sname && pc - (uint64_t)(uintptr_t)di.dli_saddr < 0x10000)      /* only exported names are known: a far one is a neighbour */
                fprintf(f, " (%s+0x%llx)", di.dli_sname, (unsigned long long)(pc - (uint64_t)(uintptr_t)di.dli_saddr));
        }
        fprintf(f, "%s\n", i == lr_frame ? "  [lr]" : "");
    }
}

/* ------------------------------------------------------------------------------------------------ alternate stacks */
#define ALT_SIZE (256u << 10)
static pthread_key_t g_alt_key;
static pthread_once_t g_alt_once = PTHREAD_ONCE_INIT;
static _Thread_local char *t_alt;
static void alt_free(void *m) {
    stack_t ss = {.ss_flags = SS_DISABLE};
    sigaltstack(&ss, NULL);
    munmap(m, ALT_SIZE + VMEM_PAGE);
}
static void alt_key(void) { pthread_key_create(&g_alt_key, alt_free); }
int crash_thread_init(void) {
    if (t_alt) return 0;
    stack_t cur;
    if (sigaltstack(NULL, &cur) == 0 && !(cur.ss_flags & SS_DISABLE) && cur.ss_size >= ALT_SIZE / 2) { t_alt = (char *)cur.ss_sp; return 0; }   /* someone else's, big enough */
    size_t size = ALT_SIZE;
    unsigned long minsig = getauxval(AT_MINSIGSTKSZ);    /* SVE/SME signal frames: the kernel says how big one can be */
    if (minsig && size < 4 * minsig) size = (4 * minsig + VMEM_PAGE - 1) & ~(size_t)(VMEM_PAGE - 1);
    pthread_once(&g_alt_once, alt_key);
    char *m = mmap(NULL, size + VMEM_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (m == MAP_FAILED) return -errno;
    mprotect(m, VMEM_PAGE, PROT_NONE);                   /* guard below the alternate stack */
    stack_t ss = {.ss_sp = m + VMEM_PAGE, .ss_size = size, .ss_flags = 0};
    if (sigaltstack(&ss, NULL)) { int e = errno; munmap(m, size + VMEM_PAGE); return -e; }
    t_alt = m;
    if (size == ALT_SIZE) pthread_setspecific(g_alt_key, m);   /* freed at thread exit (an enlarged one is leaked, rare) */
    return 0;
}

/* ------------------------------------------------------------------------------------------------ the report */
static crash_report_fn g_report;
static int g_exit_code = 4;
static int g_owner;                                       /* tid of the thread reporting, 0 = none */
static int g_installed;

static void raw(const char *s) { ssize_t r = write(2, s, strlen(s)); (void)r; }
static void on_alarm(int sig) {
    (void)sig;
    raw("\ncrash report: timed out (a lock held by the crashed code?); exiting\n");
    _exit(__atomic_load_n(&g_exit_code, __ATOMIC_RELAXED));
}
/* 1: this thread now owns the report. Otherwise does not return. */
static void take_report(void) {
    int me = (int)syscall(SYS_gettid), prev = 0;
    if (__atomic_compare_exchange_n(&g_owner, &prev, me, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        int t = 20;
        const char *e = getenv("SSBU_CRASH_TIMEOUT");
        if (e && atoi(e) > 0) t = atoi(e);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_alarm;
        sigaction(SIGALRM, &sa, NULL);
        sigset_t s;
        sigemptyset(&s); sigaddset(&s, SIGALRM);
        pthread_sigmask(SIG_UNBLOCK, &s, NULL);
        alarm((unsigned)t);
        return;
    }
    if (prev == me) { raw("\nHOST EXCEPTION inside the crash report: exiting\n"); _exit(__atomic_load_n(&g_exit_code, __ATOMIC_RELAXED)); }
    for (;;) pause();                                     /* another thread reports and ends the process */
}
__attribute__((noreturn)) static void finish(crash_info *ci) {
    fflush(stdout);
    crash_report_fn fn = __atomic_load_n(&g_report, __ATOMIC_ACQUIRE);
    if (fn) fn(ci);
    fflush(stdout); fflush(stderr);
    _exit(__atomic_load_n(&g_exit_code, __ATOMIC_RELAXED));
}
static void print_stack(crash_info *ci) {
    printf("host call stack (frame pointers, innermost first):\n");
    crash_print_frames(stdout, ci->pcs, ci->npcs, ci->lr_frame, ci->sig != 0);
    fflush(stdout);
}

static const char *sig_name(int s) {
    switch (s) { case SIGSEGV: return "SIGSEGV"; case SIGBUS: return "SIGBUS"; case SIGILL: return "SIGILL"; case SIGFPE: return "SIGFPE"; case SIGTRAP: return "SIGTRAP"; default: return "signal"; }
}
static const char *code_name(int s, int c) {
    if (c <= 0) return c == SI_USER ? "sent by kill" : c == SI_TKILL ? "sent by tgkill/raise" : "sent by a process";
    switch (s) {
    case SIGSEGV: return c == SEGV_MAPERR ? "SEGV_MAPERR: address not mapped" : c == SEGV_ACCERR ? "SEGV_ACCERR: no permission for this access" : "SEGV";
    case SIGBUS: return c == BUS_ADRALN ? "BUS_ADRALN: misaligned address" : c == BUS_ADRERR ? "BUS_ADRERR: no such physical address" : c == BUS_OBJERR ? "BUS_OBJERR: object error (file mapping beyond EOF?)" : "BUS";
    case SIGILL: return c == ILL_ILLOPC ? "ILL_ILLOPC: illegal opcode" : c == ILL_PRVOPC ? "ILL_PRVOPC: privileged opcode" : c == ILL_ILLTRP ? "ILL_ILLTRP: illegal trap" : "ILL";
    case SIGTRAP: return c == TRAP_BRKPT ? "TRAP_BRKPT: brk instruction" : c == TRAP_TRACE ? "TRAP_TRACE" : "TRAP";
    case SIGFPE: return c == FPE_INTDIV ? "FPE_INTDIV" : c == FPE_FLTDIV ? "FPE_FLTDIV" : c == FPE_FLTINV ? "FPE_FLTINV" : "FPE";
    default: return "?";
    }
}
/* ESR_EL1 of the fault, from the signal frame's records (struct esr_context, ESR_MAGIC); 0 if absent. */
static uint64_t frame_esr(const ucontext_t *uc) {
#if defined(__aarch64__)
    const unsigned char *p = (const unsigned char *)uc->uc_mcontext.__reserved;
    size_t off = 0, end = sizeof uc->uc_mcontext.__reserved;
    while (off + 8 <= end) {
        uint32_t magic, size;
        memcpy(&magic, p + off, 4); memcpy(&size, p + off + 4, 4);
        if (!magic || size < 8 || off + size > end) break;
        if (magic == 0x45535201u && size >= 16) { uint64_t esr; memcpy(&esr, p + off + 8, 8); return esr; }
        off += size;
    }
#endif
    (void)uc;
    return 0;
}

static void on_signal(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    take_report();
    static crash_info ci;                                 /* only the owner gets here */
    memset(&ci, 0, sizeof ci);
    ci.sig = sig; ci.code = si->si_code; ci.what = sig_name(sig);
    ci.addr = (uint64_t)(uintptr_t)si->si_addr;
    ci.lr_frame = -1;
#if defined(__aarch64__)
    const mcontext_t *mc = &uc->uc_mcontext;
    ci.pc = mc->pc; ci.sp = mc->sp; ci.fp = mc->regs[29]; ci.lr = crash_strip_pac(mc->regs[30]);
    ci.x = (const uint64_t *)mc->regs;
    uint64_t esr = frame_esr(uc);
    unsigned ec = (unsigned)(esr >> 26) & 0x3f;
    if (ec == 0x24 || ec == 0x25) ci.access = (esr >> 6) & 1 ? 'w' : 'r';
    else if (ec == 0x20 || ec == 0x21) ci.access = 'x';
#endif
    printf("\nHOST EXCEPTION %s (signal %d, %s) at host address 0x%llx\n", ci.what, sig, code_name(sig, ci.code), (unsigned long long)ci.pc);
    if (sig == SIGSEGV || sig == SIGBUS) {
        printf("  %s address 0x%llx\n", ci.access == 'w' ? "writing" : ci.access == 'r' ? "reading" : ci.access == 'x' ? "executing" : "accessing", (unsigned long long)ci.addr);
        if (ci.addr + (64u << 10) >= ci.sp && ci.addr < ci.sp + (64u << 10))
            printf("  (the address is next to the stack pointer 0x%llx: probably a stack overflow)\n", (unsigned long long)ci.sp);
    }
#if defined(__aarch64__)
    {   uint32_t insn;
        if (crash_read(ci.pc, &insn, 4)) {
            printf("  instruction at pc: %08x\n", insn);
            if ((insn & 0xffe0001fu) == 0xd4200000u) printf("  brk #0x%x\n", (insn >> 5) & 0xffff);   /* (no ESR record for brk) */
        } else if (ec == 0x3c) printf("  brk #0x%llx\n", (unsigned long long)(esr & 0xffff));
    }
#endif
    printf("host registers: pc %016llx lr %016llx sp %016llx fp %016llx\n", (unsigned long long)ci.pc, (unsigned long long)ci.lr, (unsigned long long)ci.sp, (unsigned long long)ci.fp);
    for (int i = 0; ci.x && i < 29; i++) printf("  x%-2d %016llx%s", i, (unsigned long long)ci.x[i], i % 4 == 3 || i == 28 ? "\n" : "");
    ci.pcs[ci.npcs++] = ci.pc;
    uint64_t chain[CRASH_MAX_FRAMES];
    int n = crash_walk_fp(ci.fp, chain, CRASH_MAX_FRAMES - 2);
    if (ci.lr && ci.lr != ci.pc && (n == 0 || chain[0] != ci.lr)) { ci.lr_frame = ci.npcs; ci.pcs[ci.npcs++] = ci.lr; }   /* a leaf before (or without) its frame record */
    for (int i = 0; i < n && ci.npcs < CRASH_MAX_FRAMES; i++) ci.pcs[ci.npcs++] = chain[i];
    print_stack(&ci);
    finish(&ci);
}

__attribute__((noinline, noreturn)) void crash_report_here(uint32_t code, const char *what) {
    take_report();
    static crash_info ci;
    memset(&ci, 0, sizeof ci);
    ci.exc_code = code; ci.what = what; ci.lr_frame = -1;
    ci.fp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    ci.npcs = crash_walk_fp(ci.fp, ci.pcs, CRASH_MAX_FRAMES);      /* pcs[0] = the return address into our caller */
    ci.pc = ci.npcs ? ci.pcs[0] - 4 : 0;                 /* the call instruction */
    printf("\nHOST EXCEPTION 0x%08x (%s) at host address 0x%llx\n", code, what ? what : "software exception", (unsigned long long)ci.pc);
    print_stack(&ci);
    finish(&ci);
}

void crash_install(crash_report_fn report, int exit_code) {
    __atomic_store_n(&g_exit_code, exit_code, __ATOMIC_RELAXED);
    __atomic_store_n(&g_report, report, __ATOMIC_RELEASE);
    crash_thread_init();
    if (__atomic_exchange_n(&g_installed, 1, __ATOMIC_ACQ_REL)) return;
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;    /* NODEFER: a fault inside the report re-enters and exits */
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
}

/* ------------------------------------------------------------------------------------------------ A64 inspection */
/* DecodeBitMasks (Arm ARM) for a logical immediate; 0 if the encoding is reserved. */
static int a64_bitmask(int sf, unsigned n, unsigned immr, unsigned imms, uint64_t *out) {
    unsigned x = (n << 6) | (~imms & 0x3f);
    if (!x || (!sf && n)) return 0;
    int len = 31 - __builtin_clz(x);
    if (len < 1) return 0;
    unsigned esize = 1u << len, levels = esize - 1, s = imms & levels, r = immr & levels;
    if (s == levels) return 0;
    uint64_t emask = esize == 64 ? ~0ull : (1ull << esize) - 1;
    uint64_t w = s + 1 == 64 ? ~0ull : (1ull << (s + 1)) - 1;
    uint64_t e = r ? ((w >> r) | (w << (esize - r))) & emask : w;
    uint64_t v = 0;
    for (unsigned i = 0; i < 64; i += esize) v |= e << i;
    *out = sf ? v : v & 0xffffffffull;
    return 1;
}
int a64_trivial(const void *code, long long *x0v, int *sets_x0) {
    const unsigned char *p = code;
    uint64_t val[32] = {0};
    uint32_t known = 1u << 31, vknown = 0, vzero = 0;      /* x31 as a source of mov/str = zr */
    int stores = 0, hops = 0;
    *x0v = 0; *sets_x0 = 0;
    if (!p) return 0;
#define SETX(rd, v, sf) do { if ((rd) == 0 || (rd) == 31) return 0; val[rd] = (sf) ? (uint64_t)(v) : (uint64_t)(uint32_t)(v); known |= 1u << (rd); } while (0)
#define KNOWN(r) ((known >> (r)) & 1u)
#define STORE_X0(v) do { *x0v = (long long)(v); *sets_x0 = 1; } while (0)
    for (int step = 0; step < 48; step++) {
        uint32_t i;
        memcpy(&i, p, 4);
        unsigned rd = i & 31, rn = (i >> 5) & 31;
        if (i == 0xd65f03c0u || i == 0xd65f0bffu) return stores ? 2 : 1;               /* ret, retaa */
        if ((i & 0xfffff01fu) == 0xd503201fu) { p += 4; continue; }                     /* hints: nop, bti, paciasp, autiasp */
        if ((i & 0xfc000000u) == 0x14000000u) {                                         /* b: tail call / thunk */
            if (++hops > 4) return 0;
            p += (int64_t)((int32_t)(i << 6) >> 4);
            continue;
        }
        if ((i & 0xffc07fffu) == 0xa9807bfdu || (i & 0xffc07fffu) == 0xa8c07bfdu || i == 0x910003fdu) { p += 4; continue; }   /* frame record push/pop, mov x29, sp */
        if ((i & 0x1f800000u) == 0x12800000u) {                                         /* movn / movz / movk */
            int sf = i >> 31, opc = (i >> 29) & 3, hw = (i >> 21) & 3;
            uint64_t imm = (uint64_t)((i >> 5) & 0xffff) << (16 * hw);
            if ((!sf && hw > 1) || opc == 1) return 0;
            if (opc == 0) SETX(rd, ~imm, sf);
            else if (opc == 2) SETX(rd, imm, sf);
            else { if (!KNOWN(rd) || rd == 31) return 0; SETX(rd, (val[rd] & ~((uint64_t)0xffff << (16 * hw))) | imm, sf); }
            p += 4; continue;
        }
        if ((i & 0x7f800000u) == 0x32000000u) {                                         /* orr rd, rn, #bitmask (mov rd, #imm when rn = zr) */
            uint64_t imm;
            int sf = i >> 31;
            if (!a64_bitmask(sf, (i >> 22) & 1, (i >> 16) & 63, (i >> 10) & 63, &imm) || !KNOWN(rn)) return 0;
            SETX(rd, (rn == 31 ? 0 : val[rn]) | imm, sf);
            p += 4; continue;
        }
        if ((i & 0x7fe0ffe0u) == 0x2a0003e0u) {                                         /* mov rd, rm (orr rd, zr, rm) */
            unsigned rm = (i >> 16) & 31;
            if (!KNOWN(rm)) return 0;
            SETX(rd, rm == 31 ? 0 : val[rm], i >> 31);
            p += 4; continue;
        }
        if ((i & 0xffffffe0u) == 0x9e6703e0u || (i & 0xffffffe0u) == 0x1e2703e0u) { vknown |= 1u << rd; vzero |= 1u << rd; p += 4; continue; }   /* fmov d/s, xzr/wzr */
        if ((i & 0xff201fe0u) == 0x1e201000u) { vknown |= 1u << rd; vzero &= ~(1u << rd); p += 4; continue; }   /* fmov s/d/h, #imm */
        if ((i & 0x9ff80c00u) == 0x0f000400u && !((i >> 5) & 31) && !((i >> 16) & 7)) {   /* movi / mvni / fmov (vector, immediate) with imm 0 */
            unsigned op = (i >> 29) & 1, cmode = (i >> 12) & 15;
            int zero = (!op && !(cmode & 1) && cmode != 12) || (op && cmode == 14);
            int other = (!op && cmode == 15) || (op && cmode == 15);                    /* fmov #2.0 */
            if (!zero && !other) return 0;
            vknown |= 1u << rd;
            if (zero) vzero |= 1u << rd; else vzero &= ~(1u << rd);
            p += 4; continue;
        }
        /* integer stores, base x0 */
        if ((i & 0x3fc00000u) == 0x39000000u || (i & 0x3fe00c00u) == 0x38000000u) {     /* str (unsigned offset) / stur */
            unsigned size = i >> 30;
            int64_t off = (i & 0x01000000u) ? (int64_t)(((i >> 10) & 0xfff) << size) : (int64_t)((int32_t)(i << 11) >> 23);
            if (rn != 0 || !KNOWN(rd)) return 0;
            uint64_t v = rd == 31 ? 0 : val[rd];
            if (off == 0 && size == 3) STORE_X0(v);
            else if (off == 0 && size == 2) STORE_X0((uint32_t)v);
            stores++; p += 4; continue;
        }
        if ((i & 0x7fc00000u) == 0x29000000u) {                                         /* stp w/x (signed offset) */
            unsigned opc = i >> 30, rt2 = (i >> 10) & 31;
            if (opc != 0 && opc != 2) return 0;
            int64_t off = (int64_t)((int32_t)(i << 10) >> 25) * (opc ? 8 : 4);
            if (rn != 0 || !KNOWN(rd) || !KNOWN(rt2)) return 0;
            uint64_t v1 = rd == 31 ? 0 : val[rd], v2 = rt2 == 31 ? 0 : val[rt2];
            if (off == 0) STORE_X0(opc ? v1 : (uint32_t)v1 | ((uint64_t)(uint32_t)v2 << 32));
            else if (opc && off == -8) STORE_X0(v2);
            stores++; p += 4; continue;
        }
        /* vector stores, base x0 */
        if ((i & 0x3f400000u) == 0x3d000000u || (i & 0x3f600c00u) == 0x3c000000u) {     /* str / stur b/h/s/d/q */
            unsigned scale = ((i >> 30) & 3) | (((i >> 23) & 1) << 2);
            int64_t off = (i & 0x01000000u) ? (int64_t)(((i >> 10) & 0xfff) << scale) : (int64_t)((int32_t)(i << 11) >> 23);
            if (rn != 0 || !((vknown >> rd) & 1)) return 0;
            if (off == 0 && scale >= 2 && ((vzero >> rd) & 1)) STORE_X0(0);
            stores++; p += 4; continue;
        }
        if ((i & 0x3fc00000u) == 0x2d000000u) {                                         /* stp s/d/q (signed offset) */
            unsigned opc = i >> 30, rt2 = (i >> 10) & 31;
            if (opc == 3) return 0;
            int64_t off = (int64_t)((int32_t)(i << 10) >> 25) * (4 << opc);
            if (rn != 0 || !((vknown >> rd) & 1) || !((vknown >> rt2) & 1)) return 0;
            if (off == 0 && opc >= 1 && ((vzero >> rd) & 1)) STORE_X0(0);
            stores++; p += 4; continue;
        }
        return 0;
    }
#undef SETX
#undef KNOWN
#undef STORE_X0
    return 0;
}
#endif /* !_WIN32 */
