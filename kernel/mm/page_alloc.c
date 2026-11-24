/*
 * kernel/mm/page_alloc.c — Physical page allocator
 *
 * Bitmap-based allocator managing 4 KiB physical pages.  Each bit in
 * the bitmap represents one page: 1 = allocated, 0 = free.
 *
 * Reserved regions (kernel code, rodata, data, BSS, stack, page tables,
 * boot info, MMIO, out-of-bounds) are marked allocated at init time and
 * can never be handed out.
 *
 * Target: QEMU ARM64 "virt", 256 MiB RAM at 0x40000000.
 */

#include <stddef.h>
#include <stdint.h>

#include <mm/mm.h>

/* --- Bitmap layout ----------------------------------------------------
 *
 * Total pages in RAM: AURORA_RAM_SIZE / AURORA_PAGE_SIZE
 * Bitmap bits:      one per page
 * Bitmap bytes:     total_pages / 8
 * Bitmap pages:     ceil(bitmap_bytes / PAGE_SIZE)
 *
 * The bitmap itself lives in reserved memory, right after the kernel
 * image end (__kernel_end).
 */

#define TOTAL_PAGES  (AURORA_RAM_SIZE / AURORA_PAGE_SIZE)
#define BITMAP_BYTES ((TOTAL_PAGES + 7) / 8)
#define BITMAP_PAGES ((BITMAP_BYTES + AURORA_PAGE_SIZE - 1) / AURORA_PAGE_SIZE)

/* Bitmap storage starts right after the kernel image (__kernel_end). */
static inline uint8_t *bitmap_addr(void)
{
    return (uint8_t *)__kernel_end;
}

static unsigned long free_pages;  /* running count of free pages      */
static int initialized = 0;       /* allocator initialized flag       */

/* Helper: page index from physical address (relative to RAM base) */
static inline unsigned long pa_to_index(uintptr_t pa)
{
    return ((pa - AURORA_RAM_BASE) / AURORA_PAGE_SIZE);
}

/* Helper: physical address from page index */
static inline uintptr_t index_to_pa(unsigned long idx)
{
    return (AURORA_RAM_BASE + idx * AURORA_PAGE_SIZE);
}

/* Test/set/clear bitmap bits */
static inline int bitmap_test(uint8_t *bm, unsigned long idx)
{
    return (bm[idx >> 3] >> (idx & 7)) & 1u;
}

static inline void bitmap_set(uint8_t *bm, unsigned long idx)
{
    bm[idx >> 3] |= (1u << (idx & 7));
}

static inline void bitmap_clear(uint8_t *bm, unsigned long idx)
{
    bm[idx >> 3] &= ~(1u << (idx & 7));
}

/* --- Reserved range table --------------------------------------------
 *
 * Filled in aurora_mem_init() from linker symbols and fixed MMIO
 * addresses.  Each entry is {start, end} (page-aligned, end exclusive).
 */

static uintptr_t reserved[AURORA_RESERVED_COUNT * 2];
uintptr_t aurora_reserved_ranges[AURORA_RESERVED_COUNT * 2];

static void reserve_range(uintptr_t start, uintptr_t end)
{
    /* Align start up, end down to page boundaries */
    start = (start + AURORA_PAGE_SIZE - 1) & ~(AURORA_PAGE_SIZE - 1);
    end &= ~(AURORA_PAGE_SIZE - 1);
    if (end <= start)
        return;

    unsigned long idx = pa_to_index(start);
    unsigned long end_idx = pa_to_index(end);
    for (; idx < end_idx; idx++)
        bitmap_set(bitmap_addr(), idx);
}

static void init_reserved_ranges(void)
{
    /* Kernel image: from AURORA_KERNEL_LOAD to __kernel_end */
    reserve_range(AURORA_KERNEL_LOAD, (uintptr_t)__kernel_end);

    /* Bitmap storage itself */
    reserve_range((uintptr_t)bitmap_addr(),
                  (uintptr_t)bitmap_addr() + BITMAP_PAGES * AURORA_PAGE_SIZE);

    /* PL011 UART MMIO (device memory, not RAM allocatable) */
    reserve_range(AURORA_UART_BASE, AURORA_UART_BASE + AURORA_UART_SIZE);

    /* Copy into the public array */
    for (int i = 0; i < AURORA_RESERVED_COUNT * 2; i++)
        aurora_reserved_ranges[i] = reserved[i];
}

int aurora_is_reserved(uintptr_t addr)
{
    if (!initialized)
        return 0;
    unsigned long idx = pa_to_index(addr);
    return bitmap_test(bitmap_addr(), idx);
}

/* --- Allocator initialization ---------------------------------------- */

int aurora_mem_init(void)
{
    if (initialized)
        return 0;

    /* Mark every page allocated initially */
    for (size_t i = 0; i < BITMAP_BYTES; i++)
        bitmap_addr()[i] = 0xFF;

    /* Mark RAM pages below the kernel load address as reserved
       (they exist but are below our load address) */
    reserve_range(AURORA_RAM_BASE, AURORA_KERNEL_LOAD);

    /* Mark RAM pages above the kernel end as free */
    uintptr_t free_start = (uintptr_t)__kernel_end;
    uintptr_t free_end = AURORA_RAM_END;
    if (free_end > free_start) {
        unsigned long idx = pa_to_index(free_start);
        unsigned long end_idx = pa_to_index(free_end);
        for (; idx < end_idx; idx++)
            bitmap_clear(bitmap_addr(), idx);
    }

    init_reserved_ranges();

    /* Count free pages */
    free_pages = 0;
    for (unsigned long i = 0; i < TOTAL_PAGES; i++) {
        if (!bitmap_test(bitmap_addr(), i))
            free_pages++;
    }

    initialized = 1;
    return 0;
}

/* --- Page allocation ------------------------------------------------- */

uintptr_t aurora_alloc_page(void)
{
    if (!initialized)
        return 0;

    for (unsigned long i = 0; i < TOTAL_PAGES; i++) {
        if (!bitmap_test(bitmap_addr(), i)) {
            bitmap_set(bitmap_addr(), i);
            free_pages--;
            return index_to_pa(i);
        }
    }
    return 0; /* out of memory */
}

void aurora_free_page(uintptr_t addr)
{
    if (!initialized || addr == 0)
        return;

    if ((addr & (AURORA_PAGE_SIZE - 1)) != 0)
        return; /* not page-aligned */

    if (addr < AURORA_RAM_BASE || addr >= AURORA_RAM_END)
        return; /* out of RAM range */

    unsigned long idx = pa_to_index(addr);
    if (idx >= TOTAL_PAGES)
        return;

    if (!bitmap_test(bitmap_addr(), idx))
        return; /* already free — double free, ignore safely */

    bitmap_clear(bitmap_addr(), idx);
    free_pages++;
}

unsigned long aurora_free_page_count(void)
{
    return free_pages;
}

unsigned long aurora_total_page_count(void)
{
    return TOTAL_PAGES;
}
