/* vmem.h - guest address space and Windows-style virtual memory for POSIX hosts.
 *
 * The guest window [VMEM_GUEST_LO, VMEM_GUEST_HI) = [0x71_0000_0000, 0x76_0000_0000) (GUEST 0x71, NRO 0x72,
 * IMPORT 0x73, HOSTFN 0x74, SDK 0x75) is one PROT_NONE MAP_NORESERVE mapping made at startup by vmem_init(). Inside
 * it, reservations / commits / protections / releases only change that mapping (MAP_FIXED over our own pages), so
 * nothing else in the process (SDL, Vulkan ICD, glibc) can ever land there.
 *
 * Every region made through this API (inside the window or anywhere else) is in a region table: page runs with
 * {allocation base, state, protection}. That gives release-without-size (VirtualFree(p, 0, MEM_RELEASE)) and a
 * VirtualQuery with Windows answers. Addresses outside the table are answered from /proc/self/maps.
 *
 * Protections are POSIX PROT_* bits here; the Win32 shims in plat.h (VirtualAlloc / VirtualFree / VirtualProtect /
 * VirtualQuery) translate PAGE_* and MEM_* and are implemented in vmem.c. All functions are thread-safe.
 */
#ifndef SSBU_VMEM_H
#define SSBU_VMEM_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VMEM_GUEST_LO 0x7100000000ull
#define VMEM_GUEST_HI 0x7600000000ull
#define VMEM_PAGE 4096ull
#define VMEM_GRANULE 0x10000ull          /* Windows allocation granularity: vmem_alloc bases are 64 KiB aligned */

enum { VMEM_STATE_FREE = 0, VMEM_STATE_RESERVED = 1, VMEM_STATE_COMMITTED = 2 };
enum { VMEM_TYPE_NONE = 0, VMEM_TYPE_PRIVATE = 1, VMEM_TYPE_MAPPED = 2, VMEM_TYPE_IMAGE = 3 };

typedef struct vmem_info {
    uint64_t base, size;                 /* from addr's page to the end of the run with the same state and protection
                                          * (Win32 VirtualQuery: BaseAddress is the page of addr) */
    uint64_t alloc_base;                 /* base of the allocation (reservation), 0 if free */
    int alloc_prot;                      /* protection given when the allocation was made */
    int state;                           /* VMEM_STATE_* */
    int prot;                            /* PROT_* of the run (0 for reserved/free) */
    int type;                            /* VMEM_TYPE_* */
    int tracked;                         /* 1: from the region table; 0: from /proc/self/maps or the window */
} vmem_info;

/* Reserve the guest window. Call first thing in main (before SDL / Vulkan load anything). Idempotent; every other
 * vmem function and the Virtual* shims call it on first use too. Fatal on failure: page size other than 4096, or the
 * window already occupied (prints the conflicting /proc/self/maps lines; re-execs itself once with
 * ADDR_NO_RANDOMIZE unless SSBU_VMEM_NO_REEXEC is set). */
void vmem_init(void);
/* Same without exiting or re-exec: 0, or -errno (-EEXIST: occupied, -EINVAL: page size). For tests. */
int vmem_try_init(void);
int vmem_in_guest(uint64_t addr, uint64_t size);   /* [addr, addr+size) inside the window */

/* prot argument meaning "reserve only, commit nothing" (prot 0 = PROT_NONE commits inaccessible pages). */
#define VMEM_RESERVE_ONLY (-1)
/* Inside the window: a new allocation at [addr, addr+size) (addr rounded down to 64 KiB, the end up to a page).
 * NULL (errno EEXIST) if any page of it is already allocated, EINVAL if not inside the window. */
void *vmem_reserve(uint64_t addr, size_t size, int prot);
/* Anywhere outside the window (mmap chooses, 64 KiB aligned; below 2^47 on a 48-bit kernel by default). */
void *vmem_alloc(size_t size, int prot);
/* Outside the window at a fixed address (MAP_FIXED_NOREPLACE): NULL (EEXIST) if anything is mapped there. */
void *vmem_alloc_at(uint64_t addr, size_t size, int prot);

/* Within a tracked allocation: commit (fresh zero pages for reserved pages; committed pages keep their contents and
 * get prot), decommit (pages dropped, range stays reserved), protect (all pages committed; old = prot of the first).
 * Return 0 or -errno (-EINVAL: not inside one tracked allocation / not committed). */
int vmem_commit(void *addr, size_t size, int prot);
int vmem_decommit(void *addr, size_t size);
int vmem_protect(void *addr, size_t size, int prot, int *old_prot);
/* Release a whole allocation by its base (no size). Inside the window the pages go back to the window's PROT_NONE
 * reservation; outside they are unmapped. 0 or -EINVAL. */
int vmem_release(void *base);

/* VirtualQuery replacement: 1 and *out filled, or 0 if addr is above the user address range. */
int vmem_query(const void *addr, vmem_info *out);
/* 1 if every byte of [addr, addr+len) can be read now: the region table first (window pages: committed and
 * PROT_READ), else a process_vm_readv probe of each page on our own pid (EFAULT = unreadable). len 0 => 1. */
int vmem_readable(const void *addr, size_t len);

/* Diagnostics: number of table runs, and a dump of the table. */
size_t vmem_region_count(void);
void vmem_dump(void *file /* FILE* */);

#ifdef __cplusplus
}
#endif
#endif /* SSBU_VMEM_H */
