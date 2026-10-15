/* test_readable.c - the readable check behind nncxxrt.c cxxrt_readable (unwinder / RTTI correctness) and the
 * never-faulting reads of the crash walker: vmem_readable and crash_read on mapped, unmapped, PROT_NONE, reserved,
 * decommitted, released, straddling and untracked (raw mmap, stack, heap) memory. cxxrt_readable on Linux is
 * `p >= 0x10000 && vmem_readable(p, n ? n : 1)`; its copy below is checked against the same cases. */
#include "tp.h"
#include "plat_crash.h"
#include <sys/mman.h>

static int cxxrt_readable_linux(uint64_t p, size_t n) { if (p < 0x10000) return 0; return vmem_readable((const void *)(uintptr_t)p, n ? n : 1); }
static int both(const void *p, size_t n) {                /* vmem_readable, crash_read and cxxrt_readable agree; returns the answer */
    uint64_t a = (uint64_t)(uintptr_t)p;
    int r1 = vmem_readable(p, n);
    unsigned char buf[256];
    int r2 = n <= sizeof buf ? crash_read(a, buf, n) : r1;
    int r3 = cxxrt_readable_linux(a, n);
    CHECKF(r1 == r2, "vmem_readable %d vs crash_read %d at %p+%zu", r1, r2, p, n);
    if (a >= 0x10000 && n) CHECKF(r1 == r3, "cxxrt_readable %d at %p+%zu", r3, p, n);
    if (r2 && n && n <= sizeof buf) CHECK(memcmp(buf, p, n) == 0);
    return r1;
}

int main(void) {
    vmem_init();
    const size_t PG = 4096;

    SECTION("vmem allocation outside the window: committed / PROT_NONE page / straddle / released");
    uint8_t *a = vmem_alloc(0x20000, PROT_READ | PROT_WRITE);
    CHECK(a != NULL);
    memset(a, 0x5a, 0x20000);
    CHECK(both(a, 0x20000) == 1);
    CHECK(both(a + 0x1ffff, 1) == 1);
    CHECK(both(a + 0x20000 - 8, 16) == 0);                /* runs past the end (next 64 KiB granule is free or someone else's: tested by the table) */
    CHECK(vmem_protect(a + 0x8000, PG, PROT_NONE, NULL) == 0);
    CHECK(both(a + 0x8000, 8) == 0);
    CHECK(both(a + 0x8000 - 8, 16) == 0);                 /* straddles into the PROT_NONE page */
    CHECK(both(a + 0x7000, PG) == 1);
    CHECK(both(a + 0x9000, 8) == 1);
    CHECK(vmem_protect(a + 0x8000, PG, PROT_READ, NULL) == 0);
    CHECK(both(a + 0x8000, 8) == 1);                      /* read-only is readable */
    CHECK(vmem_decommit(a + 0x10000, 0x10000) == 0);
    CHECK(both(a + 0x10000, 8) == 0);                     /* decommitted = reserved */
    CHECK(both(a + 0xfff8, 8) == 1);
    CHECK(vmem_release(a) == 0);
    CHECK(both(a, 8) == 0);
    CHECK(both(a + 0x9000, 8) == 0);

    SECTION("inside the guest window: free, reserved, committed, decommitted, PROT_NONE commit");
    uint64_t g = 0x7240000000ull;                         /* NRO range: nothing else in this test uses it */
    CHECK(both((void *)(uintptr_t)g, 8) == 0);            /* free window page */
    CHECK(vmem_reserve(g, 0x40000, VMEM_RESERVE_ONLY) == (void *)(uintptr_t)g);
    CHECK(both((void *)(uintptr_t)g, 8) == 0);            /* reserved only */
    CHECK(vmem_commit((void *)(uintptr_t)(g + 0x10000), 0x10000, PROT_READ | PROT_WRITE) == 0);
    CHECK(both((void *)(uintptr_t)(g + 0x10000), 0x10000) == 1);
    CHECK(both((void *)(uintptr_t)(g + 0x10000 - 4), 8) == 0);
    CHECK(both((void *)(uintptr_t)(g + 0x20000 - 4), 8) == 0);
    CHECK(vmem_commit((void *)(uintptr_t)(g + 0x20000), PG, PROT_NONE) == 0);
    CHECK(both((void *)(uintptr_t)(g + 0x20000), 8) == 0);   /* committed but PROT_NONE */
    CHECK(vmem_decommit((void *)(uintptr_t)(g + 0x10000), 0x10000) == 0);
    CHECK(both((void *)(uintptr_t)(g + 0x10000), 8) == 0);
    CHECK(vmem_release((void *)(uintptr_t)g) == 0);
    CHECK(both((void *)(uintptr_t)(g + 0x10000), 8) == 0);

    SECTION("untracked host memory: stack, heap, raw mmap RW / PROT_NONE / unmapped, null, kernel, overflow");
    volatile uint64_t local = 0x1122334455667788ull;
    CHECK(both((const void *)&local, 8) == 1);
    char *h = malloc(1000);
    memset(h, 1, 1000);
    CHECK(both(h, 256) == 1);
    free(h);
    uint8_t *m = mmap(NULL, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    m[0] = 7;
    CHECK(both(m, 3 * PG > 256 ? 256 : 3 * PG) == 1);
    CHECK(vmem_readable(m, 3 * PG) == 1);
    mprotect(m + PG, PG, PROT_NONE);
    CHECK(both(m + PG, 8) == 0);
    CHECK(both(m + PG - 4, 8) == 0);
    CHECK(both(m + 2 * PG, 8) == 1);
    CHECK(vmem_readable(m, 3 * PG) == 0);
    munmap(m, 3 * PG);
    CHECK(both(m, 8) == 0);
    CHECK(both(m + 2 * PG, 8) == 0);
    CHECK(vmem_readable(NULL, 8) == 0);
    CHECK(crash_read(0, (void *)&local, 8) == 0);
    CHECK(cxxrt_readable_linux(0x8000, 8) == 0);          /* below 0x10000: never (null-page objects) */
    CHECK(both((void *)(uintptr_t)0xffff800000001000ull, 8) == 0);
    CHECK(vmem_readable((void *)(uintptr_t)0xfffffffffffffff0ull, 0x20) == 0);   /* wraps */
    CHECK(vmem_readable((const void *)&local, 0) == 1);
    CHECK(cxxrt_readable_linux((uint64_t)(uintptr_t)&local, 0) == 1);

    SECTION("frame-pointer walk of this thread reaches main's caller");
    uint64_t pcs[64];
    int n = crash_backtrace(pcs, 64);
    CHECK(n >= 1);                                        /* main (then libc frames, if libc keeps frame records) */
    CHECK(n < 64);
    for (int i = 0; i < n; i++) CHECK(pcs[i] != 0);

    return tp_done("test_readable");
}
