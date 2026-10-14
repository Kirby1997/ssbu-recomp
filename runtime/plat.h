/* plat.h - the Win32 subset the runtime uses, for non-Windows hosts (native aarch64 Linux / Steam Frame).
 *
 * On _WIN32 this is exactly <windows.h>: the Windows build does not change. Elsewhere it provides the same names
 * with Win32 semantics over pthreads, futexes and mmap. Only what runtime/ calls is here (grep, 2026-10-14); file
 * system, GDI, COM/Media Foundation, crash handling and module path policy are not (other phases).
 *
 * Build: compile every runtime file with -D_GNU_SOURCE (or include plat.h first). plat_posix.c and vmem.c are the
 * implementation; fiber_aarch64.S is the context switch.
 *
 * Semantics that differ from Win32 are marked "DIFF:" below; the list is repeated in tests/plat/README.
 */
#ifndef SSBU_PLAT_H
#define SSBU_PLAT_H

#ifdef _WIN32
#include <windows.h>
#else /* ------------------------------------------------------------------------------------------- POSIX host */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1          /* no effect if a system header came first: build with -D_GNU_SOURCE */
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------------------------- keywords */
#define WINAPI
#define WINAPIV
#define APIENTRY
#define CALLBACK
#define STDMETHODCALLTYPE
#define NTAPI
#ifndef __stdcall
#define __stdcall
#endif
#ifndef __cdecl
#define __cdecl
#endif
#ifndef FORCEINLINE
#define FORCEINLINE static inline __attribute__((always_inline))
#endif
#ifndef DECLSPEC_NOINLINE
#define DECLSPEC_NOINLINE __attribute__((noinline))
#endif
/* __declspec(x) -> PLAT_DECLSPEC_x. thread: _Thread_local (initial-exec/local-exec in the executable).
 * dllexport: default visibility. dllimport: nothing (ELF resolves at load). */
#define __declspec(x) PLAT_DECLSPEC_##x
#ifdef __cplusplus
#define PLAT_DECLSPEC_thread thread_local
#else
#define PLAT_DECLSPEC_thread _Thread_local
#endif
#define PLAT_DECLSPEC_dllexport __attribute__((visibility("default")))
#define PLAT_DECLSPEC_dllimport
#define PLAT_DECLSPEC_noinline __attribute__((noinline))
#define PLAT_DECLSPEC_noreturn __attribute__((noreturn))
#define PLAT_DECLSPEC_selectany __attribute__((weak))
#define PLAT_DECLSPEC_align(n) __attribute__((aligned(n)))

/* ---------------------------------------------------------------------------------------------- types */
typedef int BOOL;
typedef unsigned char BYTE, UCHAR, BOOLEAN;
typedef char CHAR;
typedef short SHORT;
typedef unsigned short WORD, USHORT;
typedef uint16_t WCHAR;                 /* UTF-16 code unit as on Windows (wchar_t is 32-bit here; L"" literals differ) */
typedef int INT;
typedef unsigned int UINT;
typedef int32_t LONG, INT32;            /* Win64 LONG is 32-bit: so here. Not `long` (64-bit on LP64). */
typedef uint32_t ULONG, DWORD, UINT32;  /* DIFF: DWORD is `unsigned int`: printf("%lu", dword) needs a cast. */
typedef int64_t LONG64, LONGLONG, INT64;
typedef uint64_t ULONG64, ULONGLONG, DWORD64, UINT64;
typedef intptr_t INT_PTR, LONG_PTR, SSIZE_T;
typedef uintptr_t UINT_PTR, ULONG_PTR, DWORD_PTR, SIZE_T;
typedef float FLOAT;
#define VOID void
typedef void *PVOID, *LPVOID, *HANDLE, *HMODULE, *HINSTANCE;
typedef const void *LPCVOID;
typedef char *LPSTR, *PSTR;
typedef const char *LPCSTR, *PCSTR;
typedef WCHAR *LPWSTR, *PWSTR;
typedef const WCHAR *LPCWSTR, *PCWSTR;
typedef BOOL *PBOOL, *LPBOOL;
typedef BYTE *PBYTE, *LPBYTE;
typedef WORD *PWORD, *LPWORD;
typedef DWORD *PDWORD, *LPDWORD;
typedef LONG *PLONG, *LPLONG;
typedef ULONG *PULONG;
typedef DWORD_PTR *PDWORD_PTR;
typedef SIZE_T *PSIZE_T;
typedef HANDLE *PHANDLE, *LPHANDLE;
typedef LONG HRESULT;
typedef INT_PTR (*FARPROC)(void);
typedef struct _SECURITY_ATTRIBUTES { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES, *PSECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;
typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    struct { DWORD LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;
typedef union _ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; };
    struct { DWORD LowPart; DWORD HighPart; } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;
typedef struct _FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME, *PFILETIME, *LPFILETIME;
typedef struct _SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;
typedef struct _SYSTEM_INFO {
    WORD wProcessorArchitecture, wReserved;
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress, lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
    WORD wProcessorLevel, wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;
typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress, AllocationBase;
    DWORD AllocationProtect;
    WORD PartitionId;
    SIZE_T RegionSize;
    DWORD State, Protect, Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

/* ---------------------------------------------------------------------------------------------- constants */
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#define INFINITE 0xFFFFFFFFu
#define MAX_PATH 4096                   /* DIFF: PATH_MAX, not 260 (Linux paths are longer; watch snprintf truncation) */
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define STILL_ACTIVE 259
#define MAXIMUM_WAIT_OBJECTS 64
#define WAIT_OBJECT_0 0u
#define WAIT_ABANDONED 0x80u
#define WAIT_ABANDONED_0 0x80u
#define WAIT_TIMEOUT 0x102u
#define WAIT_FAILED 0xFFFFFFFFu
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_OUTOFMEMORY 14
#define ERROR_NO_MORE_FILES 18
#define ERROR_NOT_SUPPORTED 50
#define ERROR_INVALID_PARAMETER 87
#define ERROR_CALL_NOT_IMPLEMENTED 120
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_MOD_NOT_FOUND 126
#define ERROR_PROC_NOT_FOUND 127
#define ERROR_ALREADY_EXISTS 183
#define ERROR_NOT_OWNER 288
#define ERROR_TOO_MANY_POSTS 298
#define ERROR_INVALID_ADDRESS 487
#define ERROR_ALREADY_FIBER 1280
#define ERROR_ALREADY_THREAD 1281
#define ERROR_TIMEOUT 1460
/* threads */
#define CREATE_SUSPENDED 0x00000004u
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000u
#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_PRIORITY_ERROR_RETURN 0x7fffffff
#define DUPLICATE_CLOSE_SOURCE 0x1u
#define DUPLICATE_SAME_ACCESS 0x2u
#define SYNCHRONIZE 0x00100000u
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000u
#define TIMER_ALL_ACCESS 0x1F0003u
#define EVENT_ALL_ACCESS 0x1F0003u
#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x1u
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2u
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1u
#define INIT_ONCE_CHECK_ONLY 0x1u
#define INIT_ONCE_ASYNC 0x2u
/* virtual memory (implemented in vmem.c) */
#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_WRITECOPY 0x08u
#define PAGE_EXECUTE 0x10u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u
#define PAGE_EXECUTE_WRITECOPY 0x80u
#define PAGE_GUARD 0x100u
#define PAGE_NOCACHE 0x200u
#define PAGE_WRITECOMBINE 0x400u
#define MEM_COMMIT 0x00001000u
#define MEM_RESERVE 0x00002000u
#define MEM_DECOMMIT 0x00004000u
#define MEM_RELEASE 0x00008000u
#define MEM_FREE 0x00010000u
#define MEM_PRIVATE 0x00020000u
#define MEM_MAPPED 0x00040000u
#define MEM_RESET 0x00080000u
#define MEM_TOP_DOWN 0x00100000u
#define MEM_WRITE_WATCH 0x00200000u      /* DIFF: not supported (VirtualAlloc fails, message on stderr) */
#define MEM_LARGE_PAGES 0x20000000u
#define MEM_IMAGE 0x01000000u
#define PROCESSOR_ARCHITECTURE_ARM64 12

/* ---------------------------------------------------------------------------------------------- errors */
DWORD GetLastError(void);
void SetLastError(DWORD e);
/* Fatal error helper: message to stderr, then abort(). */
__attribute__((noreturn, format(printf, 1, 2))) void plat_fatal(const char *fmt, ...);

/* ---------------------------------------------------------------------------------------------- Interlocked
 * __atomic builtins, SEQ_CST (a full barrier on both sides, as Win32). Written as type-generic macros with a size
 * check so `LONG`, `long long`, `int64_t` and `uint64_t` objects all work and a 64-bit op on a 32-bit object (or the
 * reverse) is a compile error, not a silent width change. Results have the type of the target object. */
#define PLAT_ATOMIC_PTR(p, n) (__extension__({ _Static_assert(sizeof(*(p)) == (n), "Interlocked: operand width"); (p); }))
#define PLAT_SC __ATOMIC_SEQ_CST
#define PLAT_CAS_(d, x, c, n) (__extension__({ __typeof__(*(d) + 0) plat_cmp_ = (c); \
        __atomic_compare_exchange_n(PLAT_ATOMIC_PTR(d, n), &plat_cmp_, (x), 0, PLAT_SC, PLAT_SC); plat_cmp_; }))
/* 32-bit */
#define InterlockedIncrement(p) __atomic_add_fetch(PLAT_ATOMIC_PTR(p, 4), 1, PLAT_SC)
#define InterlockedDecrement(p) __atomic_sub_fetch(PLAT_ATOMIC_PTR(p, 4), 1, PLAT_SC)
#define InterlockedExchange(p, v) __atomic_exchange_n(PLAT_ATOMIC_PTR(p, 4), (v), PLAT_SC)
#define InterlockedExchangeAdd(p, v) __atomic_fetch_add(PLAT_ATOMIC_PTR(p, 4), (v), PLAT_SC)
#define InterlockedAdd(p, v) __atomic_add_fetch(PLAT_ATOMIC_PTR(p, 4), (v), PLAT_SC)
#define InterlockedOr(p, v) __atomic_fetch_or(PLAT_ATOMIC_PTR(p, 4), (v), PLAT_SC)
#define InterlockedAnd(p, v) __atomic_fetch_and(PLAT_ATOMIC_PTR(p, 4), (v), PLAT_SC)
#define InterlockedXor(p, v) __atomic_fetch_xor(PLAT_ATOMIC_PTR(p, 4), (v), PLAT_SC)
#define InterlockedCompareExchange(d, x, c) PLAT_CAS_(d, x, c, 4)
/* 64-bit */
#define InterlockedIncrement64(p) __atomic_add_fetch(PLAT_ATOMIC_PTR(p, 8), 1, PLAT_SC)
#define InterlockedDecrement64(p) __atomic_sub_fetch(PLAT_ATOMIC_PTR(p, 8), 1, PLAT_SC)
#define InterlockedExchange64(p, v) __atomic_exchange_n(PLAT_ATOMIC_PTR(p, 8), (v), PLAT_SC)
#define InterlockedExchangeAdd64(p, v) __atomic_fetch_add(PLAT_ATOMIC_PTR(p, 8), (v), PLAT_SC)
#define InterlockedAdd64(p, v) __atomic_add_fetch(PLAT_ATOMIC_PTR(p, 8), (v), PLAT_SC)
#define InterlockedOr64(p, v) __atomic_fetch_or(PLAT_ATOMIC_PTR(p, 8), (v), PLAT_SC)
#define InterlockedAnd64(p, v) __atomic_fetch_and(PLAT_ATOMIC_PTR(p, 8), (v), PLAT_SC)
#define InterlockedCompareExchange64(d, x, c) PLAT_CAS_(d, x, c, 8)
/* 8/16-bit */
#define InterlockedOr8(p, v) __atomic_fetch_or(PLAT_ATOMIC_PTR(p, 1), (v), PLAT_SC)
#define InterlockedAnd8(p, v) __atomic_fetch_and(PLAT_ATOMIC_PTR(p, 1), (v), PLAT_SC)
#define InterlockedExchange8(p, v) __atomic_exchange_n(PLAT_ATOMIC_PTR(p, 1), (v), PLAT_SC)
#define InterlockedIncrement16(p) __atomic_add_fetch(PLAT_ATOMIC_PTR(p, 2), 1, PLAT_SC)
#define InterlockedDecrement16(p) __atomic_sub_fetch(PLAT_ATOMIC_PTR(p, 2), 1, PLAT_SC)
#define InterlockedCompareExchange16(d, x, c) (__extension__({ __typeof__(*(d)) plat_cmp16_ = (c); \
        __atomic_compare_exchange_n(PLAT_ATOMIC_PTR(d, 2), &plat_cmp16_, (x), 0, PLAT_SC, PLAT_SC); plat_cmp16_; }))
/* pointers: the Win32 prototypes take PVOID volatile *; any pointer-to-pointer is accepted here */
static inline PVOID plat_xchg_ptr_(PVOID volatile *p, PVOID v) { return __atomic_exchange_n(p, v, PLAT_SC); }
static inline PVOID plat_cas_ptr_(PVOID volatile *d, PVOID x, PVOID c) { __atomic_compare_exchange_n(d, &c, x, 0, PLAT_SC, PLAT_SC); return c; }
#define InterlockedExchangePointer(p, v) plat_xchg_ptr_((PVOID volatile *)PLAT_ATOMIC_PTR(p, sizeof(void *)), (PVOID)(v))
#define InterlockedCompareExchangePointer(d, x, c) plat_cas_ptr_((PVOID volatile *)PLAT_ATOMIC_PTR(d, sizeof(void *)), (PVOID)(x), (PVOID)(c))
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define _ReadWriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#if defined(__aarch64__) || defined(__arm__)
#define YieldProcessor() __asm__ __volatile__("yield" ::: "memory")
#elif defined(__x86_64__) || defined(__i386__)
#define YieldProcessor() __asm__ __volatile__("pause" ::: "memory")
#else
#define YieldProcessor() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#endif

/* ---------------------------------------------------------------------------------------------- time */
/* QueryPerformanceCounter: CLOCK_MONOTONIC in ns, frequency 1e9 (DIFF: Windows is usually 1e7; integer code that
 * multiplies a raw counter by 1e6+ before dividing overflows after ~2.5 h of uptime instead of ~10 days). */
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *c) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    c->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000LL; return TRUE; }
static inline ULONGLONG GetTickCount64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000u + (ULONGLONG)ts.tv_nsec / 1000000u;
}
static inline DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }   /* DIFF: CLOCK_MONOTONIC stops in suspend */
void Sleep(DWORD ms);                                 /* 0: sched_yield; INFINITE: forever; restarts after signals */
static inline BOOL SwitchToThread(void) { return sched_yield() == 0; }   /* DIFF: always TRUE-ish (no "nothing ran") */
void GetSystemTimeAsFileTime(LPFILETIME ft);          /* CLOCK_REALTIME, 100 ns since 1601 */
void GetSystemTime(LPSYSTEMTIME st);                  /* UTC */
void GetLocalTime(LPSYSTEMTIME st);                   /* TZ / localtime_r */

/* ---------------------------------------------------------------------------------------------- critical section
 * A non-recursive pthread mutex with the recursion, owner and lock count kept here (Win32 field names): os_threads.c
 * and nvn_vk_wd.c read OwningThread. Leave from another thread than Enter works (as on Windows: needed when a guest
 * fiber holding a guest mutex migrates), pthread recursive mutexes would return EPERM. */
typedef struct _RTL_CRITICAL_SECTION {
    pthread_mutex_t mu;
    HANDLE OwningThread;                /* (HANDLE)(uintptr_t)tid of the owner, 0 when free; read racily by diagnostics */
    LONG RecursionCount;
    LONG LockCount;                     /* DIFF: number of threads inside Enter (owner + waiters) - 1; -1 when free */
    ULONG_PTR SpinCount;
    void *DebugInfo;
} CRITICAL_SECTION, *LPCRITICAL_SECTION, *PCRITICAL_SECTION;
void InitializeCriticalSection(LPCRITICAL_SECTION cs);
BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin);
BOOL InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD spin, DWORD flags);
void DeleteCriticalSection(LPCRITICAL_SECTION cs);
void EnterCriticalSection(LPCRITICAL_SECTION cs);
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs);
void LeaveCriticalSection(LPCRITICAL_SECTION cs);

/* ---------------------------------------------------------------------------------------------- SRW lock
 * pthread_rwlock (writer-preferring when _GNU_SOURCE took effect, like Win32 SRW: no recursive shared acquire).
 * DIFF: an exclusive lock must be released by the thread that acquired it (glibc tracks the writer tid); Win32
 * tolerates a cross-thread release. Do not hold an SRW lock across a fiber switch. Recursive exclusive acquire is
 * fatal here (deadlock on Windows). */
typedef struct _RTL_SRWLOCK { pthread_rwlock_t rw; } SRWLOCK, *PSRWLOCK;
#ifdef PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP
#define SRWLOCK_INIT { PTHREAD_RWLOCK_WRITER_NONRECURSIVE_INITIALIZER_NP }
#else
#define SRWLOCK_INIT { PTHREAD_RWLOCK_INITIALIZER }
#endif
__attribute__((noreturn)) void plat_lock_failed_(const char *what, int err);
void InitializeSRWLock(PSRWLOCK l);
static inline void AcquireSRWLockExclusive(PSRWLOCK l) { int e = pthread_rwlock_wrlock(&l->rw); if (__builtin_expect(e != 0, 0)) plat_lock_failed_("AcquireSRWLockExclusive", e); }
static inline void AcquireSRWLockShared(PSRWLOCK l) { int e = pthread_rwlock_rdlock(&l->rw); if (__builtin_expect(e != 0, 0)) plat_lock_failed_("AcquireSRWLockShared", e); }
static inline BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK l) { return pthread_rwlock_trywrlock(&l->rw) == 0; }
static inline BOOLEAN TryAcquireSRWLockShared(PSRWLOCK l) { return pthread_rwlock_tryrdlock(&l->rw) == 0; }
static inline void ReleaseSRWLockExclusive(PSRWLOCK l) { int e = pthread_rwlock_unlock(&l->rw); if (__builtin_expect(e != 0, 0)) plat_lock_failed_("ReleaseSRWLockExclusive", e); }
static inline void ReleaseSRWLockShared(PSRWLOCK l) { int e = pthread_rwlock_unlock(&l->rw); if (__builtin_expect(e != 0, 0)) plat_lock_failed_("ReleaseSRWLockShared", e); }

/* ---------------------------------------------------------------------------------------------- condition variable
 * A FIFO of waiters (nodes on the waiters' stacks) under a small mutex, each waiter sleeping on its own futex word:
 * works with both CRITICAL_SECTION and SRWLOCK, Wake wakes exactly one thread that was waiting at the time of the
 * call, timeouts are CLOCK_MONOTONIC. Timeout: FALSE with GetLastError() == ERROR_TIMEOUT. Spurious TRUE returns are
 * possible (EINTR), as on Windows. */
struct plat_cv_node;
typedef struct _RTL_CONDITION_VARIABLE { pthread_mutex_t lk; struct plat_cv_node *head, *tail; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT { PTHREAD_MUTEX_INITIALIZER, NULL, NULL }
void InitializeConditionVariable(PCONDITION_VARIABLE cv);
BOOL SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms);
BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK l, DWORD ms, ULONG flags);
void WakeConditionVariable(PCONDITION_VARIABLE cv);
void WakeAllConditionVariable(PCONDITION_VARIABLE cv);

/* ---------------------------------------------------------------------------------------------- one-time init */
typedef union _RTL_RUN_ONCE { PVOID Ptr; } INIT_ONCE, *PINIT_ONCE, *LPINIT_ONCE;
#define INIT_ONCE_STATIC_INIT { NULL }
typedef BOOL (WINAPI *PINIT_ONCE_FN)(PINIT_ONCE once, PVOID param, PVOID *ctx);
void InitOnceInitialize(PINIT_ONCE once);
/* The callback returning FALSE leaves the object uninitialised (a later call retries) and returns FALSE, as Win32.
 * ctx (optional) receives the callback's context; its low 2 bits must be 0. Only flags 0 is supported. */
BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, LPVOID *ctx);

/* ---------------------------------------------------------------------------------------------- handles
 * HANDLE is a pointer to a refcounted object: thread, event, semaphore, mutex, waitable timer, process, or a kind
 * registered by another subsystem (plat_handle_create). CloseHandle drops a reference; a running thread keeps its
 * own. GetCurrentThread()/GetCurrentProcess() are the Win32 pseudo handles (-2 / -1). */
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID arg);
typedef DWORD (WINAPI *PTHREAD_START_ROUTINE)(LPVOID arg);
typedef void (CALLBACK *PTIMERAPCROUTINE)(LPVOID arg, DWORD lo, DWORD hi);
static inline HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
static inline HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
BOOL CloseHandle(HANDLE h);
BOOL DuplicateHandle(HANDLE src_process, HANDLE src, HANDLE dst_process, LPHANDLE dst, DWORD access, BOOL inherit, DWORD options);
/* Extension point for other subsystems (files, mappings): a handle CloseHandle understands. destroy() runs when the
 * last reference goes. Such handles are not waitable (WaitFor* fails with ERROR_INVALID_HANDLE). */
typedef struct plat_handle_ops { const char *name; void (*destroy)(void *payload); } plat_handle_ops;
HANDLE plat_handle_create(const plat_handle_ops *ops, void *payload);
void *plat_handle_payload(HANDLE h, const plat_handle_ops *ops);   /* NULL if h is not of that kind */

/* threads. stack: 0 = 8 MiB default, else that size (reserve or commit flag both mean "size", memory is lazily
 * committed by Linux). The thread is detached: its stack goes when it ends, the handle stays waitable. Any number of
 * threads may wait on one thread, with timeouts. CREATE_SUSPENDED + ResumeThread work; GetThreadId is valid as soon
 * as CreateThread returns. DIFF: SuspendThread of a running thread is not supported (returns (DWORD)-1,
 * ERROR_NOT_SUPPORTED): profilers/watchdogs must use signals. */
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD tid);
DWORD ResumeThread(HANDLE h);
DWORD SuspendThread(HANDLE h);
BOOL GetExitCodeThread(HANDLE h, LPDWORD code);
DWORD GetThreadId(HANDLE h);
DWORD GetCurrentThreadId(void);                       /* gettid() (cached per thread, reset after fork) */
static inline DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
DWORD GetCurrentProcessorNumber(void);                /* sched_getcpu */
/* Priority: recorded per thread (GetThreadPriority returns it) and applied as nice: TIME_CRITICAL -10, HIGHEST -5,
 * ABOVE_NORMAL -2, BELOW_NORMAL 2, LOWEST 5, IDLE 19. DIFF: raising needs CAP_SYS_NICE/RLIMIT_NICE; without it the
 * raise is ignored (still TRUE). Lowering is applied only when RLIMIT_NICE lets the thread come back to 0 later
 * (SteamOS: RLIMIT_NICE=0 => below-normal is recorded, not applied). */
BOOL SetThreadPriority(HANDLE h, int prio);
int GetThreadPriority(HANDLE h);
BOOL SetThreadPriorityBoost(HANDLE h, BOOL disable);  /* no-op, TRUE */
DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask);   /* sched_setaffinity on the tid; returns old mask */
BOOL GetProcessAffinityMask(HANDLE process, PDWORD_PTR process_mask, PDWORD_PTR system_mask);
/* events (name ignored: DIFF, not shared across processes) */
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual_reset, BOOL initial, LPCSTR name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
/* semaphores (name ignored) */
HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name);
BOOL ReleaseSemaphore(HANDLE h, LONG n, LPLONG prev);
/* mutexes: recursive, owned by a thread. A named mutex is one object per name in the process and is also exclusive
 * across processes (flock on $XDG_RUNTIME_DIR or /tmp/ssbu-mutex-<name>.lock) for single-object waits. */
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL initial_owner, LPCSTR name);
BOOL ReleaseMutex(HANDLE h);
/* waitable timers: due < 0 relative (100 ns units), > 0 absolute FILETIME; period in ms; APC not supported */
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access);
HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES sa, BOOL manual_reset, LPCWSTR name);
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period, PTIMERAPCROUTINE apc, LPVOID arg, BOOL resume);
BOOL CancelWaitableTimer(HANDLE h);
/* processes: OpenProcess(SYNCHRONIZE...) gives a handle that is signalled when the process is gone (polled) */
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid);
/* waits: threads, events, semaphores, mutexes, timers, processes. all=TRUE consumes every object atomically. */
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms);

/* ---------------------------------------------------------------------------------------------- fibers
 * Over fiber.h (aarch64 assembly switch). The fiber's stack is mmap'd (reserve size, NORESERVE) with a guard page.
 * Fibers may be resumed by another thread than the one that ran them before: see the TLS note in fiber.h. */
typedef void (WINAPI *LPFIBER_START_ROUTINE)(LPVOID arg);
typedef LPFIBER_START_ROUTINE PFIBER_START_ROUTINE;
LPVOID CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, LPVOID arg);
LPVOID CreateFiber(SIZE_T stack, LPFIBER_START_ROUTINE fn, LPVOID arg);
LPVOID ConvertThreadToFiber(LPVOID arg);              /* NULL + ERROR_ALREADY_FIBER if already converted */
BOOL ConvertFiberToThread(void);
void SwitchToFiber(LPVOID fiber);
void DeleteFiber(LPVOID fiber);                       /* DIFF: deleting the running fiber is fatal (Win32: ExitThread) */
PVOID GetCurrentFiber(void);
PVOID GetFiberData(void);
BOOL IsThreadAFiber(void);

/* ---------------------------------------------------------------------------------------------- virtual memory
 * Implemented in vmem.c over the guest window reserved by vmem_init() and a region table: see vmem.h. */
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect, PDWORD old);
SIZE_T VirtualQuery(LPCVOID addr, PMEMORY_BASIC_INFORMATION info, SIZE_T len);
void GetSystemInfo(LPSYSTEM_INFO si);
static inline void GetNativeSystemInfo(LPSYSTEM_INFO si) { GetSystemInfo(si); }
BOOL FlushInstructionCache(HANDLE process, LPCVOID addr, SIZE_T size);

/* ---------------------------------------------------------------------------------------------- CRT / misc */
void *_aligned_malloc(size_t size, size_t align);
void *_aligned_realloc(void *p, size_t size, size_t align);   /* keeps the alignment */
size_t _aligned_msize(void *p, size_t align, size_t offset);  /* DIFF: malloc_usable_size (>= requested size) */
void _aligned_free(void *p);
static inline int _stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
static inline int _strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
static inline char *_strdup(const char *s) { return strdup(s); }
static inline char *_strlwr(char *s) { for (char *p = s; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32); return s; }
static inline char *_strupr(char *s) { for (char *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32); return s; }
static inline int _fseeki64(FILE *f, long long off, int whence) { return fseeko(f, (off_t)off, whence); }
static inline long long _ftelli64(FILE *f) { return (long long)ftello(f); }
static inline int _fileno(FILE *f) { return fileno(f); }
static inline int _getpid(void) { return (int)getpid(); }
int _putenv(const char *assignment);                  /* "NAME=value"; "NAME=" removes NAME */
char *_fullpath(char *abs, const char *rel, size_t n); /* lexical: the path need not exist; NULL abs => malloc */
DWORD GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n);   /* NULL: /proc/self/exe; Win32 truncation rules */
DWORD GetTempPathA(DWORD n, LPSTR buf);               /* $TMPDIR or /tmp, with a trailing '/' */
/* DIFF: plain dlopen/dlsym; no ".dll" -> ".so" or '\\' -> '/' translation (nnro.c chooses the file name). */
HMODULE LoadLibraryA(LPCSTR path);
HMODULE GetModuleHandleA(LPCSTR name);                /* NULL: the executable; else RTLD_NOLOAD */
FARPROC GetProcAddress(HMODULE m, LPCSTR name);
BOOL FreeLibrary(HMODULE m);
const char *plat_dlerror(void);                     /* dlerror() text of the last failed call above, this thread */
/* MSVC CRT globals (sdklift_rt.c): filled by a constructor from the arguments glibc passes to init functions. */
extern int __argc;
extern char **__argv;
/* RtlGenRandom (os_threads.c declares it itself): getrandom(2). */
BOOLEAN NTAPI SystemFunction036(PVOID buffer, ULONG length);
/* fflush(NULL) + _exit: no atexit handlers (graphics drivers' handlers with threads alive deadlock). */
__attribute__((noreturn)) void ExitProcess(UINT code);

#ifdef __cplusplus
}
#endif

#include "vmem.h"
#include "fiber.h"

#endif /* !_WIN32 */
#endif /* SSBU_PLAT_H */
