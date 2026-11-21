/*
 * tests/kernel-host/uvlayout_test.c - the user address-space layout and its guards (kernel/include/uvlayout.h).
 * The expectations below are written as LITERAL numbers, independent of the header's constants, so a wrong constant cannot make both sides agree.
 */
#include <stdio.h>
#include <stdint.h>
#include "uvlayout.h"

static int fails = 0, total = 0;
static void check(const char *what, int ok) { total++; if (!ok) { fails++; printf("  FAIL: %s\n", what); } }

/* the independent oracle: which pages (offsets from 0x4000000000, in pages) may be mapped inside the 2 MiB process window */
static int oracle(uint64_t pageno)
{
    if (pageno == 0 || pageno == 1) return 1;                     /* code, data */
    if (pageno == 3 || pageno == 4) return 1;                     /* the two stack pages */
    if (pageno >= 0x10 && pageno < 0x100) return 1;               /* ELF window: +0x10000 .. +0x100000 */
    if (pageno >= 0x110 && pageno < 0x130) return 1;              /* heap: +0x110000 .. +0x130000 (32 pages) */
    return 0;                                                     /* everything else in the window is a guard */
}

int main(void)
{
    /* the literal layout */
    check("the heap starts at 0x4000110000", USER_HEAP_VA == 0x4000110000ULL);
    check("the heap ends at 0x4000130000 (32 pages)", USER_HEAP_END == 0x4000130000ULL && USER_HEAP_MAX_PAGES == 32);
    check("the ELF window is 0x4000010000..0x4000100000", ELF_VA_MIN == 0x4000010000ULL && ELF_VA_MAX == 0x4000100000ULL);
    check("the guard below the heap is 16 pages and starts exactly where the ELF window ends", USER_HEAP_VA - ELF_VA_MAX == 0x10000 && USER_HEAP_VA - ELF_VA_MAX == USER_HEAP_GUARD_BYTES);
    check("the guard above the heap is at least 16 pages inside the 2 MiB table", USER_VA_BASE + USER_WINDOW_BYTES - USER_HEAP_END >= 0x10000);

    /* every page in and around the window, against the oracle */
    int mism = 0, n = 0;
    for (uint64_t off = 0; off < USER_WINDOW_BYTES; off += 0x1000) {
        n++;
        if (uv_mappable(USER_VA_BASE + off) != oracle(off >> 12)) { mism++; printf("    mismatch at +0x%llx\n", (unsigned long long)off); }
    }
    check("all 512 pages of the process window agree with the independent oracle (code, data, stack, ELF window and heap mappable; every guard not)", n == 512 && mism == 0);
    check("the page just below the user region is never mappable", !uv_mappable(USER_VA_BASE - 0x1000));
    check("the page just above the window is not a window guard (the existing range rules decide)", uv_mappable(USER_VA_BASE + 0x200000));

    /* guards, named */
    check("the page between data and stack (+0x2000) is a guard", !uv_mappable(USER_VA_BASE + 0x2000));
    check("the page above the stack (+0x5000) is a guard", !uv_mappable(USER_VA_BASE + 0x5000));
    check("the page below the ELF window (+0xF000) is a guard", !uv_mappable(USER_VA_BASE + 0xF000));
    check("the first page past the ELF window (+0x100000) is a guard", !uv_mappable(USER_VA_BASE + 0x100000));
    check("the last page below the heap (+0x10F000) is a guard", !uv_mappable(USER_VA_BASE + 0x10F000));
    check("the first heap page (+0x110000) is mappable", uv_mappable(USER_VA_BASE + 0x110000));
    check("the last heap page (+0x12F000) is mappable", uv_mappable(USER_VA_BASE + 0x12F000));
    check("the first page past the heap (+0x130000) is a guard", !uv_mappable(USER_VA_BASE + 0x130000));
    check("the last page of the 2 MiB table (+0x1FF000) is a guard", !uv_mappable(USER_VA_BASE + 0x1FF000));

    /* hostile inputs never trap, wrap or become mappable below the region */
    const uint64_t odd[] = { 0, 1, USER_VA_BASE - 1, USER_VA_BASE - 0x1000, ~0ULL, ~0ULL - 0xFFF, 0x8000000000ULL, 0x7FFFFFF000ULL, 0x40000000ULL };
    int odd_bad = 0;
    for (unsigned i = 0; i < sizeof odd / sizeof *odd; i++) if (odd[i] < USER_VA_BASE && uv_mappable(odd[i])) odd_bad = 1;
    check("addresses below the user region are never mappable (0, kernel slots, the top of the address space)", !odd_bad);
    check("the top of the address space does not wrap into a mappable page for the window rules (it is left to the slot range check)", uv_mappable(~0ULL & ~0xFFFULL) == 1);

    /* the heap's own page addresses */
    check("heap page 0 is the heap start", uv_heap_page_va(0) == USER_HEAP_VA);
    check("heap page 31 is the last page below the heap end", uv_heap_page_va(31) == USER_HEAP_END - 0x1000);
    check("heap page 32 does not exist", uv_heap_page_va(32) == 0);
    check("a huge heap page number does not wrap into an address", uv_heap_page_va(~0ULL) == 0 && uv_heap_page_va(1ULL << 63) == 0 && uv_heap_page_va((1ULL << 52) + 5) == 0);
    int all_ok = 1;
    for (uint64_t i = 0; i < 32; i++) { uint64_t va = uv_heap_page_va(i); if (!va || !uv_mappable(va) || va < USER_HEAP_VA || va >= USER_HEAP_END) all_ok = 0; }
    check("every page the heap can own is inside the heap range and mappable", all_ok);
    int no_overlap = 1;
    for (uint64_t i = 0; i < 32; i++) { uint64_t va = uv_heap_page_va(i); if (va >= ELF_VA_MIN && va < ELF_VA_MAX) no_overlap = 0; if (va >= USER_STACK_VA && va < USER_STACK_TOP) no_overlap = 0; }
    check("no heap page lies in the ELF window or the stack", no_overlap);

    printf("uvlayout: %d passed, %d failed\n", total - fails, fails);
    return fails != 0;
}
