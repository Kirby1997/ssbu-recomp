/* plat_crash.c - crash reports (signals + frame-pointer walks) and A64 code inspection for POSIX aarch64 hosts.
 * Interface and behaviour: plat_crash.h. Replaces hostrt.c's SetUnhandledExceptionFilter / RtlVirtualUnwind and
 * nncxxrt.c's RtlCaptureContext walk on Linux; the Windows build does not compile anything here. */
#ifndef _WIN32
#include "plat.h"
#include "plat_crash.h"
#include <signal.h>
#include <setjmp.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <link.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
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
static int g_owner;                                       /* tid of the thread reporting a crash, 0 = none */
static int g_rd_pipe[2] = {-1, -1};                       /* crash_install: the reporting thread's reader without process_vm_readv */
/* write(2) from a buffer that is not readable fails with EFAULT instead of faulting: copy through a pipe, in pieces the pipe
 * takes whole. Only the reporting thread uses the pipe. */
static int pipe_read(uint64_t addr, void *out, size_t n) {
    char *o = out;
    while (n) {
        size_t c = n < 4096 ? n : 4096;
        ssize_t k = write(g_rd_pipe[1], (const void *)(uintptr_t)addr, c), r = 0;
        while (k > 0 && r < k) { ssize_t d = read(g_rd_pipe[0], o + r, (size_t)(k - r)); if (d <= 0) break; r += d; }   /* drain it */
        if (k != (ssize_t)c) return 0;
        o += c; addr += c; n -= c;
    }
    return 1;
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
    if (g_rd_pipe[1] >= 0 && __atomic_load_n(&g_owner, __ATOMIC_ACQUIRE) == (int)syscall(SYS_gettid)) {   /* in the crash report: no locks */
        int e = errno, ok = pipe_read(addr, out, n);
        errno = e;
        return ok;
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

/* ------------------------------------------------------------------------------------------------ report text
 * Everything the handler writes goes through these: a static buffer and write(2), no stdio, no malloc. */
struct crash_out {
    int fd;                      /* -1 once a write failed (closed pipe, full disk): this copy is lost, not the other */
    int file;                    /* the report file's copy: also the module list */
    size_t n;
    char buf[4096];
};
static void out_flush(crash_out *o) {
    size_t off = 0;
    while (off < o->n && o->fd >= 0) {
        ssize_t k = write(o->fd, o->buf + off, o->n - off);
        if (k > 0) off += (size_t)k;
        else if (k < 0 && errno == EINTR) continue;
        else o->fd = -1;
    }
    o->n = 0;
}
void crash_putn(crash_out *o, const char *s, size_t n) {
    while (n) {
        if (o->n == sizeof o->buf) out_flush(o);
        size_t k = sizeof o->buf - o->n < n ? sizeof o->buf - o->n : n;
        memcpy(o->buf + o->n, s, k);
        o->n += k; s += k; n -= k;
    }
}
void crash_put(crash_out *o, const char *s) { if (s) crash_putn(o, s, strlen(s)); }
void crash_put_hex(crash_out *o, uint64_t v, int width) {
    char t[16];
    int n = 0;
    do { t[15 - n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v && n < 16);
    while (n < width && n < 16) t[15 - n++] = '0';
    crash_putn(o, t + 16 - n, (size_t)n);
}
static void put_udec(crash_out *o, unsigned long long v, int width, char pad) {     /* width < 0: left-aligned */
    char t[24];
    int n = 0;
    do { t[23 - n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int w = width; w > n && w > 0; w--) crash_putn(o, &pad, 1);
    crash_putn(o, t + 24 - n, (size_t)n);
    for (int w = -width; w > n; w--) crash_putn(o, " ", 1);
}
void crash_put_dec(crash_out *o, long long v) {
    if (v < 0) { crash_putn(o, "-", 1); put_udec(o, 0ull - (unsigned long long)v, 0, ' '); }
    else put_udec(o, (unsigned long long)v, 0, ' ');
}
static void put_hex16(crash_out *o, uint64_t v) { crash_put_hex(o, v, 16); }
static int64_t clock_ns(clockid_t id) { struct timespec ts; clock_gettime(id, &ts); return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec; }
/* "YYYY-MM-DD HH:MM:SS" of a UTC time (days to civil date: H. Hinnant's algorithm; gmtime_r is not async-signal-safe) */
static void put_utc(crash_out *o, long long t) {
    long long z = (t >= 0 ? t : t - 86399) / 86400, s = t - z * 86400;
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    long long y = (long long)yoe + era * 400 + (m <= 2);
    put_udec(o, (unsigned long long)y, 4, '0'); crash_put(o, "-"); put_udec(o, m, 2, '0'); crash_put(o, "-"); put_udec(o, d, 2, '0');
    crash_put(o, " "); put_udec(o, (unsigned long long)(s / 3600), 2, '0'); crash_put(o, ":"); put_udec(o, (unsigned long long)(s / 60 % 60), 2, '0');
    crash_put(o, ":"); put_udec(o, (unsigned long long)(s % 60), 2, '0');
}
static void put_secs(crash_out *o, int64_t ns) {             /* "12.34" */
    if (ns < 0) ns = 0;
    put_udec(o, (unsigned long long)(ns / 1000000000), 0, ' '); crash_put(o, "."); put_udec(o, (unsigned long long)(ns / 10000000 % 100), 2, '0');
}

/* ------------------------------------------------------------------------------------------------ module snapshot
 * Taken with dl_iterate_phdr outside any handler (crash_install, crash_thread_init, crash_modules_refresh after a
 * dlopen); the handler only reads the published copy. Two tables: a refresh fills the one not published. */
#define MAX_MODS 512
typedef struct {
    uint64_t lo, hi, base;       /* PT_LOAD span; load bias (vaddr = address - base, what addr2line takes) */
    uint64_t symtab, strtab, strsz;  /* .dynsym / .dynstr: the exported names, as dladdr knows them */
    uint32_t nsyms;
    uint8_t bidn, bid[20];       /* NT_GNU_BUILD_ID */
    char name[64];               /* file name only: reports get posted */
} crash_mod;
typedef struct { int n; int64_t when; crash_mod m[MAX_MODS]; } mod_table;
static mod_table g_modtab[2];
static mod_table *g_mods;                                 /* published (release / acquire) */
static pthread_mutex_t g_mods_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long g_mods_adds, g_mods_subs;
static char g_exe_name[64] = "?";
static int64_t g_start_boot = -1;                         /* process start on CLOCK_BOOTTIME (ns), from /proc/self/stat */

static void copy_base(char *out, size_t cap, const char *path) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    size_t n = strlen(b);
    if (n >= cap) n = cap - 1;
    memcpy(out, b, n);
    out[n] = 0;
}
static void note_build_id(crash_mod *m, const unsigned char *p, size_t size, size_t align) {
    size_t off = 0;
    while (off + 12 <= size) {
        uint32_t nsz, dsz, type;
        memcpy(&nsz, p + off, 4); memcpy(&dsz, p + off + 4, 4); memcpy(&type, p + off + 8, 4);
        size_t name = off + 12, desc = name + ((nsz + align - 1) & ~(align - 1)), next = desc + ((dsz + align - 1) & ~(align - 1));
        if (next > size || next <= off) return;
        if (type == 3 && nsz == 4 && !memcmp(p + name, "GNU", 4) && dsz) {
            m->bidn = (uint8_t)(dsz < sizeof m->bid ? dsz : sizeof m->bid);
            memcpy(m->bid, p + desc, m->bidn);
            return;
        }
        off = next;
    }
}
/* number of .dynsym entries: DT_HASH's nchain, or the end of the last DT_GNU_HASH chain */
static uint32_t dyn_nsyms(const uint32_t *hash, const uint32_t *gnu) {
    if (gnu) {
        uint32_t nb = gnu[0], symoff = gnu[1], bloom = gnu[2], last = 0;
        const uint32_t *buckets = (const uint32_t *)((const uint64_t *)(gnu + 4) + bloom), *chain = buckets + nb;
        for (uint32_t b = 0; b < nb; b++) if (buckets[b] > last) last = buckets[b];
        if (last < symoff) return symoff;
        while (!(chain[last - symoff] & 1)) last++;
        return last + 1;
    }
    return hash ? hash[1] : 0;
}
static int mod_cb(struct dl_phdr_info *in, size_t size, void *arg) {
    (void)size;
    mod_table *t = arg;
    if (t->n >= MAX_MODS) return 1;
    crash_mod *m = &t->m[t->n];
    memset(m, 0, sizeof *m);
    uint64_t base = in->dlpi_addr, lo = UINT64_MAX, hi = 0;
    const ElfW(Dyn) *dyn = NULL;
    for (int i = 0; i < in->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &in->dlpi_phdr[i];
        if (p->p_type == PT_LOAD) {
            if (base + p->p_vaddr < lo) lo = base + p->p_vaddr;
            if (base + p->p_vaddr + p->p_memsz > hi) hi = base + p->p_vaddr + p->p_memsz;
        } else if (p->p_type == PT_DYNAMIC) dyn = (const ElfW(Dyn) *)(uintptr_t)(base + p->p_vaddr);
        else if (p->p_type == PT_NOTE && !m->bidn) note_build_id(m, (const unsigned char *)(uintptr_t)(base + p->p_vaddr), p->p_memsz, p->p_align == 8 ? 8 : 4);
    }
    if (hi <= lo) return 0;
    m->lo = lo; m->hi = hi; m->base = base;
    if (in->dlpi_name && in->dlpi_name[0]) copy_base(m->name, sizeof m->name, in->dlpi_name);
    else memcpy(m->name, t->n == 0 ? g_exe_name : "?", t->n == 0 ? sizeof m->name : 2);   /* the program itself has no name here */
    if (dyn) {
        uint64_t hash = 0, gnu = 0;
        for (const ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; d++) {
            if (d->d_tag == DT_SYMTAB) m->symtab = d->d_un.d_ptr;
            else if (d->d_tag == DT_STRTAB) m->strtab = d->d_un.d_ptr;
            else if (d->d_tag == DT_STRSZ) m->strsz = d->d_un.d_val;
            else if (d->d_tag == DT_HASH) hash = d->d_un.d_ptr;
            else if (d->d_tag == DT_GNU_HASH) gnu = d->d_un.d_ptr;
        }
#define DYNPTR(v) ((v) && (v) < base ? (v) + base : (v))  /* ld.so relocates these in place, except in read-only dynamic sections (vDSO) */
        m->symtab = DYNPTR(m->symtab); m->strtab = DYNPTR(m->strtab);
        if (m->symtab && m->strtab) m->nsyms = dyn_nsyms((const uint32_t *)(uintptr_t)DYNPTR(hash), (const uint32_t *)(uintptr_t)DYNPTR(gnu));
#undef DYNPTR
    }
    t->n++;
    return 0;
}
static int count_cb(struct dl_phdr_info *in, size_t size, void *arg) {
    unsigned long long *c = arg;
    if (size >= offsetof(struct dl_phdr_info, dlpi_subs) + sizeof in->dlpi_subs) { c[0] = in->dlpi_adds; c[1] = in->dlpi_subs; }
    return 1;
}
static int g_installed;
void crash_modules_refresh(void) {
    if (!__atomic_load_n(&g_installed, __ATOMIC_ACQUIRE)) return;
    unsigned long long c[2] = {0, 0};
    pthread_mutex_lock(&g_mods_mu);
    dl_iterate_phdr(count_cb, c);                         /* loads / unloads so far: nothing to do when they are the same */
    mod_table *cur = __atomic_load_n(&g_mods, __ATOMIC_ACQUIRE);
    if (!cur || c[0] != g_mods_adds || c[1] != g_mods_subs) {
        mod_table *t = cur == &g_modtab[0] ? &g_modtab[1] : &g_modtab[0];
        t->n = 0;
        t->when = clock_ns(CLOCK_BOOTTIME);
        dl_iterate_phdr(mod_cb, t);
        __atomic_store_n(&g_mods, t, __ATOMIC_RELEASE);
        g_mods_adds = c[0]; g_mods_subs = c[1];
    }
    pthread_mutex_unlock(&g_mods_mu);
}
static const crash_mod *mod_of(const mod_table *t, uint64_t a) {
    for (int i = 0; t && i < t->n; i++) if (a >= t->m[i].lo && a < t->m[i].hi) return &t->m[i];
    return NULL;
}
/* Exported symbol of m around `at`, as dladdr: one that contains it, or (size 0) the nearest below within 64 KiB. Reads the
 * module's own tables: only inside a guarded part of the report (an unloaded module faults, the part is skipped). */
static __attribute__((noinline)) void put_symbol(crash_out *o, const crash_mod *m, uint64_t at, uint64_t pc) {
    const ElfW(Sym) *s = (const ElfW(Sym) *)(uintptr_t)m->symtab, *best = NULL;
    uint64_t best_a = 0;
    for (uint32_t i = 1; s && i < m->nsyms; i++) {
        unsigned type = ELF64_ST_TYPE(s[i].st_info);
        if (s[i].st_shndx == SHN_UNDEF || s[i].st_shndx == SHN_ABS || (type != STT_FUNC && type != STT_GNU_IFUNC && type != STT_NOTYPE)) continue;
        uint64_t a = m->base + s[i].st_value;
        if (a > at || a < best_a || (best && a == best_a)) continue;
        if (s[i].st_size ? at - a >= s[i].st_size : at - a >= 0x10000) continue;
        best = &s[i]; best_a = a;
    }
    if (!best || best->st_name >= m->strsz) return;
    const char *nm = (const char *)(uintptr_t)m->strtab + best->st_name;
    size_t n = 0;
    while (n < 120 && best->st_name + n < m->strsz && nm[n]) n++;
    crash_put(o, " ("); crash_putn(o, nm, n); crash_put(o, "+0x"); crash_put_hex(o, pc - best_a, 0); crash_put(o, ")");
}
/* The mapping of /proc/self/maps that holds a: its file name and the file offset of a. Only open/read: safe in a handler,
 * and current (a module loaded after the last snapshot). */
static int maps_line(const char *l, uint64_t a, char *name, size_t cap, uint64_t *off) {
    uint64_t v[3] = {0, 0, 0};
    for (int f = 0; f < 3; f++) {                        /* lo-hi perms offset */
        while (*l == ' ') l++;
        if (f == 2) { while (*l && *l != ' ') l++; while (*l == ' ') l++; }      /* skip the permissions */
        int d = 0;
        for (;; l++, d++) {
            unsigned c = (unsigned char)*l, x = c - '0' < 10 ? c - '0' : (c | 32) - 'a' < 6 ? (c | 32) - 'a' + 10 : 99;
            if (x > 15) break;
            v[f] = v[f] << 4 | x;
        }
        if (!d) return 0;
        if (f == 0 && *l++ != '-') return 0;
    }
    if (a < v[0] || a >= v[1]) return 0;
    for (int f = 0; f < 2; f++) { while (*l == ' ') l++; while (*l && *l != ' ') l++; }   /* device, inode */
    while (*l == ' ') l++;
    if (*l) copy_base(name, cap, l); else copy_base(name, cap, "[anonymous]");
    *off = a - v[0] + v[2];
    return 1;
}
static int maps_lookup(uint64_t a, char *name, size_t cap, uint64_t *off) {
    static char buf[4096], line[600];
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC), found = 0;
    if (fd < 0) return 0;
    size_t ln = 0;
    while (!found) {
        ssize_t k = read(fd, buf, sizeof buf);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        for (ssize_t i = 0; i < k && !found; i++) {
            if (buf[i] != '\n') { if (ln < sizeof line - 1) line[ln++] = buf[i]; continue; }
            line[ln] = 0; ln = 0;
            found = maps_line(line, a, name, cap, off);
        }
    }
    close(fd);
    return found;
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
    crash_modules_refresh();                             /* a new thread often follows new modules (drivers, audio): cheap when none */
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
static const int g_sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT};
#define NSIGS (int)(sizeof g_sigs / sizeof g_sigs[0])
static struct sigaction g_prev[NSIGS];                    /* what was installed before us: chained to after the report */
static crash_report_fn g_report;
static crash_section_fn g_section;
static int g_exit_code = 4, g_end_code = -1;
static int g_timeout = 20;                                /* SSBU_CRASH_TIMEOUT, read at install */
static int g_core;                                        /* SSBU_CORE=1, read at install: end by the signal (core dump) */
static int g_file_set, g_file_written;                    /* a report file path is set; this crash's copy went there */
static int g_spare_fd = -1;                               /* /dev/null, closed to open the report file when out of descriptors */
static char g_file_path[4096];
static char g_build[512];                                 /* "<exe>, <size> bytes, modified ..., started ..., process ..." */
enum { PH_NONE, PH_REPORT, PH_PART, PH_CALLBACK, PH_END };
static volatile sig_atomic_t g_phase;                     /* the owner's step: what a fault inside the report does */
static sigjmp_buf g_rep_jb, g_part_jb, g_end_jb;
static crash_info g_ci;                                   /* only the owner writes it */
static crash_out g_out;
static int g_tid, g_cpu;
static int64_t g_now, g_up;                              /* the crash's time (both copies say the same) */
static char g_comm[17];
static const siginfo_t *g_si;

static void raw(const char *s) { ssize_t r = write(2, s, strlen(s)); (void)r; }
static int exit_code(void) { return g_end_code >= 0 ? g_end_code : __atomic_load_n(&g_exit_code, __ATOMIC_RELAXED); }
/* The process ends: with SSBU_CORE=1 and core dumps on (RLIMIT_CORE, read now), by sig's default action, so a core is written and
 * the exit status names the signal; otherwise (and for a software exception, sig 0) _exit(code) as the Windows build's
 * ExitProcess(4). Not by default: SteamOS has RLIMIT_CORE unlimited and pipes every core to steamos-log-submitter (a minidump for
 * its crash reports) and systemd-coredump, and a core of this process holds its committed guest memory and stacks. */
__attribute__((noreturn)) static void die(int sig, int code) {
    struct rlimit rl;
    if (sig > 0 && __atomic_load_n(&g_core, __ATOMIC_RELAXED) && syscall(SYS_prlimit64, 0, RLIMIT_CORE, NULL, &rl) == 0 && rl.rlim_cur != 0) {
        struct sigaction d;
        memset(&d, 0, sizeof d);
        d.sa_handler = SIG_DFL;
        sigaction(sig, &d, NULL);
        sigset_t s;
        sigemptyset(&s); sigaddset(&s, sig);
        pthread_sigmask(SIG_UNBLOCK, &s, NULL);
        raise(sig);
    }
    _exit(code);
}
static void on_alarm(int sig) {
    (void)sig;
    static int again;
    if (!__atomic_exchange_n(&again, 1, __ATOMIC_ACQ_REL)) {
        alarm(2);                                         /* if even this write blocks (a full stderr pipe), the next alarm ends it */
        raw("\ncrash report: timed out (a lock held by the crashed code?); exiting\n");
    }
    die(g_ci.sig, exit_code());
}
/* This thread now owns the report; otherwise does not return (a fault inside the report continues with the next part or ends the
 * process; a second crashing thread waits for the first to end the process). */
static void take_report(void) {
    int me = (int)syscall(SYS_gettid), prev = 0;
    if (__atomic_compare_exchange_n(&g_owner, &prev, me, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_alarm;
        sa.sa_flags = SA_NODEFER;
        sigaction(SIGALRM, &sa, NULL);
        sigset_t s;
        sigemptyset(&s); sigaddset(&s, SIGALRM);
        pthread_sigmask(SIG_UNBLOCK, &s, NULL);
        sigemptyset(&s); sigaddset(&s, SIGPIPE);          /* a closed stdout: EPIPE, not death before the report file is written */
        pthread_sigmask(SIG_BLOCK, &s, NULL);
        alarm((unsigned)__atomic_load_n(&g_timeout, __ATOMIC_RELAXED));
        return;
    }
    if (prev == me) {
        switch (g_phase) {
        case PH_PART: g_phase = PH_REPORT; siglongjmp(g_part_jb, 1);
        case PH_REPORT: siglongjmp(g_rep_jb, 1);
        case PH_CALLBACK: raw("\nHOST EXCEPTION inside the crash report: ending the process\n"); g_phase = PH_END; siglongjmp(g_end_jb, 1);
        default: raw("\nHOST EXCEPTION inside the crash report: exiting\n"); die(g_ci.sig, exit_code());
        }
    }
    for (;;) pause();
}
/* a guarded part of the report: a fault inside it is reported in the text and the report goes on */
#define PART(o, stmt) do { \
        if (!sigsetjmp(g_part_jb, 0)) { g_phase = PH_PART; stmt; } \
        else crash_put(o, " [fault while writing this]\n"); \
        g_phase = PH_REPORT; \
    } while (0)

static const char *sig_name(int s) {
    switch (s) { case SIGSEGV: return "SIGSEGV"; case SIGBUS: return "SIGBUS"; case SIGILL: return "SIGILL"; case SIGFPE: return "SIGFPE"; case SIGTRAP: return "SIGTRAP"; case SIGABRT: return "SIGABRT"; default: return "signal"; }
}
static const char *code_name(int s, int c) {
    if (c <= 0) return c == SI_USER ? "sent by kill" : c == SI_TKILL ? "sent by tgkill/raise" : "sent by a process";
    switch (s) {
    case SIGSEGV: return c == SEGV_MAPERR ? "SEGV_MAPERR: address not mapped" : c == SEGV_ACCERR ? "SEGV_ACCERR: no permission for this access" : NULL;
    case SIGBUS: return c == BUS_ADRALN ? "BUS_ADRALN: misaligned address" : c == BUS_ADRERR ? "BUS_ADRERR: no such physical address" : c == BUS_OBJERR ? "BUS_OBJERR: object error (file mapping beyond EOF?)" : NULL;
    case SIGILL: return c == ILL_ILLOPC ? "ILL_ILLOPC: illegal opcode" : c == ILL_PRVOPC ? "ILL_PRVOPC: privileged opcode" : c == ILL_ILLTRP ? "ILL_ILLTRP: illegal trap" : NULL;
    case SIGTRAP: return c == TRAP_BRKPT ? "TRAP_BRKPT: brk instruction" : c == TRAP_TRACE ? "TRAP_TRACE" : NULL;
    case SIGFPE: return c == FPE_INTDIV ? "FPE_INTDIV" : c == FPE_FLTDIV ? "FPE_FLTDIV" : c == FPE_FLTINV ? "FPE_FLTINV" : NULL;
    default: return NULL;
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

/* "  module+0x<vaddr>": the snapshot, else /proc/self/maps (file offset) */
static void put_module_of(crash_out *o, uint64_t pc, uint64_t at, int with_symbol) {
    const crash_mod *m = mod_of(__atomic_load_n(&g_mods, __ATOMIC_ACQUIRE), at);
    if (m) {
        crash_put(o, m->name); crash_put(o, "+0x"); crash_put_hex(o, pc - m->base, 0);
        if (with_symbol && m->nsyms) PART(o, put_symbol(o, m, at, pc));
        return;
    }
    char name[64];
    uint64_t off;
    if (maps_lookup(at, name, sizeof name, &off)) { crash_put(o, name); crash_put(o, "+0x"); crash_put_hex(o, off + (pc - at), 0); crash_put(o, " (file offset)"); }
    else crash_put(o, "?");
}
static void put_frames(crash_out *o, const crash_info *ci) {
    crash_put(o, "host call stack (frame pointers, innermost first):\n");
    for (int i = 0; i < ci->npcs; i++) {
        uint64_t pc = ci->pcs[i], at = i || !ci->sig ? pc - 4 : pc;   /* a return address names the instruction after the call */
        crash_put(o, "  #"); put_udec(o, (unsigned)i, -2, ' '); crash_put(o, " "); put_hex16(o, pc); crash_put(o, "  ");
        put_module_of(o, pc, at, 1);
        crash_put(o, i == ci->lr_frame ? "  [lr]\n" : "\n");
    }
}
static void put_stack_words(crash_out *o, uint64_t sp) {   /* 48 words from sp-0x40, as the Windows report; unreadable ones as ? */
    crash_put(o, "host stack from sp-0x40:\n");
    for (int i = 0; i < 48; i++) {
        uint64_t w, a = sp - 0x40 + (uint64_t)i * 8;
        int d = (i - 8) * 8;
        crash_put(o, "  sp"); crash_put(o, d < 0 ? "-" : "+"); put_udec(o, (unsigned)(d < 0 ? -d : d), 3, ' '); crash_put(o, "  ");
        if (crash_read(a, &w, 8)) put_hex16(o, w); else crash_put(o, "????????????????");
        crash_put(o, i % 2 ? "\n" : "");
    }
}
static void put_bid(crash_out *o, const crash_mod *m) {
    if (!m || !m->bidn) { crash_put(o, "none"); return; }
    for (int i = 0; i < m->bidn; i++) crash_put_hex(o, m->bid[i], 2);
}
/* One copy of the report (the file's, then the console's: the same text, the file adds the module list). */
static void report_pass(crash_out *o, const ucontext_t *uc) {
    const crash_info *ci = &g_ci;
    crash_put(o, "\n=== "); put_utc(o, g_now); crash_put(o, " UTC  HOST EXCEPTION ");
    if (ci->sig) {
        const char *cn = code_name(ci->sig, ci->code);
        crash_put(o, ci->what); crash_put(o, " (signal "); crash_put_dec(o, ci->sig); crash_put(o, ", ");
        if (cn) crash_put(o, cn); else { crash_put(o, "si_code "); crash_put_dec(o, ci->code); }
        crash_put(o, ")");
    } else { crash_put(o, "0x"); crash_put_hex(o, ci->exc_code, 8); crash_put(o, " ("); crash_put(o, ci->what ? ci->what : "software exception"); crash_put(o, ")"); }
    crash_put(o, " at host address 0x"); crash_put_hex(o, ci->pc, 0); crash_put(o, " ("); put_module_of(o, ci->pc, ci->pc, 0); crash_put(o, ")\n");
    if (ci->sig && ci->code <= 0 && g_si) { crash_put(o, "  sent by process "); crash_put_dec(o, g_si->si_pid); crash_put(o, " (this is "); crash_put_dec(o, getpid()); crash_put(o, ")\n"); }
    if (ci->sig == SIGSEGV || ci->sig == SIGBUS) {
        crash_put(o, ci->access == 'w' ? "  writing" : ci->access == 'r' ? "  reading" : ci->access == 'x' ? "  executing" : "  accessing");
        crash_put(o, " address 0x"); crash_put_hex(o, ci->addr, 0); crash_put(o, "\n");
        if (ci->addr + (64u << 10) >= ci->sp && ci->addr < ci->sp + (64u << 10)) { crash_put(o, "  (the address is next to the stack pointer 0x"); crash_put_hex(o, ci->sp, 0); crash_put(o, ": probably a stack overflow)\n"); }
    }
#if defined(__aarch64__)
    if (uc) {
        uint32_t insn;
        uint64_t esr = frame_esr(uc);
        if (crash_read(ci->pc, &insn, 4)) {
            crash_put(o, "  instruction at pc: "); crash_put_hex(o, insn, 8); crash_put(o, "\n");
            if ((insn & 0xffe0001fu) == 0xd4200000u) { crash_put(o, "  brk #0x"); crash_put_hex(o, (insn >> 5) & 0xffff, 0); crash_put(o, "\n"); }   /* (no ESR record for brk) */
        } else if (((esr >> 26) & 0x3f) == 0x3c) { crash_put(o, "  brk #0x"); crash_put_hex(o, esr & 0xffff, 0); crash_put(o, "\n"); }
    }
#endif
    crash_put(o, "thread "); crash_put_dec(o, g_tid); crash_put(o, " ("); crash_put(o, g_comm); crash_put(o, "), on logical processor "); crash_put_dec(o, g_cpu);
    crash_put(o, "; process "); crash_put_dec(o, getpid()); crash_put(o, ", up ");
    if (g_start_boot >= 0) put_secs(o, g_up); else crash_put(o, "?");
    crash_put(o, " s\n");
    if (ci->x) {                                         /* a signal: the interrupted code's registers (lr without its PAC bits) */
        crash_put(o, "host registers: pc "); put_hex16(o, ci->pc); crash_put(o, " lr "); put_hex16(o, ci->lr); crash_put(o, " sp "); put_hex16(o, ci->sp);
        crash_put(o, " fp "); put_hex16(o, ci->fp); crash_put(o, " pstate "); put_hex16(o, ci->pstate); crash_put(o, "\n");
        for (int i = 0; i < 31; i++) {
            crash_put(o, "  x"); put_udec(o, (unsigned)i, -2, ' '); crash_put(o, " "); put_hex16(o, ci->x[i]);
            crash_put(o, i % 4 == 3 || i == 30 ? "\n" : "");
        }
        put_stack_words(o, ci->sp);
    }
    put_frames(o, ci);
    crash_section_fn sec = __atomic_load_n(&g_section, __ATOMIC_ACQUIRE);
    if (sec) PART(o, sec(o, ci));
    const mod_table *t = __atomic_load_n(&g_mods, __ATOMIC_ACQUIRE);
    crash_put(o, "build: "); crash_put(o, g_build); crash_put(o, ", build-id "); put_bid(o, t && t->n ? &t->m[0] : NULL); crash_put(o, "\n");
    if (o->file) {
        crash_put(o, "modules (as loaded at up ");
        if (t && g_start_boot >= 0) put_secs(o, t->when - g_start_boot); else crash_put(o, "?");
        crash_put(o, " s; an address outside them was placed with /proc/self/maps):\n");
        for (int i = 0; t && i < t->n; i++) {
            crash_put(o, "  "); put_hex16(o, t->m[i].lo); crash_put(o, "-"); put_hex16(o, t->m[i].hi); crash_put(o, "  build-id ");
            put_bid(o, &t->m[i]); crash_put(o, "  "); crash_put(o, t->m[i].name); crash_put(o, "\n");
        }
        crash_put(o, "=== end of crash report\n");
    } else if (g_file_written) { crash_put(o, "this report, with the module list, is also in "); crash_put(o, g_file_path); crash_put(o, "\n"); }
}
/* The report file, opened (created) now: open(2) is async-signal-safe, and a run that does not crash leaves no file. Out of
 * descriptors: the spare one kept for this is closed first. */
static int open_report_file(void) {
    if (!__atomic_load_n(&g_file_set, __ATOMIC_ACQUIRE)) return -1;
    int fd;
    do fd = open(g_file_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644); while (fd < 0 && errno == EINTR);
    if (fd < 0 && (errno == EMFILE || errno == ENFILE) && g_spare_fd >= 0) {
        close(g_spare_fd);
        g_spare_fd = -1;
        fd = open(g_file_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
    return fd;
}
/* Both copies; a fault in one ends that copy (marked) and the next one is still written. The file first: a console that blocks
 * (a full pipe) cannot cost it. */
static void write_report(const ucontext_t *uc) {
    static volatile int pass, file_fd = -1;
    for (pass = 0; pass < 2; pass++) {
        int fd = pass == 0 ? (file_fd = open_report_file()) : 1;
        if (fd < 0) continue;
        g_out.fd = fd; g_out.file = pass == 0; g_out.n = 0;
        if (!sigsetjmp(g_rep_jb, 0)) { g_phase = PH_REPORT; report_pass(&g_out, uc); }
        else { g_phase = PH_END; crash_put(&g_out, "\n[the crash report faulted here]\n"); }    /* (a fault in what follows: the end) */
        out_flush(&g_out);
        if (pass == 0) { g_file_written = g_out.fd >= 0; close(file_fd); }
    }
}
static void thread_facts(void) {
    g_tid = (int)syscall(SYS_gettid);
    g_cpu = sched_getcpu();
    g_now = clock_ns(CLOCK_REALTIME) / 1000000000;
    g_up = clock_ns(CLOCK_BOOTTIME) - g_start_boot;
    memset(g_comm, 0, sizeof g_comm);
    if (prctl(PR_GET_NAME, g_comm, 0, 0, 0)) memcpy(g_comm, "?", 2);
}
/* The callback (hostrt.c: the run's summaries with stdio, then report_and_exit -> ExitProcess, which comes back here through
 * crash_on_exit). Not async-signal-safe: everything that matters is written before it, and SSBU_CRASH_TIMEOUT bounds it. */
static void run_callback(void) {
    crash_report_fn fn = __atomic_load_n(&g_report, __ATOMIC_ACQUIRE);
    if (fn) fn(&g_ci);
}

/* take_report blocked SIGPIPE, so a console copy to a closed stdout leaves one pending; returning to the previous handler
 * (sigreturn restores the mask) would then end the process by SIGPIPE before the fault re-executes into that handler. */
static void drop_sigpipe(const ucontext_t *uc) {
    sigset_t p, s;
    if (sigismember(&uc->uc_sigmask, SIGPIPE) || sigpending(&p) || !sigismember(&p, SIGPIPE)) return;
    sigemptyset(&s); sigaddset(&s, SIGPIPE);
    struct timespec z = {0, 0};
    sigtimedwait(&s, NULL, &z);
}
static void on_signal(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    int saved_errno = errno;
    take_report();
    crash_info *ci = &g_ci;
    memset(ci, 0, sizeof *ci);
    g_si = si;
    ci->sig = sig; ci->code = si->si_code; ci->what = sig_name(sig);
    ci->addr = (uint64_t)(uintptr_t)si->si_addr;
    ci->lr_frame = -1;
#if defined(__aarch64__)
    const mcontext_t *mc = &uc->uc_mcontext;
    ci->pc = mc->pc; ci->sp = mc->sp; ci->fp = mc->regs[29]; ci->lr = crash_strip_pac(mc->regs[30]); ci->pstate = mc->pstate;
    ci->x = (const uint64_t *)mc->regs;
    uint64_t esr = frame_esr(uc);
    unsigned ec = (unsigned)(esr >> 26) & 0x3f;
    if (ec == 0x24 || ec == 0x25) ci->access = (esr >> 6) & 1 ? 'w' : 'r';
    else if (ec == 0x20 || ec == 0x21) ci->access = 'x';
#endif
    ci->pcs[ci->npcs++] = ci->pc;
    uint64_t chain[CRASH_MAX_FRAMES];
    int n = crash_walk_fp(ci->fp, chain, CRASH_MAX_FRAMES - 2);
    if (ci->lr && ci->lr != ci->pc && (n == 0 || chain[0] != ci->lr)) { ci->lr_frame = ci->npcs; ci->pcs[ci->npcs++] = ci->lr; }   /* a leaf before (or without) its frame record */
    for (int i = 0; i < n && ci->npcs < CRASH_MAX_FRAMES; i++) ci->pcs[ci->npcs++] = chain[i];
    thread_facts();
    write_report(uc);
    g_phase = PH_CALLBACK;
    if (!sigsetjmp(g_end_jb, 0)) run_callback();
    g_phase = PH_END;
    alarm(0);
    /* chain: the handler that was there before us gets the signal as if we had not been installed */
    int k = 0;
    while (k < NSIGS && g_sigs[k] != sig) k++;
    struct sigaction *p = k < NSIGS ? &g_prev[k] : NULL;
    if (p && (p->sa_flags & SA_SIGINFO ? p->sa_sigaction != NULL : p->sa_handler != SIG_DFL && p->sa_handler != SIG_IGN)) {
        sigaction(sig, p, NULL);
        if (si->si_code > 0 && sig != SIGABRT) { drop_sigpipe(uc); errno = saved_errno; return; }    /* a fault: the instruction runs again and faults into it */
        sigset_t s;                                       /* sent (kill, raise, abort): send it again; it runs now */
        sigemptyset(&s); sigaddset(&s, sig);
        pthread_sigmask(SIG_UNBLOCK, &s, NULL);
        raise(sig);
    }
    die(sig, exit_code());
}

__attribute__((noinline, noreturn)) void crash_report_here(uint32_t code, const char *what) {
    take_report();
    crash_info *ci = &g_ci;
    memset(ci, 0, sizeof *ci);
    g_si = NULL;
    ci->exc_code = code; ci->what = what; ci->lr_frame = -1;
    ci->fp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    ci->npcs = crash_walk_fp(ci->fp, ci->pcs, CRASH_MAX_FRAMES);      /* pcs[0] = the return address into our caller */
    ci->pc = ci->npcs ? ci->pcs[0] - 4 : 0;              /* the call instruction */
    thread_facts();
    write_report(NULL);
    g_phase = PH_CALLBACK;
    if (!sigsetjmp(g_end_jb, 0)) run_callback();
    g_phase = PH_END;
    die(0, exit_code());
}

/* ExitProcess (plat_posix.c) on the thread that reports: back to the end of the handler (chain / core dump / exit code). */
void crash_on_exit(int code) {
    if (__atomic_load_n(&g_owner, __ATOMIC_ACQUIRE) == (int)syscall(SYS_gettid) && g_phase == PH_CALLBACK) {
        g_end_code = code;
        g_phase = PH_END;
        siglongjmp(g_end_jb, 1);
    }
}
int crash_set_report_file(const char *path) {
    size_t n = path ? strlen(path) : 0;
    if (!n) return -EINVAL;
    if (n >= sizeof g_file_path) return -ENAMETOOLONG;
    char dir[sizeof g_file_path];                        /* the handler creates the file: its directory must be writable now */
    memcpy(dir, path, n + 1);
    char *s = strrchr(dir, '/');
    if (s) s[s == dir] = 0;
    if (access(s ? dir : ".", W_OK) || (access(path, F_OK) == 0 && access(path, W_OK))) return -errno;
    if (g_spare_fd < 0) g_spare_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    __atomic_store_n(&g_file_set, 0, __ATOMIC_RELEASE);
    memcpy(g_file_path, path, n + 1);
    __atomic_store_n(&g_file_set, 1, __ATOMIC_RELEASE);
    return 0;
}
void crash_set_section(crash_section_fn fn) { __atomic_store_n(&g_section, fn, __ATOMIC_RELEASE); }

/* At install (normal context): the exe's name, size and time, the process start time (for "up ... s"). */
static void build_identity(void) {
    char exe[4096], stat_buf[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = 0;
    copy_base(g_exe_name, sizeof g_exe_name, n > 0 ? exe : "?");
    long tck = sysconf(_SC_CLK_TCK);
    int fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    ssize_t k = fd >= 0 ? read(fd, stat_buf, sizeof stat_buf - 1) : -1;
    if (fd >= 0) close(fd);
    if (k > 0 && tck > 0) {
        stat_buf[k] = 0;
        const char *p = strrchr(stat_buf, ')');
        for (int f = 2; p && f < 22; f++) p = strchr(p + 1, ' ');      /* field 22: starttime, clock ticks after boot */
        if (p) {
            unsigned long long st = strtoull(p + 1, NULL, 10);
            g_start_boot = (int64_t)(st / (unsigned long long)tck) * 1000000000 + (int64_t)(st % (unsigned long long)tck) * (1000000000 / tck);
        }
    }
    struct stat sb;
    char mod[32] = "?", start[32] = "?";
    struct tm tm;
    time_t t0 = time(NULL) - (g_start_boot >= 0 ? (time_t)((clock_ns(CLOCK_BOOTTIME) - g_start_boot) / 1000000000) : 0);
    int have = n > 0 && stat(exe, &sb) == 0;
    if (have && gmtime_r(&sb.st_mtime, &tm)) strftime(mod, sizeof mod, "%Y-%m-%d %H:%M:%S", &tm);
    if (gmtime_r(&t0, &tm)) strftime(start, sizeof start, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(g_build, sizeof g_build, "%s, %lld bytes, modified %s UTC; process %ld started %s UTC", g_exe_name, have ? (long long)sb.st_size : -1ll,
             mod, (long)getpid(), start);
}

void crash_install(crash_report_fn report, int exit_code) {
    __atomic_store_n(&g_exit_code, exit_code, __ATOMIC_RELAXED);
    __atomic_store_n(&g_report, report, __ATOMIC_RELEASE);
    if (__atomic_exchange_n(&g_installed, 1, __ATOMIC_ACQ_REL)) { crash_thread_init(); return; }
    const char *e = getenv("SSBU_CRASH_TIMEOUT");
    if (e && atoi(e) > 0) __atomic_store_n(&g_timeout, atoi(e), __ATOMIC_RELAXED);
    if ((e = getenv("SSBU_CORE")) && atoi(e) > 0) __atomic_store_n(&g_core, 1, __ATOMIC_RELAXED);
    build_identity();
    if (pipe2(g_rd_pipe, O_CLOEXEC | O_NONBLOCK)) g_rd_pipe[0] = g_rd_pipe[1] = -1;
    crash_modules_refresh();
    crash_thread_init();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;    /* NODEFER: a fault inside the report re-enters (take_report) */
    sigemptyset(&sa.sa_mask);
    for (int i = 0; i < NSIGS; i++) sigaction(g_sigs[i], &sa, &g_prev[i]);
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
