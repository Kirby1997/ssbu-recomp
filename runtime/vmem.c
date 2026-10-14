/* vmem.c - guest address space, region table, Win32 Virtual* shims (see vmem.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "plat.h"
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/personality.h>
#include <fcntl.h>
#include <stdarg.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#define USER_TOP (1ull << 48)                 /* aarch64 48-bit VA; VirtualQuery answers 0 above */
#define PAGE_DOWN(a) ((uint64_t)(a) & ~(VMEM_PAGE - 1))
#define PAGE_UP(a) (((uint64_t)(a) + VMEM_PAGE - 1) & ~(VMEM_PAGE - 1))

typedef struct run {
    uint64_t lo, hi;                          /* [lo, hi), page aligned */
    uint64_t abase;                           /* allocation base */
    int aprot, state, prot, in_window;
} run;

static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;
static run *g_runs;
static size_t g_n, g_cap;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_init_rc;

/* ------------------------------------------------------------------------------------------------ init */
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
/* /proc/self/maps, line by line, with read(2) and a stack buffer (no malloc: usable from crash paths). The callback
 * returns nonzero to stop. */
typedef int (*maps_fn)(uint64_t lo, uint64_t hi, const char *perms, const char *path, void *ctx);
static void maps_each(maps_fn fn, void *ctx) {
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buf[8192], line[4352];
    size_t ll = 0;
    int stop = 0;
    for (ssize_t n; !stop && (n = read(fd, buf, sizeof buf)) > 0;) {
        for (ssize_t i = 0; i < n && !stop; i++) {
            char ch = buf[i];
            if (ch != '\n') { if (ll < sizeof line - 1) line[ll++] = ch; continue; }
            line[ll] = 0;
            ll = 0;
            unsigned long long lo, hi;
            char perms[8] = {0};
            int off = 0;
            if (sscanf(line, "%llx-%llx %7s %*s %*s %*s%n", &lo, &hi, perms, &off) < 3) continue;
            const char *path = line + off;
            while (*path == ' ') path++;
            stop = fn(lo, hi, perms, path, ctx);
        }
    }
    close(fd);
}
static int dump_line(uint64_t lo, uint64_t hi, const char *perms, const char *path, void *ctx) {
    int overlap = lo < VMEM_GUEST_HI && hi > VMEM_GUEST_LO;
    if (!ctx || overlap)
        say("  %s%012llx-%012llx %s %s\n", overlap ? "** " : "   ", (unsigned long long)lo, (unsigned long long)hi, perms, path);
    return 0;
}
static void init_once(void) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps != (long)VMEM_PAGE) { g_init_rc = -EINVAL; return; }
    uint64_t size = VMEM_GUEST_HI - VMEM_GUEST_LO;
    void *p = mmap((void *)(uintptr_t)VMEM_GUEST_LO, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) { g_init_rc = -errno; return; }
    if (p != (void *)(uintptr_t)VMEM_GUEST_LO) { munmap(p, size); g_init_rc = -EEXIST; return; }   /* kernel < 4.17 ignores NOREPLACE */
    g_init_rc = 0;
}
int vmem_try_init(void) {
    pthread_once(&g_once, init_once);
    return g_init_rc;
}
static void reexec_without_aslr(void) {
    if (getenv("SSBU_VMEM_NO_REEXEC") || getenv("SSBU_VMEM_REEXECED")) return;
    static char cmd[65536];
    static char *argv[1024];
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t n = 0, r;
    while (n < (ssize_t)sizeof cmd - 1 && (r = read(fd, cmd + n, sizeof cmd - 1 - (size_t)n)) > 0) n += r;
    close(fd);
    if (n <= 0) return;
    cmd[n] = 0;
    int argc = 0;
    for (ssize_t i = 0; i < n && argc < 1023; i += (ssize_t)strlen(cmd + i) + 1) argv[argc++] = cmd + i;
    argv[argc] = NULL;
    setenv("SSBU_VMEM_REEXECED", "1", 1);
    int pers = personality(0xffffffff);
    if (pers == -1 || personality((unsigned long)pers | ADDR_NO_RANDOMIZE) == -1) return;
    say("vmem: re-executing once with ADDR_NO_RANDOMIZE (set SSBU_VMEM_NO_REEXEC=1 to disable)\n");
    fflush(NULL);
    execv("/proc/self/exe", argv);
    say("vmem: re-exec failed: %s\n", strerror(errno));
}
void vmem_init(void) {
    int rc = vmem_try_init();
    if (rc == 0) return;
    if (rc == -EINVAL) {
        say("FATAL vmem: page size is %ld bytes; this build needs 4096 (GUEST_RO_END and the NRO data split are "
            "4 KiB aligned: a 16K/64K-page kernel needs the RO/RW split reworked, see PORT_AUDIT Phase 0)\n", sysconf(_SC_PAGESIZE));
        _exit(70);
    }
    say("FATAL vmem: cannot reserve the guest window [0x%llx, 0x%llx): %s\n", (unsigned long long)VMEM_GUEST_LO,
        (unsigned long long)VMEM_GUEST_HI, strerror(-rc));
    say("vmem: mappings overlapping the window (marked **):\n");
    maps_each(dump_line, (void *)1);
    say("vmem: full /proc/self/maps:\n");
    maps_each(dump_line, NULL);
    if (rc == -EEXIST) reexec_without_aslr();
    _exit(70);
}
static void ensure_init(void) {
    if (__builtin_expect(vmem_try_init() != 0, 0)) vmem_init();
}
int vmem_in_guest(uint64_t addr, uint64_t size) {
    return addr >= VMEM_GUEST_LO && addr <= VMEM_GUEST_HI && size <= VMEM_GUEST_HI - addr;
}

/* ------------------------------------------------------------------------------------------------ region table */
/* first run with hi > a (runs are sorted and disjoint) */
static size_t lower(uint64_t a) {
    size_t l = 0, h = g_n;
    while (l < h) { size_t m = (l + h) / 2; if (g_runs[m].hi <= a) l = m + 1; else h = m; }
    return l;
}
static int insert_at(size_t i, run r) {
    if (g_n == g_cap) {
        size_t cap = g_cap ? g_cap * 2 : 256;
        run *n = realloc(g_runs, cap * sizeof *n);
        if (!n) return -ENOMEM;
        g_runs = n;
        g_cap = cap;
    }
    memmove(&g_runs[i + 1], &g_runs[i], (g_n - i) * sizeof *g_runs);
    g_runs[i] = r;
    g_n++;
    return 0;
}
static void remove_range(size_t i, size_t count) {
    memmove(&g_runs[i], &g_runs[i + count], (g_n - i - count) * sizeof *g_runs);
    g_n -= count;
}
/* make a run boundary at a (if a is inside a run) */
static int split(uint64_t a) {
    size_t i = lower(a);
    if (i >= g_n || g_runs[i].lo >= a) return 0;
    run r = g_runs[i];
    r.lo = a;
    g_runs[i].hi = a;
    return insert_at(i + 1, r);
}
static void coalesce(void) {
    size_t w = 0;
    for (size_t i = 0; i < g_n; i++) {
        if (w && g_runs[w - 1].hi == g_runs[i].lo && g_runs[w - 1].abase == g_runs[i].abase && g_runs[w - 1].state == g_runs[i].state &&
            g_runs[w - 1].prot == g_runs[i].prot && g_runs[w - 1].in_window == g_runs[i].in_window)
            g_runs[w - 1].hi = g_runs[i].hi;
        else
            g_runs[w++] = g_runs[i];
    }
    g_n = w;
}
/* [lo, hi) is fully covered, without gaps, by runs of ONE allocation: after splitting at lo and hi, the runs are
 * the index range [*a, *b). */
static int span_of_one_allocation(uint64_t lo, uint64_t hi, size_t *a, size_t *b) {
    if (hi <= lo) return -EINVAL;
    size_t i = lower(lo);
    if (i >= g_n || g_runs[i].lo > lo) return -EINVAL;
    uint64_t abase = g_runs[i].abase, at = g_runs[i].hi;
    for (i++; at < hi; i++) {
        if (i >= g_n || g_runs[i].lo != at || g_runs[i].abase != abase) return -EINVAL;
        at = g_runs[i].hi;
    }
    if (split(lo) || split(hi)) return -ENOMEM;
    *a = lower(lo);
    *b = lower(hi);                                       /* no run straddles hi any more */
    return 0;
}
static int map_fixed(uint64_t lo, uint64_t hi, int prot, int noreserve) {
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | (noreserve ? MAP_NORESERVE : 0), -1, 0);
    return p == MAP_FAILED ? -errno : 0;
}

/* ------------------------------------------------------------------------------------------------ allocation */
static void *new_allocation(uint64_t lo, uint64_t hi, int prot, int in_window) {
    run r = {lo, hi, lo, prot < 0 ? 0 : prot, prot < 0 ? VMEM_STATE_RESERVED : VMEM_STATE_COMMITTED, prot < 0 ? 0 : prot, in_window};
    if (prot >= 0 && map_fixed(lo, hi, prot, 0) != 0) return NULL;
    if (insert_at(lower(lo), r) != 0) return NULL;
    coalesce();
    return (void *)(uintptr_t)lo;
}
void *vmem_reserve(uint64_t addr, size_t size, int prot) {
    ensure_init();
    uint64_t lo = addr & ~(VMEM_GRANULE - 1), hi = PAGE_UP(addr + size);
    if (!size || !vmem_in_guest(lo, hi - lo)) { errno = EINVAL; return NULL; }
    pthread_rwlock_wrlock(&g_lock);
    size_t i = lower(lo);
    void *res = NULL;
    if (i < g_n && g_runs[i].lo < hi) errno = EEXIST;
    else if (!(res = new_allocation(lo, hi, prot, 1))) errno = ENOMEM;
    pthread_rwlock_unlock(&g_lock);
    return res;
}
/* Anonymous allocations are placed from just above the guest window upward (below 2^47), not where mmap would put
 * them: on arm64 with 48-bit VA the kernel's top-down area is at 0xffff_xxxx_xxxx, above what the guest may hold.
 * A bump cursor with MAP_FIXED_NOREPLACE probing; it wraps once it reaches 2^47 so released ranges get reused. */
#define LOW_START VMEM_GUEST_HI
#define LOW_END (1ull << 47)
static uint64_t g_cursor = LOW_START;
static uint64_t place_low(uint64_t len) {
    uint64_t at = __atomic_load_n(&g_cursor, __ATOMIC_RELAXED);
    for (int tries = 0; tries < 4096; tries++) {
        if (at + len > LOW_END) at = LOW_START;
        void *p = mmap((void *)(uintptr_t)at, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == (void *)(uintptr_t)at) {
            __atomic_store_n(&g_cursor, (at + len + VMEM_GRANULE - 1) & ~(VMEM_GRANULE - 1), __ATOMIC_RELAXED);
            return at;
        }
        if (p != MAP_FAILED) munmap(p, len);
        if (p == MAP_FAILED && errno != EEXIST) return 0;
        at += tries < 64 ? (len + VMEM_GRANULE - 1) & ~(VMEM_GRANULE - 1) : 1ull << 30;   /* step past, then jump by 1 GiB */
        at &= ~(VMEM_GRANULE - 1);
    }
    errno = ENOMEM;
    return 0;
}
static void *alloc_outside(uint64_t want, size_t size, int prot) {
    ensure_init();
    uint64_t len = PAGE_UP(size);
    if (!size) { errno = EINVAL; return NULL; }
    uint64_t lo;
    if (want) {
        lo = want & ~(VMEM_GRANULE - 1);
        len = PAGE_UP(want + size) - lo;
        if (lo < VMEM_GUEST_HI && lo + len > VMEM_GUEST_LO) { errno = EEXIST; return NULL; }
        void *p = mmap((void *)(uintptr_t)lo, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) return NULL;
        if (p != (void *)(uintptr_t)lo) { munmap(p, len); errno = EEXIST; return NULL; }
    } else if (!(lo = place_low(len))) return NULL;
    pthread_rwlock_wrlock(&g_lock);
    void *res = new_allocation(lo, lo + len, prot, 0);
    pthread_rwlock_unlock(&g_lock);
    if (!res) { int e = errno; munmap((void *)(uintptr_t)lo, len); errno = e ? e : ENOMEM; }
    return res;
}
void *vmem_alloc(size_t size, int prot) { return alloc_outside(0, size, prot); }
void *vmem_alloc_at(uint64_t addr, size_t size, int prot) {
    if (!addr) { errno = EINVAL; return NULL; }
    return alloc_outside(addr, size, prot);
}

int vmem_commit(void *addr, size_t size, int prot) {
    ensure_init();
    uint64_t lo = PAGE_DOWN(addr), hi = PAGE_UP((uint64_t)(uintptr_t)addr + size);
    if (!size || prot < 0) return -EINVAL;
    pthread_rwlock_wrlock(&g_lock);
    size_t a, b;
    int rc = span_of_one_allocation(lo, hi, &a, &b);
    for (size_t i = a; rc == 0 && i < b; i++) {
        run *r = &g_runs[i];
        if (r->state == VMEM_STATE_COMMITTED) { if (r->prot != prot && mprotect((void *)(uintptr_t)r->lo, r->hi - r->lo, prot)) rc = -errno; }
        else rc = map_fixed(r->lo, r->hi, prot, 0);
        if (rc == 0) { r->state = VMEM_STATE_COMMITTED; r->prot = prot; }
    }
    coalesce();
    pthread_rwlock_unlock(&g_lock);
    return rc;
}
int vmem_decommit(void *addr, size_t size) {
    ensure_init();
    uint64_t lo = PAGE_DOWN(addr), hi = PAGE_UP((uint64_t)(uintptr_t)addr + size);
    if (!size) return -EINVAL;
    pthread_rwlock_wrlock(&g_lock);
    size_t a, b;
    int rc = span_of_one_allocation(lo, hi, &a, &b);
    if (rc == 0) rc = map_fixed(lo, hi, PROT_NONE, 1);
    for (size_t i = a; rc == 0 && i < b; i++) { g_runs[i].state = VMEM_STATE_RESERVED; g_runs[i].prot = 0; }
    coalesce();
    pthread_rwlock_unlock(&g_lock);
    return rc;
}
int vmem_protect(void *addr, size_t size, int prot, int *old_prot) {
    ensure_init();
    uint64_t lo = PAGE_DOWN(addr), hi = PAGE_UP((uint64_t)(uintptr_t)addr + size);
    if (!size || prot < 0) return -EINVAL;
    pthread_rwlock_wrlock(&g_lock);
    size_t a, b;
    int rc = span_of_one_allocation(lo, hi, &a, &b);
    for (size_t i = a; rc == 0 && i < b; i++) if (g_runs[i].state != VMEM_STATE_COMMITTED) rc = -EINVAL;
    if (rc == 0) {
        if (old_prot) *old_prot = g_runs[a].prot;
        if (mprotect((void *)(uintptr_t)lo, hi - lo, prot)) rc = -errno;
        for (size_t i = a; rc == 0 && i < b; i++) g_runs[i].prot = prot;
    }
    coalesce();
    pthread_rwlock_unlock(&g_lock);
    return rc;
}
int vmem_release(void *base) {
    ensure_init();
    uint64_t lo = (uint64_t)(uintptr_t)base;
    pthread_rwlock_wrlock(&g_lock);
    size_t a = lower(lo), b = a;
    int rc = 0;
    if (a >= g_n || g_runs[a].lo != lo || g_runs[a].abase != lo) rc = -EINVAL;
    else {
        while (b < g_n && g_runs[b].abase == lo && (b == a || g_runs[b].lo == g_runs[b - 1].hi)) b++;
        uint64_t hi = g_runs[b - 1].hi;
        if (g_runs[a].in_window) rc = map_fixed(lo, hi, PROT_NONE, 1);
        else if (munmap((void *)(uintptr_t)lo, hi - lo)) rc = -errno;
        if (rc == 0) remove_range(a, b - a);
    }
    pthread_rwlock_unlock(&g_lock);
    return rc;
}

/* ------------------------------------------------------------------------------------------------ query */
typedef struct { uint64_t addr, prev_end, next_lo; int found; uint64_t lo, hi; char perms[8]; int type; } mq;
static int query_line(uint64_t lo, uint64_t hi, const char *perms, const char *path, void *ctx) {
    mq *q = ctx;
    if (hi <= q->addr) { q->prev_end = hi; return 0; }
    if (lo > q->addr) { q->next_lo = lo; return 1; }
    q->found = 1;
    q->lo = lo;
    q->hi = hi;
    memcpy(q->perms, perms, sizeof q->perms);
    if (path[0] != '/') q->type = perms[3] == 's' ? VMEM_TYPE_MAPPED : VMEM_TYPE_PRIVATE;
    else {
        q->type = strstr(path, ".so") ? VMEM_TYPE_IMAGE : VMEM_TYPE_MAPPED;
        if (q->type == VMEM_TYPE_MAPPED) {
            char exe[1024];
            ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
            if (k > 0) { exe[k] = 0; if (strcmp(exe, path) == 0) q->type = VMEM_TYPE_IMAGE; }
        }
    }
    return 1;
}
int vmem_query(const void *p, vmem_info *out) {
    ensure_init();
    uint64_t a = (uint64_t)(uintptr_t)p, pg = PAGE_DOWN(a);
    memset(out, 0, sizeof *out);
    if (a >= USER_TOP) return 0;
    pthread_rwlock_rdlock(&g_lock);
    size_t i = lower(a);
    uint64_t next_tracked = i < g_n ? g_runs[i].lo : USER_TOP;
    if (i < g_n && g_runs[i].lo <= a) {
        run *r = &g_runs[i];
        out->base = pg; out->size = r->hi - pg; out->alloc_base = r->abase; out->alloc_prot = r->aprot;
        out->state = r->state; out->prot = r->state == VMEM_STATE_COMMITTED ? r->prot : 0; out->type = VMEM_TYPE_PRIVATE; out->tracked = 1;
        pthread_rwlock_unlock(&g_lock);
        return 1;
    }
    pthread_rwlock_unlock(&g_lock);
    if (a >= VMEM_GUEST_LO && a < VMEM_GUEST_HI) {             /* window, not allocated: free for the game */
        out->base = pg; out->size = (next_tracked < VMEM_GUEST_HI ? next_tracked : VMEM_GUEST_HI) - pg;
        out->state = VMEM_STATE_FREE;
        return 1;
    }
    mq q = {a, 0, USER_TOP, 0, 0, 0, {0}, 0};
    maps_each(query_line, &q);
    uint64_t limit = next_tracked;
    if (a < VMEM_GUEST_LO && VMEM_GUEST_LO < limit) limit = VMEM_GUEST_LO;
    if (!q.found) {
        uint64_t end = q.next_lo < limit ? q.next_lo : limit;
        out->base = pg; out->size = end - pg; out->state = VMEM_STATE_FREE;
        return 1;
    }
    uint64_t end = q.hi < limit ? q.hi : limit;
    int prot = (q.perms[0] == 'r' ? PROT_READ : 0) | (q.perms[1] == 'w' ? PROT_WRITE : 0) | (q.perms[2] == 'x' ? PROT_EXEC : 0);
    out->base = pg; out->size = end - pg; out->alloc_base = q.lo; out->alloc_prot = prot;
    out->state = prot ? VMEM_STATE_COMMITTED : VMEM_STATE_RESERVED; out->prot = prot; out->type = q.type;
    return 1;
}

/* ------------------------------------------------------------------------------------------------ readable */
static pthread_mutex_t g_pipe_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_pipe[2] = {-1, -1};
static int probe_pipe(uint64_t lo, uint64_t hi) {       /* fallback when process_vm_readv is unavailable (seccomp) */
    int ok = 1;
    pthread_mutex_lock(&g_pipe_lock);
    if (g_pipe[0] < 0 && pipe2(g_pipe, O_CLOEXEC | O_NONBLOCK)) { pthread_mutex_unlock(&g_pipe_lock); return 0; }
    for (uint64_t p = lo; ok && p < hi; p = PAGE_DOWN(p) + VMEM_PAGE) {
        ssize_t w = write(g_pipe[1], (const void *)(uintptr_t)p, 1);
        if (w == 1) { char c; if (read(g_pipe[0], &c, 1) != 1) ok = 0; }
        else ok = 0;
    }
    pthread_mutex_unlock(&g_pipe_lock);
    return ok;
}
static int probe(uint64_t lo, uint64_t hi) {
    static int no_pvr;
    if (__atomic_load_n(&no_pvr, __ATOMIC_RELAXED)) return probe_pipe(lo, hi);
    char local[64];
    struct iovec liov = {local, 0}, riov[64];
    uint64_t p = lo;
    while (p < hi) {
        int n = 0;
        for (; n < 64 && p < hi; n++, p = PAGE_DOWN(p) + VMEM_PAGE) { riov[n].iov_base = (void *)(uintptr_t)p; riov[n].iov_len = 1; }
        liov.iov_len = (size_t)n;
        ssize_t r = process_vm_readv(getpid(), &liov, 1, riov, (unsigned long)n, 0);
        if (r == n) continue;
        if (r < 0 && (errno == ENOSYS || errno == EPERM)) { __atomic_store_n(&no_pvr, 1, __ATOMIC_RELAXED); return probe_pipe(lo, hi); }
        return 0;                                        /* EFAULT or a short read: some page is not readable */
    }
    return 1;
}
int vmem_readable(const void *addr, size_t len) {
    ensure_init();
    uint64_t lo = (uint64_t)(uintptr_t)addr, hi = lo + len;
    if (!len) return 1;
    if (hi < lo || hi > USER_TOP) return 0;
    pthread_rwlock_rdlock(&g_lock);
    uint64_t at = lo;
    int ok = 1, covered = 1;
    for (size_t i = lower(lo); i < g_n && g_runs[i].lo < hi; i++) {
        if (g_runs[i].lo > at) { covered = 0; if (at < VMEM_GUEST_HI && g_runs[i].lo > VMEM_GUEST_LO) ok = 0; }   /* free window pages */
        if (g_runs[i].state != VMEM_STATE_COMMITTED || !(g_runs[i].prot & PROT_READ)) ok = 0;
        at = g_runs[i].hi;
    }
    if (at < hi) { covered = 0; if (at < VMEM_GUEST_HI && hi > VMEM_GUEST_LO) ok = 0; }
    pthread_rwlock_unlock(&g_lock);
    if (!ok) return 0;
    if (covered) return 1;
    return probe(lo, hi);
}

size_t vmem_region_count(void) {
    pthread_rwlock_rdlock(&g_lock);
    size_t n = g_n;
    pthread_rwlock_unlock(&g_lock);
    return n;
}
void vmem_dump(void *file) {
    FILE *f = file ? file : stderr;
    static const char *st[] = {"free", "reserved", "committed"};
    pthread_rwlock_rdlock(&g_lock);
    for (size_t i = 0; i < g_n; i++)
        fprintf(f, "  %012llx-%012llx %-9s prot %d alloc %012llx%s\n", (unsigned long long)g_runs[i].lo, (unsigned long long)g_runs[i].hi,
                st[g_runs[i].state], g_runs[i].prot, (unsigned long long)g_runs[i].abase, g_runs[i].in_window ? " (window)" : "");
    pthread_rwlock_unlock(&g_lock);
}

/* ------------------------------------------------------------------------------------------------ Win32 shims */
static int page_to_prot(DWORD p) {
    switch (p & 0xffu) {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE: case PAGE_WRITECOPY: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE: return PROT_EXEC;
    case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return -1;
    }
}
static DWORD prot_to_page(int p) {
    switch (p & (PROT_READ | PROT_WRITE | PROT_EXEC)) {
    case PROT_NONE: return PAGE_NOACCESS;
    case PROT_READ: return PAGE_READONLY;
    case PROT_WRITE: case PROT_READ | PROT_WRITE: return PAGE_READWRITE;
    case PROT_EXEC: return PAGE_EXECUTE;
    case PROT_READ | PROT_EXEC: return PAGE_EXECUTE_READ;
    default: return PAGE_EXECUTE_READWRITE;
    }
}
static DWORD rc_to_error(int rc) { return rc == -ENOMEM ? ERROR_NOT_ENOUGH_MEMORY : rc == -EEXIST || rc == -EINVAL ? ERROR_INVALID_ADDRESS : ERROR_INVALID_PARAMETER; }

LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect) {
    if (type & MEM_WRITE_WATCH) {
        static int said;
        if (!__atomic_exchange_n(&said, 1, __ATOMIC_RELAXED))
            fprintf(stderr, "VirtualAlloc: MEM_WRITE_WATCH is not supported on this platform (its only user was the OpenGL "
                            "renderer, which is not built here): drop the flag at the call site\n");
        SetLastError(ERROR_NOT_SUPPORTED);
        return NULL;
    }
    int prot = page_to_prot(protect);
    if (!size || prot < 0 || (type & ~(MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN)) || !(type & (MEM_COMMIT | MEM_RESERVE))) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    if (protect & PAGE_GUARD) prot = PROT_NONE;             /* DIFF: guard pages are plain no-access pages */
    uint64_t a = (uint64_t)(uintptr_t)addr;
    if ((type & MEM_RESERVE) || !addr) {
        int p = (type & MEM_COMMIT) ? prot : VMEM_RESERVE_ONLY;
        void *r;
        errno = 0;
        if (!addr) r = vmem_alloc(size, p);
        else if (a >= VMEM_GUEST_LO && a < VMEM_GUEST_HI) r = vmem_reserve(a, size, p);
        else r = vmem_alloc_at(a, size, p);
        if (!r) SetLastError(errno == ENOMEM ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_ADDRESS);
        return r;
    }
    int rc = vmem_commit(addr, size, prot);
    if (rc) { SetLastError(rc_to_error(rc)); return NULL; }
    return (LPVOID)(uintptr_t)PAGE_DOWN(a);
}
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type) {
    int rc;
    if (type == MEM_RELEASE) {
        if (size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        rc = vmem_release(addr);
    } else if (type == MEM_DECOMMIT) {
        if (!size) {
            vmem_info mi;
            if (!vmem_query(addr, &mi) || !mi.tracked || mi.alloc_base != (uint64_t)(uintptr_t)addr) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
            uint64_t end = mi.base;                                  /* end of the allocation */
            for (vmem_info m = mi; vmem_query((void *)(uintptr_t)end, &m) && m.tracked && m.alloc_base == mi.alloc_base;) end = m.base + m.size;
            size = end - (uint64_t)(uintptr_t)addr;
        }
        rc = vmem_decommit(addr, size);
    } else { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (rc) { SetLastError(rc_to_error(rc)); return FALSE; }
    return TRUE;
}
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect, PDWORD old) {
    int prot = page_to_prot(protect), oldp = 0;
    if (prot < 0 || !size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (protect & PAGE_GUARD) prot = PROT_NONE;
    vmem_info mi;
    uint64_t a = (uint64_t)(uintptr_t)addr;
    if (vmem_query(addr, &mi) && !mi.tracked && !vmem_in_guest(a, 1)) {   /* host memory we did not allocate: plain mprotect */
        if (mprotect((void *)(uintptr_t)PAGE_DOWN(a), PAGE_UP(a + size) - PAGE_DOWN(a), prot)) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
        if (old) *old = prot_to_page(mi.prot);
        return TRUE;
    }
    int rc = vmem_protect(addr, size, prot, &oldp);
    if (rc) { SetLastError(rc_to_error(rc)); return FALSE; }
    if (old) *old = prot_to_page(oldp);
    return TRUE;
}
SIZE_T VirtualQuery(LPCVOID addr, PMEMORY_BASIC_INFORMATION info, SIZE_T len) {
    vmem_info mi;
    if (!info || len < sizeof *info) { SetLastError(24 /* ERROR_BAD_LENGTH */); return 0; }
    if (!vmem_query(addr, &mi)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    memset(info, 0, sizeof *info);
    info->BaseAddress = (PVOID)(uintptr_t)mi.base;
    info->RegionSize = (SIZE_T)mi.size;
    info->AllocationBase = (PVOID)(uintptr_t)mi.alloc_base;
    info->AllocationProtect = mi.state == VMEM_STATE_FREE ? 0 : prot_to_page(mi.alloc_prot);
    info->State = mi.state == VMEM_STATE_FREE ? MEM_FREE : mi.state == VMEM_STATE_RESERVED ? MEM_RESERVE : MEM_COMMIT;
    info->Protect = mi.state == VMEM_STATE_FREE ? PAGE_NOACCESS : mi.state == VMEM_STATE_RESERVED ? 0 : prot_to_page(mi.prot);
    info->Type = mi.state == VMEM_STATE_FREE ? 0 : mi.type == VMEM_TYPE_IMAGE ? MEM_IMAGE : mi.type == VMEM_TYPE_MAPPED ? MEM_MAPPED : MEM_PRIVATE;
    return sizeof *info;
}
void GetSystemInfo(LPSYSTEM_INFO si) {
    memset(si, 0, sizeof *si);
#if defined(__aarch64__)
    si->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM64;
#else
    si->wProcessorArchitecture = 9;   /* PROCESSOR_ARCHITECTURE_AMD64 */
#endif
    si->dwPageSize = (DWORD)sysconf(_SC_PAGESIZE);
    si->lpMinimumApplicationAddress = (LPVOID)(uintptr_t)0x10000;
    si->lpMaximumApplicationAddress = (LPVOID)(uintptr_t)(USER_TOP - 0x10001);   /* arm64 48-bit: libraries, heap and stacks live above 2^47 */
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    si->dwNumberOfProcessors = n > 0 ? (DWORD)n : 1;
    si->dwActiveProcessorMask = si->dwNumberOfProcessors >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << si->dwNumberOfProcessors) - 1);
    si->dwAllocationGranularity = (DWORD)VMEM_GRANULE;
}
BOOL FlushInstructionCache(HANDLE process, LPCVOID addr, SIZE_T size) {
    (void)process;
    if (addr && size) __builtin___clear_cache((char *)(uintptr_t)addr, (char *)(uintptr_t)addr + size);
    return TRUE;
}
