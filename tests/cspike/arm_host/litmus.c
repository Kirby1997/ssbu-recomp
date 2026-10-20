/* Multi-threaded litmus tests of the code recomp.py emits for guest LDAR / STLR / LDXR / LDAXR / STXR / STLXR /
 * DMB / DSB / CLREX, run on an AArch64 host (PORT_AUDIT Phase 2). The guest programs are in test_lifter_atomics.py;
 * litmus_gen.h is their lifted C (python3 test_lifter_atomics.py --gen litmus_gen.h).
 *
 *   cc -O1 -fno-strict-aliasing -ffp-contract=off -I../../runtime -I. litmus.c -o litmus -lpthread
 *   ./litmus [iterations per thread = 1000000] [threads = 8]
 *
 * Must hold (exit status 1 otherwise):
 *   MP  (message passing)   str data; stlr flag   ||  ldar flag; ldr data          data seen once flag is seen
 *                           str data; dmb ishst; str flag || ldr flag; dmb ishld; ldr data
 *   SB  (store buffering)   str x; dmb ish / dsb sy; ldr y  ||  str y; dmb ish; ldr x   not both 0
 *                           stlr x; ldar y  ||  stlr y; ldar x                           not both 0 (STLR->LDAR is ordered)
 *   lock counters           ldaxr/stxr ... stlr,  ldxr/stxr+dmb ... ldxr/stlxr,  byte lock,  ldaxr/stlxr fetch-add,
 *                           ldxr/stxr fetch-add: the count is exactly threads x iterations
 *   clrex                   a STXR after CLREX fails, the next LDXR/STXR pair succeeds
 * Controls (plain str/ldr, no barrier): the number of reorderings seen is printed for information only; a non-zero
 * SB count shows the harness is able to observe reordering on this machine. */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t g_stx_fail;
void stx_diag(Ctx *c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits)
{
    (void)c; (void)a; (void)expected; (void)seen; (void)v; (void)bits;
    __atomic_fetch_add(&g_stx_fail, 1, __ATOMIC_RELAXED);
}

#include "litmus_gen.h"

typedef void (*GFn)(Ctx *);
static long g_iters = 1000000;
static int g_threads = 8;
static int g_failed;

static void result(const char *name, long bad, int must_be_zero, const char *what)
{
    if (must_be_zero) {
        printf("%-4s %-44s %ld %s\n", bad ? "FAIL" : "ok", name, bad, what);
        if (bad) g_failed = 1;
    } else
        printf("info %-44s %ld %s (control, informational)\n", name, bad, what);
}

/* ------------------------------------------------------------------ message passing */
#define MP_SLOTS (1 << 20)
static uint64_t *mp_data, *mp_flag;
static GFn mp_w, mp_r;
static long mp_bad;

static void *mp_writer(void *arg)
{
    (void)arg;
    Ctx c;
    memset(&c, 0, sizeof c);
    for (long i = 0; i < MP_SLOTS; i++) {
        c.x[0] = (uint64_t)(uintptr_t)&mp_data[i];
        c.x[1] = (uint64_t)(uintptr_t)&mp_flag[i];
        c.x[2] = (uint64_t)i + 1;
        mp_w(&c);
    }
    return NULL;
}
static void *mp_reader(void *arg)
{
    (void)arg;
    Ctx c;
    long bad = 0;
    memset(&c, 0, sizeof c);
    for (long i = 0; i < MP_SLOTS; i++) {
        c.x[0] = (uint64_t)(uintptr_t)&mp_data[i];
        c.x[1] = (uint64_t)(uintptr_t)&mp_flag[i];
        mp_r(&c);
        if (c.x[4] != (uint64_t)i + 1) bad++;
    }
    mp_bad = bad;
    return NULL;
}
static void run_mp(const char *name, GFn w, GFn r, int must)
{
    long rounds = (g_iters + MP_SLOTS - 1) / MP_SLOTS * 4, bad = 0;     /* 4 pairs' worth of slots per thread pair */
    mp_w = w; mp_r = r;
    for (long k = 0; k < rounds; k++) {
        memset(mp_data, 0, MP_SLOTS * sizeof *mp_data);
        memset(mp_flag, 0, MP_SLOTS * sizeof *mp_flag);
        pthread_t a, b;
        pthread_create(&b, NULL, mp_reader, NULL);
        pthread_create(&a, NULL, mp_writer, NULL);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
        bad += mp_bad;
    }
    char what[96];
    snprintf(what, sizeof what, "stale reads in %ld messages", rounds * MP_SLOTS);
    result(name, bad, must, what);
}

/* ------------------------------------------------------------------ store buffering */
typedef struct { uint64_t v; char pad[120]; } Line;
static Line *sb_x, *sb_y;
static uint64_t *sb_r0, *sb_r1;
static GFn sb_f;
static struct { volatile long pos; char pad[120]; } sb_pos[2];

static void *sb_thread(void *arg)
{
    int me = (int)(intptr_t)arg;
    Ctx c;
    memset(&c, 0, sizeof c);
    for (long i = 0; i < g_iters; i++) {
        /* loose lock-step with relaxed accesses only (they add no ordering to the test's own store -> load) */
        __atomic_store_n(&sb_pos[me].pos, i, __ATOMIC_RELAXED);
        while (__atomic_load_n(&sb_pos[!me].pos, __ATOMIC_RELAXED) < i) { }
        Line *mine = me ? &sb_y[i & 1023] : &sb_x[i & 1023], *other = me ? &sb_x[i & 1023] : &sb_y[i & 1023];
        c.x[0] = (uint64_t)(uintptr_t)&mine->v;
        c.x[1] = (uint64_t)(uintptr_t)&other->v;
        sb_f(&c);
        (me ? sb_r1 : sb_r0)[i] = c.x[3];
        /* both threads have run slot i before it is cleared for slot i + 1024 */
        __atomic_store_n(&sb_pos[me].pos, i + 1, __ATOMIC_RELAXED);
        while (__atomic_load_n(&sb_pos[!me].pos, __ATOMIC_RELAXED) < i + 1) { }
        if (!me) { sb_x[i & 1023].v = 0; sb_y[i & 1023].v = 0; }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
    return NULL;
}
static void run_sb(const char *name, GFn f, int must)
{
    sb_f = f;
    memset(sb_x, 0, 1024 * sizeof *sb_x);
    memset(sb_y, 0, 1024 * sizeof *sb_y);
    sb_pos[0].pos = sb_pos[1].pos = -1;
    pthread_t a, b;
    pthread_create(&a, NULL, sb_thread, (void *)0);
    pthread_create(&b, NULL, sb_thread, (void *)1);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    long bad = 0;
    for (long i = 0; i < g_iters; i++) bad += sb_r0[i] == 0 && sb_r1[i] == 0;
    char what[96];
    snprintf(what, sizeof what, "both-zero outcomes in %ld runs", g_iters);
    result(name, bad, must, what);
}

/* ------------------------------------------------------------------ lock / atomic counters */
static struct { union { uint64_t lock; uint8_t lock8; }; char pad[120]; uint64_t counter; char pad2[120]; } cnt;
static GFn cnt_f;
static pthread_barrier_t cnt_start;

static void *cnt_thread(void *arg)
{
    (void)arg;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.ex_addr = ~0ull;
    c.x[0] = (uint64_t)(uintptr_t)&cnt.lock;
    c.x[5] = (uint64_t)(uintptr_t)&cnt.counter;
    c.x[6] = (uint64_t)g_iters;
    pthread_barrier_wait(&cnt_start);
    cnt_f(&c);
    return NULL;
}
static void run_counter(const char *name, GFn f, int width32)
{
    cnt.lock = 0;
    cnt.counter = 0;
    cnt_f = f;
    g_stx_fail = 0;
    pthread_t t[64];
    pthread_barrier_init(&cnt_start, NULL, (unsigned)g_threads);
    for (int i = 0; i < g_threads; i++) pthread_create(&t[i], NULL, cnt_thread, NULL);
    for (int i = 0; i < g_threads; i++) pthread_join(t[i], NULL);
    pthread_barrier_destroy(&cnt_start);
    uint64_t want = (uint64_t)g_iters * (uint64_t)g_threads, got = width32 ? (uint32_t)cnt.counter : cnt.counter;
    char what[160];
    snprintf(what, sizeof what, "lost updates: counter %llu of %llu (%d threads x %ld; %llu failed stxr)",
             (unsigned long long)got, (unsigned long long)want, g_threads, g_iters, (unsigned long long)g_stx_fail);
    result(name, (long)(want - got), 1, what);
}

static void run_clrex(void)
{
    Ctx c;
    uint64_t word = 0x1234;
    memset(&c, 0, sizeof c);
    c.x[0] = (uint64_t)(uintptr_t)&word;
    c.x[2] = c.x[3] = 77;
    g_clrex_check(&c);
    long bad = (c.x[2] != 1) + (c.x[3] != 0) + (word != 0x1234);
    result("clrex: stxr after clrex fails, next pair ok", bad, 1, "wrong results");
}

int main(int argc, char **argv)
{
    if (argc > 1) g_iters = atol(argv[1]);
    if (argc > 2) g_threads = atoi(argv[2]);
    if (g_threads < 2 || g_threads > 64) g_threads = 8;
    mp_data = calloc(MP_SLOTS, sizeof *mp_data);
    mp_flag = calloc(MP_SLOTS, sizeof *mp_flag);
    sb_x = calloc(1024, sizeof *sb_x);
    sb_y = calloc(1024, sizeof *sb_y);
    sb_r0 = calloc((size_t)g_iters, sizeof *sb_r0);
    sb_r1 = calloc((size_t)g_iters, sizeof *sb_r1);
    if (!mp_data || !mp_flag || !sb_x || !sb_y || !sb_r0 || !sb_r1) { puts("out of memory"); return 2; }

    run_clrex();
    run_mp("MP  stlr / ldar", g_mp_rel_writer, g_mp_acq_reader, 1);
    run_mp("MP  dmb ishst / dmb ishld", g_mp_fence_writer, g_mp_fence_reader, 1);
    run_mp("MP  plain str / ldr", g_mp_plain_writer, g_mp_plain_reader, 0);
    run_sb("SB  str; dmb ish; ldr", g_sb_fence, 1);
    run_sb("SB  str; dsb sy; ldr", g_sb_dsb, 1);
    run_sb("SB  stlr; ldar", g_sb_rel_acq, 1);
    run_sb("SB  plain str; ldr", g_sb_plain, 0);
    run_counter("lock ldaxr/stxr .. stlr (32-bit)", g_lock_inc_a, 0);
    run_counter("lock ldxr/stxr+dmb ish .. ldxr/stlxr", g_lock_inc_b, 0);
    run_counter("lock ldaxrb/stxrb .. stlrb (8-bit)", g_lock_inc_byte, 0);
    run_counter("fetch-add ldaxr/stlxr (64-bit)", g_atomic_inc_sc, 0);
    run_counter("fetch-add ldxr/stxr (32-bit)", g_atomic_inc_relaxed, 1);
    printf("%s\n", g_failed ? "LITMUS FAILED" : "LITMUS PASSED");
    return g_failed;
}
