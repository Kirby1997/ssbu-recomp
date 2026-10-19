/* plat_posix.c - implementation of plat.h on Linux (see plat.h for semantics and the DIFF notes). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "plat.h"
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <dlfcn.h>
#include <link.h>
#include <malloc.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <sys/random.h>

#if defined(__SANITIZE_THREAD__)
#define PLAT_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PLAT_TSAN 1
#endif
#endif
#ifdef PLAT_TSAN
void *__tsan_get_current_fiber(void);
void *__tsan_create_fiber(unsigned flags);
void __tsan_destroy_fiber(void *fiber);
void __tsan_switch_to_fiber(void *fiber, unsigned flags);
#endif

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "INIT_ONCE futex uses the low word of a pointer");

#define NOINL __attribute__((noinline))   /* everything that touches TLS: safe across fiber migration (fiber.h) */

/* ------------------------------------------------------------------------------------------------ basics */
static int futex_wait(uint32_t *a, uint32_t v, const struct timespec *rel) {
    return syscall(SYS_futex, a, FUTEX_WAIT_PRIVATE, v, rel, NULL, 0) == 0 ? 0 : errno;
}
static void futex_wake(uint32_t *a, int n) { syscall(SYS_futex, a, FUTEX_WAKE_PRIVATE, n, NULL, NULL, 0); }
static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static struct timespec ns_ts(int64_t ns) {
    if (ns < 0) ns = 0;
    struct timespec t = {(time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL)};
    return t;
}
void plat_fatal(const char *fmt, ...) {
    va_list ap;
    fputs("FATAL plat: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    abort();
}
void plat_lock_failed_(const char *what, int err) {
    plat_fatal("%s: %s (recursive exclusive acquire, or release by another thread than the acquirer?)", what, strerror(err));
}

static _Thread_local DWORD t_err;
static _Thread_local pid_t t_tid;
NOINL DWORD GetLastError(void) { return t_err; }
NOINL void SetLastError(DWORD e) { t_err = e; }
NOINL DWORD GetCurrentThreadId(void) {
    if (__builtin_expect(!t_tid, 0)) t_tid = (pid_t)syscall(SYS_gettid);
    return (DWORD)t_tid;
}
static void atfork_child(void) { t_tid = 0; }
__attribute__((constructor)) static void plat_ctor(void) { pthread_atfork(NULL, NULL, atfork_child); }

/* ------------------------------------------------------------------------------------------------ critical section */
void InitializeCriticalSection(LPCRITICAL_SECTION cs) {
    memset(cs, 0, sizeof *cs);
    pthread_mutex_init(&cs->mu, NULL);          /* PTHREAD_MUTEX_NORMAL: no owner check, recursion is ours */
    cs->LockCount = -1;
}
BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin) { InitializeCriticalSection(cs); cs->SpinCount = spin; return TRUE; }
BOOL InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD spin, DWORD flags) { (void)flags; return InitializeCriticalSectionAndSpinCount(cs, spin); }
void DeleteCriticalSection(LPCRITICAL_SECTION cs) { pthread_mutex_destroy(&cs->mu); }
static inline HANDLE tid_handle(DWORD t) { return (HANDLE)(uintptr_t)t; }
NOINL void EnterCriticalSection(LPCRITICAL_SECTION cs) {
    HANDLE me = tid_handle(GetCurrentThreadId());
    if (__atomic_load_n(&cs->OwningThread, __ATOMIC_RELAXED) == me) { cs->RecursionCount++; return; }
    __atomic_add_fetch(&cs->LockCount, 1, __ATOMIC_RELAXED);
    int e = pthread_mutex_lock(&cs->mu);
    if (e) plat_lock_failed_("EnterCriticalSection", e);
    __atomic_store_n(&cs->OwningThread, me, __ATOMIC_RELAXED);
    cs->RecursionCount = 1;
}
NOINL BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs) {
    HANDLE me = tid_handle(GetCurrentThreadId());
    if (__atomic_load_n(&cs->OwningThread, __ATOMIC_RELAXED) == me) { cs->RecursionCount++; return TRUE; }
    if (pthread_mutex_trylock(&cs->mu)) return FALSE;
    __atomic_add_fetch(&cs->LockCount, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&cs->OwningThread, me, __ATOMIC_RELAXED);
    cs->RecursionCount = 1;
    return TRUE;
}
NOINL void LeaveCriticalSection(LPCRITICAL_SECTION cs) {
    if (--cs->RecursionCount > 0) return;
    if (cs->RecursionCount < 0) plat_fatal("LeaveCriticalSection(%p): not entered", (void *)cs);
    __atomic_store_n(&cs->OwningThread, (HANDLE)0, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&cs->LockCount, 1, __ATOMIC_RELAXED);
    int e = pthread_mutex_unlock(&cs->mu);
    if (e) plat_lock_failed_("LeaveCriticalSection", e);
}

/* ------------------------------------------------------------------------------------------------ SRW */
void InitializeSRWLock(PSRWLOCK l) {
    pthread_rwlockattr_t a;
    pthread_rwlockattr_init(&a);
#ifdef PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP
    pthread_rwlockattr_setkind_np(&a, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
    pthread_rwlock_init(&l->rw, &a);
    pthread_rwlockattr_destroy(&a);
}

/* ------------------------------------------------------------------------------------------------ condition variable */
struct plat_cv_node { struct plat_cv_node *next; uint32_t signaled; };
void InitializeConditionVariable(PCONDITION_VARIABLE cv) {
    pthread_mutex_init(&cv->lk, NULL);
    cv->head = cv->tail = NULL;
}
typedef struct { int kind; PCRITICAL_SECTION cs; PSRWLOCK srw; int shared; LONG saved; } cv_lock;
static void cv_release(cv_lock *l) {
    if (l->kind == 0) {
        PCRITICAL_SECTION cs = l->cs;
        if (cs->RecursionCount <= 0) plat_fatal("SleepConditionVariableCS: critical section %p not entered", (void *)cs);
        l->saved = cs->RecursionCount;
        cs->RecursionCount = 0;
        __atomic_store_n(&cs->OwningThread, (HANDLE)0, __ATOMIC_RELAXED);
        __atomic_sub_fetch(&cs->LockCount, 1, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&cs->mu);
    } else if (l->shared) ReleaseSRWLockShared(l->srw);
    else ReleaseSRWLockExclusive(l->srw);
}
static void cv_reacquire(cv_lock *l) {
    if (l->kind == 0) {
        PCRITICAL_SECTION cs = l->cs;
        __atomic_add_fetch(&cs->LockCount, 1, __ATOMIC_RELAXED);
        pthread_mutex_lock(&cs->mu);
        __atomic_store_n(&cs->OwningThread, tid_handle(GetCurrentThreadId()), __ATOMIC_RELAXED);
        cs->RecursionCount = l->saved;
    } else if (l->shared) AcquireSRWLockShared(l->srw);
    else AcquireSRWLockExclusive(l->srw);
}
static BOOL cv_sleep(PCONDITION_VARIABLE cv, cv_lock *l, DWORD ms) {
    struct plat_cv_node n = {NULL, 0};
    int64_t deadline = ms == INFINITE ? INT64_MAX : mono_ns() + (int64_t)ms * 1000000;
    pthread_mutex_lock(&cv->lk);
    if (cv->tail) cv->tail->next = &n; else cv->head = &n;
    cv->tail = &n;
    pthread_mutex_unlock(&cv->lk);
    cv_release(l);
    while (!__atomic_load_n(&n.signaled, __ATOMIC_ACQUIRE)) {
        struct timespec ts, *tp = NULL;
        if (deadline != INT64_MAX) {
            int64_t left = deadline - mono_ns();
            if (left <= 0) break;
            ts = ns_ts(left);
            tp = &ts;
        }
        futex_wait(&n.signaled, 0, tp);          /* EINTR / EAGAIN / ETIMEDOUT: the loop decides */
    }
    pthread_mutex_lock(&cv->lk);
    int signaled = (int)n.signaled;
    if (!signaled) {                             /* timed out: unlink our node */
        struct plat_cv_node **pp = &cv->head, *prev = NULL;
        while (*pp && *pp != &n) { prev = *pp; pp = &(*pp)->next; }
        if (*pp) { *pp = n.next; if (cv->tail == &n) cv->tail = prev; }
    }
    pthread_mutex_unlock(&cv->lk);
    cv_reacquire(l);
    if (!signaled) { SetLastError(ERROR_TIMEOUT); return FALSE; }
    return TRUE;
}
NOINL BOOL SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms) {
    cv_lock l = {0, cs, NULL, 0, 0};
    return cv_sleep(cv, &l, ms);
}
NOINL BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK srw, DWORD ms, ULONG flags) {
    cv_lock l = {1, NULL, srw, (flags & CONDITION_VARIABLE_LOCKMODE_SHARED) != 0, 0};
    return cv_sleep(cv, &l, ms);
}
static void cv_signal(struct plat_cv_node *n) {   /* under cv->lk: the waiter cannot leave before we are done */
    __atomic_store_n(&n->signaled, 1, __ATOMIC_RELEASE);
    futex_wake(&n->signaled, 1);
}
void WakeConditionVariable(PCONDITION_VARIABLE cv) {
    pthread_mutex_lock(&cv->lk);
    struct plat_cv_node *n = cv->head;
    if (n) {
        cv->head = n->next;
        if (!cv->head) cv->tail = NULL;
        cv_signal(n);
    }
    pthread_mutex_unlock(&cv->lk);
}
void WakeAllConditionVariable(PCONDITION_VARIABLE cv) {
    pthread_mutex_lock(&cv->lk);
    struct plat_cv_node *n = cv->head;
    cv->head = cv->tail = NULL;
    while (n) { struct plat_cv_node *next = n->next; cv_signal(n); n = next; }
    pthread_mutex_unlock(&cv->lk);
}

/* ------------------------------------------------------------------------------------------------ init once */
void InitOnceInitialize(PINIT_ONCE once) { once->Ptr = NULL; }
BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, LPVOID *ctx) {
    uintptr_t *w = (uintptr_t *)&once->Ptr;
    for (;;) {
        uintptr_t v = __atomic_load_n(w, __ATOMIC_ACQUIRE);
        if ((v & 3) == 2) { if (ctx) *ctx = (LPVOID)(v & ~(uintptr_t)3); return TRUE; }
        if ((v & 3) == 0) {
            if (__atomic_compare_exchange_n(w, &v, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
            continue;
        }
        futex_wait((uint32_t *)w, 1, NULL);      /* low word is 1 while the callback runs */
    }
    PVOID c = NULL;
    BOOL ok = fn(once, param, &c);
    __atomic_store_n(w, ok ? (((uintptr_t)c & ~(uintptr_t)3) | 2) : 0, __ATOMIC_RELEASE);
    futex_wake((uint32_t *)w, INT_MAX);
    if (ok && ctx) *ctx = c;
    return ok;
}

/* ------------------------------------------------------------------------------------------------ time */
void Sleep(DWORD ms) {
    if (ms == 0) { sched_yield(); return; }
    if (ms == INFINITE) for (;;) pause();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t t = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec + (int64_t)ms * 1000000;
    ts = ns_ts(t);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}
#define FILETIME_UNIX_EPOCH 116444736000000000LL
void GetSystemTimeAsFileTime(LPFILETIME ft) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t v = (uint64_t)ts.tv_sec * 10000000u + (uint64_t)ts.tv_nsec / 100u + FILETIME_UNIX_EPOCH;
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}
static void fill_systemtime(LPSYSTEMTIME st, int local) {
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t s = ts.tv_sec;
    if (local) localtime_r(&s, &tm); else gmtime_r(&s, &tm);
    st->wYear = (WORD)(tm.tm_year + 1900); st->wMonth = (WORD)(tm.tm_mon + 1); st->wDayOfWeek = (WORD)tm.tm_wday;
    st->wDay = (WORD)tm.tm_mday; st->wHour = (WORD)tm.tm_hour; st->wMinute = (WORD)tm.tm_min;
    st->wSecond = (WORD)(tm.tm_sec > 59 ? 59 : tm.tm_sec); st->wMilliseconds = (WORD)(ts.tv_nsec / 1000000);
}
void GetSystemTime(LPSYSTEMTIME st) { fill_systemtime(st, 0); }
void GetLocalTime(LPSYSTEMTIME st) { fill_systemtime(st, 1); }

/* ------------------------------------------------------------------------------------------------ handles */
enum { K_THREAD = 1, K_EVENT, K_SEM, K_MUTEX, K_TIMER, K_PROCESS, K_CUSTOM };
#define H_MAGIC 0x504c4854u
typedef struct waiter { uint32_t futex; } waiter;
typedef struct wnode { struct wnode *next, *prev; waiter *w; } wnode;
typedef struct hobj {
    uint32_t magic;
    int kind;
    int refs;
    wnode *waiters;
    /* thread */
    pthread_t pt;
    pid_t tid;
    LPTHREAD_START_ROUTINE fn;
    void *arg;
    DWORD exit_code;
    int finished, prio, nice_applied, suspend;
    uint32_t started, gate;
    /* event / timer */
    int signaled, manual;
    /* semaphore */
    LONG count, max;
    /* mutex */
    pid_t owner;
    LONG recursion;
    int lockfd, flocked;
    char *name;
    struct hobj *next_named;
    /* timer */
    int64_t due;
    LONG period;
    int set;
    /* process */
    pid_t pid;
    int pidfd;
    /* custom */
    const plat_handle_ops *ops;
    void *payload;
} hobj;

static pthread_mutex_t g_hmu = PTHREAD_MUTEX_INITIALIZER;   /* all waitable state */
static hobj *g_named;                                        /* named mutexes (under g_hmu) */
static _Thread_local hobj *t_self;
static void fls_thread_end(void);
static pthread_key_t g_self_key;
static pthread_once_t g_self_once = PTHREAD_ONCE_INIT;

#define PSEUDO_PROCESS ((HANDLE)(intptr_t)-1)
#define PSEUDO_THREAD ((HANDLE)(intptr_t)-2)

static hobj *hnew(int kind) {
    hobj *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->magic = H_MAGIC;
    h->kind = kind;
    h->refs = 1;
    h->lockfd = h->pidfd = -1;
    return h;
}
static void hdestroy(hobj *h) {
    if (h->kind == K_MUTEX && h->name) {
        pthread_mutex_lock(&g_hmu);
        for (hobj **pp = &g_named; *pp; pp = &(*pp)->next_named) if (*pp == h) { *pp = h->next_named; break; }
        pthread_mutex_unlock(&g_hmu);
    }
    if (h->lockfd >= 0) close(h->lockfd);
    if (h->pidfd >= 0) close(h->pidfd);
    if (h->kind == K_CUSTOM && h->ops && h->ops->destroy) h->ops->destroy(h->payload);
    free(h->name);
    h->magic = 0;
    free(h);
}
static void hunref(hobj *h) {
    if (__atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL) == 0) hdestroy(h);
}
static hobj *hget(HANDLE x) {
    if (!x || x == PSEUDO_PROCESS || x == PSEUDO_THREAD) return NULL;
    hobj *h = (hobj *)x;
    return h->magic == H_MAGIC ? h : NULL;
}
static void wake_waiters(hobj *h) {              /* under g_hmu */
    for (wnode *n = h->waiters; n; n = n->next) {
        __atomic_store_n(&n->w->futex, 1, __ATOMIC_RELEASE);
        futex_wake(&n->w->futex, 1);
    }
}

HANDLE plat_handle_create(const plat_handle_ops *ops, void *payload) {
    hobj *h = hnew(K_CUSTOM);
    if (!h) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    h->ops = ops;
    h->payload = payload;
    return h;
}
void *plat_handle_payload(HANDLE x, const plat_handle_ops *ops) {
    hobj *h = hget(x);
    return h && h->kind == K_CUSTOM && h->ops == ops ? h->payload : NULL;
}
BOOL CloseHandle(HANDLE x) {
    if (x == PSEUDO_PROCESS || x == PSEUDO_THREAD) return TRUE;
    hobj *h = hget(x);
    if (!h) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    hunref(h);
    return TRUE;
}

/* ---- threads */
static void thread_finish(hobj *h, DWORD code) {
    pthread_mutex_lock(&g_hmu);
    h->finished = 1;
    h->exit_code = code;
    wake_waiters(h);
    pthread_mutex_unlock(&g_hmu);
    hunref(h);                                   /* the thread's own reference */
}
static void self_key_dtor(void *p) { thread_finish(p, 0); }
static void self_key_init(void) { pthread_key_create(&g_self_key, self_key_dtor); }
/* The calling thread's object; made on first use for threads we did not create (main thread, driver threads). */
static NOINL hobj *self_obj(void) {
    hobj *h = t_self;
    if (h) return h;
    pthread_once(&g_self_once, self_key_init);
    h = hnew(K_THREAD);
    if (!h) plat_fatal("out of memory");
    h->tid = (pid_t)GetCurrentThreadId();
    h->pt = pthread_self();
    h->started = h->gate = 1;
    t_self = h;
    pthread_setspecific(g_self_key, h);
    return h;
}
static hobj *thread_of(HANDLE x) {
    if (x == PSEUDO_THREAD) return self_obj();
    hobj *h = hget(x);
    return h && h->kind == K_THREAD ? h : NULL;
}
static NOINL void *thread_start(void *p) {
    hobj *h = p;
    t_self = h;
    h->tid = (pid_t)GetCurrentThreadId();
    __atomic_store_n(&h->started, 1, __ATOMIC_RELEASE);
    futex_wake(&h->started, INT_MAX);
    while (!__atomic_load_n(&h->gate, __ATOMIC_ACQUIRE)) futex_wait(&h->gate, 0, NULL);   /* CREATE_SUSPENDED */
    DWORD code = h->fn(h->arg);
    fls_thread_end();                            /* FLS callbacks before the handle is signalled, as Win32 */
    t_self = NULL;
    thread_finish(h, code);
    return NULL;
}
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD tid) {
    (void)sa;
    hobj *h = hnew(K_THREAD);
    if (!h || !fn) { free(h); SetLastError(fn ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER); return NULL; }
    h->refs = 2;                                 /* the caller's handle + the running thread */
    h->fn = fn;
    h->arg = arg;
    h->suspend = (flags & CREATE_SUSPENDED) ? 1 : 0;
    h->gate = h->suspend ? 0 : 1;
    pthread_attr_t at;
    pthread_attr_init(&at);
    size_t sz = stack ? (size_t)stack : (size_t)8 << 20;
    if (sz < (size_t)PTHREAD_STACK_MIN) sz = PTHREAD_STACK_MIN;
    sz = (sz + 4095) & ~(size_t)4095;
    pthread_attr_setstacksize(&at, sz);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int e = pthread_create(&h->pt, &at, thread_start, h);
    pthread_attr_destroy(&at);
    if (e) { free(h); SetLastError(e == EAGAIN ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER); return NULL; }
    while (!__atomic_load_n(&h->started, __ATOMIC_ACQUIRE)) futex_wait(&h->started, 0, NULL);
    if (tid) *tid = (DWORD)h->tid;
    return h;
}
DWORD ResumeThread(HANDLE x) {
    hobj *h = thread_of(x);
    if (!h) { SetLastError(ERROR_INVALID_HANDLE); return (DWORD)-1; }
    pthread_mutex_lock(&g_hmu);
    DWORD prev = (DWORD)h->suspend;
    if (h->suspend > 0 && --h->suspend == 0) {
        __atomic_store_n(&h->gate, 1, __ATOMIC_RELEASE);
        futex_wake(&h->gate, INT_MAX);
    }
    pthread_mutex_unlock(&g_hmu);
    return prev;
}
DWORD SuspendThread(HANDLE x) {
    hobj *h = thread_of(x);
    if (!h) { SetLastError(ERROR_INVALID_HANDLE); return (DWORD)-1; }
    pthread_mutex_lock(&g_hmu);
    DWORD prev = (DWORD)-1;
    if (h->suspend > 0) prev = (DWORD)h->suspend++;   /* still suspended since creation: count it */
    pthread_mutex_unlock(&g_hmu);
    if (prev == (DWORD)-1) SetLastError(ERROR_NOT_SUPPORTED);
    return prev;
}
BOOL GetExitCodeThread(HANDLE x, LPDWORD code) {
    hobj *h = thread_of(x);
    if (!h || !code) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    pthread_mutex_lock(&g_hmu);
    *code = h->finished ? h->exit_code : STILL_ACTIVE;
    pthread_mutex_unlock(&g_hmu);
    return TRUE;
}
DWORD GetThreadId(HANDLE x) {
    hobj *h = thread_of(x);
    if (!h) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    return (DWORD)h->tid;
}
DWORD GetCurrentProcessorNumber(void) { int c = sched_getcpu(); return c < 0 ? 0 : (DWORD)c; }

static int nice_for(int prio) {
    if (prio >= THREAD_PRIORITY_TIME_CRITICAL) return -10;
    if (prio >= THREAD_PRIORITY_HIGHEST) return -5;
    if (prio == THREAD_PRIORITY_ABOVE_NORMAL) return -2;
    if (prio == THREAD_PRIORITY_NORMAL) return 0;
    if (prio == THREAD_PRIORITY_BELOW_NORMAL) return 2;
    if (prio > THREAD_PRIORITY_IDLE) return 5;
    return 19;
}
static int can_return_to_zero(void) {           /* may a thread that raised its nice value lower it back to 0? */
    static int v = -1;
    if (v < 0) {
        struct rlimit rl;
        v = geteuid() == 0 || (getrlimit(RLIMIT_NICE, &rl) == 0 && (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur >= 20));
    }
    return v;
}
BOOL SetThreadPriority(HANDLE x, int prio) {
    hobj *h = thread_of(x);
    if (!h) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    h->prio = prio;
    int n = nice_for(prio);
    if (n > 0 && !can_return_to_zero()) n = 0;  /* would be a one-way trip: record only */
    if (h->finished || n == h->nice_applied) return TRUE;
    if (setpriority(PRIO_PROCESS, (id_t)h->tid, n) == 0) h->nice_applied = n;
    return TRUE;                                 /* no permission to raise: ignored, as documented */
}
int GetThreadPriority(HANDLE x) {
    hobj *h = thread_of(x);
    if (!h) { SetLastError(ERROR_INVALID_HANDLE); return THREAD_PRIORITY_ERROR_RETURN; }
    return h->prio;
}
BOOL SetThreadPriorityBoost(HANDLE x, BOOL disable) { (void)x; (void)disable; return TRUE; }
static DWORD_PTR set_to_mask(const cpu_set_t *s) {
    DWORD_PTR m = 0;
    for (int i = 0; i < 64; i++) if (CPU_ISSET(i, s)) m |= (DWORD_PTR)1 << i;
    return m;
}
DWORD_PTR SetThreadAffinityMask(HANDLE x, DWORD_PTR mask) {
    hobj *h = thread_of(x);
    if (!h || !mask) { SetLastError(h ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE); return 0; }
    cpu_set_t old, s;
    CPU_ZERO(&old);
    CPU_ZERO(&s);
    if (sched_getaffinity(h->tid, sizeof old, &old)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    for (int i = 0; i < 64; i++) if (mask >> i & 1) CPU_SET(i, &s);
    if (sched_setaffinity(h->tid, sizeof s, &s)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    return set_to_mask(&old);
}
BOOL GetProcessAffinityMask(HANDLE process, PDWORD_PTR pm, PDWORD_PTR sm) {
    (void)process;
    cpu_set_t s;
    CPU_ZERO(&s);
    if (sched_getaffinity(0, sizeof s, &s)) return FALSE;
    if (pm) *pm = set_to_mask(&s);
    if (sm) {
        long n = sysconf(_SC_NPROCESSORS_CONF);
        *sm = n >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << (n > 0 ? n : 1)) - 1);
    }
    return TRUE;
}
BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE dp, LPHANDLE dst, DWORD access, BOOL inherit, DWORD options) {
    (void)sp; (void)dp; (void)access; (void)inherit;
    hobj *h;
    if (!dst) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (src == PSEUDO_THREAD) h = self_obj();
    else if (src == PSEUDO_PROCESS) { *dst = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)getpid()); return *dst != NULL; }
    else if (!(h = hget(src))) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    __atomic_add_fetch(&h->refs, 1, __ATOMIC_ACQ_REL);
    *dst = h;
    if (options & DUPLICATE_CLOSE_SOURCE) CloseHandle(src);
    return TRUE;
}

/* ---- events, semaphores, mutexes, timers, processes */
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name) {
    (void)sa; (void)name;
    hobj *h = hnew(K_EVENT);
    if (!h) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    h->manual = manual != 0;
    h->signaled = initial != 0;
    return h;
}
static hobj *of_kind(HANDLE x, int kind) {
    hobj *h = hget(x);
    if (!h || h->kind != kind) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
    return h;
}
BOOL SetEvent(HANDLE x) {
    hobj *h = of_kind(x, K_EVENT);
    if (!h) return FALSE;
    pthread_mutex_lock(&g_hmu);
    h->signaled = 1;
    wake_waiters(h);
    pthread_mutex_unlock(&g_hmu);
    return TRUE;
}
BOOL ResetEvent(HANDLE x) {
    hobj *h = of_kind(x, K_EVENT);
    if (!h) return FALSE;
    pthread_mutex_lock(&g_hmu);
    h->signaled = 0;
    pthread_mutex_unlock(&g_hmu);
    return TRUE;
}
HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name) {
    (void)sa; (void)name;
    if (maximum <= 0 || initial < 0 || initial > maximum) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    hobj *h = hnew(K_SEM);
    if (!h) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    h->count = initial;
    h->max = maximum;
    return h;
}
BOOL ReleaseSemaphore(HANDLE x, LONG n, LPLONG prev) {
    hobj *h = of_kind(x, K_SEM);
    if (!h) return FALSE;
    if (n <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    pthread_mutex_lock(&g_hmu);
    if (n > h->max - h->count) { pthread_mutex_unlock(&g_hmu); SetLastError(ERROR_TOO_MANY_POSTS); return FALSE; }
    if (prev) *prev = h->count;
    h->count += n;
    wake_waiters(h);
    pthread_mutex_unlock(&g_hmu);
    return TRUE;
}
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL initial_owner, LPCSTR name) {
    (void)sa;
    hobj *h = NULL;
    if (name && *name) {
        pthread_mutex_lock(&g_hmu);
        for (hobj *m = g_named; m; m = m->next_named)
            if (strcmp(m->name, name) == 0) { h = m; __atomic_add_fetch(&h->refs, 1, __ATOMIC_ACQ_REL); break; }
        if (h) { pthread_mutex_unlock(&g_hmu); SetLastError(ERROR_ALREADY_EXISTS); return h; }
        h = hnew(K_MUTEX);
        if (!h || !(h->name = strdup(name))) { pthread_mutex_unlock(&g_hmu); free(h); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
        char path[MAX_PATH], safe[200];
        size_t k = 0;
        for (const char *p = name; *p && k < sizeof safe - 1; p++) safe[k++] = (char)((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' ? *p : '_');
        safe[k] = 0;
        const char *dir = getenv("XDG_RUNTIME_DIR");
        snprintf(path, sizeof path, "%s/ssbu-mutex-%s.lock", dir && *dir ? dir : "/tmp", safe);
        h->lockfd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        h->next_named = g_named;
        g_named = h;
        pthread_mutex_unlock(&g_hmu);
    } else if (!(h = hnew(K_MUTEX))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (initial_owner) WaitForSingleObject(h, INFINITE);
    SetLastError(0);
    return h;
}
BOOL ReleaseMutex(HANDLE x) {
    hobj *h = of_kind(x, K_MUTEX);
    if (!h) return FALSE;
    pid_t me = (pid_t)GetCurrentThreadId();
    pthread_mutex_lock(&g_hmu);
    if (h->owner != me || h->recursion <= 0) { pthread_mutex_unlock(&g_hmu); SetLastError(ERROR_NOT_OWNER); return FALSE; }
    if (--h->recursion == 0) {
        h->owner = 0;
        if (h->flocked) { flock(h->lockfd, LOCK_UN); h->flocked = 0; }
        wake_waiters(h);
    }
    pthread_mutex_unlock(&g_hmu);
    return TRUE;
}
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access) {
    (void)sa; (void)name; (void)access;
    hobj *h = hnew(K_TIMER);
    if (!h) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    h->manual = (flags & CREATE_WAITABLE_TIMER_MANUAL_RESET) != 0;
    return h;
}
HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES sa, BOOL manual, LPCWSTR name) {
    return CreateWaitableTimerExW(sa, name, manual ? CREATE_WAITABLE_TIMER_MANUAL_RESET : 0, TIMER_ALL_ACCESS);
}
BOOL SetWaitableTimer(HANDLE x, const LARGE_INTEGER *due, LONG period, PTIMERAPCROUTINE apc, LPVOID arg, BOOL resume) {
    (void)arg; (void)resume;
    hobj *h = of_kind(x, K_TIMER);
    if (!h) return FALSE;
    if (!due || period < 0 || apc) { SetLastError(apc ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER); return FALSE; }
    int64_t now = mono_ns(), when;
    if (due->QuadPart <= 0) when = now - due->QuadPart * 100;
    else {                                       /* absolute UTC FILETIME */
        struct timespec rt;
        clock_gettime(CLOCK_REALTIME, &rt);
        int64_t real_now = (int64_t)rt.tv_sec * 1000000000LL + rt.tv_nsec;
        when = now + ((due->QuadPart - FILETIME_UNIX_EPOCH) * 100 - real_now);
    }
    pthread_mutex_lock(&g_hmu);
    h->due = when;
    h->period = period;
    h->set = 1;
    h->signaled = 0;
    wake_waiters(h);                             /* waiters recompute their wake-up time */
    pthread_mutex_unlock(&g_hmu);
    return TRUE;
}
BOOL CancelWaitableTimer(HANDLE x) {
    hobj *h = of_kind(x, K_TIMER);
    if (!h) return FALSE;
    pthread_mutex_lock(&g_hmu);
    h->set = 0;
    wake_waiters(h);
    pthread_mutex_unlock(&g_hmu);
    return TRUE;
}
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid) {
    (void)access; (void)inherit;
    if (kill((pid_t)pid, 0) != 0 && errno == ESRCH) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    hobj *h = hnew(K_PROCESS);
    if (!h) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    h->pid = (pid_t)pid;
#ifdef SYS_pidfd_open
    h->pidfd = (int)syscall(SYS_pidfd_open, (pid_t)pid, 0);
#endif
    return h;
}

/* ---- waiting (all under g_hmu) */
#define POLL_NS 10000000LL                       /* process exit and contended named mutexes are polled */
static int is_signaled(hobj *h, pid_t me, int64_t now, int64_t *wake_at) {
    switch (h->kind) {
    case K_THREAD: return h->finished;
    case K_EVENT: return h->signaled;
    case K_SEM: return h->count > 0;
    case K_MUTEX:
        if (h->owner == me) return 1;
        if (h->owner) return 0;
        if (h->lockfd >= 0 && !h->flocked) {     /* named: also exclusive across processes */
            if (flock(h->lockfd, LOCK_EX | LOCK_NB) != 0) { if (now + POLL_NS / 5 < *wake_at) *wake_at = now + POLL_NS / 5; return 0; }
            h->flocked = 1;
        }
        return 1;
    case K_TIMER:
        if (h->signaled) return 1;
        if (!h->set) return 0;
        if (now >= h->due) {
            h->signaled = 1;
            if (h->period > 0) { int64_t p = (int64_t)h->period * 1000000; while (h->due <= now) h->due += p; }
            else h->set = 0;
            return 1;
        }
        if (h->due < *wake_at) *wake_at = h->due;
        return 0;
    case K_PROCESS: {
        int gone;
        if (h->pidfd >= 0) { struct pollfd pf = {h->pidfd, POLLIN, 0}; gone = poll(&pf, 1, 0) > 0; }
        else gone = kill(h->pid, 0) != 0 && errno == ESRCH;
        if (!gone && now + POLL_NS < *wake_at) *wake_at = now + POLL_NS;
        return gone;
    }
    default: return 0;
    }
}
static void consume(hobj *h, pid_t me) {
    switch (h->kind) {
    case K_EVENT: if (!h->manual) h->signaled = 0; break;
    case K_SEM: h->count--; break;
    case K_MUTEX: h->owner = me; h->recursion++; break;
    case K_TIMER: if (!h->manual) h->signaled = 0; break;
    default: break;
    }
}
static void unclaim(hobj *h) {                   /* wait-all not satisfied: give back a cross-process claim */
    if (h->kind == K_MUTEX && h->flocked && h->owner == 0) { flock(h->lockfd, LOCK_UN); h->flocked = 0; }
}
static DWORD wait_objects(DWORD n, hobj **o, BOOL all, DWORD ms) {
    pid_t me = (pid_t)GetCurrentThreadId();
    int64_t deadline = ms == INFINITE ? INT64_MAX : mono_ns() + (int64_t)ms * 1000000;
    wnode nodes[MAXIMUM_WAIT_OBJECTS];
    waiter w;
    pthread_mutex_lock(&g_hmu);
    for (;;) {
        int64_t now = mono_ns(), wake_at = deadline;
        if (all) {
            int ok = 1;
            for (DWORD i = 0; i < n; i++) if (!is_signaled(o[i], me, now, &wake_at)) ok = 0;
            if (ok) {
                for (DWORD i = 0; i < n; i++) consume(o[i], me);
                pthread_mutex_unlock(&g_hmu);
                return WAIT_OBJECT_0;
            }
            for (DWORD i = 0; i < n; i++) unclaim(o[i]);
        } else {
            for (DWORD i = 0; i < n; i++)
                if (is_signaled(o[i], me, now, &wake_at)) {
                    consume(o[i], me);
                    pthread_mutex_unlock(&g_hmu);
                    return WAIT_OBJECT_0 + i;
                }
        }
        if (now >= deadline) { pthread_mutex_unlock(&g_hmu); return WAIT_TIMEOUT; }
        w.futex = 0;
        for (DWORD i = 0; i < n; i++) {
            nodes[i].w = &w;
            nodes[i].prev = NULL;
            nodes[i].next = o[i]->waiters;
            if (o[i]->waiters) o[i]->waiters->prev = &nodes[i];
            o[i]->waiters = &nodes[i];
        }
        pthread_mutex_unlock(&g_hmu);
        struct timespec ts = ns_ts(wake_at - now);
        futex_wait(&w.futex, 0, wake_at == INT64_MAX ? NULL : &ts);
        pthread_mutex_lock(&g_hmu);
        for (DWORD i = 0; i < n; i++) {
            if (nodes[i].prev) nodes[i].prev->next = nodes[i].next; else o[i]->waiters = nodes[i].next;
            if (nodes[i].next) nodes[i].next->prev = nodes[i].prev;
        }
    }
}
static hobj *waitable(HANDLE x) {
    if (x == PSEUDO_THREAD) return self_obj();
    hobj *h = hget(x);
    return h && h->kind != K_CUSTOM ? h : NULL;
}
DWORD WaitForSingleObject(HANDLE x, DWORD ms) {
    if (x == PSEUDO_PROCESS) { Sleep(ms); return WAIT_TIMEOUT; }   /* never signalled while we run */
    hobj *h = waitable(x);
    if (!h) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    return wait_objects(1, &h, FALSE, ms);
}
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) {
    hobj *o[MAXIMUM_WAIT_OBJECTS];
    if (!n || n > MAXIMUM_WAIT_OBJECTS || !hs) { SetLastError(ERROR_INVALID_PARAMETER); return WAIT_FAILED; }
    for (DWORD i = 0; i < n; i++)
        if (!(o[i] = waitable(hs[i]))) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    return wait_objects(n, o, all, ms);
}

/* ------------------------------------------------------------------------------------------------ fiber-local storage
 * One value set (struct plat_fls) per fiber (plat_fiber.fls) and per thread (t_fls_own, used while no fiber is current),
 * made at the first non-NULL FlsSetValue. The set the calling thread sees is the current fiber's: t_fiber_cur, which
 * SwitchToFiber / ConvertThreadToFiber / plat_fiber_set_current maintain, so the values travel with the fiber object
 * when another thread resumes it. Only the running fiber's own thread reads or writes its values (no lock): the slots
 * are atomics only because FlsFree and the end of a set (under g_fls_mu) take values out of them. Every set is also on
 * g_fls_all (under g_fls_mu) for FlsFree. A thread that may own a set (or a converted fiber) at its end arms a pthread
 * key whose destructor frees them (plat's own CreateThread threads do it before the handle is signalled). Callbacks
 * run with no lock held. */
static _Thread_local plat_fiber *t_fiber_cur;          /* GetCurrentFiber */
PLAT_TLS_ACCESSORS(plat_fiber *, t_fiber_cur)
struct plat_fls {
    struct plat_fls *prev, *next;                      /* g_fls_all */
    void *v[FLS_MAXIMUM_AVAILABLE];
};
static pthread_mutex_t g_fls_mu = PTHREAD_MUTEX_INITIALIZER;
static struct plat_fls g_fls_all = {&g_fls_all, &g_fls_all, {0}};   /* list head (under g_fls_mu) */
static size_t g_fls_n;                                 /* sets on the list (atomic reads: plat_fls_sets) */
static uint8_t g_fls_state[FLS_MAXIMUM_AVAILABLE];     /* 0 free, 1 allocated, 2 FlsFree running (written under g_fls_mu) */
static PFLS_CALLBACK_FUNCTION g_fls_cb[FLS_MAXIMUM_AVAILABLE];   /* under g_fls_mu */
static _Thread_local struct plat_fls *t_fls_own;      /* the thread's own values */
static _Thread_local int t_fls_armed;                  /* the key below is set for this thread */
static pthread_key_t g_fls_key;
static pthread_once_t g_fls_once = PTHREAD_ONCE_INIT;

static void fls_key_dtor(void *p) { (void)p; t_fls_armed = 0; fls_thread_end(); }
static void fls_key_init(void) { if (pthread_key_create(&g_fls_key, fls_key_dtor)) plat_fatal("FLS: pthread_key_create failed"); }
static NOINL void fls_arm(void) {                     /* the end of this thread frees the sets it has then */
    if (t_fls_armed) return;
    pthread_once(&g_fls_once, fls_key_init);
    t_fls_armed = 1;
    pthread_setspecific(g_fls_key, (void *)1);
}
/* Where the calling thread's current set lives: the current fiber's field, else the thread's own. No switch can come
 * between this and the use of the result (the callers do not switch). */
static NOINL struct plat_fls **fls_cur(void) {
    plat_fiber *f = t_fiber_cur;
    return f ? &f->fls : &t_fls_own;
}
/* Takes s off the list, calls the callbacks of its non-NULL values (after unlocking), frees it. */
static void fls_set_free(struct plat_fls *s) {
    if (!s) return;
    PFLS_CALLBACK_FUNCTION cb[FLS_MAXIMUM_AVAILABLE];
    void *val[FLS_MAXIMUM_AVAILABLE];
    int n = 0;
    pthread_mutex_lock(&g_fls_mu);
    s->prev->next = s->next;
    s->next->prev = s->prev;
    __atomic_store_n(&g_fls_n, g_fls_n - 1, __ATOMIC_RELAXED);
    for (int i = 0; i < FLS_MAXIMUM_AVAILABLE; i++) {
        void *v = __atomic_exchange_n(&s->v[i], NULL, __ATOMIC_RELAXED);
        if (v && g_fls_state[i] && g_fls_cb[i]) { cb[n] = g_fls_cb[i]; val[n] = v; n++; }
    }
    pthread_mutex_unlock(&g_fls_mu);
    free(s);
    for (int k = 0; k < n; k++) cb[k](val[k]);
}
/* The end of a thread: a fiber made from this thread by ConvertThreadToFiber that is still current goes (its object
 * and values; os_threads.c's per-thread fiber is one), then the current fiber's values and the thread's own. A callback
 * that sets a value again makes a new set: it is freed in the next round (4 rounds, then it is left, as pthread keys). */
static NOINL void fls_thread_end(void) {
    plat_fiber *conv = t_fiber_cur;
    if (conv && conv->is_thread) { t_fiber_cur = NULL; plat_fiber_delete(conv); }
    for (int round = 0; round < 4; round++) {
        plat_fiber *f = t_fiber_cur;
        struct plat_fls *a = f ? f->fls : NULL, *b = t_fls_own;
        if (f) f->fls = NULL;
        t_fls_own = NULL;
        if (!a && !b) return;
        fls_set_free(a);
        fls_set_free(b);
    }
}
/* Index 0 is never handed out: a zeroed DWORD is not a valid index. */
DWORD FlsAlloc(PFLS_CALLBACK_FUNCTION callback) {
    pthread_mutex_lock(&g_fls_mu);
    for (DWORD i = 1; i < FLS_MAXIMUM_AVAILABLE; i++) {
        if (g_fls_state[i]) continue;
        g_fls_cb[i] = callback;
        __atomic_store_n(&g_fls_state[i], 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&g_fls_mu);
        return i;
    }
    pthread_mutex_unlock(&g_fls_mu);
    SetLastError(ERROR_NO_MORE_ITEMS);
    return FLS_OUT_OF_INDEXES;
}
/* Every set's value in the slot is taken out and given to the callback (lock released around each call, then the scan
 * starts over: sets may have gone meanwhile). The index is handed out again only after that. */
BOOL FlsFree(DWORD i) {
    pthread_mutex_lock(&g_fls_mu);
    if (i >= FLS_MAXIMUM_AVAILABLE || g_fls_state[i] != 1) { pthread_mutex_unlock(&g_fls_mu); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    PFLS_CALLBACK_FUNCTION cb = g_fls_cb[i];
    __atomic_store_n(&g_fls_state[i], 2, __ATOMIC_RELAXED);   /* Get / Set fail from here on */
    for (;;) {
        void *v = NULL;
        for (struct plat_fls *s = g_fls_all.next; s != &g_fls_all && !v; s = s->next) v = __atomic_exchange_n(&s->v[i], NULL, __ATOMIC_RELAXED);
        if (!v) break;
        if (!cb) continue;
        pthread_mutex_unlock(&g_fls_mu);
        cb(v);
        pthread_mutex_lock(&g_fls_mu);
    }
    g_fls_cb[i] = NULL;
    __atomic_store_n(&g_fls_state[i], 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_fls_mu);
    return TRUE;
}
NOINL PVOID FlsGetValue(DWORD i) {
    if (i >= FLS_MAXIMUM_AVAILABLE || __atomic_load_n(&g_fls_state[i], __ATOMIC_RELAXED) != 1) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    struct plat_fls *s = *fls_cur();
    return s ? __atomic_load_n(&s->v[i], __ATOMIC_RELAXED) : NULL;
}
NOINL BOOL FlsSetValue(DWORD i, PVOID value) {
    if (i >= FLS_MAXIMUM_AVAILABLE || __atomic_load_n(&g_fls_state[i], __ATOMIC_RELAXED) != 1) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    struct plat_fls **ref = fls_cur(), *s = *ref;
    if (!s) {
        if (!value) return TRUE;
        s = calloc(1, sizeof *s);
        if (!s) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        pthread_mutex_lock(&g_fls_mu);
        s->prev = &g_fls_all;
        s->next = g_fls_all.next;
        g_fls_all.next->prev = s;
        g_fls_all.next = s;
        __atomic_store_n(&g_fls_n, g_fls_n + 1, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&g_fls_mu);
        *ref = s;
        fls_arm();
    }
    __atomic_store_n(&s->v[i], value, __ATOMIC_RELAXED);
    return TRUE;
}
size_t plat_fls_sets(void) { return __atomic_load_n(&g_fls_n, __ATOMIC_RELAXED); }
/* A fiber that brings values to a thread that has not armed its key yet arms it: that thread may end while running it. */
NOINL void plat_fiber_set_current(plat_fiber *f) {
    t_fiber_cur = f;
    if (f && f->fls && !t_fls_armed) fls_arm();
}

/* ------------------------------------------------------------------------------------------------ fibers */
#if defined(__aarch64__)
static uint64_t read_fpcr(void) { uint64_t v; __asm__ __volatile__("mrs %0, fpcr" : "=r"(v)); return v; }
#define FIBER_GUARD 0x10000u
plat_fiber *plat_fiber_create(size_t stack_size, plat_fiber_fn fn, void *arg) {
    size_t size = stack_size < 0x10000 ? 0x10000 : (stack_size + 4095) & ~(size_t)4095;
    plat_fiber *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    uint8_t *m = mmap(NULL, size + FIBER_GUARD, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (m == MAP_FAILED) { free(f); return NULL; }
    if (mprotect(m + FIBER_GUARD, size, PROT_READ | PROT_WRITE)) { munmap(m, size + FIBER_GUARD); free(f); return NULL; }
    f->stack_map = m;
    f->stack_map_size = size + FIBER_GUARD;
    f->fn = fn;
    f->arg = arg;
    f->regs[PLAT_FIBER_X19 + 0] = (uint64_t)(uintptr_t)fn;
    f->regs[PLAT_FIBER_X19 + 1] = (uint64_t)(uintptr_t)arg;
    f->regs[PLAT_FIBER_X19 + 2] = (uint64_t)(uintptr_t)f;
    f->regs[PLAT_FIBER_FP] = 0;
    f->regs[PLAT_FIBER_LR] = (uint64_t)(uintptr_t)&plat_fiber_trampoline;
    f->regs[PLAT_FIBER_SP] = ((uint64_t)(uintptr_t)(m + FIBER_GUARD + size)) & ~(uint64_t)15;
    f->regs[PLAT_FIBER_FPCR] = read_fpcr();
#ifdef PLAT_TSAN
    f->tsan = __tsan_create_fiber(0);
#endif
    return f;
}
plat_fiber *plat_fiber_convert_thread(void *arg) {
    plat_fiber *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->arg = arg;
    f->is_thread = 1;
    f->regs[PLAT_FIBER_FPCR] = read_fpcr();
#ifdef PLAT_TSAN
    f->tsan = __tsan_get_current_fiber();
#endif
    return f;
}
NOINL void plat_fiber_switch(plat_fiber *from, plat_fiber *to) {
#ifdef PLAT_TSAN
    __tsan_switch_to_fiber(to->tsan, 0);
#endif
    plat_fiber_switch_raw(from, to);
}
void plat_fiber_delete(plat_fiber *f) {
    if (!f) return;
    struct plat_fls *v = f->fls;
    f->fls = NULL;
    fls_set_free(v);                             /* DeleteFiber runs the fiber's FLS callbacks */
    if (f->stack_map) munmap(f->stack_map, f->stack_map_size);
#ifdef PLAT_TSAN
    if (!f->is_thread && f->tsan) __tsan_destroy_fiber(f->tsan);
#endif
    free(f);
}
__attribute__((noreturn, used)) void plat_fiber_entry_returned(plat_fiber *f) {
    plat_fatal("fiber %p: its entry function returned (a fiber must switch away; on Windows this would end the thread)", (void *)f);
}
LPVOID ConvertThreadToFiber(LPVOID arg) {
    if (t_fiber_cur_get()) { SetLastError(ERROR_ALREADY_FIBER); return NULL; }
    plat_fiber *f = plat_fiber_convert_thread(arg);
    if (!f) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    f->fls = t_fls_own;                          /* the thread's FLS values become the fiber's (Win32) */
    t_fls_own = NULL;
    plat_fiber_set_current(f);
    fls_arm();                                   /* the end of the thread frees f if it is still current then */
    return f;
}
BOOL ConvertFiberToThread(void) {
    plat_fiber *f = t_fiber_cur_get();
    if (!f) { SetLastError(ERROR_ALREADY_THREAD); return FALSE; }
    plat_fiber_set_current(NULL);
    if (!t_fls_own) { t_fls_own = f->fls; f->fls = NULL; }   /* the fiber's FLS values stay with the thread */
    if (f->is_thread) plat_fiber_delete(f);      /* a created fiber's object stays (we are on its stack) */
    return TRUE;
}
LPVOID CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, LPVOID arg) {
    (void)flags;
    size_t size = reserve ? reserve : commit ? commit : (size_t)1 << 20;
    plat_fiber *f = fn ? plat_fiber_create(size, (plat_fiber_fn)fn, arg) : NULL;
    if (!f) SetLastError(fn ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER);
    return f;
}
LPVOID CreateFiber(SIZE_T stack, LPFIBER_START_ROUTINE fn, LPVOID arg) { return CreateFiberEx(0, stack, 0, fn, arg); }
NOINL void SwitchToFiber(LPVOID to) {
    plat_fiber *cur = t_fiber_cur_get();
    if (!cur) plat_fatal("SwitchToFiber(%p) from a thread that is not a fiber (ConvertThreadToFiber first)", to);
    if (cur == (plat_fiber *)to) return;
    plat_fiber_set_current(to);                  /* the thread that later resumes `cur` sets it back for itself */
    plat_fiber_switch(cur, to);
    /* possibly another thread now: no TLS access here */
}
void DeleteFiber(LPVOID x) {
    if (!x) return;
    if (x == t_fiber_cur_get()) plat_fatal("DeleteFiber(%p): the running fiber (Win32 would exit the thread; not supported)", x);
    plat_fiber_delete(x);
}
PVOID GetCurrentFiber(void) { return t_fiber_cur_get(); }
PVOID GetFiberData(void) { plat_fiber *f = t_fiber_cur_get(); return f ? f->arg : NULL; }
BOOL IsThreadAFiber(void) { return t_fiber_cur_get() != NULL; }
#else  /* fibers need the aarch64 switch */
#define NOFIB(name) plat_fatal("%s: fibers are implemented for aarch64 only", name)
plat_fiber *plat_fiber_create(size_t s, plat_fiber_fn fn, void *arg) { (void)s; (void)fn; (void)arg; NOFIB("plat_fiber_create"); }
plat_fiber *plat_fiber_convert_thread(void *arg) { (void)arg; NOFIB("plat_fiber_convert_thread"); }
void plat_fiber_switch(plat_fiber *a, plat_fiber *b) { (void)a; (void)b; NOFIB("plat_fiber_switch"); }
void plat_fiber_delete(plat_fiber *f) { (void)f; NOFIB("plat_fiber_delete"); }
LPVOID ConvertThreadToFiber(LPVOID arg) { (void)arg; NOFIB("ConvertThreadToFiber"); }
BOOL ConvertFiberToThread(void) { NOFIB("ConvertFiberToThread"); }
LPVOID CreateFiberEx(SIZE_T c, SIZE_T r, DWORD fl, LPFIBER_START_ROUTINE fn, LPVOID arg) { (void)c; (void)r; (void)fl; (void)fn; (void)arg; NOFIB("CreateFiberEx"); }
LPVOID CreateFiber(SIZE_T s, LPFIBER_START_ROUTINE fn, LPVOID arg) { (void)s; (void)fn; (void)arg; NOFIB("CreateFiber"); }
void SwitchToFiber(LPVOID f) { (void)f; NOFIB("SwitchToFiber"); }
void DeleteFiber(LPVOID f) { (void)f; NOFIB("DeleteFiber"); }
PVOID GetCurrentFiber(void) { return NULL; }
PVOID GetFiberData(void) { return NULL; }
BOOL IsThreadAFiber(void) { return FALSE; }
#endif

/* ------------------------------------------------------------------------------------------------ CRT / misc */
void *_aligned_malloc(size_t size, size_t align) {
    void *p = NULL;
    if (align < sizeof(void *)) align = sizeof(void *);
    if (align & (align - 1)) { errno = EINVAL; return NULL; }
    if (posix_memalign(&p, align, size ? size : 1)) return NULL;
    return p;
}
void *_aligned_realloc(void *p, size_t size, size_t align) {
    if (!p) return _aligned_malloc(size, align);
    if (!size) { free(p); return NULL; }
    if (align <= 16 && !(align & (align - 1))) return realloc(p, size);   /* glibc malloc is 16-aligned on LP64 */
    void *n = _aligned_malloc(size, align);
    if (!n) return NULL;
    size_t old = malloc_usable_size(p);
    memcpy(n, p, old < size ? old : size);
    free(p);
    return n;
}
size_t _aligned_msize(void *p, size_t align, size_t offset) { (void)align; (void)offset; return p ? malloc_usable_size(p) : 0; }
void _aligned_free(void *p) { free(p); }
int _putenv(const char *a) {
    const char *eq = a ? strchr(a, '=') : NULL;
    if (!eq || eq == a) { errno = EINVAL; return -1; }
    char name[512];
    size_t n = (size_t)(eq - a);
    if (n >= sizeof name) { errno = EINVAL; return -1; }
    memcpy(name, a, n);
    name[n] = 0;
    return eq[1] ? setenv(name, eq + 1, 1) : unsetenv(name);
}
char *_fullpath(char *abs, const char *rel, size_t n) {
    char tmp[PATH_MAX * 2];
    if (!rel || !*rel) rel = ".";
    if (rel[0] == '/') snprintf(tmp, sizeof tmp, "%s", rel);
    else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) return NULL;
        snprintf(tmp, sizeof tmp, "%s/%s", cwd, rel);
    }
    char out[PATH_MAX * 2];
    size_t len = 0;
    for (char *s = tmp, *e; *s; s = e) {         /* lexical: ".", "..", repeated '/' */
        while (*s == '/') s++;
        if (!*s) break;
        for (e = s; *e && *e != '/'; e++) {}
        size_t k = (size_t)(e - s);
        if (k == 1 && s[0] == '.') continue;
        if (k == 2 && s[0] == '.' && s[1] == '.') { while (len > 0 && out[--len] != '/') {} continue; }
        if (len + 1 + k >= sizeof out) { errno = ERANGE; return NULL; }
        out[len++] = '/';
        memcpy(out + len, s, k);
        len += k;
    }
    if (!len) out[len++] = '/';
    out[len] = 0;
    if (!abs) { n = PATH_MAX; if (!(abs = malloc(n))) return NULL; }
    if (len + 1 > n) { errno = ERANGE; return NULL; }
    memcpy(abs, out, len + 1);
    return abs;
}
static DWORD copy_out(const char *s, LPSTR buf, DWORD n) {
    size_t len = strlen(s);
    if (!n) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    if (len >= n) { memcpy(buf, s, n - 1); buf[n - 1] = 0; SetLastError(ERROR_INSUFFICIENT_BUFFER); return n; }
    memcpy(buf, s, len + 1);
    return (DWORD)len;
}
DWORD GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n) {
    char path[PATH_MAX];
    const char *name = NULL;
    if (m) {
        struct link_map *lm = NULL;
        if (dlinfo(m, RTLD_DI_LINKMAP, &lm) == 0 && lm && lm->l_name && lm->l_name[0]) name = lm->l_name;
    }
    if (!name) {
        ssize_t k = readlink("/proc/self/exe", path, sizeof path - 1);
        if (k <= 0) { SetLastError(ERROR_FILE_NOT_FOUND); return 0; }
        path[k] = 0;
        name = path;
    }
    return copy_out(name, buf, n);
}
DWORD GetTempPathA(DWORD n, LPSTR buf) {
    const char *t = getenv("TMPDIR");
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s", t && *t ? t : "/tmp");
    size_t len = strlen(p);
    if (len && p[len - 1] != '/' && len + 1 < sizeof p) { p[len++] = '/'; p[len] = 0; }
    if (len + 1 > n) return (DWORD)(len + 1);
    memcpy(buf, p, len + 1);
    return (DWORD)len;
}
static _Thread_local char t_dlerr[256];
NOINL const char *plat_dlerror(void) { return t_dlerr; }
static NOINL void keep_dlerror(void) {
    const char *e = dlerror();
    snprintf(t_dlerr, sizeof t_dlerr, "%s", e ? e : "");
}
HMODULE LoadLibraryA(LPCSTR path) {
    void *h = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!h) { keep_dlerror(); SetLastError(ERROR_MOD_NOT_FOUND); }
    return h;
}
HMODULE GetModuleHandleA(LPCSTR name) {
    void *h = dlopen(name, RTLD_NOW | (name ? RTLD_NOLOAD : 0));
    if (!h) { keep_dlerror(); SetLastError(ERROR_MOD_NOT_FOUND); return NULL; }
    if (name) dlclose(h);                        /* GetModuleHandle does not add a reference */
    return h;
}
FARPROC GetProcAddress(HMODULE m, LPCSTR name) {
    void *s = dlsym(m ? m : RTLD_DEFAULT, name);
    FARPROC f;
    if (!s) { keep_dlerror(); SetLastError(ERROR_PROC_NOT_FOUND); }
    memcpy(&f, &s, sizeof f);
    return f;
}
BOOL FreeLibrary(HMODULE m) { return m && dlclose(m) == 0; }
void ExitProcess(UINT code) {
    fflush(NULL);
    _exit((int)code);
}
int __argc;
char **__argv;
__attribute__((constructor)) static void plat_args(int argc, char **argv) { __argc = argc; __argv = argv; }
BOOLEAN NTAPI SystemFunction036(PVOID buffer, ULONG length) {
    for (ULONG done = 0; done < length;) {
        ssize_t n = getrandom((char *)buffer + done, length - done, 0);
        if (n < 0) { if (errno == EINTR) continue; return FALSE; }
        done += (ULONG)n;
    }
    return TRUE;
}
