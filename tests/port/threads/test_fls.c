/* test_fls.c - fiber-local storage on Linux (plat.h FlsAlloc / FlsFree / FlsGetValue / FlsSetValue) and the guest
 * fibers of runtime/os_threads.c, which make each guest fiber plat's current fiber (WP-U3):
 *   - the API: indexes, invalid ones, running out, FlsFree;
 *   - Win32 fibers: a new fiber starts empty, ConvertThreadToFiber / ConvertFiberToThread keep the thread's values,
 *     DeleteFiber runs the callback once (non-NULL values only);
 *   - the end of a thread (plat CreateThread, pthread, a converted thread, a callback that sets a value again);
 *   - FlsFree: the callback once for every fiber's and thread's value, the index empty everywhere when handed out again;
 *   - nn::os guest fibers: two on one host thread see their own values and their own GetCurrentFiber, the thread's own
 *     value survives the first switch; a fiber resumed alternately by two nn::os threads keeps its value; FinalizeFiber
 *     and the end of an nn::os thread run the callbacks once;
 *   - fighter_sdk/win32_posix.h + fighter_host.c's FsFiber pattern: two guest fibers, each moved to another thread, see
 *     their own FsFiber (the Phase 4 review's major finding);
 *   - soak: more than 10,000 fibers (Win32 and guest) and threads, each with a value: every callback runs, the number of
 *     live value sets and the heap come back (the old 4,096-entry exception-state table could not do this).
 * "Guest" functions are host functions void f(Ctx *) (stubs.c: call_guest calls them). Fiber functions use no TLS. */
#include "tp.h"
#include "runtime.h"
#include "hostrt.h"
#include <setjmp.h>
#include <malloc.h>
#include <sys/syscall.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "fighter_sdk/win32_posix.h"       /* must build against plat.h's FLS (it no longer has its own) */
#pragma GCC diagnostic pop

ImportFn tst_import(const char *name);
#define OS(n) "_ZN2nn2os" n
#define I_CREATE OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi")
#define I_START OS("11StartThreadEPNS0_10ThreadTypeE")
#define I_WAIT OS("10WaitThreadEPNS0_10ThreadTypeE")
#define I_DESTROY OS("13DestroyThreadEPNS0_10ThreadTypeE")
#define I_FIB_INIT OS("15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi")
#define I_FIB_FINI OS("13FinalizeFiberEPNS0_9FiberTypeE")
#define I_FIB_SWITCH OS("13SwitchToFiberEPNS0_9FiberTypeE")
#define I_FIB_CUR OS("15GetCurrentFiberEv")
static uint64_t imp(const char *name, Ctx *c, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4; c->x[5] = a5;
    tst_import(name)(c);
    return c->x[0];
}
static uint64_t P(const void *p) { return (uint64_t)(uintptr_t)p; }
static pid_t tid_now(void) { return (pid_t)syscall(SYS_gettid); }
static uint8_t g_gstack[0x10000];

/* a value that counts its callbacks */
typedef struct { int calls; } Rec;
static void WINAPI cb_rec(PVOID v) { __atomic_add_fetch(&((Rec *)v)->calls, 1, __ATOMIC_RELAXED); }
static int calls(Rec *r) { return __atomic_load_n(&r->calls, __ATOMIC_ACQUIRE); }
/* the callback of a thread's end runs after nn::os WaitThread returns (as on Windows): wait for it, then for strays */
static int calls_settled(Rec *r, int want) {
    for (int i = 0; i < 2000 && calls(r) < want; i++) Sleep(1);
    Sleep(5);
    return calls(r);
}
static size_t heap_used(void) { struct mallinfo2 m = mallinfo2(); return m.uordblks + m.hblkhd; }

/* runs fn(arg) on a new nn::os thread and waits for it */
static void run_guest_thread(uint8_t *tt, void (*fn)(Ctx *), uint64_t arg) {
    Ctx c;
    memset(&c, 0, sizeof c);
    imp(I_CREATE, &c, P(tt), P(fn), arg, P(g_gstack), sizeof g_gstack, 16);
    imp(I_START, &c, P(tt), 0, 0, 0, 0, 0);
}
static void wait_guest_thread(uint8_t *tt) {
    Ctx c;
    memset(&c, 0, sizeof c);
    imp(I_WAIT, &c, P(tt), 0, 0, 0, 0, 0);
    imp(I_DESTROY, &c, P(tt), 0, 0, 0, 0, 0);
}

/* ---------------------------------------------------------------- the API */
static DWORD g_ix;                         /* FlsAlloc(cb_rec), used by most sections */
static void test_api(void) {
    SECTION("FlsAlloc / FlsGetValue / FlsSetValue / FlsFree: indexes, invalid ones, running out");
    g_ix = FlsAlloc(cb_rec);
    CHECK(g_ix != FLS_OUT_OF_INDEXES && g_ix != 0 && g_ix < FLS_MAXIMUM_AVAILABLE);
    CHECK(FlsGetValue(g_ix) == NULL);
    SetLastError(0); CHECK(FlsGetValue(0) == NULL && GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0); CHECK(FlsGetValue(FLS_OUT_OF_INDEXES) == NULL && GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0); CHECK(!FlsSetValue(5000, (void *)1) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(FlsSetValue(g_ix, NULL));                                  /* NULL into nothing: no value set is made */
    size_t sets0 = plat_fls_sets();
    DWORD ix[FLS_MAXIMUM_AVAILABLE + 2];
    int n = 0;
    while (n < FLS_MAXIMUM_AVAILABLE + 1 && (ix[n] = FlsAlloc(NULL)) != FLS_OUT_OF_INDEXES) n++;
    CHECKF(n == FLS_MAXIMUM_AVAILABLE - 2 && GetLastError() == ERROR_NO_MORE_ITEMS, "%d more indexes", n);   /* 0 is never used, g_ix is taken */
    int distinct = 1;
    for (int i = 0; i < n; i++) for (int j = 0; j < i; j++) if (ix[i] == ix[j] || ix[i] == g_ix) distinct = 0;
    CHECK(distinct);
    CHECK(FlsSetValue(ix[n - 1], (void *)0x55) && FlsGetValue(ix[n - 1]) == (void *)0x55 && plat_fls_sets() == sets0 + 1);
    int freed = 1;
    for (int i = 0; i < n; i++) freed &= FlsFree(ix[i]) != 0;
    CHECK(freed);
    SetLastError(0); CHECK(!FlsFree(ix[0]) && GetLastError() == ERROR_INVALID_PARAMETER);   /* twice */
    SetLastError(0); CHECK(FlsGetValue(ix[n - 1]) == NULL && GetLastError() == ERROR_INVALID_PARAMETER);
    DWORD again = FlsAlloc(NULL);
    CHECK(again != FLS_OUT_OF_INDEXES && FlsGetValue(again) == NULL);   /* the value 0x55 went with FlsFree */
    FlsFree(again);
}

/* ---------------------------------------------------------------- Win32 fibers on one thread */
static LPVOID g_main_fiber;
static void *g_seen_first[2];
static int g_bad_resume;
static Rec g_wr[2];
static int g_clear1;
static void WINAPI w32_fiber(LPVOID arg) {
    int i = (int)(uintptr_t)arg;
    Rec *r = g_wr;
    g_seen_first[i] = FlsGetValue(g_ix);                             /* a new fiber: nothing yet */
    FlsSetValue(g_ix, &r[i]);
    for (;;) {
        SwitchToFiber(g_main_fiber);
        if (FlsGetValue(g_ix) != &r[i] || GetCurrentFiber() == g_main_fiber) g_bad_resume++;
        if (i == 1 && g_clear1) FlsSetValue(g_ix, NULL);              /* fiber 1 ends with no value: no callback */
    }
}
static void test_win32_fibers(void) {
    SECTION("Win32 fibers: own values, ConvertThreadToFiber / ConvertFiberToThread keep the thread's, DeleteFiber callback");
    static Rec t0;
    CHECK(FlsSetValue(g_ix, &t0) && FlsGetValue(g_ix) == &t0);
    size_t sets0 = plat_fls_sets();                                   /* the main thread has its set now */
    g_main_fiber = ConvertThreadToFiber(NULL);
    CHECK(g_main_fiber && GetCurrentFiber() == g_main_fiber && FlsGetValue(g_ix) == &t0);    /* handed over */
    LPVOID f[2];
    for (int i = 0; i < 2; i++) f[i] = CreateFiber(1 << 16, w32_fiber, (LPVOID)(uintptr_t)i);
    for (int i = 0; i < 2; i++) SwitchToFiber(f[i]);
    CHECK(g_seen_first[0] == NULL && g_seen_first[1] == NULL);
    CHECKF(FlsGetValue(g_ix) == &t0 && plat_fls_sets() == sets0 + 2, "%zu -> %zu", sets0, plat_fls_sets());   /* and the two fibers' */
    for (int k = 0; k < 3; k++) for (int i = 0; i < 2; i++) SwitchToFiber(f[i]);
    g_clear1 = 1;
    SwitchToFiber(f[1]);
    CHECK(g_bad_resume == 0 && FlsGetValue(g_ix) == &t0);
    Rec *r0 = &g_wr[0], *r1 = &g_wr[1];
    CHECK(calls(r0) == 0 && calls(r1) == 0);
    DeleteFiber(f[0]);
    DeleteFiber(f[1]);
    CHECKF(calls(r0) == 1 && calls(r1) == 0, "%d %d", calls(r0), calls(r1));   /* fiber 1's value was NULL */
    CHECK(ConvertFiberToThread() && GetCurrentFiber() == NULL && FlsGetValue(g_ix) == &t0 && calls(&t0) == 0);
    CHECK(plat_fls_sets() == sets0);
    FlsSetValue(g_ix, NULL);
}

/* ---------------------------------------------------------------- the end of a thread */
static Rec g_rt, g_rp, g_rconv_thread, g_rconv_fiber, g_rx, g_ry;
static DWORD g_ix2;
static DWORD WINAPI plat_thread_fn(LPVOID p) { (void)p; FlsSetValue(g_ix, &g_rt); return 0; }
static void *pthread_fn(void *p) { (void)p; FlsSetValue(g_ix, &g_rp); return NULL; }
static LPVOID g_conv_main, g_conv_other;
static void WINAPI conv_fiber(LPVOID p) { (void)p; FlsSetValue(g_ix, &g_rconv_fiber); SwitchToFiber(g_conv_main); for (;;) SwitchToFiber(g_conv_main); }
static void *conv_thread(void *p) {     /* converts, sets values in its own fiber and in another, ends converted */
    (void)p;
    FlsSetValue(g_ix, &g_rconv_thread);
    g_conv_main = ConvertThreadToFiber(NULL);
    g_conv_other = CreateFiber(1 << 16, conv_fiber, NULL);
    SwitchToFiber(g_conv_other);
    return NULL;
}
static void WINAPI cb_again(PVOID v) {          /* the first call sets a value again: a second round frees it */
    cb_rec(v);
    if (v == &g_rx) FlsSetValue(g_ix2, &g_ry);
}
static void *again_thread(void *p) { (void)p; FlsSetValue(g_ix2, &g_rx); return NULL; }
static void *quiet_thread(void *p) { (void)p; return FlsGetValue(g_ix); }
static void test_thread_end(void) {
    SECTION("the end of a thread: plat CreateThread (before its handle is signalled), pthread, converted thread, a callback that sets again");
    size_t sets0 = plat_fls_sets();
    HANDLE h = CreateThread(NULL, 0, plat_thread_fn, NULL, 0, NULL);
    CHECK(WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0 && calls(&g_rt) == 1);
    CloseHandle(h);
    pthread_t t;
    void *r = (void *)1;
    pthread_create(&t, NULL, pthread_fn, NULL); pthread_join(t, NULL);
    CHECK(calls(&g_rp) == 1);
    pthread_create(&t, NULL, conv_thread, NULL); pthread_join(t, NULL);
    CHECK(calls(&g_rconv_thread) == 1 && calls(&g_rconv_fiber) == 0);   /* the other fiber is not running: its value stays with it */
    DeleteFiber(g_conv_other);
    CHECK(calls(&g_rconv_fiber) == 1);
    g_ix2 = FlsAlloc(cb_again);
    pthread_create(&t, NULL, again_thread, NULL); pthread_join(t, NULL);
    CHECKF(calls(&g_rx) == 1 && calls(&g_ry) == 1, "%d %d", calls(&g_rx), calls(&g_ry));
    FlsFree(g_ix2);
    pthread_create(&t, NULL, quiet_thread, NULL); pthread_join(t, &r);
    CHECK(r == NULL);
    CHECKF(plat_fls_sets() == sets0, "%zu -> %zu", sets0, plat_fls_sets());
}

/* ---------------------------------------------------------------- FlsFree */
static Rec g_fm, g_ff[3], g_flive;
static DWORD g_ixf;
static LPVOID g_ffib[3], g_ffmain;
static void *g_after_free[3];
static void WINAPI free_fiber(LPVOID p) {
    int i = (int)(uintptr_t)p;
    if (i < 2) FlsSetValue(g_ixf, &g_ff[i]);              /* fiber 2 never sets one */
    SwitchToFiber(g_ffmain);
    for (;;) { g_after_free[i] = FlsGetValue(g_ixf); SwitchToFiber(g_ffmain); }
}
static HANDLE g_live_go, g_live_set;
static DWORD WINAPI live_thread(LPVOID p) { (void)p; FlsSetValue(g_ixf, &g_flive); SetEvent(g_live_set); WaitForSingleObject(g_live_go, INFINITE); return 0; }
static void test_fls_free(void) {
    SECTION("FlsFree: the callback once for each fiber's and thread's non-NULL value, the index empty when handed out again");
    size_t sets0 = plat_fls_sets();
    g_ixf = FlsAlloc(cb_rec);
    FlsSetValue(g_ixf, &g_fm);
    g_ffmain = ConvertThreadToFiber(NULL);
    for (int i = 0; i < 3; i++) { g_ffib[i] = CreateFiber(1 << 16, free_fiber, (LPVOID)(uintptr_t)i); SwitchToFiber(g_ffib[i]); }
    g_live_go = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_live_set = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, live_thread, NULL, 0, NULL);
    WaitForSingleObject(g_live_set, INFINITE);
    CHECK(FlsFree(g_ixf));
    CHECKF(calls(&g_fm) == 1 && calls(&g_ff[0]) == 1 && calls(&g_ff[1]) == 1 && calls(&g_flive) == 1, "%d %d %d %d", calls(&g_fm), calls(&g_ff[0]), calls(&g_ff[1]), calls(&g_flive));
    SetLastError(0); CHECK(FlsGetValue(g_ixf) == NULL && GetLastError() == ERROR_INVALID_PARAMETER);
    DWORD re = FlsAlloc(cb_rec);                                      /* very likely the same index */
    for (int i = 0; i < 3; i++) { g_after_free[i] = (void *)1; SwitchToFiber(g_ffib[i]); }
    CHECK(g_after_free[0] == NULL && g_after_free[1] == NULL && g_after_free[2] == NULL && FlsGetValue(re) == NULL);
    SetEvent(g_live_go);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    for (int i = 0; i < 3; i++) DeleteFiber(g_ffib[i]);
    ConvertFiberToThread();
    CHECK(calls(&g_fm) == 1 && calls(&g_ff[0]) == 1 && calls(&g_ff[1]) == 1 && calls(&g_flive) == 1);   /* nothing twice */
    FlsFree(re);
    CloseHandle(g_live_go); CloseHandle(g_live_set);
    CHECKF(plat_fls_sets() == sets0, "%zu -> %zu", sets0, plat_fls_sets());
}

/* ---------------------------------------------------------------- nn::os guest fibers on one host thread */
static uint8_t g_gf[2][0x100];
static Rec g_grec[2], g_gthread;
static void *g_gcur[2];
static uint64_t g_gnext[2];
static int g_gruns[2], g_gbad;
static void gfib_fn(Ctx *c) {
    int i = (int)c->x[0];
    void *me = GetCurrentFiber();
    g_gcur[i] = me;
    if (FlsGetValue(g_ix) != NULL) g_gbad |= 1;                      /* a new guest fiber starts empty */
    FlsSetValue(g_ix, &g_grec[i]);
    for (;;) {
        g_gruns[i]++;
        if (FlsGetValue(g_ix) != &g_grec[i]) g_gbad |= 2;
        if (GetCurrentFiber() != me || !me) g_gbad |= 4;
        if (imp(I_FIB_CUR, c, 0, 0, 0, 0, 0, 0) != P(g_gf[i])) g_gbad |= 8;
        imp(I_FIB_SWITCH, c, g_gnext[i], 0, 0, 0, 0, 0);
    }
}
static int g_tbad;
static DWORD WINAPI one_thread_fn(LPVOID p) {
    (void)p;
    Ctx c;
    memset(&c, 0, sizeof c);
    FlsSetValue(g_ix, &g_gthread);                                     /* before the first switch: the thread's own */
    for (int i = 0; i < 2; i++) imp(I_FIB_INIT, &c, P(g_gf[i]), P(gfib_fn), (uint64_t)i, P(g_gstack), sizeof g_gstack, 0);
    for (int k = 0; k < 50; k++) {
        g_gnext[0] = k & 1 ? P(g_gf[1]) : 0;                            /* fiber 0 -> fiber 1 directly every other round */
        g_gnext[1] = 0;
        imp(I_FIB_SWITCH, &c, P(g_gf[0]), 0, 0, 0, 0, 0);
        if (!(k & 1)) imp(I_FIB_SWITCH, &c, P(g_gf[1]), 0, 0, 0, 0, 0);
        if (FlsGetValue(g_ix) != &g_gthread) g_tbad |= 1;              /* the thread's value, carried by its own fiber */
        if (imp(I_FIB_CUR, &c, 0, 0, 0, 0, 0, 0) != 0) g_tbad |= 2;
    }
    if (!GetCurrentFiber() || GetCurrentFiber() == g_gcur[0] || GetCurrentFiber() == g_gcur[1]) g_tbad |= 4;
    for (int i = 0; i < 2; i++) imp(I_FIB_FINI, &c, P(g_gf[i]), 0, 0, 0, 0, 0);
    if (calls(&g_grec[0]) != 1 || calls(&g_grec[1]) != 1 || calls(&g_gthread) != 0) g_tbad |= 8;   /* FinalizeFiber */
    return 0;
}
static void test_guest_one_thread(void) {
    SECTION("nn::os fibers: two on one host thread, own values and GetCurrentFiber; the thread's value kept; FinalizeFiber callback");
    size_t sets0 = plat_fls_sets();
    HANDLE h = CreateThread(NULL, 0, one_thread_fn, NULL, 0, NULL);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    CHECKF(g_gbad == 0 && g_tbad == 0, "fiber %x thread %x", g_gbad, g_tbad);
    CHECK(g_gruns[0] == 50 && g_gruns[1] == 50 && g_gcur[0] && g_gcur[1] && g_gcur[0] != g_gcur[1]);
    CHECK(calls(&g_grec[0]) == 1 && calls(&g_grec[1]) == 1 && calls(&g_gthread) == 1);   /* the thread's at its end */
    CHECKF(plat_fls_sets() == sets0, "%zu -> %zu", sets0, plat_fls_sets());
}

/* ---------------------------------------------------------------- one guest fiber resumed by two nn::os threads */
#define MIG_ROUNDS 3000
static uint8_t g_mf[0x100], g_mtt[2][0x200];
static Rec g_mrec, g_mthread[2];
static int g_mturn, g_mbad, g_mruns;
static pid_t g_mtid;
static void mig_fiber(Ctx *c) {
    void *me = GetCurrentFiber();
    FlsSetValue(g_ix, &g_mrec);
    for (;;) {
        g_mruns++;
        if (tid_now() != g_mtid) g_mbad |= 1;
        if (FlsGetValue(g_ix) != &g_mrec) g_mbad |= 2;                 /* its own value, whichever thread runs it */
        if (GetCurrentFiber() != me) g_mbad |= 4;
        if (g_mruns % 5 == 0) { c->x[0] = 0; return; }                  /* by return: fiber_main switches to the running thread */
        imp(I_FIB_SWITCH, c, 0, 0, 0, 0, 0, 0);
    }
}
static void mig_thread(Ctx *c) {
    int me = (int)c->x[0];
    FlsSetValue(g_ix, &g_mthread[me]);
    for (int k = 0; k < MIG_ROUNDS; k++) {
        for (int spin = 0; __atomic_load_n(&g_mturn, __ATOMIC_ACQUIRE) != me; spin++) if (spin > 1000) sched_yield(); else YieldProcessor();
        g_mtid = tid_now();
        imp(I_FIB_SWITCH, c, P(g_mf), 0, 0, 0, 0, 0);
        if (FlsGetValue(g_ix) != &g_mthread[me]) g_mbad |= 16;
        __atomic_store_n(&g_mturn, 1 - me, __ATOMIC_RELEASE);
    }
}
static void test_guest_migration(void) {
    SECTION("nn::os fiber resumed alternately by two nn::os threads: its value follows it; the threads' values at their end");
    size_t sets0 = plat_fls_sets();
    Ctx c;
    memset(&c, 0, sizeof c);
    imp(I_FIB_INIT, &c, P(g_mf), P(mig_fiber), 0, P(g_gstack), sizeof g_gstack, 0);
    for (int i = 0; i < 2; i++) run_guest_thread(g_mtt[i], mig_thread, (uint64_t)i);
    for (int i = 0; i < 2; i++) wait_guest_thread(g_mtt[i]);
    CHECKF(g_mbad == 0, "mask %x", g_mbad);
    CHECK(g_mruns == 2 * MIG_ROUNDS);
    CHECK(calls_settled(&g_mthread[0], 1) == 1 && calls_settled(&g_mthread[1], 1) == 1 && calls(&g_mrec) == 0);
    imp(I_FIB_FINI, &c, P(g_mf), 0, 0, 0, 0, 0);
    CHECK(calls(&g_mrec) == 1);
    CHECKF(plat_fls_sets() == sets0, "%zu -> %zu", sets0, plat_fls_sets());   /* also: the threads' own fiber objects went */
}

/* ---------------------------------------------------------------- fighter_host.c's FsFiber */
typedef struct { jmp_buf jb; int rec_sp, runs; } FsFiber;      /* the fields that matter here */
static DWORD g_fs_fls = FLS_OUT_OF_INDEXES;
static __attribute__((noinline)) FsFiber *fs_fiber(void) {    /* fighter_host.c, verbatim (noinline: not into the setjmp frame) */
    FsFiber *ff = FlsGetValue(g_fs_fls);
    if (!ff) { ff = calloc(1, sizeof *ff); FlsSetValue(g_fs_fls, ff); }
    return ff;
}
static uint8_t g_hf[2][0x100], g_htt[2][0x200];
static FsFiber *g_hff[2], *g_hthread_ff[2];
static int g_hbad;
static pid_t g_htid[2];
static void fs_fiber_fn(Ctx *c) {
    int i = (int)c->x[0];
    for (volatile int run = 0;; run++) {
        FsFiber *ff = fs_fiber();
        if (run == 0) { g_hff[i] = ff; ff->rec_sp = 100 * (i + 1); }
        if (ff != g_hff[i] || ff->rec_sp != 100 * (i + 1) + run || ff->runs != run) g_hbad |= 1 << i;
        g_htid[i] = tid_now();
        volatile int jumped = 0;
        if (setjmp(ff->jb) == 0) longjmp(ff->jb, 1); else jumped = 1;   /* the jmp_buf works where the fiber now runs */
        if (!jumped) g_hbad |= 4;
        ff->rec_sp++; ff->runs++;
        imp(I_FIB_SWITCH, c, 0, 0, 0, 0, 0, 0);
    }
}
static void fs_thread(Ctx *c) {                                  /* thread 0 starts both fibers, thread 1 resumes both */
    int me = (int)c->x[0];
    g_hthread_ff[me] = fs_fiber();                               /* the thread's own, before any switch */
    if (me == 0) for (int i = 0; i < 2; i++) imp(I_FIB_INIT, c, P(g_hf[i]), P(fs_fiber_fn), (uint64_t)i, P(g_gstack), sizeof g_gstack, 0);
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 2; i++) {
            imp(I_FIB_SWITCH, c, P(g_hf[i]), 0, 0, 0, 0, 0);
            if (g_htid[i] != tid_now()) g_hbad |= 8;
        }
    if (fs_fiber() != g_hthread_ff[me]) g_hbad |= 16;
}
static void test_fighter_fsfiber(void) {
    SECTION("fighter_sdk FsFiber (FlsAlloc(NULL) through win32_posix.h): two guest fibers, each moved to another thread, own block");
    g_fs_fls = FlsAlloc(NULL);
    CHECK(g_fs_fls != FLS_OUT_OF_INDEXES);
    run_guest_thread(g_htt[0], fs_thread, 0); wait_guest_thread(g_htt[0]);
    pid_t first[2] = {g_htid[0], g_htid[1]};
    run_guest_thread(g_htt[1], fs_thread, 1); wait_guest_thread(g_htt[1]);
    CHECKF(g_hbad == 0, "mask %x", g_hbad);
    CHECK(first[0] != g_htid[0] && first[1] != g_htid[1]);           /* both moved */
    CHECK(g_hff[0] && g_hff[1] && g_hff[0] != g_hff[1] && g_hthread_ff[0] != g_hthread_ff[1]);
    CHECK(g_hff[0]->runs == 4 && g_hff[1]->runs == 4 && g_hff[0]->rec_sp == 104 && g_hff[1]->rec_sp == 204);
    for (int i = 0; i < 2; i++) CHECK(g_hthread_ff[i] != g_hff[0] && g_hthread_ff[i] != g_hff[1]);
    Ctx c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < 2; i++) imp(I_FIB_FINI, &c, P(g_hf[i]), 0, 0, 0, 0, 0);
    for (int i = 0; i < 2; i++) { free(g_hff[i]); free(g_hthread_ff[i]); }   /* fighter_host.c passes no callback: its blocks are never freed */
}

/* ---------------------------------------------------------------- soak */
#define SOAK 10500
static int g_soak_calls;
static void WINAPI cb_count(PVOID v) { (void)v; __atomic_add_fetch(&g_soak_calls, 1, __ATOMIC_RELAXED); }
static DWORD g_ixs;
static LPVOID g_smain;
static void WINAPI soak_fiber(LPVOID p) { FlsSetValue(g_ixs, p); SwitchToFiber(g_smain); for (;;) SwitchToFiber(g_smain); }
static DWORD WINAPI soak_thread(LPVOID p) { FlsSetValue(g_ixs, p); return 0; }
static void *soak_pthread(void *p) { FlsSetValue(g_ixs, p); return NULL; }
static uint8_t g_sf[0x100];
static void soak_guest_fiber(Ctx *c) { FlsSetValue(g_ixs, (void *)c->x[0]); c->x[0] = 0; }
static DWORD WINAPI soak_guest_driver(LPVOID p) {
    int n = (int)(intptr_t)p;
    Ctx c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < n; i++) {
        imp(I_FIB_INIT, &c, P(g_sf), P(soak_guest_fiber), (uint64_t)(i + 1), P(g_gstack), sizeof g_gstack, 0);
        imp(I_FIB_SWITCH, &c, P(g_sf), 0, 0, 0, 0, 0);
        imp(I_FIB_FINI, &c, P(g_sf), 0, 0, 0, 0, 0);
    }
    return 0;
}
static void test_soak(void) {
    char title[200];
    snprintf(title, sizeof title, "soak: %d Win32 fibers, %d nn::os fibers, %d plat threads + 500 pthreads, each with an FLS value", SOAK, SOAK, SOAK);
    SECTION(title);
    g_ixs = FlsAlloc(cb_count);
    size_t sets0 = plat_fls_sets(), regions0 = vmem_region_count();
    int64_t t0 = tp_now_us();
    /* Win32 fibers (the first 100 warm the heap up) */
    g_smain = ConvertThreadToFiber(NULL);
    size_t heap0 = 0;
    for (int i = 0; i < SOAK; i++) {
        if (i == 100) heap0 = heap_used();
        LPVOID f = CreateFiber(1 << 16, soak_fiber, (LPVOID)(uintptr_t)(i + 1));
        SwitchToFiber(f);
        DeleteFiber(f);
    }
    ConvertFiberToThread();
    long dheap_fib = (long)heap_used() - (long)heap0;
    CHECKF(g_soak_calls == SOAK, "%d callbacks", g_soak_calls);
    CHECKF(plat_fls_sets() == sets0, "%zu -> %zu", sets0, plat_fls_sets());
    /* nn::os fibers on one host thread, one FiberType reused (as the game's fiber pools do) */
    __atomic_store_n(&g_soak_calls, 0, __ATOMIC_RELAXED);
    HANDLE h = CreateThread(NULL, 0, soak_guest_driver, (LPVOID)(intptr_t)100, 0, NULL);
    WaitForSingleObject(h, INFINITE); CloseHandle(h);
    size_t heap1 = heap_used();
    h = CreateThread(NULL, 0, soak_guest_driver, (LPVOID)(intptr_t)SOAK, 0, NULL);
    WaitForSingleObject(h, INFINITE); CloseHandle(h);
    long dheap_guest = (long)heap_used() - (long)heap1;
    CHECKF(g_soak_calls == SOAK + 100, "%d callbacks", g_soak_calls);
    CHECKF(plat_fls_sets() == sets0, "%zu -> %zu", sets0, plat_fls_sets());
    CHECKF(vmem_region_count() <= regions0 + 4, "vmem regions %zu -> %zu", regions0, vmem_region_count());
    /* threads */
    __atomic_store_n(&g_soak_calls, 0, __ATOMIC_RELAXED);
    size_t heap2 = 0;
    for (int i = 0; i < SOAK; i++) {
        if (i == 100) heap2 = heap_used();
        h = CreateThread(NULL, 1 << 16, soak_thread, (LPVOID)(uintptr_t)(i + 1), 0, NULL);
        if (!h) { CHECKF(0, "CreateThread %d", i); break; }
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    long dheap_thr = (long)heap_used() - (long)heap2;
    for (int i = 0; i < 500; i++) { pthread_t t; pthread_create(&t, NULL, soak_pthread, (void *)(uintptr_t)(i + 1)); pthread_join(t, NULL); }
    CHECKF(g_soak_calls == SOAK + 500, "%d callbacks", g_soak_calls);
    CHECKF(plat_fls_sets() == sets0, "%zu -> %zu", sets0, plat_fls_sets());
    FlsFree(g_ixs);
    printf("    heap growth: Win32 fibers %ld B, threads %ld B, nn::os fibers %ld B (%.0f B per fiber: os_threads.c's guest Ctx, %zu B, is never freed - as on Windows); %.1f s\n",
           dheap_fib, dheap_thr, dheap_guest, (double)dheap_guest / SOAK, sizeof(Ctx), (tp_now_us() - t0) / 1e6);
#if !defined(TP_TSAN) && !defined(__SANITIZE_ADDRESS__)
    CHECKF(dheap_fib < 64 * 1024 && dheap_thr < 64 * 1024, "%ld %ld", dheap_fib, dheap_thr);
    CHECKF(dheap_guest < (long)SOAK * (long)(sizeof(Ctx) + 64) + 64 * 1024, "%ld", dheap_guest);   /* nothing beyond the known Ctx */
#endif
}

int main(void) {
    vmem_init();
    test_api();
    test_win32_fibers();
    test_thread_end();
    test_fls_free();
    test_guest_one_thread();
    test_guest_migration();
    test_fighter_fsfiber();
    test_soak();
    return tp_done("test_fls");
}
