#ifndef AURORA_UVLAYOUT_H
#define AURORA_UVLAYOUT_H
#include <stdint.h>
#include <elf.h>

/*
 * The user virtual-address layout, in ONE place, as pure constants and one pure predicate (no kernel state: the host tests build this header alone).
 *
 * Every process sees the same layout (different physical pages). Inside the first 2 MiB of the user slot (the one L3 table every process has):
 *
 *   +0x000000  code     RX, 1 page            USER_CODE_VA
 *   +0x001000  data     RW, 1 page            USER_DATA_VA
 *   +0x002000  GUARD (never mapped)
 *   +0x003000  stack    RW, 2 pages           USER_STACK_VA .. USER_STACK_TOP (+0x5000)
 *   +0x005000  GUARD (never mapped)           up to the ELF load window
 *   +0x010000  ELF load window                ELF_VA_MIN .. ELF_VA_MAX (+0x100000): only the ELF loader's segments
 *   +0x100000  GUARD, 16 pages (never mapped) USER_HEAP_GUARD_BYTES
 *   +0x110000  heap     RW + XN, grown by SYS_MEM, at most USER_HEAP_MAX_PAGES pages     USER_HEAP_VA .. USER_HEAP_END
 *   +0x130000  GUARD (never mapped)           up to the end of the 2 MiB table
 *
 * A guard is not a constant: it is the ABSENCE of a mapping, and aspace_map() refuses to create one (uv_mappable below), whoever asks. Touching it from EL0
 * is a translation fault. The heap lives OUTSIDE the ELF window: an installed program (linked at ELF_VA_MIN) can never be sitting on the heap's pages.
 */
#define USER_VA_BASE   0x4000000000ULL          /* 256 GiB: L1 slot 256, disjoint from the kernel's slots 0-1 */
#define USER_VA_LIMIT  0x4040000000ULL          /* one 1 GiB slot */
#define USER_CODE_VA   (USER_VA_BASE + 0x0000)  /* RX, one page */
#define USER_DATA_VA   (USER_VA_BASE + 0x1000)  /* RW, one page of private data */
/* 0x2000 is an unmapped guard page */
#define USER_STACK_VA  (USER_VA_BASE + 0x3000)  /* RW, two pages */
#define USER_STACK_TOP (USER_VA_BASE + 0x5000)

#define USER_PAGE_BYTES        0x1000ULL
#define USER_WINDOW_BYTES      0x200000ULL      /* the 2 MiB covered by the process's single L3 table */
#define USER_HEAP_GUARD_BYTES  0x10000ULL       /* 16 pages of unmapped space on each side of the heap */
#define USER_HEAP_MAX_PAGES    32u              /* per-process budget: 128 KiB */
#define USER_HEAP_VA           (ELF_VA_MAX + USER_HEAP_GUARD_BYTES)
#define USER_HEAP_END          (USER_HEAP_VA + (uint64_t)USER_HEAP_MAX_PAGES * USER_PAGE_BYTES)   /* first address past the heap's last possible page */

/* Bounded allocations (MEM-03): placed above the heap guard, within the 2 MiB process window.
 * Each slot gets BOUNDED_MAX_PAGES pages (max 16 pages = 64 KiB).
 * Remaining space after heap guard: 2 MiB - 0x140000 = 0xC0000 = 768 KiB = 12 slots of 64 KiB.
 * Let's place them at a fixed base above the heap guard. */
#define BOUNDED_MAX_PAGES        16u  /* max pages per bounded allocation (64 KiB) */
#define USER_BOUNDED_VA_BASE     (USER_HEAP_END + USER_HEAP_GUARD_BYTES)
#define USER_BOUNDED_MAX_SLOTS   12  /* max slots that fit in remaining 2 MiB window */

/* The layout is a proof obligation: if anybody moves a constant so that two regions touch, the build fails. */
_Static_assert(USER_VA_BASE + 0x5000 + 0x1000 <= ELF_VA_MIN, "a guard must separate the stack from the ELF window");
_Static_assert(ELF_VA_MIN >= USER_VA_BASE && ELF_VA_MAX > ELF_VA_MIN, "the ELF window is inside the user slot");
_Static_assert(USER_HEAP_VA >= ELF_VA_MAX + USER_HEAP_GUARD_BYTES, "the heap starts a full guard above the ELF window");
_Static_assert(USER_HEAP_VA % USER_PAGE_BYTES == 0 && USER_HEAP_END > USER_HEAP_VA, "the heap is page aligned and non-empty");
_Static_assert(USER_HEAP_END + USER_HEAP_GUARD_BYTES <= USER_VA_BASE + USER_WINDOW_BYTES, "a full guard separates the heap from the end of the L3 table");
_Static_assert(USER_BOUNDED_VA_BASE + USER_BOUNDED_MAX_SLOTS * BOUNDED_MAX_PAGES * USER_PAGE_BYTES <= USER_VA_BASE + USER_WINDOW_BYTES, "bounded allocations fit in the 2 MiB process window");

/* May aspace_map() ever create a mapping at this page-aligned address?
 *   - inside the 2 MiB process window: ONLY the code, data, stack, ELF-window and heap pages above (so every guard stays unmapped);
 *   - elsewhere in the user slot: the existing range rules in aspace_map decide (this predicate says yes);
 *   - below the user slot: no.
 * Only comparisons against constants: no address arithmetic that could wrap, for any input. */
static inline int uv_mappable(uint64_t va)
{
    if (va < USER_VA_BASE) return 0;
    if (va >= USER_VA_BASE + USER_WINDOW_BYTES) return 1;
    if (va < USER_VA_BASE + 0x2000) return 1;                              /* code and data */
    if (va >= USER_STACK_VA && va < USER_STACK_TOP) return 1;              /* stack */
    if (va >= ELF_VA_MIN && va < ELF_VA_MAX) return 1;                     /* ELF segments */
    if (va >= USER_HEAP_VA && va < USER_HEAP_END) return 1;                /* heap */
    return 0;                                                              /* a guard */
}

/* The heap's page number i (0-based), or 0 if i is not a page the heap may ever own. */
static inline uint64_t uv_heap_page_va(uint64_t i)
{
    return i < USER_HEAP_MAX_PAGES ? USER_HEAP_VA + i * USER_PAGE_BYTES : 0;
}
#endif
