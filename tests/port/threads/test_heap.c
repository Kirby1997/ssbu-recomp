/* test_heap.c - every malloc result below 2^47 (runtime/plat_heap.c: no per-thread arenas, no mmap'd chunks, the
 * -no-pie brk heap), and outside the guest window. Blocks of 24 B / 1 MiB / 64 MiB / 1 GiB, calloc, realloc growth,
 * _aligned_malloc and posix_memalign, from main, from 16 plat CreateThread threads (glibc stacks) and from 16
 * os_threads.c host threads (vmem stacks, also checked) at the same time. Built without plat_heap.o (-DNOFIX) it is the
 * control: it reports how many addresses are high (expected: some) and does not fail.
 * Lists the mappings above 2^47 at the end (what the guest must never be handed). */
#include "tp.h"
#include <malloc.h>
#include <sys/mman.h>

struct host_thread;
struct host_thread *host_thread_create(size_t stack, LPTHREAD_START_ROUTINE fn, void *arg, const char *name, int suspended);
HANDLE host_thread_handle(struct host_thread *t);
void host_thread_release(struct host_thread *t);
DWORD host_thread_exit_code(struct host_thread *t);

#define NT 16
static int g_high, g_window, g_total;
static int low_ok(const void *p) {
    uint64_t a = (uint64_t)(uintptr_t)p;
    TP_BUMP(g_total);
    if (a >= (1ull << 47)) { if (TP_BUMP(g_high) <= 8) printf("    high: %p\n", p); return 0; }
    if (a >= VMEM_GUEST_LO && a < VMEM_GUEST_HI) { TP_BUMP(g_window); printf("    in the guest window: %p\n", p); return 0; }
    return 1;
}
static void touch(void *p, size_t n) { if (p) { ((volatile char *)p)[0] = 1; ((volatile char *)p)[n - 1] = 1; } }
static HANDLE g_hold;                      /* manual event: every thread holds its blocks until all have allocated */
static volatile LONG g_ready;
static DWORD WINAPI alloc_fn(LPVOID arg) {
    int local;
    int bad = 0;
    int mode = (int)(intptr_t)arg;         /* 0 plat thread, 1 os_threads host thread (its stack is checked too), 2 main */
    if (mode == 1) bad += !low_ok(&local);
    void *small[100];
    for (int i = 0; i < 100; i++) { small[i] = malloc(24); bad += !low_ok(small[i]); }
    static const size_t sizes[] = {1 << 20, 64 << 20, (size_t)1 << 30};
    void *big[3];
    for (int i = 0; i < 3; i++) { big[i] = malloc(sizes[i]); touch(big[i], sizes[i]); bad += !big[i] || !low_ok(big[i]); }
    void *z = calloc(1, 4 << 20);
    bad += !z || !low_ok(z) || ((char *)z)[12345] != 0;
    void *r = malloc(1 << 20);
    r = realloc(r, 32 << 20);
    touch(r, 32 << 20);
    bad += !r || !low_ok(r);
    void *a1 = _aligned_malloc(1 << 20, 4096), *a2 = _aligned_malloc(8 << 20, 65536), *pm = NULL;
    bad += !a1 || !low_ok(a1) || ((uintptr_t)a1 & 4095) || !a2 || !low_ok(a2) || ((uintptr_t)a2 & 65535);
    bad += posix_memalign(&pm, 256, 3 << 20) != 0 || !low_ok(pm);
    InterlockedIncrement(&g_ready);
    if (mode != 2) WaitForSingleObject(g_hold, INFINITE);
    for (int i = 0; i < 100; i++) free(small[i]);
    for (int i = 0; i < 3; i++) free(big[i]);
    free(z); free(r); _aligned_free(a1); _aligned_free(a2); free(pm);
    return (DWORD)bad;
}
static void maps_above_2_47(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    printf("    mappings at or above 2^47 (glibc thread stacks, libraries, vdso: never handed to the guest):\n");
    int anon = 0;
    while (f && fgets(line, sizeof line, f)) {
        unsigned long long lo = strtoull(line, NULL, 16), hi = strtoull(strchr(line, '-') + 1, NULL, 16);
        if (lo < (1ull << 47)) continue;
        if (strchr(line, '/') || strchr(line, '[')) printf("      %s", line);
        else anon++;
        (void)hi;
    }
    printf("      + %d anonymous mappings (glibc thread stacks and their guards, ld.so/libc data)\n", anon);
    if (f) fclose(f);
}
int main(void) {
    vmem_init();
    void *brk0 = sbrk(0);
    g_hold = CreateEventA(NULL, TRUE, FALSE, NULL);
    int bad = 0;
    SECTION("main thread");
    {   /* main thread alone first */
        void *p[4] = {malloc(24), malloc(1 << 20), malloc(64 << 20), malloc((size_t)1 << 30)};
        for (int i = 0; i < 4; i++) { bad += !p[i] || !low_ok(p[i]); }
        touch(p[3], (size_t)1 << 30);
        for (int i = 0; i < 4; i++) free(p[i]);
    }
    SECTION("16 plat threads + 16 os_threads host threads + main, all holding their blocks at once");
    HANDLE h[NT];
    struct host_thread *ht[NT];
    for (int i = 0; i < NT; i++) h[i] = CreateThread(NULL, 0, alloc_fn, NULL, 0, NULL);
    for (int i = 0; i < NT; i++) ht[i] = host_thread_create((size_t)256 << 20, alloc_fn, (void *)1, "heap", 0);
    bad += (int)alloc_fn((void *)2);
    while (LOAD(g_ready) < 2 * NT + 1) Sleep(1);
    {
        char *buf = NULL; size_t n = 0; int arenas = 0;
        FILE *m = open_memstream(&buf, &n);
        malloc_info(0, m);                 /* one <heap nr=...> per arena */
        fclose(m);
        for (char *q = buf; q && (q = strstr(q, "<heap nr=")); q++) arenas++;
        free(buf);
        printf("    brk heap %p -> %p (%.1f GiB); malloc arenas: %d\n", brk0, sbrk(0), (double)((char *)sbrk(0) - (char *)brk0) / (1 << 30), arenas);
#ifndef NOFIX
        CHECK(arenas == 1);
#endif
    }
    SetEvent(g_hold);
    for (int i = 0; i < NT; i++) {
        DWORD c = 1;
        WaitForSingleObject(h[i], INFINITE); GetExitCodeThread(h[i], &c); bad += (int)c; CloseHandle(h[i]);
        WaitForSingleObject(host_thread_handle(ht[i]), INFINITE); bad += (int)host_thread_exit_code(ht[i]); CloseHandle(host_thread_handle(ht[i])); host_thread_release(ht[i]);
    }
    maps_above_2_47();
    printf("    %d addresses checked: %d at or above 2^47, %d in the guest window\n", g_total, g_high, g_window);
#ifdef NOFIX
    printf("control build (no plat_heap.o): %s\n", g_high ? "high addresses seen, as expected without the fix" : "no high address (control did not trigger)");
    return 0;
#else
    CHECK(bad == 0 && g_high == 0 && g_window == 0);
    return tp_done("test_heap");
#endif
}
