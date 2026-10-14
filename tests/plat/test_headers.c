/* test_headers.c - plat.h together with every system header the runtime includes (and the Linux ones the port will
 * add), in both orders: compiled twice, with -DPLAT_FIRST and without. Built with -Wall -Wextra -Werror. */
#ifdef PLAT_FIRST
#include "plat.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdint.h>
#include <math.h>
#include <stdarg.h>
#include <setjmp.h>
#include <time.h>
#include <stddef.h>
#include <ctype.h>
#include <stdbool.h>
#include <inttypes.h>
#include <limits.h>
#include <errno.h>
#include <float.h>
#include <wchar.h>
#include <locale.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <dirent.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ucontext.h>
#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#endif
#if __has_include(<SDL3/SDL.h>)
#include <SDL3/SDL.h>
#endif
#ifndef PLAT_FIRST
#include "plat.h"
#endif
#if defined(WITH_RUNTIME_H) && __has_include("runtime.h")
#include "runtime.h"
#endif

/* the shapes the runtime relies on */
_Static_assert(sizeof(LONG) == 4 && sizeof(DWORD) == 4 && sizeof(LONG64) == 8 && sizeof(SIZE_T) == sizeof(void *), "types");
_Static_assert(sizeof(LARGE_INTEGER) == 8 && sizeof(FILETIME) == 8 && sizeof(SYSTEMTIME) == 16 && sizeof(WCHAR) == 2, "structs");
_Static_assert(INFINITE == 0xFFFFFFFFu && WAIT_TIMEOUT == 0x102 && MEM_COMMIT == 0x1000 && PAGE_READWRITE == 4, "constants");
static __declspec(thread) HANDLE t_h;
static SRWLOCK s_l = SRWLOCK_INIT;
static CONDITION_VARIABLE s_cv = CONDITION_VARIABLE_INIT;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static volatile LONG s_n;
static volatile LONG64 s_n64;
static DWORD WINAPI thread_fn(LPVOID arg) { return (DWORD)(uintptr_t)arg; }
static BOOL CALLBACK once_fn(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; return TRUE; }
static void WINAPI fiber_fn(LPVOID arg) { (void)arg; }
#ifdef PLAT_FIRST
#define ENTRY headers_plat_first
#else
#define ENTRY headers_plat_last
#endif
int ENTRY(void);
int ENTRY(void) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    MEMORY_BASIC_INFORMATION mi;
    CRITICAL_SECTION cs;
    InitializeCriticalSection(&cs);
    AcquireSRWLockExclusive(&s_l);
    ReleaseSRWLockExclusive(&s_l);
    WakeAllConditionVariable(&s_cv);
    InitOnceExecuteOnce(&s_once, once_fn, NULL, NULL);
    InterlockedIncrement(&s_n);
    InterlockedAdd64(&s_n64, 2);
    t_h = CreateThread(NULL, 0, thread_fn, NULL, 0, NULL);
    WaitForSingleObject(t_h, INFINITE);
    CloseHandle(t_h);
    (void)CreateFiberEx;
    (void)fiber_fn;
    DeleteCriticalSection(&cs);
    return (int)VirtualQuery(&mi, &mi, sizeof mi) + _stricmp("a", "A") + (int)(q.QuadPart & 0);
}
#ifdef PLAT_FIRST
int headers_plat_last(void);
int main(void) {
    int r = headers_plat_first() + headers_plat_last();
    printf("%-14s %s\n", "test_headers", r > 0 ? "PASS" : "FAIL");
    return r > 0 ? 0 : 1;
}
#endif
