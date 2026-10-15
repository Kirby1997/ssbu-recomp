/* test_os_threads.c - runtime/os_threads.c on Linux: host threads (stacks below 2^47, names, waits, exit codes,
 * release), nn::os threads through the import handlers (names, priorities, console-core affinity on the big cores,
 * reuse), sleep accuracy, the real-time deadline of std::condition_variable, fibers migrating between threads (TLS,
 * thread block, guest mutex locked on one thread and unlocked on another), concurrent thread starts.
 * "Guest" functions are host functions void f(Ctx *) (stubs.c: call_guest calls them). Build at -O1 and -O2. */
#include "tp.h"
#include "runtime.h"
#include "hostrt.h"
#include <sys/resource.h>
#include <sys/syscall.h>
#include <signal.h>

ImportFn tst_import(const char *name);
struct host_thread;
struct host_thread *host_thread_create(size_t stack, LPTHREAD_START_ROUTINE fn, void *arg, const char *name, int suspended);
void host_thread_resume(struct host_thread *t);
HANDLE host_thread_handle(struct host_thread *t);
DWORD host_thread_exit_code(struct host_thread *t);
void host_thread_release(struct host_thread *t);
int pick_big_cpus(DWORD_PTR allowed, DWORD_PTR *out, int want);

#define OS(n) "_ZN2nn2os" n
#define I_CREATE OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi")
#define I_CREATE_CORE OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii")
#define I_START OS("11StartThreadEPNS0_10ThreadTypeE")
#define I_WAIT OS("10WaitThreadEPNS0_10ThreadTypeE")
#define I_DESTROY OS("13DestroyThreadEPNS0_10ThreadTypeE")
#define I_NAME OS("13SetThreadNameEPNS0_10ThreadTypeEPKc")
#define I_CUR OS("16GetCurrentThreadEv")
#define I_PRIO OS("20ChangeThreadPriorityEPNS0_10ThreadTypeEi")
#define I_CORE OS("20GetCurrentCoreNumberEv")
#define I_SETMASK OS("17SetThreadCoreMaskEPNS0_10ThreadTypeEim")
#define I_SLEEP OS("11SleepThreadENS_8TimeSpanE")
#define I_MTX_LOCK OS("9LockMutexEPNS0_9MutexTypeE")
#define I_MTX_UNLOCK OS("11UnlockMutexEPNS0_9MutexTypeE")
#define I_MTX_TRY OS("12TryLockMutexEPNS0_9MutexTypeE")
#define I_FIB_INIT OS("15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi")
#define I_FIB_FINI OS("13FinalizeFiberEPNS0_9FiberTypeE")
#define I_FIB_SWITCH OS("13SwitchToFiberEPNS0_9FiberTypeE")
#define I_FIB_CUR OS("15GetCurrentFiberEv")
#define I_STD_TIMED "_ZNSt3__118condition_variable15__do_timed_waitERNS_11unique_lockINS_5mutexEEENS_6chrono10time_pointINS5_12system_clockENS5_8durationIxNS_5ratioILl1ELl1000000000EEEEEEE"
#define I_STD_LOCK "_ZNSt3__15mutex4lockEv"
#define I_STD_UNLOCK "_ZNSt3__15mutex6unlockEv"

static uint64_t imp(const char *name, Ctx *c, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4; c->x[5] = a5; c->x[6] = a6;
    tst_import(name)(c);
    return c->x[0];
}
#define IMP(name, c, ...) imp(name, c, __VA_ARGS__)
static uint64_t P(const void *p) { return (uint64_t)(uintptr_t)p; }
static pid_t tid_now(void) { return (pid_t)syscall(SYS_gettid); }
static int low_addr(const void *p) {        /* below 2^47 and outside the guest window */
    uint64_t a = P(p);
    return a < (1ull << 47) && !(a >= VMEM_GUEST_LO && a < VMEM_GUEST_HI);
}
static _Thread_local int t_probe;

/* ---------------------------------------------------------------- host threads */
typedef struct { HANDLE go; pid_t tid; char name[32]; const void *stack, *tls; int ran, alt; } ht_arg;
static int has_altstack(void) { stack_t ss; return sigaltstack(NULL, &ss) == 0 && !(ss.ss_flags & SS_DISABLE) && ss.ss_size >= 0x10000; }
static DWORD WINAPI ht_fn(LPVOID p) {
    ht_arg *a = p;
    int local = 0;
    a->tid = tid_now();
    pthread_getname_np(pthread_self(), a->name, sizeof a->name);
    a->stack = &local;
    a->tls = &t_probe;
    a->alt = has_altstack();                 /* crash_thread_init in host_thread_start */
    __atomic_store_n(&a->ran, 1, __ATOMIC_RELEASE);
    if (a->go) WaitForSingleObject(a->go, INFINITE);
    return 42;
}
static HANDLE g_wait_target;
static DWORD WINAPI waiter_fn(LPVOID p) { (void)p; return WaitForSingleObject(g_wait_target, INFINITE) == WAIT_OBJECT_0 ? 7 : 8; }
static void test_host_threads(void) {
    SECTION("host threads: low stack + TLS, name (15 chars), waits (twice, 4 waiters, timeout), exit code, suspended start, release");
    ht_arg a = {0};
    a.go = CreateEventA(NULL, TRUE, FALSE, NULL);
    struct host_thread *t = host_thread_create((size_t)256 << 20, ht_fn, &a, "a-very-long-thread-name", 0);
    CHECK(t != NULL);
    HANDLE h = host_thread_handle(t);
    CHECK(h != NULL);
    while (!LOAD(a.ran)) sched_yield();
    CHECKF(low_addr(a.stack), "stack local at %p", a.stack);
    CHECKF(low_addr(a.tls), "TLS at %p", a.tls);
    CHECKF(!strcmp(a.name, "a-very-long-thr"), "name '%s'", a.name);
    CHECK(a.alt);
    CHECK(GetThreadId(h) == (DWORD)a.tid);
    CHECK(WaitForSingleObject(h, 30) == WAIT_TIMEOUT);
    CHECK(host_thread_exit_code(t) == STILL_ACTIVE);
    g_wait_target = h;
    HANDLE w[4];
    for (int i = 0; i < 4; i++) w[i] = CreateThread(NULL, 0, waiter_fn, NULL, 0, NULL);
    Sleep(20);
    SetEvent(a.go);
    for (int i = 0; i < 4; i++) { DWORD code = 0; CHECK(WaitForSingleObject(w[i], 5000) == WAIT_OBJECT_0); GetExitCodeThread(w[i], &code); CHECK(code == 7); CloseHandle(w[i]); }
    CHECK(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(h, 0) == WAIT_OBJECT_0);           /* waited on twice */
    CHECK(WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0);
    CHECK(host_thread_exit_code(t) == 42);
    vmem_info vi;
    CHECK(vmem_query(a.stack, &vi) && vi.state == VMEM_STATE_COMMITTED && vi.tracked);
    CloseHandle(h);
    host_thread_release(t);
    CHECK(vmem_query(a.stack, &vi) && vi.state == VMEM_STATE_FREE);   /* joined once, stack gone */
    /* suspended */
    ht_arg b = {0};
    t = host_thread_create(1 << 20, ht_fn, &b, "susp", 1);
    Sleep(30);
    CHECK(!LOAD(b.ran));
    host_thread_resume(t);
    CHECK(WaitForSingleObject(host_thread_handle(t), 5000) == WAIT_OBJECT_0);
    CHECK(LOAD(b.ran) && !strcmp(b.name, "susp"));
    CloseHandle(host_thread_handle(t));
    host_thread_release(t);
    /* stack size 0: the minimum (256 KiB, above PTHREAD_STACK_MIN) */
    ht_arg z = {0};
    t = host_thread_create(0, ht_fn, &z, "small", 0);
    CHECK(t != NULL && WaitForSingleObject(host_thread_handle(t), 5000) == WAIT_OBJECT_0 && LOAD(z.ran) && low_addr(z.stack));
    if (t) { CloseHandle(host_thread_handle(t)); host_thread_release(t); }
    /* release while running: the join happens later */
    ht_arg d = {0};
    d.go = CreateEventA(NULL, TRUE, FALSE, NULL);
    t = host_thread_create(1 << 20, ht_fn, &d, "late", 0);
    HANDLE dh = host_thread_handle(t);
    while (!LOAD(d.ran)) sched_yield();
    host_thread_release(t);                                       /* still running: stays on the list */
    CHECK(vmem_query(d.stack, &vi) && vi.state == VMEM_STATE_COMMITTED);
    SetEvent(d.go);
    CHECK(WaitForSingleObject(dh, 5000) == WAIT_OBJECT_0);
    CloseHandle(dh);
    /* many: no address space or region leak */
    size_t regions0 = vmem_region_count();
    for (int i = 0; i < 200; i++) {
        ht_arg e = {0};
        t = host_thread_create((size_t)256 << 20, ht_fn, &e, "many", 0);
        CHECK(t != NULL);
        if (!t) break;
        WaitForSingleObject(host_thread_handle(t), INFINITE);
        CloseHandle(host_thread_handle(t));
        host_thread_release(t);
    }
    CHECK(vmem_query(d.stack, &vi) && vi.state == VMEM_STATE_FREE);   /* the late one went at a later release */
    CHECKF(vmem_region_count() <= regions0, "regions %zu -> %zu", regions0, vmem_region_count());
    /* 32 at once, 256 MiB each */
    struct host_thread *ts[32];
    ht_arg as[32];
    HANDLE go = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (int i = 0; i < 32; i++) { memset(&as[i], 0, sizeof as[i]); as[i].go = go; ts[i] = host_thread_create((size_t)256 << 20, ht_fn, &as[i], "par", 0); CHECK(ts[i] != NULL); }
    for (int i = 0; i < 32; i++) while (ts[i] && !LOAD(as[i].ran)) sched_yield();
    for (int i = 0; i < 32; i++) CHECK(low_addr(as[i].stack) && low_addr(as[i].tls));
    SetEvent(go);
    for (int i = 0; i < 32; i++) if (ts[i]) { CHECK(WaitForSingleObject(host_thread_handle(ts[i]), 5000) == WAIT_OBJECT_0); CloseHandle(host_thread_handle(ts[i])); host_thread_release(ts[i]); }
    CloseHandle(go); CloseHandle(a.go); CloseHandle(d.go);
}

/* ---------------------------------------------------------------- nn::os threads */
typedef struct {
    uint64_t self_tt, block_tt;           /* GetCurrentThread, thread block +0x1f8 */
    char name[32];
    int nice, core, wait_name, alt;
    uint64_t affinity;
    const void *stack;
    pid_t tid;
    volatile int go;
} gt_rec;
static void gt_fn(Ctx *c) {
    gt_rec *r = (gt_rec *)(uintptr_t)c->x[0];
    int local;
    r->stack = &local;
    r->tid = tid_now();
    r->alt = has_altstack();
    r->block_tt = rd64(c->tpidr + 0x1f8);
    r->self_tt = IMP(I_CUR, c, 0, 0, 0, 0, 0, 0, 0);
    while (r->wait_name && !LOAD(r->go)) sched_yield();
    pthread_getname_np(pthread_self(), r->name, sizeof r->name);
    errno = 0;
    r->nice = getpriority(PRIO_PROCESS, 0);
    cpu_set_t s;
    CPU_ZERO(&s);
    sched_getaffinity(0, sizeof s, &s);
    r->affinity = 0;
    for (int i = 0; i < 64; i++) if (CPU_ISSET(i, &s)) r->affinity |= 1ull << i;
    r->core = (int)IMP(I_CORE, c, 0, 0, 0, 0, 0, 0, 0);
}
static uint8_t g_tt[16][0x200];
static uint8_t g_gstack[0x10000];
static void run_guest_thread(int idx, gt_rec *r, int prio, int core, const char *name_before, const char *name_after) {
    Ctx c;
    memset(&c, 0, sizeof c);
    if (core < 0) IMP(I_CREATE, &c, P(g_tt[idx]), P(gt_fn), P(r), P(g_gstack), sizeof g_gstack, (uint64_t)prio, 0);
    else IMP(I_CREATE_CORE, &c, P(g_tt[idx]), P(gt_fn), P(r), P(g_gstack), sizeof g_gstack, (uint64_t)prio, (uint64_t)core);
    if (name_before) IMP(I_NAME, &c, P(g_tt[idx]), P(name_before), 0, 0, 0, 0, 0);
    r->wait_name = name_after != NULL;
    IMP(I_START, &c, P(g_tt[idx]), 0, 0, 0, 0, 0, 0);
    if (name_after) { IMP(I_NAME, &c, P(g_tt[idx]), P(name_after), 0, 0, 0, 0, 0); __atomic_store_n(&r->go, 1, __ATOMIC_RELEASE); }
    IMP(I_WAIT, &c, P(g_tt[idx]), 0, 0, 0, 0, 0, 0);
    IMP(I_WAIT, &c, P(g_tt[idx]), 0, 0, 0, 0, 0, 0);              /* WaitThread twice */
    IMP(I_DESTROY, &c, P(g_tt[idx]), 0, 0, 0, 0, 0, 0);
}
static DWORD_PTR g_big[3];
static int g_nbig;
static void test_guest_threads(void) {
    SECTION("nn::os threads: Create/SetName/Start/Wait x2/Destroy, thread block, names, priorities, big-core affinity, reuse");
    gt_rec r;
    memset(&r, 0, sizeof r);
    run_guest_thread(0, &r, 16, -1, "GuestWorkerThreadLongName", NULL);
    CHECK(r.self_tt == P(g_tt[0]) && r.block_tt == P(g_tt[0]));
    CHECKF(!strcmp(r.name, "GuestWorkerThre"), "name '%s'", r.name);
    CHECKF(low_addr(r.stack), "host stack %p", r.stack);
    CHECK(r.nice == 0);
    CHECK(r.alt);
    memset(&r, 0, sizeof r);
    run_guest_thread(1, &r, 0, -1, NULL, "RenamedAfterStart");         /* priority 0: raise without CAP_SYS_NICE -> stays 0 */
    CHECKF(!strcmp(r.name, "RenamedAfterSta"), "name '%s'", r.name);
    struct rlimit rl;
    getrlimit(RLIMIT_NICE, &rl);
    int privileged = geteuid() == 0 || rl.rlim_cur > 20;
    CHECKF(privileged ? r.nice <= 0 : r.nice == 0, "nice %d for nn::os priority 0", r.nice);
    printf("    nn::os priority 0 -> nice %d (RLIMIT_NICE %lld, euid %d)\n", r.nice, (long long)rl.rlim_cur, (int)geteuid());
    memset(&r, 0, sizeof r);
    run_guest_thread(2, &r, 31, -1, NULL, NULL);                        /* lowest: recorded, applied only if nice can return to 0 */
    printf("    nn::os priority 31 -> nice %d\n", r.nice);
    CHECK(r.nice == 0 || r.nice == 5);
    /* console core model (SSBU_OS_CONSOLE_CORES=1 set in main): cores 0..2 -> the biggest processors */
    for (int core = 0; core < 3 && core < g_nbig; core++) {
        memset(&r, 0, sizeof r);
        run_guest_thread(3 + core, &r, 16, core, "core", NULL);
        CHECKF(r.affinity == (uint64_t)g_big[core], "core %d: affinity %llx, want %llx", core, (unsigned long long)r.affinity, (unsigned long long)g_big[core]);
        CHECKF(r.core == core, "core %d: GetCurrentCoreNumber %d", core, r.core);
    }
    /* reuse of one ThreadType: each host stack is gone after DestroyThread (the 64 KiB thread block of new_ctx stays,
     * as on Windows: dump_guest_threads keeps every thread's context) */
    for (int i = 0; i < 64; i++) {
        memset(&r, 0, sizeof r);
        run_guest_thread(6, &r, 16, -1, "reuse", NULL);
        CHECK(r.self_tt == P(g_tt[6]));
        vmem_info vi;
        CHECKF(vmem_query(r.stack, &vi) && vi.state == VMEM_STATE_FREE, "run %d: stack %p state %d", i, r.stack, vi.state);
    }
}

/* ---------------------------------------------------------------- concurrent starts (oslog spinlock, reap list) */
static DWORD WINAPI starter_fn(LPVOID p) {
    int base = (int)(intptr_t)p;
    for (int i = 0; i < 4; i++) {
        gt_rec r;
        memset(&r, 0, sizeof r);
        Ctx c;
        memset(&c, 0, sizeof c);
        static uint8_t tts[8][4][0x200];
        uint8_t *tt = tts[base][i];
        IMP(I_CREATE, &c, P(tt), P(gt_fn), P(&r), P(g_gstack), sizeof g_gstack, 16, 0);
        IMP(I_START, &c, P(tt), 0, 0, 0, 0, 0, 0);
        IMP(I_WAIT, &c, P(tt), 0, 0, 0, 0, 0, 0);
        IMP(I_DESTROY, &c, P(tt), 0, 0, 0, 0, 0, 0);
        if (r.self_tt != P(tt)) return 1;
    }
    return 0;
}
static void test_concurrent_starts(void) {
    SECTION("8 host threads each starting 4 nn::os threads at once");
    HANDLE h[8];
    for (int i = 0; i < 8; i++) h[i] = CreateThread(NULL, 0, starter_fn, (LPVOID)(intptr_t)i, 0, NULL);
    for (int i = 0; i < 8; i++) { DWORD code = 9; CHECK(WaitForSingleObject(h[i], 20000) == WAIT_OBJECT_0); GetExitCodeThread(h[i], &code); CHECK(code == 0); CloseHandle(h[i]); }
}

/* ---------------------------------------------------------------- sleeps and the real-time deadline */
static int cmp64(const void *a, const void *b) { int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return x < y ? -1 : x > y; }
static DWORD WINAPI sleep_fn(LPVOID p) {
    Ctx c;
    memset(&c, 0, sizeof c);
    static const int64_t asks[] = {100000, 500000, 1000000, 2000000, 5000000};
    int bad = 0;
    for (int k = 0; k < 5; k++) {
        int64_t over[40];
        for (int i = 0; i < 40; i++) {
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            IMP(I_SLEEP, &c, (uint64_t)asks[k], 0, 0, 0, 0, 0, 0);
            QueryPerformanceCounter(&b);
            over[i] = b.QuadPart - a.QuadPart - asks[k];
            if (over[i] < 0) bad++;
        }
        qsort(over, 40, sizeof over[0], cmp64);
        printf("    sleep %5lld us: late by p50 %4lld us, p90 %4lld us, max %5lld us\n", (long long)asks[k] / 1000, (long long)over[20] / 1000,
               (long long)over[36] / 1000, (long long)over[39] / 1000);
        if (over[20] > 1000000) bad += 1000;       /* median more than 1 ms late */
    }
    return (DWORD)bad;
}
static void test_sleep(void) {
    SECTION("nn::os::SleepThread accuracy (clock_nanosleep TIMER_ABSTIME, 1 ns timer slack) + std::condition_variable real-time deadline");
    struct host_thread *t = host_thread_create(1 << 20, sleep_fn, NULL, "sleeper", 0);
    CHECK(WaitForSingleObject(host_thread_handle(t), 30000) == WAIT_OBJECT_0);
    CHECKF(host_thread_exit_code(t) == 0, "early wake-ups / late median: %u", (unsigned)host_thread_exit_code(t));
    CloseHandle(host_thread_handle(t));
    host_thread_release(t);
    /* real-time deadline: system_clock ns since 1970 */
    static uint8_t cv[64], mtx[64];
    uint64_t lock = P(mtx);                                  /* unique_lock<mutex>: first word is the mutex pointer */
    Ctx c;
    memset(&c, 0, sizeof c);
    for (int k = 0; k < 2; k++) {
        int64_t after = k ? -5000000 : 30000000;           /* 30 ms ahead; 5 ms in the past */
        IMP(I_STD_LOCK, &c, P(mtx), 0, 0, 0, 0, 0, 0);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        int64_t deadline = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec + after;
        int64_t t0 = tp_now_us();
        IMP(I_STD_TIMED, &c, P(cv), P(&lock), (uint64_t)deadline, 0, 0, 0, 0);
        int64_t dt = tp_now_us() - t0;
        IMP(I_STD_UNLOCK, &c, P(mtx), 0, 0, 0, 0, 0, 0);
        clock_gettime(CLOCK_REALTIME, &ts);
        int64_t now = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
        printf("    std::condition_variable deadline %+lld ms: returned after %.2f ms, %s the deadline\n", (long long)after / 1000000, dt / 1000.0, now >= deadline ? "at/after" : "BEFORE");
        CHECK(now >= deadline);
        CHECK(dt < (k ? 5000 : 60000));
    }
}

/* ---------------------------------------------------------------- fibers migrating between threads */
#define PP_ROUNDS 20000
static uint8_t g_fib[0x100], g_fmtx[64], g_ptt[2][0x200];
static int g_turn;
static pid_t g_resumer_tid;
static uint64_t g_resumer_tt;
static int g_fbad, g_runs, g_returns, g_low_bad;
static void fiber_fn(Ctx *c) {
    for (int j = 0;; j++) {
        int local;
        if (!low_addr(&local)) g_low_bad++;
        if (tid_now() != g_resumer_tid) g_fbad |= 1;
        if (rd64(c->tpidr + 0x1f8) != g_resumer_tt) g_fbad |= 2;                 /* the thread block of the resuming thread */
        if (IMP(I_CUR, c, 0, 0, 0, 0, 0, 0, 0) != g_resumer_tt) g_fbad |= 4;      /* t_thread of the host thread running us */
        if (IMP(I_FIB_CUR, c, 0, 0, 0, 0, 0, 0, 0) != P(g_fib)) g_fbad |= 8;
        if (g_runs++ % 2 == 0) IMP(I_MTX_LOCK, c, P(g_fmtx), 0, 0, 0, 0, 0, 0);  /* locked here, unlocked by the next run on the other thread */
        else IMP(I_MTX_UNLOCK, c, P(g_fmtx), 0, 0, 0, 0, 0, 0);
        if (j % 7 == 6) { g_returns++; c->x[0] = 0; return; }                    /* function returns: fiber_main switches to the current thread */
        IMP(I_FIB_SWITCH, c, 0, 0, 0, 0, 0, 0, 0);                               /* back to the thread that resumed us */
    }
}
static void pp_thread(Ctx *c) {
    int me = (int)c->x[0];
    for (int k = 0; k < PP_ROUNDS; k++) {
        for (int spin = 0; __atomic_load_n(&g_turn, __ATOMIC_ACQUIRE) != me; spin++) if (spin > 1000) sched_yield(); else YieldProcessor();
        pid_t tid = tid_now();
        g_resumer_tid = tid;
        g_resumer_tt = P(g_ptt[me]);
        IMP(I_FIB_SWITCH, c, P(g_fib), 0, 0, 0, 0, 0, 0);
        if (tid_now() != tid) g_fbad |= 16;                                      /* a thread's own context comes back on that thread */
        if (IMP(I_FIB_CUR, c, 0, 0, 0, 0, 0, 0, 0) != 0) g_fbad |= 32;
        if (rd64(c->tpidr + 0x1f8) != P(g_ptt[me])) g_fbad |= 64;
        __atomic_store_n(&g_turn, 1 - me, __ATOMIC_RELEASE);
    }
}
static void test_fibers(void) {
    SECTION("fibers: one nn::os fiber resumed alternately by two threads (TLS, thread block, mutex across threads)");
    Ctx c;
    memset(&c, 0, sizeof c);
    size_t regions0 = vmem_region_count();
    IMP(I_FIB_INIT, &c, P(g_fib), P(fiber_fn), 0, P(g_gstack), sizeof g_gstack, 0, 0);
    for (int i = 0; i < 2; i++) IMP(I_CREATE_CORE, &c, P(g_ptt[i]), P(pp_thread), (uint64_t)i, P(g_gstack), sizeof g_gstack, 16, (uint64_t)i);   /* two different big cores */
    for (int i = 0; i < 2; i++) IMP(I_START, &c, P(g_ptt[i]), 0, 0, 0, 0, 0, 0);
    int64_t t0 = tp_now_us();
    for (int i = 0; i < 2; i++) IMP(I_WAIT, &c, P(g_ptt[i]), 0, 0, 0, 0, 0, 0);
    int64_t dt = tp_now_us() - t0;
    for (int i = 0; i < 2; i++) IMP(I_DESTROY, &c, P(g_ptt[i]), 0, 0, 0, 0, 0, 0);
    printf("    %d resumes (%d by function return) in %.1f ms\n", g_runs, g_returns, dt / 1000.0);
    CHECKF(g_fbad == 0, "mask %x", g_fbad);
    CHECK(g_low_bad == 0);
    CHECK(g_runs == 2 * PP_ROUNDS && g_returns > 0);
    CHECK(IMP(I_MTX_TRY, &c, P(g_fmtx), 0, 0, 0, 0, 0, 0) == 1);           /* even number of runs: the mutex is free */
    IMP(I_MTX_UNLOCK, &c, P(g_fmtx), 0, 0, 0, 0, 0, 0);
    IMP(I_FIB_FINI, &c, P(g_fib), 0, 0, 0, 0, 0, 0);
    CHECKF(vmem_region_count() <= regions0 + 4, "regions %zu -> %zu", regions0, vmem_region_count());
}

int main(void) {
    setenv("SSBU_OS_CONSOLE_CORES", "1", 1);
    vmem_init();
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    g_nbig = pick_big_cpus(proc, g_big, 3);
    {   /* independent expectation: capacities from sysfs, biggest first, lowest number among equals */
        long cap[64]; int n = 0; DWORD_PTR want[3] = {0};
        for (int i = 0; i < 64; i++) {
            cap[i] = -1;
            if (!(proc >> i & 1)) continue;
            char path[80]; snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpu_capacity", i);
            FILE *f = fopen(path, "r"); cap[i] = 1024;
            if (f) { if (fscanf(f, "%ld", &cap[i]) != 1) cap[i] = 1024; fclose(f); }
        }
        for (; n < 3; n++) { int b = -1; for (int i = 0; i < 64; i++) if (cap[i] >= 0 && (b < 0 || cap[i] > cap[b])) b = i; if (b < 0) break; want[n] = (DWORD_PTR)1 << b; cap[b] = -1; }
        printf("big cores for nn::os cores 0..2: %llx %llx %llx (expected %llx %llx %llx)\n", (unsigned long long)g_big[0], (unsigned long long)g_big[1],
               (unsigned long long)g_big[2], (unsigned long long)want[0], (unsigned long long)want[1], (unsigned long long)want[2]);
        for (int i = 0; i < 3; i++) CHECK(g_big[i] == want[i]);
    }
    test_host_threads();
    test_guest_threads();
    test_concurrent_starts();
    test_sleep();
    test_fibers();
    return tp_done("test_os_threads");
}
