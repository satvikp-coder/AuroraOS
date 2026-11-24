/*
 * kernel/mm/page_table.c — ARM64 stage-1 page table management
 *
 * Builds an identity-mapped page table for the kernel and RAM region,
 * plus a device mapping for the PL011 UART.  Uses a 4 KiB translation
 * granule with a 4-level table (L0..L3).
 *
 * Target: QEMU ARM64 "virt", Cortex-A53, EL2 (stage-1 via EL2 regs).
 */

#include <stddef.h>
#include <stdint.h>

#include <mm/mm.h>
#include <aarch64/mmu.h>

/* --- Page table storage ----------------------------------------------
 *
 * Allocated dynamically from the physical page allocator.  We need:
 *   L0 table: 1 page (covers entire 48-bit VA space)
 *   L1 table: 1 page
 *   L2 table: 1 page (covers 2 GB, enough for our 256 MB RAM + MMIO)
 *   L3 table: 1 page (for 4 KiB page mappings, e.g. UART)
 *
 * Total: 4 pages, allocated at init time.
 */

static uintptr_t pg_l0; /* physical address of L0 table */
static uintptr_t pg_l1; /* physical address of L1 table */
static uintptr_t pg_l2; /* physical address of L2 table */
static uintptr_t pg_l3; /* physical address of L3 table */

/* --- Helper: write a 64-bit page table entry ------------------------- */

static void pte_write(uintptr_t *pte, uint64_t val)
{
    *pte = val;
    dsb_ish();
}

/* --- Recursive table walker ------------------------------------------
 *
 * Walks (creating intermediate tables as needed) to the L3 entry for
 * a given virtual address.  Returns the address of the PTE.
 */

static uintptr_t *table_walk(uintptr_t va, int alloc_missing)
{
    uint64_t *l0 = (uint64_t *)(uintptr_t)pg_l0;
    uint64_t l0e = l0[VA_L0_INDEX(va)];

    if (!(l0e & PTE_VALID)) {
        if (!alloc_missing)
            return NULL;
        uintptr_t new_page = aurora_alloc_page();
        if (!new_page)
            return NULL;
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++)
            ((uint64_t *)(uintptr_t)new_page)[i] = 0;
        pte_write(&l0[VA_L0_INDEX(va)],
                  new_page | PTE_VALID | PTE_TABLE);
        l0e = new_page | PTE_VALID | PTE_TABLE;
    }
    uint64_t *l1 = (uint64_t *)(uintptr_t)(l0e & ~0xFFFULL);
    uint64_t l1e = l1[VA_L1_INDEX(va)];

    if (!(l1e & PTE_VALID)) {
        if (!alloc_missing)
            return NULL;
        uintptr_t new_page = aurora_alloc_page();
        if (!new_page)
            return NULL;
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++)
            ((uint64_t *)(uintptr_t)new_page)[i] = 0;
        pte_write(&l1[VA_L1_INDEX(va)],
                  new_page | PTE_VALID | PTE_TABLE);
        l1e = new_page | PTE_VALID | PTE_TABLE;
    }
    uint64_t *l2 = (uint64_t *)(uintptr_t)(l1e & ~0xFFFULL);
    uint64_t l2e = l2[VA_L2_INDEX(va)];

    if (!(l2e & PTE_VALID)) {
        if (!alloc_missing)
            return NULL;
        uintptr_t new_page = aurora_alloc_page();
        if (!new_page)
            return NULL;
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++)
            ((uint64_t *)(uintptr_t)new_page)[i] = 0;
        pte_write(&l2[VA_L2_INDEX(va)],
                  new_page | PTE_VALID | PTE_TABLE);
        l2e = new_page | PTE_VALID | PTE_TABLE;
    }
    uint64_t *l3 = (uint64_t *)(uintptr_t)(l2e & ~0xFFFULL);
    return &l3[VA_L3_INDEX(va)];
}

/* --- Map a single 4 KiB page ---------------------------------------- */

int map_page(uintptr_t va, uintptr_t pa, uint64_t attr)
{
    uintptr_t *pte = table_walk(va, 1);
    if (!pte)
        return -1;
    *pte = pa | attr | PTE_VALID | PTE_AF;
    dsb_ish();
    isb();
    return 0;
}

/* --- Map a 2 MiB block at L2 --------------------------------------- */

int map_block(uintptr_t va, uintptr_t pa, uint64_t attr)
{
    uint64_t *l0 = (uint64_t *)(uintptr_t)pg_l0;
    uint64_t l0e = l0[VA_L0_INDEX(va)];

    if (!(l0e & PTE_VALID)) {
        uintptr_t new_page = aurora_alloc_page();
        if (!new_page)
            return -1;
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++)
            ((uint64_t *)(uintptr_t)new_page)[i] = 0;
        pte_write(&l0[VA_L0_INDEX(va)],
                  new_page | PTE_VALID | PTE_TABLE);
        l0e = new_page | PTE_VALID | PTE_TABLE;
    }
    uint64_t *l1 = (uint64_t *)(uintptr_t)(l0e & ~0xFFFULL);
    uint64_t l1e = l1[VA_L1_INDEX(va)];

    if (!(l1e & PTE_VALID)) {
        uintptr_t new_page = aurora_alloc_page();
        if (!new_page)
            return -1;
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++)
            ((uint64_t *)(uintptr_t)new_page)[i] = 0;
        pte_write(&l1[VA_L1_INDEX(va)],
                  new_page | PTE_VALID | PTE_TABLE);
        l1e = new_page | PTE_VALID | PTE_TABLE;
    }
    uint64_t *l2 = (uint64_t *)(uintptr_t)(l1e & ~0xFFFULL);
    l2[VA_L2_INDEX(va)] =
        pa | attr | PTE_VALID | PTE_AF | PTE_BLOCK;
    dsb_ish();
    isb();
    return 0;
}

/* --- Identity-map the kernel and RAM regions ----------------------- */

int mmu_map_memory(void)
{
    /* Map the entire 256 MiB RAM region using 2 MiB blocks at L2.
       Normal memory, cacheable, write-back. */
    uintptr_t va = AURORA_RAM_BASE;
    while (va < AURORA_RAM_END) {
        if (map_block(va, va,
                      PTE_ATTR_NORMAL_WB | PTE_SHARED | PTE_AF) < 0)
            return -1;
        va += L2_BLOCK_SIZE;
    }

    /* Map the PL011 UART MMIO region using 4 KiB pages at L3,
       device memory, non-cacheable. */
    uintptr_t uva = AURORA_UART_BASE;
    while (uva < AURORA_UART_BASE + AURORA_UART_SIZE) {
        if (map_page(uva, uva,
                     PTE_ATTR_DEVICE | PTE_SHARED | PTE_AF) < 0)
            return -1;
        uva += L3_PAGE_SIZE;
    }

    return 0;
}

/* --- Get the L0 table physical address (for TTBR0_EL2) ------------ */

uintptr_t mmu_get_l0_table(void)
{
    return pg_l0;
}

/* --- Allocate and zero all page table pages ----------------------- */

int mmu_alloc_tables(void)
{
    pg_l0 = aurora_alloc_page();
    pg_l1 = aurora_alloc_page();
    pg_l2 = aurora_alloc_page();
    pg_l3 = aurora_alloc_page();

    if (!pg_l0 || !pg_l1 || !pg_l2 || !pg_l3)
        return -1;

    for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        ((uint64_t *)(uintptr_t)pg_l0)[i] = 0;
        ((uint64_t *)(uintptr_t)pg_l1)[i] = 0;
        ((uint64_t *)(uintptr_t)pg_l2)[i] = 0;
        ((uint64_t *)(uintptr_t)pg_l3)[i] = 0;
    }

    /* Point L0[0] -> L1, L1[0] -> L2, L2[0] -> L3 */
    ((uint64_t *)(uintptr_t)pg_l0)[0] = pg_l1 | PTE_VALID | PTE_TABLE;
    ((uint64_t *)(uintptr_t)pg_l1)[0] = pg_l2 | PTE_VALID | PTE_TABLE;
    ((uint64_t *)(uintptr_t)pg_l2)[0] = pg_l3 | PTE_VALID | PTE_TABLE;

    dsb_ish();
    return 0;
}
