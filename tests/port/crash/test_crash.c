/* test_crash.c - plat_crash.c crash reports, each scenario in a child process (this program re-executed with a mode):
 * the child crashes, the parent checks the report text and the exit status.
 *   SEGV / ILL / BRK (SIGTRAP) on the main thread and on a pthread; read of a PROT_NONE vmem page; a fault in a leaf
 *   (the caller comes from lr); stack overflow on a pthread and on a fiber (caught on the alternate stack); overflow on a
 *   thread WITHOUT crash_thread_init (dies of SIGSEGV, no report: why threads must call it); two threads crashing at
 *   once (one report); crash_report_here (RaiseException replacement); a fault inside the report callback; a callback
 *   that hangs (SSBU_CRASH_TIMEOUT); a callback that picks the exit code (as hostrt.c's report_and_exit). */
#include "tp.h"
#include "plat_crash.h"
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define EXIT_CODE 4

/* ---------------------------------------------------------------- child side */
static volatile int g_sink;
static void cb_print(const crash_info *ci) {
    printf("CALLBACK sig=%d exc=%x npcs=%d lr_frame=%d addr=%llx access=%c\n", ci->sig, ci->exc_code, ci->npcs, ci->lr_frame,
           (unsigned long long)ci->addr, ci->access ? ci->access : '-');
    if (getenv("CB_EXIT")) { fflush(stdout); _exit(atoi(getenv("CB_EXIT"))); }      /* hostrt.c: report_and_exit(4) */
    if (getenv("CB_FAULT")) *(volatile int *)(uintptr_t)0x18 = 1;
    if (getenv("CB_HANG")) for (;;) pause();
}
__attribute__((noinline)) int crash_leaf_write(volatile int *p) { *p = 1; return 0; }       /* leaf: no frame record (clang) */
__attribute__((noinline)) int crash_mid(volatile int *p) { int r = crash_leaf_write(p); __asm__ volatile("" ::: "memory"); return r + 1; }
__attribute__((noinline)) int crash_outer(volatile int *p) { int r = crash_mid(p); __asm__ volatile("" ::: "memory"); return r + 1; }
__attribute__((noinline)) void do_ill(void) { __asm__ volatile("udf #0"); }
__attribute__((noinline)) void do_brk(void) { __asm__ volatile("brk #0x7"); }
__attribute__((noinline)) int do_read(volatile int *p) { return *p; }
__attribute__((noinline)) int recurse(int d) {
    volatile char buf[1024];
    buf[0] = (char)d;
    int r = recurse(d + 1);
    __asm__ volatile("" ::: "memory");
    return r + buf[0];
}
__attribute__((noinline)) void trap_mid(void) { crash_report_here(0xE0000001, "guest trap"); }
__attribute__((noinline)) void trap_outer(void) { trap_mid(); __asm__ volatile("" ::: "memory"); }

static const char *g_mode;
static pthread_barrier_t g_bar;
__attribute__((noinline)) void run_mode(const char *m) {
    if (!strcmp(m, "segv")) crash_outer((volatile int *)(uintptr_t)0x1234);
    else if (!strcmp(m, "ill")) do_ill();
    else if (!strcmp(m, "brk")) do_brk();
    else if (!strcmp(m, "read")) {
        uint8_t *p = vmem_alloc(0x20000, PROT_READ | PROT_WRITE);
        vmem_protect(p + 0x10000, 0x10000, PROT_NONE, NULL);
        printf("PROTADDR %llx\n", (unsigned long long)(uintptr_t)(p + 0x10008)); fflush(stdout);
        g_sink = do_read((volatile int *)(p + 0x10008));
    } else if (!strcmp(m, "overflow")) g_sink = recurse(0);
    else if (!strcmp(m, "trap")) trap_outer();
    else if (!strcmp(m, "both")) { pthread_barrier_wait(&g_bar); crash_outer((volatile int *)(uintptr_t)0x40); }
}
__attribute__((noinline)) void *thread_entry(void *arg) {
    if (!arg) crash_thread_init();                       /* what os_threads.c / plat_posix.c thread starts must do */
    run_mode(g_mode);
    return NULL;
}
static plat_fiber *g_main_fiber;
void fiber_entry(void *arg) { (void)arg; g_sink = recurse(0); plat_fiber_switch(NULL, g_main_fiber); }
__attribute__((noinline)) void *fiber_thread(void *arg) {
    (void)arg;
    crash_thread_init();
    g_main_fiber = plat_fiber_convert_thread(NULL);
    plat_fiber *f = plat_fiber_create(256 << 10, fiber_entry, NULL);
    plat_fiber_switch(g_main_fiber, f);
    return NULL;
}
__attribute__((noinline)) int child(const char *where, const char *mode) {
    vmem_init();
    crash_install(cb_print, EXIT_CODE);
    g_mode = mode;
    pthread_t t, t2;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 << 10);
    if (!strcmp(where, "main")) run_mode(mode);
    else if (!strcmp(where, "thread")) { pthread_create(&t, &at, thread_entry, NULL); pthread_join(t, NULL); }
    else if (!strcmp(where, "noalt")) { pthread_create(&t, &at, thread_entry, (void *)1); pthread_join(t, NULL); }
    else if (!strcmp(where, "fiber")) { pthread_create(&t, &at, fiber_thread, NULL); pthread_join(t, NULL); }
    else if (!strcmp(where, "two")) {
        pthread_barrier_init(&g_bar, NULL, 2);
        pthread_create(&t, &at, thread_entry, NULL); pthread_create(&t2, &at, thread_entry, NULL);
        pthread_join(t, NULL); pthread_join(t2, NULL);
    }
    printf("NO CRASH\n");
    return 0;
}

/* ---------------------------------------------------------------- parent side */
typedef struct { int status; char out[65536]; } Run;
static void run(Run *r, const char *where, const char *mode, const char *env) {
    int fd[2];
    if (pipe(fd)) { perror("pipe"); exit(2); }
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fd[1], 1); dup2(fd[1], 2); close(fd[0]); close(fd[1]);
        if (env) { char e[256]; snprintf(e, sizeof e, "%s", env); char *eq = strchr(e, '='); *eq = 0; setenv(e, eq + 1, 1); }
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
}
static int exited(const Run *r, int code) { return WIFEXITED(r->status) && WEXITSTATUS(r->status) == code; }
static int has(const Run *r, const char *s) { return strstr(r->out, s) != NULL; }
/* the names appear in this order in the report */
static int in_order(const Run *r, const char *a, const char *b, const char *c) {
    const char *pa = strstr(r->out, a), *pb = pa ? strstr(pa, b) : NULL, *pc = !c ? pb : pb ? strstr(pb, c) : NULL;
    return pa && pb && pc;
}
static int count(const Run *r, const char *s) { int n = 0; for (const char *p = r->out; (p = strstr(p, s)); p++) n++; return n; }
static void show(const char *name, const Run *r) {
    if (getenv("VERBOSE")) printf("------ %s (status %#x)\n%s------\n", name, r->status, r->out);
}
#define SHOWFAIL(name, r) do { if (tp_fail != fails_before) printf("------ %s output (status %#x):\n%s------\n", name, (r)->status, (r)->out); } while (0)

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--child")) return child(argv[2], argv[3]);
    static Run r;
    int fails_before;

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
        CHECK(w == 0 ? has(&r, "(main+0x") : has(&r, "(thread_entry+0x"));        /* the walk reaches the entry */
        CHECK(has(&r, "CALLBACK sig=11 exc=0"));
        CHECK(has(&r, "addr=1234 access=w"));
        SHOWFAIL(name, &r);

        snprintf(name, sizeof name, "SIGILL (udf) on %s", wheres[w]); SECTION(name);
        fails_before = tp_fail;
        run(&r, wheres[w], "ill", NULL); show(name, &r);
        CHECK(exited(&r, EXIT_CODE));
        CHECK(has(&r, "HOST EXCEPTION SIGILL (signal 4, ILL_ILLOPC: illegal opcode)"));
        CHECK(has(&r, "instruction at pc: 00000000"));
        CHECK(in_order(&r, "(do_ill+0x", w == 0 ? "(main+0x" : "(thread_entry+0x", NULL));   /* run_mode may tail-call it (clang) */
        CHECK(has(&r, "CALLBACK sig=4"));
        SHOWFAIL(name, &r);

        snprintf(name, sizeof name, "SIGTRAP (brk) on %s", wheres[w]); SECTION(name);
        fails_before = tp_fail;
        run(&r, wheres[w], "brk", NULL); show(name, &r);
        CHECK(exited(&r, EXIT_CODE));
        CHECK(has(&r, "HOST EXCEPTION SIGTRAP (signal 5, TRAP_BRKPT: brk instruction)"));
        CHECK(has(&r, "  brk #0x7\n"));
        CHECK(has(&r, "instruction at pc: d42000e0"));
        CHECK(in_order(&r, "(do_brk+0x", w == 0 ? "(main+0x" : "(thread_entry+0x", NULL));
        SHOWFAIL(name, &r);
    }

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

    SECTION("stack overflow on a fiber (plat_fiber_create guard page)");
    fails_before = tp_fail;
    run(&r, "fiber", "overflow", NULL); show("fiber overflow", &r);
    CHECK(exited(&r, EXIT_CODE));
    CHECK(has(&r, "probably a stack overflow"));
    CHECK(has(&r, "(recurse+0x"));
    CHECK(has(&r, "CALLBACK sig=11"));
    SHOWFAIL("fiber overflow", &r);

    SECTION("stack overflow on a thread without crash_thread_init: killed by SIGSEGV, no report");
    fails_before = tp_fail;
    run(&r, "noalt", "overflow", NULL); show("noalt", &r);
    CHECK(WIFSIGNALED(r.status) && WTERMSIG(r.status) == SIGSEGV);
    CHECK(!has(&r, "CALLBACK"));
    SHOWFAIL("noalt", &r);

    SECTION("two threads crash at once: one report");
    fails_before = tp_fail;
    run(&r, "two", "both", NULL); show("two", &r);
    CHECK(exited(&r, EXIT_CODE));
    CHECK(count(&r, "HOST EXCEPTION") == 1);
    CHECK(count(&r, "CALLBACK") == 1);
    SHOWFAIL("two", &r);

    SECTION("crash_report_here (RaiseException replacement)");
    fails_before = tp_fail;
    run(&r, "thread", "trap", NULL); show("trap", &r);
    CHECK(exited(&r, EXIT_CODE));
    CHECK(has(&r, "HOST EXCEPTION 0xe0000001 (guest trap) at host address 0x"));
    CHECK(in_order(&r, "(trap_mid+0x", "(trap_outer+0x", "(thread_entry+0x"));
    CHECK(has(&r, "CALLBACK sig=0 exc=e0000001"));
    SHOWFAIL("trap", &r);

    SECTION("the callback chooses the exit code (hostrt.c report_and_exit)");
    fails_before = tp_fail;
    run(&r, "main", "segv", "CB_EXIT=7");
    CHECK(exited(&r, 7));
    CHECK(has(&r, "CALLBACK sig=11"));
    SHOWFAIL("cb exit", &r);

    SECTION("a fault inside the callback ends the process");
    fails_before = tp_fail;
    run(&r, "thread", "segv", "CB_FAULT=1"); show("cb fault", &r);
    CHECK(exited(&r, EXIT_CODE));
    CHECK(has(&r, "HOST EXCEPTION inside the crash report"));
    SHOWFAIL("cb fault", &r);

    SECTION("a hanging callback is cut by SSBU_CRASH_TIMEOUT");
    fails_before = tp_fail;
    setenv("SSBU_CRASH_TIMEOUT", "1", 1);
    int64_t t0 = tp_now_us();
    run(&r, "thread", "segv", "CB_HANG=1"); show("cb hang", &r);
    unsetenv("SSBU_CRASH_TIMEOUT");
    CHECK(exited(&r, EXIT_CODE));
    CHECK(has(&r, "crash report: timed out"));
    CHECK(tp_now_us() - t0 < 5000000);
    SHOWFAIL("cb hang", &r);

    return tp_done("test_crash");
}
