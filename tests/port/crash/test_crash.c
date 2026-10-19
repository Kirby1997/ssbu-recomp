/* test_crash.c - plat_crash.c crash reports, each scenario in a child process (this program re-executed with a mode):
 * the child crashes, the parent checks the report text (console and report file) and the exit status.
 *   SEGV / ILL / BRK (SIGTRAP) on the main thread and on a pthread; read of a PROT_NONE vmem page; a fault in a leaf
 *   (the caller comes from lr); stack overflow on a pthread, on a plat CreateThread thread (its alternate stack comes from
 *   plat_posix.c) and on a fiber (caught on the alternate stack); overflow on a thread WITHOUT crash_thread_init (dies of
 *   SIGSEGV, no report: why threads must call it); two threads crashing at once (one report); crash_report_here
 *   (RaiseException replacement); a fault inside the report callback; a callback that hangs (SSBU_CRASH_TIMEOUT); a callback
 *   that picks the exit code (as hostrt.c's report_and_exit, also through ExitProcess).
 *   Report file (crash_set_report_file): SEGV / BUS / ILL / FPE / abort / BRK, a guest-style frame with a report section
 *   (hostrt.c's guest side), a plat thread, a fiber: every field, complete, the same text as the console; a section that
 *   faults; appending.
 *   Async-signal-safety: a crash while another thread holds the main malloc arena's lock (malloc_stats blocked on a full
 *   stderr pipe), and while a thread loops malloc/free.
 *   process_vm_readv blocked by seccomp (crash_read's pipe fallback in the handler).
 *   The end: chaining to the handler installed before (SA_SIGINFO, plain, one that returns; also with stdout a closed pipe),
 *   core dumps (SSBU_CORE=1 and RLIMIT_CORE != 0: the signal's default action; RLIMIT_CORE = 1 so the kernel writes no core),
 *   exit code otherwise (also with RLIMIT_CORE != 0 but no SSBU_CORE). No report file without a crash (also when killed). */
#include "tp.h"
#include "plat_crash.h"
#include <signal.h>
#include <fcntl.h>
#include <malloc.h>
#include <stddef.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define EXIT_CODE 4

/* ---------------------------------------------------------------- child side */
static volatile int g_sink;
static void cb_print(const crash_info *ci) {
    printf("CALLBACK sig=%d exc=%x npcs=%d lr_frame=%d addr=%llx access=%c\n", ci->sig, ci->exc_code, ci->npcs, ci->lr_frame,
           (unsigned long long)ci->addr, ci->access ? ci->access : '-');
    fflush(stdout);
    if (getenv("CB_EXIT")) { fflush(stdout); _exit(atoi(getenv("CB_EXIT"))); }      /* hostrt.c: report_and_exit(4) */
    if (getenv("CB_EXITPROCESS")) ExitProcess((UINT)atoi(getenv("CB_EXITPROCESS")));  /* ... which ends in ExitProcess */
    if (getenv("CB_FAULT")) *(volatile int *)(uintptr_t)0x18 = 1;
    if (getenv("CB_HANG")) for (;;) pause();
    if (getenv("CB_MALLOC")) { void *volatile p = malloc(8192); printf("CB MALLOC %p\n", p); fflush(stdout); }   /* the main arena */
}
__attribute__((noinline)) int crash_leaf_write(volatile int *p) { *p = 1; return 0; }       /* leaf: no frame record (clang) */
__attribute__((noinline)) int crash_mid(volatile int *p) { int r = crash_leaf_write(p); __asm__ volatile("" ::: "memory"); return r + 1; }
__attribute__((noinline)) int crash_outer(volatile int *p) { int r = crash_mid(p); __asm__ volatile("" ::: "memory"); return r + 1; }
__attribute__((noinline)) void do_ill(void) { __asm__ volatile("udf #0"); }
__attribute__((noinline)) void do_brk(void) { __asm__ volatile("brk #0x7"); }
__attribute__((noinline)) int do_read(volatile int *p) { return *p; }
__attribute__((noinline)) int do_read_byte(volatile char *p) { return *p; }
__attribute__((noinline)) void do_fpe(void) { raise(SIGFPE); __asm__ volatile("" ::: "memory"); }   /* A64 integer division does not trap */
__attribute__((noinline)) void do_abort(void) { abort(); }
__attribute__((noinline)) int recurse(int d) {
    volatile char buf[1024];
    buf[0] = (char)d;
    int r = recurse(d + 1);
    __asm__ volatile("" ::: "memory");
    return r + buf[0];
}
__attribute__((noinline)) void trap_mid(void) { crash_report_here(0xE0000001, "guest trap"); }
__attribute__((noinline)) void trap_outer(void) { trap_mid(); __asm__ volatile("" ::: "memory"); }

/* A guest-style frame: "lifted" functions taking a context, the context of the running guest code in a thread variable
 * (hostrt.c t_cur_ctx), and a report section that names guest functions from host pcs (hostrt.c crash_guest_section). */
typedef struct { uint64_t x[32]; uint64_t fn; } FakeCtx;
static __thread FakeCtx *volatile t_ctx;          /* volatile: the store before the call is not dead (IPA sees the callees) */
__attribute__((noinline)) void lifted_7100001000(FakeCtx *c) { c->fn = 0x7100001000; *(volatile uint64_t *)(uintptr_t)c->x[1] = c->x[0]; __asm__ volatile("" ::: "memory"); }
__attribute__((noinline)) void lifted_7100002000(FakeCtx *c) { c->fn = 0x7100002000; c->x[0] = 0x1234; c->x[1] = 0x48; lifted_7100001000(c); __asm__ volatile("" ::: "memory"); }
static uint64_t test_guest_of(uint64_t pc) {
    const uint64_t f[2][2] = {{(uint64_t)(uintptr_t)lifted_7100001000, 0x7100001000}, {(uint64_t)(uintptr_t)lifted_7100002000, 0x7100002000}};
    uint64_t best = 0, g = 0;
    for (int i = 0; i < 2; i++) if (f[i][0] <= pc && pc - f[i][0] < 0x200 && f[i][0] > best) { best = f[i][0]; g = f[i][1]; }
    return g;
}
static int g_section_fault;
static void test_section(crash_out *o, const crash_info *ci) {
    crash_put(o, "guest thread: test\n");
    if (g_section_fault) *(volatile int *)(uintptr_t)0x20 = 1;
    FakeCtx *c = t_ctx;
    uint64_t x[32], fn;
    if (c && crash_read((uint64_t)(uintptr_t)c->x, x, sizeof x) && crash_read((uint64_t)(uintptr_t)&c->fn, &fn, 8)) {
        crash_put(o, "last guest function entered: f_"); crash_put_hex(o, fn, 0); crash_put(o, "\nguest registers:\n");
        for (int i = 0; i < 31; i++) { crash_put(o, "  x"); crash_put_dec(o, i); crash_put(o, i < 10 ? "  " : " "); crash_put_hex(o, x[i], 16); crash_put(o, i % 3 == 2 ? "\n" : ""); }
        crash_put(o, "  sp  "); crash_put_hex(o, x[31], 16); crash_put(o, "\n");
    }
    crash_put(o, "guest call stack (innermost first):\n");
    uint64_t last = 0;
    for (int i = 0; i < ci->npcs; i++) {
        uint64_t g = test_guest_of(ci->pcs[i] - (ci->sig && i == 0 ? 0 : 4));
        if (g && g != last) { crash_put(o, "  f_"); crash_put_hex(o, g, 0); crash_put(o, "\n"); last = g; }
    }
}

static const char *g_mode;
static pthread_barrier_t g_bar;
__attribute__((noinline)) void run_mode(const char *m) {
    if (!strcmp(m, "segv")) crash_outer((volatile int *)(uintptr_t)0x1234);
    else if (!strcmp(m, "ill")) do_ill();
    else if (!strcmp(m, "brk")) do_brk();
    else if (!strcmp(m, "fpe")) do_fpe();
    else if (!strcmp(m, "abort")) do_abort();
    else if (!strcmp(m, "read")) {
        uint8_t *p = vmem_alloc(0x20000, PROT_READ | PROT_WRITE);
        vmem_protect(p + 0x10000, 0x10000, PROT_NONE, NULL);
        printf("PROTADDR %llx\n", (unsigned long long)(uintptr_t)(p + 0x10008)); fflush(stdout);
        g_sink = do_read((volatile int *)(p + 0x10008));
    } else if (!strcmp(m, "bus")) {                       /* a shared file mapping beyond the end of the file */
        char path[] = "/tmp/test_crash_busXXXXXX";
        int fd = mkstemp(path);
        unlink(path);
        if (fd < 0 || ftruncate(fd, 65536)) { printf("bus setup failed\n"); exit(98); }
        volatile char *p = mmap(NULL, 65536, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED || ftruncate(fd, 0)) { printf("bus setup failed\n"); exit(98); }
        printf("BUSADDR %llx\n", (unsigned long long)(uintptr_t)(p + 8192)); fflush(stdout);
        g_sink = do_read_byte(p + 8192);
    } else if (!strcmp(m, "overflow")) g_sink = recurse(0);
    else if (!strcmp(m, "trap")) trap_outer();
    else if (!strcmp(m, "kill")) raise(SIGKILL);              /* ended from outside: no atexit, no handler */
    else if (!strcmp(m, "both")) { pthread_barrier_wait(&g_bar); crash_outer((volatile int *)(uintptr_t)0x40); }
    else if (!strcmp(m, "guest")) { static FakeCtx c; t_ctx = &c; c.x[31] = 0x7ff0001000; lifted_7100002000(&c); t_ctx = NULL; }
}
__attribute__((noinline)) void *thread_entry(void *arg) {
    if (!arg) crash_thread_init();                       /* what os_threads.c / plat_posix.c thread starts must do */
    run_mode(g_mode);
    return NULL;
}
DWORD WINAPI plat_entry(LPVOID arg) { (void)arg; run_mode(g_mode); return 0; }    /* no crash_thread_init: plat_posix.c's */
static plat_fiber *g_main_fiber;
void fiber_entry(void *arg) { (void)arg; run_mode(g_mode); plat_fiber_switch(NULL, g_main_fiber); }
__attribute__((noinline)) void *fiber_thread(void *arg) {
    (void)arg;
    crash_thread_init();
    g_main_fiber = plat_fiber_convert_thread(NULL);
    plat_fiber *f = plat_fiber_create(256 << 10, fiber_entry, NULL);
    plat_fiber_switch(g_main_fiber, f);
    return NULL;
}

/* handlers installed before crash_install, for the chaining cases */
static void wr(const char *s) { ssize_t r = write(1, s, strlen(s)); (void)r; }
static void prev_info(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    char b[64] = "PREV HANDLER sig=00 code=0\n";
    b[17] = (char)('0' + sig / 10); b[18] = (char)('0' + sig % 10); b[25] = (char)('0' + (si->si_code > 0 && si->si_code < 10 ? si->si_code : 0));
    wr(b);
    _exit(42);
}
static void prev_plain(int sig) { (void)sig; wr("PREV PLAIN\n"); _exit(43); }
static void prev_quiet(int sig, siginfo_t *si, void *uc) { (void)sig; (void)uc; _exit(si->si_code == SEGV_MAPERR ? 42 : 41); }   /* stdout may be a closed pipe */
static void prev_ret(int sig) { (void)sig; wr("PREV RETURNED\n"); }

/* holds the main arena's lock: malloc_stats() writes to stderr under it, and stderr is a full pipe */
static void *arena_holder(void *a) { (void)a; malloc_stats(); return NULL; }
static volatile int g_probe_go, g_probe_done;
static void *arena_probe(void *a) {                       /* started before the lock is taken: pthread_create mallocs (the DTV) */
    (void)a;
    while (!g_probe_go) usleep(1000);
    struct mallinfo2 mi = mallinfo2();                    /* takes every arena's lock */
    g_sink = (int)mi.arena; g_probe_done = 1;
    return NULL;
}
static void *malloc_spin(void *a) {
    (void)a;
    for (unsigned i = 1;; i = i * 1103515245u + 12345u) {
        size_t n = 16 + (i >> 8) % 70000;
        char *volatile p = malloc(n);
        if (p) { p[0] = 1; p[n - 1] = 2; }
        free(p);
    }
    return NULL;
}

__attribute__((noinline)) int child(const char *where, const char *mode) {
    const char *e;
    if ((e = getenv("CORE"))) { struct rlimit rl; getrlimit(RLIMIT_CORE, &rl); rl.rlim_cur = (rlim_t)atoi(e); setrlimit(RLIMIT_CORE, &rl); }
    if ((e = getenv("PREV"))) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sigemptyset(&sa.sa_mask);
        if (!strcmp(e, "segv")) { sa.sa_sigaction = prev_info; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL); }
        else if (!strcmp(e, "segvq")) { sa.sa_sigaction = prev_quiet; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL); }
        else if (!strcmp(e, "abrt")) { sa.sa_handler = prev_plain; sigaction(SIGABRT, &sa, NULL); }
        else if (!strcmp(e, "fpe")) { sa.sa_handler = prev_ret; sigaction(SIGFPE, &sa, NULL); }
    }
    if (getenv("NOPVR")) {                               /* a sandbox without process_vm_readv: EPERM */
        struct sock_filter f[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_process_vm_readv, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        struct sock_fprog prog = {(unsigned short)(sizeof f / sizeof f[0]), f};
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) { printf("seccomp failed\n"); return 95; }
        char b[8];
        struct iovec l = {b, 8}, r = {b, 8};
        printf("PVR %s\n", process_vm_readv(getpid(), &l, 1, &r, 1, 0) < 0 && errno == EPERM ? "BLOCKED" : "OPEN");
    }
    vmem_init();
    crash_install(getenv("NOCB") ? NULL : cb_print, EXIT_CODE);
    if ((e = getenv("REPORT")) && crash_set_report_file(e)) { printf("cannot open %s\n", e); return 97; }
    if ((e = getenv("SECTION"))) { g_section_fault = atoi(e) == 2; crash_set_section(test_section); }
    g_mode = mode;
    if (!getenv("QUIET")) { printf("CHILD %s %s\n", where, mode); fflush(stdout); }      /* also gives stdout its buffer before any lock is taken */
    if (getenv("MALLOC_HOLD")) {
        int p[2];
        if (pipe(p)) return 96;
        fcntl(p[1], F_SETFL, O_NONBLOCK);
        char junk[512];
        memset(junk, 'j', sizeof junk);
        while (write(p[1], junk, sizeof junk) > 0) {}
        while (write(p[1], junk, 1) > 0) {}
        fcntl(p[1], F_SETFL, 0);
        dup2(p[1], 2);                                    /* stderr: full, nobody reads */
        pthread_t h, q;
        pthread_create(&q, NULL, arena_probe, NULL);
        pthread_create(&h, NULL, arena_holder, NULL);
        usleep(200000);
        g_probe_go = 1;
        usleep(200000);
        printf("ARENA %s\n", g_probe_done ? "FREE" : "LOCKED"); fflush(stdout);
    }
    if (getenv("MALLOC_SPIN")) { pthread_t s; pthread_create(&s, NULL, malloc_spin, NULL); usleep(20000); }
    pthread_t t, t2;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 << 10);
    if (!strcmp(where, "main")) run_mode(mode);
    else if (!strcmp(where, "thread")) { pthread_create(&t, &at, thread_entry, NULL); pthread_join(t, NULL); }
    else if (!strcmp(where, "noalt")) { pthread_create(&t, &at, thread_entry, (void *)1); pthread_join(t, NULL); }
    else if (!strcmp(where, "fiber")) { pthread_create(&t, &at, fiber_thread, NULL); pthread_join(t, NULL); }
    else if (!strcmp(where, "plat")) { HANDLE h = CreateThread(NULL, 256 << 10, plat_entry, NULL, 0, NULL); WaitForSingleObject(h, INFINITE); }
    else if (!strcmp(where, "two")) {
        pthread_barrier_init(&g_bar, NULL, 2);
        pthread_create(&t, &at, thread_entry, NULL); pthread_create(&t2, &at, thread_entry, NULL);
        pthread_join(t, NULL); pthread_join(t2, NULL);
    }
    printf("NO CRASH\n");
    return 0;
}

/* ---------------------------------------------------------------- parent side */
typedef struct { int status; int64_t us; char out[65536]; char file[262144]; } Run;
static char g_dir[256], g_report[300];
/* env: "K=V K2=V2"; REPORT is set to g_report (removed first) */
static void run(Run *r, const char *where, const char *mode, const char *env) {
    int fd[2];
    if (pipe(fd)) { perror("pipe"); exit(2); }
    unlink(g_report);
    fflush(stdout);
    int64_t t0 = tp_now_us();
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fd[1], 1); dup2(fd[1], 2); close(fd[0]); close(fd[1]);
        setenv("REPORT", g_report, 1);
        if (env) {
            char e[512];
            snprintf(e, sizeof e, "%s", env);
            for (char *s = strtok(e, " "); s; s = strtok(NULL, " ")) { char *eq = strchr(s, '='); *eq = 0; setenv(s, eq + 1, 1); }
        }
        execl("/proc/self/exe", "test_crash", "--child", where, mode, (char *)NULL);
        _exit(99);
    }
    close(fd[1]);
    size_t n = 0;
    ssize_t k;
    while ((k = read(fd[0], r->out + n, sizeof r->out - 1 - n)) > 0) n += (size_t)k;
    r->out[n] = 0;
    close(fd[0]);
    waitpid(pid, &r->status, 0);
    r->us = tp_now_us() - t0;
    r->file[0] = 0;
    int f = open(g_report, O_RDONLY);
    if (f >= 0) { n = 0; while ((k = read(f, r->file + n, sizeof r->file - 1 - n)) > 0) n += (size_t)k; r->file[n] = 0; close(f); }
}
static int exited(const Run *r, int code) { return WIFEXITED(r->status) && WEXITSTATUS(r->status) == code; }
static int signaled(const Run *r, int sig) { return WIFSIGNALED(r->status) && WTERMSIG(r->status) == sig; }
static int has(const Run *r, const char *s) { return strstr(r->out, s) != NULL; }
static int fhas(const Run *r, const char *s) { return strstr(r->file, s) != NULL; }
/* the names appear in this order in the report */
static int in_order_s(const char *t, const char *a, const char *b, const char *c) {
    const char *pa = strstr(t, a), *pb = pa ? strstr(pa, b) : NULL, *pc = !c ? pb : pb ? strstr(pb, c) : NULL;
    return pa && pb && pc;
}
static int in_order(const Run *r, const char *a, const char *b, const char *c) { return in_order_s(r->out, a, b, c); }
static int count_s(const char *t, const char *s) { int n = 0; for (const char *p = t; (p = strstr(p, s)); p++) n++; return n; }
static int count(const Run *r, const char *s) { return count_s(r->out, s); }
static void show(const char *name, const Run *r) {
    if (getenv("VERBOSE")) printf("------ %s (status %#x, %lld ms)\n%s------ file:\n%s------\n", name, r->status, (long long)(r->us / 1000), r->out, r->file);
}
#define SHOWFAIL(name, r) do { if (tp_fail != fails_before) printf("------ %s output (status %#x):\n%s------ file:\n%s------\n", name, (r)->status, (r)->out, (r)->file); } while (0)

/* The report file of one crash: every field, complete, and the console copy is the same text (without the module list). */
static void check_file(const Run *r, const char *head) {
    CHECKF(r->file[0], "no report file");
    CHECK(count_s(r->file, " UTC  HOST EXCEPTION ") == 1);
    CHECK(fhas(r, head));
    CHECK(fhas(r, "\nthread ") && fhas(r, "), on logical processor ") && fhas(r, "; process ") && fhas(r, ", up "));
    CHECK(fhas(r, "host call stack (frame pointers, innermost first):\n  #0  "));
    CHECK(fhas(r, "\nbuild: test_crash, ") && fhas(r, " bytes, modified ") && fhas(r, " started ") && fhas(r, ", build-id "));
    CHECK(fhas(r, "\nmodules (as loaded at up "));
    CHECK(fhas(r, "  test_crash\n"));
    {   const char *l = strstr(r->file, "  libc.so.6\n"), *b = l ? l : r->file;      /* libc with its 20-byte build-id */
        while (b > r->file && b[-1] != '\n') b--;
        const char *id = strstr(b, "build-id ");
        int hex = 0;
        while (id && id + 9 + hex < l && strchr("0123456789abcdef", id[9 + hex]) && id[9 + hex]) hex++;
        CHECKF(l && hex == 40, "libc module line without a build-id (%d hex digits)", hex); }
    size_t n = strlen(r->file);
    CHECKF(n > 24 && !strcmp(r->file + n - 24, "=== end of crash report\n"), "the file does not end with the end marker");
    CHECK(count_s(r->file, "=== end of crash report") == 1);
    /* console = file up to the module list */
    const char *fs = r->file, *fe = strstr(r->file, "modules (as loaded at up ");
    const char *cs = strstr(r->out, "\n=== "), *ce = cs ? strstr(cs, "this report, with the module list, is also in ") : NULL;
    CHECKF(fe && cs && ce && fe - fs == ce - cs && !memcmp(fs, cs, (size_t)(fe - fs)), "console and file copies differ");
    CHECK(ce && !strncmp(ce + 46, g_report, strlen(g_report)));
}
static void check_regs(const Run *r) {
    CHECK(fhas(r, "host registers: pc ") && fhas(r, " pstate "));
    CHECK(fhas(r, "\n  x0  ") && fhas(r, "  x29 ") && fhas(r, "  x30 "));
    CHECK(fhas(r, "host stack from sp-0x40:\n  sp- 64  "));
    CHECK(fhas(r, "  sp+312  "));
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--child")) return child(argv[2], argv[3]);
    static Run r;
    int fails_before;
    {   struct rlimit rl;                                /* core dumps off unless a case turns them on (CORE=1) */
        getrlimit(RLIMIT_CORE, &rl); rl.rlim_cur = 0; setrlimit(RLIMIT_CORE, &rl); }
    snprintf(g_dir, sizeof g_dir, "%s/test_crash_XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 2; }
    snprintf(g_report, sizeof g_report, "%s/test_crash_crash.txt", g_dir);

    const char *wheres[] = {"main", "thread"};
    for (int w = 0; w < 2; w++) {
        char name[64];
        snprintf(name, sizeof name, "SIGSEGV write on %s", wheres[w]); SECTION(name);
        fails_before = tp_fail;
        run(&r, wheres[w], "segv", NULL); show(name, &r);
        CHECK(exited(&r, EXIT_CODE));
        CHECK(has(&r, "HOST EXCEPTION SIGSEGV (signal 11, SEGV_MAPERR: address not mapped) at host address 0x"));
        CHECK(has(&r, "  writing address 0x1234\n"));
        CHECK(has(&r, "host registers: pc "));
        CHECK(has(&r, "host call stack (frame pointers, innermost first):"));
        CHECK(in_order(&r, "crash_leaf_write", "crash_mid", "crash_outer"));
        CHECK(w == 0 ? has(&r, "(main+0x") || has(&r, "(child+0x") : has(&r, "(thread_entry+0x"));   /* the walk reaches the entry (clang tail-calls child) */
        CHECK(has(&r, "CALLBACK sig=11 exc=0"));
        CHECK(has(&r, "addr=1234 access=w"));
        CHECK(in_order(&r, "HOST EXCEPTION", "this report, with the module list, is also in", "CALLBACK"));   /* the report, then the callback */
        check_file(&r, "HOST EXCEPTION SIGSEGV (signal 11, SEGV_MAPERR: address not mapped) at host address 0x");
        check_regs(&r);
        CHECK(fhas(&r, " (test_crash+0x"));                 /* header: the module of the pc */
        SHOWFAIL(name, &r);

        snprintf(name, sizeof name, "SIGILL (udf) on %s", wheres[w]); SECTION(name);
        fails_before = tp_fail;
        run(&r, wheres[w], "ill", NULL); show(name, &r);
        CHECK(exited(&r, EXIT_CODE));
        CHECK(has(&r, "HOST EXCEPTION SIGILL (signal 4, ILL_ILLOPC: illegal opcode)"));
        CHECK(has(&r, "instruction at pc: 00000000"));
        CHECK(in_order(&r, "(do_ill+0x", w == 0 ? "(child+0x" : "(thread_entry+0x", NULL) || in_order(&r, "(do_ill+0x", "(main+0x", NULL));   /* run_mode may tail-call it (clang) */
        CHECK(has(&r, "CALLBACK sig=4"));
        check_file(&r, "HOST EXCEPTION SIGILL (signal 4, ILL_ILLOPC: illegal opcode)");
        check_regs(&r);
        SHOWFAIL(name, &r);

        snprintf(name, sizeof name, "SIGTRAP (brk) on %s", wheres[w]); SECTION(name);
        fails_before = tp_fail;
        run(&r, wheres[w], "brk", NULL); show(name, &r);
        CHECK(exited(&r, EXIT_CODE));
        CHECK(has(&r, "HOST EXCEPTION SIGTRAP (signal 5, TRAP_BRKPT: brk instruction)"));
        CHECK(has(&r, "  brk #0x7\n"));
        CHECK(has(&r, "instruction at pc: d42000e0"));
        CHECK(in_order(&r, "(do_brk+0x", w == 0 ? "(child+0x" : "(thread_entry+0x", NULL) || in_order(&r, "(do_brk+0x", "(main+0x", NULL));
        check_file(&r, "HOST EXCEPTION SIGTRAP (signal 5, TRAP_BRKPT: brk instruction)");
        SHOWFAIL(name, &r);
    }

    SECTION("SIGBUS: shared file mapping beyond the end of the file");
    fails_before = tp_fail;
    run(&r, "main", "bus", NULL); show("bus", &r);
    {   unsigned long long a = 0; const char *p = strstr(r.out, "BUSADDR ");
        char want[64] = "?";
        if (p && sscanf(p, "BUSADDR %llx", &a) == 1) snprintf(want, sizeof want, "  reading address 0x%llx\n", a);
        CHECK(exited(&r, EXIT_CODE));
        check_file(&r, "HOST EXCEPTION SIGBUS (signal 7, BUS_ADRERR: no such physical address)");
        check_regs(&r);
        CHECK(fhas(&r, want));
        CHECK(in_order_s(r.file, "(do_read_byte+0x", "(run_mode+0x", "(child+0x")); }
    SHOWFAIL("bus", &r);

    SECTION("SIGFPE (raise: A64 integer division does not trap)");
    fails_before = tp_fail;
    run(&r, "thread", "fpe", NULL); show("fpe", &r);
    CHECK(exited(&r, EXIT_CODE));
    check_file(&r, "HOST EXCEPTION SIGFPE (signal 8, sent by tgkill/raise)");
    check_regs(&r);
    CHECK(fhas(&r, "  sent by process "));
    CHECK(in_order_s(r.file, "(do_fpe+0x", "(thread_entry+0x", NULL));
    SHOWFAIL("fpe", &r);

    SECTION("abort() (SIGABRT)");
    fails_before = tp_fail;
    run(&r, "main", "abort", NULL); show("abort", &r);
    CHECK(exited(&r, EXIT_CODE));
    check_file(&r, "HOST EXCEPTION SIGABRT (signal 6, sent by tgkill/raise)");
    check_regs(&r);
    CHECK(in_order_s(r.file, "(abort+0x", "(do_abort+0x", "(child+0x"));
    CHECK(has(&r, "CALLBACK sig=6"));
    SHOWFAIL("abort", &r);

    SECTION("guest-style frame: the report section (guest registers, guest call stack)");
    fails_before = tp_fail;
    run(&r, "main", "guest", "SECTION=1"); show("guest", &r);
    CHECK(exited(&r, EXIT_CODE));
    check_file(&r, "HOST EXCEPTION SIGSEGV (signal 11, SEGV_MAPERR: address not mapped)");
    check_regs(&r);
    CHECK(fhas(&r, "  writing address 0x48\n"));
    CHECK(fhas(&r, "guest thread: test\nlast guest function entered: f_7100001000\nguest registers:\n  x0  0000000000001234  x1  0000000000000048"));
    CHECK(fhas(&r, "  sp  0000007ff0001000\n"));
    CHECK(fhas(&r, "guest call stack (innermost first):\n  f_7100001000\n  f_7100002000\nbuild: "));
    CHECK(in_order_s(r.file, "(lifted_7100001000+0x", "(lifted_7100002000+0x", "guest call stack"));
    SHOWFAIL("guest", &r);

    SECTION("process_vm_readv blocked (seccomp): memory is read through crash_read's pipe");
    fails_before = tp_fail;
    run(&r, "main", "guest", "SECTION=1 NOPVR=1"); show("nopvr", &r);
    CHECK(has(&r, "PVR BLOCKED"));
    CHECK(exited(&r, EXIT_CODE));
    check_file(&r, "HOST EXCEPTION SIGSEGV (signal 11, SEGV_MAPERR: address not mapped)");
    check_regs(&r);
    CHECK(!fhas(&r, "????????????????"));                 /* the stack words were readable */
    CHECK(fhas(&r, "  instruction at pc: "));
    CHECK(fhas(&r, "last guest function entered: f_7100001000\nguest registers:\n  x0  0000000000001234"));
    CHECK(in_order_s(r.file, "(lifted_7100001000+0x", "(lifted_7100002000+0x", "(run_mode+0x"));
    SHOWFAIL("nopvr", &r);

    SECTION("a fault inside the report section: marked, the report goes on");
    fails_before = tp_fail;
    run(&r, "main", "guest", "SECTION=2"); show("section fault", &r);
    CHECK(exited(&r, EXIT_CODE));
    check_file(&r, "HOST EXCEPTION SIGSEGV");
    CHECK(fhas(&r, "guest thread: test\n [fault while writing this]\nbuild: "));
    CHECK(has(&r, "CALLBACK sig=11"));
    SHOWFAIL("section fault", &r);

    SECTION("read of a PROT_NONE vmem page (SEGV_ACCERR, reading)");
    fails_before = tp_fail;
    run(&r, "thread", "read", NULL); show("read", &r);
    {   unsigned long long a = 0; const char *p = strstr(r.out, "PROTADDR ");
        char want[64] = "?";
        if (p && sscanf(p, "PROTADDR %llx", &a) == 1) snprintf(want, sizeof want, "  reading address 0x%llx\n", a);
        CHECK(exited(&r, EXIT_CODE));
        CHECK(has(&r, "SEGV_ACCERR"));
        CHECK(has(&r, want));
        CHECK(in_order(&r, "(do_read+0x", "(run_mode+0x", "(thread_entry+0x")); }
    SHOWFAIL("read", &r);

    SECTION("fault in a leaf: the caller comes from lr");
    fails_before = tp_fail;
    run(&r, "main", "segv", NULL);
    {   const char *leaf = strstr(r.out, "crash_leaf_write"), *nl = leaf ? strchr(leaf, '\n') : NULL;
        const char *mid = nl ? strstr(nl, "crash_mid") : NULL, *mid_nl = mid ? strchr(mid, '\n') : NULL;
        int lr_tag = mid && mid_nl && (size_t)(mid_nl - mid) < 40 && memmem(mid, (size_t)(mid_nl - mid), "[lr]", 4);
        int frame_leaf = strstr(r.out, "lr_frame=-1") != NULL;       /* gcc may give the leaf a frame record: then no lr entry */
        CHECKF(lr_tag || frame_leaf, "crash_mid neither tagged [lr] nor reached through a frame record"); }
    SHOWFAIL("leaf", &r);

    SECTION("stack overflow on a pthread (256 KiB stack): caught on the alternate stack");
    fails_before = tp_fail;
    run(&r, "thread", "overflow", NULL); show("overflow", &r);
    CHECK(exited(&r, EXIT_CODE));
    CHECK(has(&r, "HOST EXCEPTION SIGSEGV"));
    CHECK(has(&r, "probably a stack overflow"));
    CHECK(count(&r, "(recurse+0x") >= 20);
    CHECK(has(&r, "CALLBACK sig=11"));
    SHOWFAIL("overflow", &r);

    SECTION("stack overflow on a plat CreateThread thread (alternate stack from plat_posix.c thread_start)");
    fails_before = tp_fail;
    run(&r, "plat", "overflow", NULL); show("plat overflow", &r);
    CHECK(exited(&r, EXIT_CODE));
    check_file(&r, "HOST EXCEPTION SIGSEGV");
    check_regs(&r);
    CHECK(fhas(&r, "probably a stack overflow"));
    CHECK(count_s(r.file, "(recurse+0x") >= 20);
    CHECK(has(&r, "CALLBACK sig=11"));
    SHOWFAIL("plat overflow", &r);

    SECTION("SIGSEGV on a plat CreateThread thread");
    fails_before = tp_fail;
    run(&r, "plat", "segv", NULL); show("plat segv", &r);
    CHECK(exited(&r, EXIT_CODE));
    check_file(&r, "HOST EXCEPTION SIGSEGV");
    CHECK(in_order_s(r.file, "(crash_mid+0x", "(plat_entry+0x", NULL));     /* (run_mode tail-calls with clang) */
    SHOWFAIL("plat segv", &r);

    SECTION("stack overflow on a fiber (plat_fiber_create guard page)");
    fails_before = tp_fail;
    run(&r, "fiber", "overflow", NULL); show("fiber overflow", &r);
    CHECK(exited(&r, EXIT_CODE));
    CHECK(has(&r, "probably a stack overflow"));
    CHECK(has(&r, "(recurse+0x"));
    CHECK(has(&r, "CALLBACK sig=11"));
    check_file(&r, "HOST EXCEPTION SIGSEGV");
    SHOWFAIL("fiber overflow", &r);

    SECTION("SIGSEGV on a fiber");
    fails_before = tp_fail;
    run(&r, "fiber", "segv", NULL); show("fiber segv", &r);
    CHECK(exited(&r, EXIT_CODE));
    check_file(&r, "HOST EXCEPTION SIGSEGV (signal 11, SEGV_MAPERR: address not mapped)");
    check_regs(&r);
    CHECK(in_order_s(r.file, "(crash_mid+0x", "(fiber_entry+0x", "(plat_fiber_trampoline+0x"));
    SHOWFAIL("fiber segv", &r);

    SECTION("stack overflow on a thread without crash_thread_init: killed by SIGSEGV, no report");
    fails_before = tp_fail;
    run(&r, "noalt", "overflow", NULL); show("noalt", &r);
    CHECK(signaled(&r, SIGSEGV));
    CHECK(!has(&r, "CALLBACK"));
    SHOWFAIL("noalt", &r);

    SECTION("two threads crash at once: one report");
    fails_before = tp_fail;
    run(&r, "two", "both", NULL); show("two", &r);
    CHECK(exited(&r, EXIT_CODE));
    CHECK(count(&r, "HOST EXCEPTION") == 1);
    CHECK(count(&r, "CALLBACK") == 1);
    check_file(&r, "HOST EXCEPTION SIGSEGV");
    SHOWFAIL("two", &r);

    SECTION("crash_report_here (RaiseException replacement)");
    fails_before = tp_fail;
    run(&r, "thread", "trap", NULL); show("trap", &r);
    CHECK(exited(&r, EXIT_CODE));
    CHECK(has(&r, "HOST EXCEPTION 0xe0000001 (guest trap) at host address 0x"));
    CHECK(in_order(&r, "(trap_mid+0x", "(trap_outer+0x", "(thread_entry+0x"));
    CHECK(has(&r, "CALLBACK sig=0 exc=e0000001"));
    check_file(&r, "HOST EXCEPTION 0xe0000001 (guest trap) at host address 0x");
    CHECK(!fhas(&r, "host registers"));
    SHOWFAIL("trap", &r);

    SECTION("the callback chooses the exit code (hostrt.c report_and_exit)");
    fails_before = tp_fail;
    run(&r, "main", "segv", "CB_EXIT=7");
    CHECK(exited(&r, 7));
    CHECK(has(&r, "CALLBACK sig=11"));
    SHOWFAIL("cb exit", &r);

    SECTION("the callback ends in ExitProcess: back to the handler, which exits with that code");
    fails_before = tp_fail;
    run(&r, "thread", "segv", "CB_EXITPROCESS=5");
    CHECK(exited(&r, 5));
    CHECK(has(&r, "CALLBACK sig=11"));
    SHOWFAIL("cb exitprocess", &r);

    SECTION("a fault inside the callback ends the process");
    fails_before = tp_fail;
    run(&r, "thread", "segv", "CB_FAULT=1"); show("cb fault", &r);
    CHECK(exited(&r, EXIT_CODE));
    CHECK(has(&r, "HOST EXCEPTION inside the crash report"));
    check_file(&r, "HOST EXCEPTION SIGSEGV");
    SHOWFAIL("cb fault", &r);

    SECTION("a hanging callback is cut by SSBU_CRASH_TIMEOUT");
    fails_before = tp_fail;
    run(&r, "thread", "segv", "CB_HANG=1 SSBU_CRASH_TIMEOUT=1"); show("cb hang", &r);
    CHECK(exited(&r, EXIT_CODE));
    CHECK(has(&r, "crash report: timed out"));
    CHECK(r.us < 5000000);
    check_file(&r, "HOST EXCEPTION SIGSEGV");
    SHOWFAIL("cb hang", &r);

    SECTION("another thread holds the main arena's malloc lock: the report is complete, no deadlock");
    fails_before = tp_fail;
    run(&r, "main", "segv", "MALLOC_HOLD=1 NOCB=1 SSBU_CRASH_TIMEOUT=10"); show("arena held", &r);
    CHECK(has(&r, "ARENA LOCKED"));
    CHECK(exited(&r, EXIT_CODE));
    CHECKF(r.us < 3000000, "%lld ms", (long long)(r.us / 1000));
    check_file(&r, "HOST EXCEPTION SIGSEGV");
    SHOWFAIL("arena held", &r);

    SECTION("... and a callback that mallocs hangs on it: cut by the timeout, the report is already complete");
    fails_before = tp_fail;
    run(&r, "main", "segv", "MALLOC_HOLD=1 CB_MALLOC=1 SSBU_CRASH_TIMEOUT=1"); show("arena held cb", &r);
    CHECK(has(&r, "ARENA LOCKED"));
    CHECK(has(&r, "CALLBACK sig=11"));
    CHECK(!has(&r, "CB MALLOC"));
    CHECK(exited(&r, EXIT_CODE));
    CHECKF(r.us < 8000000, "%lld ms", (long long)(r.us / 1000));
    check_file(&r, "HOST EXCEPTION SIGSEGV");
    SHOWFAIL("arena held cb", &r);

    SECTION("a thread loops malloc/free while the main thread crashes (x20, appended to one file)");
    fails_before = tp_fail;
    {   int ok = 0, slow = 0;
        static char all[262144];
        unlink(g_report);
        for (int i = 0; i < 20; i++) {
            int fd[2];
            if (pipe(fd)) break;
            pid_t pid = fork();
            if (pid == 0) {
                dup2(fd[1], 1); dup2(fd[1], 2); close(fd[0]); close(fd[1]);
                setenv("REPORT", g_report, 1); setenv("MALLOC_SPIN", "1", 1); setenv("CB_MALLOC", "1", 1);
                execl("/proc/self/exe", "test_crash", "--child", i % 2 ? "thread" : "main", "segv", (char *)NULL);
                _exit(99);
            }
            close(fd[1]);
            char sink[4096];
            int64_t t0 = tp_now_us();
            while (read(fd[0], sink, sizeof sink) > 0) {}
            close(fd[0]);
            int st;
            waitpid(pid, &st, 0);
            if (WIFEXITED(st) && WEXITSTATUS(st) == EXIT_CODE) ok++;
            if (tp_now_us() - t0 > 3000000) slow++;
        }
        int f = open(g_report, O_RDONLY);
        size_t n = 0;
        ssize_t k;
        if (f >= 0) { while ((k = read(f, all + n, sizeof all - 1 - n)) > 0) n += (size_t)k; close(f); }
        all[n] = 0;
        CHECKF(ok == 20, "%d of 20 exited %d", ok, EXIT_CODE);
        CHECKF(slow == 0, "%d runs over 3 s", slow);
        CHECKF(count_s(all, " UTC  HOST EXCEPTION SIGSEGV") == 20 && count_s(all, "=== end of crash report\n") == 20, "%d reports, %d ends",
               count_s(all, " UTC  HOST EXCEPTION SIGSEGV"), count_s(all, "=== end of crash report\n")); }

    SECTION("chaining: a SA_SIGINFO SIGSEGV handler installed before gets the fault after the report");
    fails_before = tp_fail;
    run(&r, "main", "segv", "PREV=segv"); show("prev segv", &r);
    CHECK(exited(&r, 42));
    CHECK(in_order(&r, "HOST EXCEPTION SIGSEGV", "CALLBACK sig=11", "PREV HANDLER sig=11 code=1"));
    check_file(&r, "HOST EXCEPTION SIGSEGV");
    SHOWFAIL("prev segv", &r);

    SECTION("chaining through ExitProcess (hostrt.c's callback): the previous handler still gets it");
    fails_before = tp_fail;
    run(&r, "thread", "segv", "PREV=segv CB_EXITPROCESS=4"); show("prev segv exitprocess", &r);
    CHECK(exited(&r, 42));
    CHECK(in_order(&r, "HOST EXCEPTION SIGSEGV", "CALLBACK sig=11", "PREV HANDLER sig=11 code=1"));
    SHOWFAIL("prev segv exitprocess", &r);

    SECTION("chaining: a plain SIGABRT handler installed before gets abort() after the report");
    fails_before = tp_fail;
    run(&r, "main", "abort", "PREV=abrt"); show("prev abrt", &r);
    CHECK(exited(&r, 43));
    CHECK(in_order(&r, "HOST EXCEPTION SIGABRT", "CALLBACK sig=6", "PREV PLAIN"));
    SHOWFAIL("prev abrt", &r);

    SECTION("chaining: a previous handler that returns (raised SIGFPE): then the exit code");
    fails_before = tp_fail;
    run(&r, "main", "fpe", "PREV=fpe"); show("prev fpe", &r);
    CHECK(exited(&r, EXIT_CODE));
    CHECK(in_order(&r, "HOST EXCEPTION SIGFPE", "CALLBACK sig=8", "PREV RETURNED"));
    SHOWFAIL("prev fpe", &r);

    SECTION("chaining with stdout a closed pipe: the console copy's SIGPIPE does not pre-empt the previous handler");
    fails_before = tp_fail;
    {   int fd[2], st = -1;
        if (!pipe(fd)) {
            unlink(g_report);
            pid_t pid = fork();
            if (pid == 0) {
                int dn = open("/dev/null", O_WRONLY);
                dup2(fd[1], 1); dup2(dn, 2); close(fd[0]); close(fd[1]);
                setenv("REPORT", g_report, 1); setenv("PREV", "segvq", 1); setenv("QUIET", "1", 1);
                execl("/proc/self/exe", "test_crash", "--child", "main", "segv", (char *)NULL);
                _exit(99);
            }
            close(fd[0]); close(fd[1]);                       /* nobody reads: the child's first write to stdout gets EPIPE */
            waitpid(pid, &st, 0);
        }
        CHECKF(WIFEXITED(st) && WEXITSTATUS(st) == 42, "status %#x", st);
        r.file[0] = 0;
        int f = open(g_report, O_RDONLY);
        if (f >= 0) { ssize_t k = read(f, r.file, sizeof r.file - 1); r.file[k > 0 ? k : 0] = 0; close(f); }
        CHECK(strstr(r.file, "=== end of crash report\n") != NULL); }

    SECTION("core dumps allowed (RLIMIT_CORE 1) but SSBU_CORE unset: exit code 4, the default");
    fails_before = tp_fail;
    run(&r, "main", "segv", "CORE=1"); show("core default", &r);
    CHECKF(exited(&r, EXIT_CODE), "status %#x", r.status);
    check_file(&r, "HOST EXCEPTION SIGSEGV");
    SHOWFAIL("core default", &r);

    SECTION("core dumps on (SSBU_CORE=1, RLIMIT_CORE 1): SIGSEGV ends by its default action, after the report");
    fails_before = tp_fail;
    run(&r, "main", "segv", "CORE=1 SSBU_CORE=1"); show("core segv", &r);
    CHECKF(signaled(&r, SIGSEGV), "status %#x", r.status);
    CHECK(has(&r, "CALLBACK sig=11"));
    check_file(&r, "HOST EXCEPTION SIGSEGV");
    printf("    (WCOREDUMP %d: RLIMIT_CORE 1 writes no core)\n", WIFSIGNALED(r.status) ? WCOREDUMP(r.status) != 0 : -1);
    SHOWFAIL("core segv", &r);

    SECTION("core dumps on: through ExitProcess, on a plat thread");
    fails_before = tp_fail;
    run(&r, "plat", "segv", "CORE=1 SSBU_CORE=1 CB_EXITPROCESS=4"); show("core exitprocess", &r);
    CHECKF(signaled(&r, SIGSEGV), "status %#x", r.status);
    SHOWFAIL("core exitprocess", &r);

    SECTION("core dumps on: abort() ends by SIGABRT");
    fails_before = tp_fail;
    run(&r, "thread", "abort", "CORE=1 SSBU_CORE=1"); show("core abort", &r);
    CHECKF(signaled(&r, SIGABRT), "status %#x", r.status);
    check_file(&r, "HOST EXCEPTION SIGABRT");
    SHOWFAIL("core abort", &r);

    SECTION("core dumps on: a hanging callback cut by the timeout still ends by the signal");
    fails_before = tp_fail;
    run(&r, "thread", "segv", "CORE=1 SSBU_CORE=1 CB_HANG=1 SSBU_CRASH_TIMEOUT=1"); show("core hang", &r);
    CHECKF(signaled(&r, SIGSEGV), "status %#x", r.status);
    CHECK(has(&r, "crash report: timed out"));
    SHOWFAIL("core hang", &r);

    SECTION("core dumps on: crash_report_here (software exception) still exits with the code");
    fails_before = tp_fail;
    run(&r, "main", "trap", "CORE=1 SSBU_CORE=1"); show("core trap", &r);
    CHECK(exited(&r, EXIT_CODE));
    SHOWFAIL("core trap", &r);

    SECTION("no crash: no report file");
    fails_before = tp_fail;
    run(&r, "main", "none", NULL);
    CHECK(exited(&r, 0) && has(&r, "NO CRASH"));
    CHECK(access(g_report, F_OK) != 0);
    SHOWFAIL("none", &r);

    SECTION("killed (SIGKILL, no exit handlers): no report file either");
    fails_before = tp_fail;
    run(&r, "main", "kill", NULL);
    CHECKF(signaled(&r, SIGKILL), "status %#x", r.status);
    CHECK(access(g_report, F_OK) != 0);
    SHOWFAIL("kill", &r);

    unlink(g_report);
    rmdir(g_dir);
    return tp_done("test_crash");
}
