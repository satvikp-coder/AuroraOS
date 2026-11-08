/*
 * kernel/mm/pmm.c — bitmap physical page allocator (4 KiB pages).
 *
 * Two bitmaps over managed RAM (capped at PMM_MAX_RAM):
 *   used  - page is not available for allocation (allocated OR reserved)
 *   rsv   - page is permanently reserved (kernel image, DTB, ...)
 * A page that is used && !rsv is allocated. Frees are fully checked, so
 * misuse is reported rather than silently corrupting accounting.
 * Single core, interrupts masked: no locking yet (add one with SMP/IRQ use).
 */
#include <mm.h>

static uint64_t used_bm[PMM_MAX_PAGES / 64];
static uint64_t rsv_bm[PMM_MAX_PAGES / 64];
static uintptr_t base;
static uint64_t npages, nfree, nrsv, hint, alloc_fail, free_err;

#define TST(bm, i) (((bm)[(i) >> 6] >> ((i) & 63)) & 1ULL)
#define SET(bm, i) ((bm)[(i) >> 6] |= 1ULL << ((i) & 63))
#define CLR(bm, i) ((bm)[(i) >> 6] &= ~(1ULL << ((i) & 63)))

int pmm_init(uintptr_t ram_base, uint64_t ram_size)
{
    if ((ram_base & (PAGE_SIZE - 1)) || ram_size < PAGE_SIZE) return -1;
    if (ram_size > PMM_MAX_RAM) ram_size = PMM_MAX_RAM;
    for (uint64_t i = 0; i < PMM_MAX_PAGES / 64; i++) used_bm[i] = rsv_bm[i] = 0;
    base = ram_base; npages = ram_size >> PAGE_SHIFT;
    nfree = npages; nrsv = 0; hint = 0; alloc_fail = free_err = 0;
    return 0;
}

int pmm_in_ram(uintptr_t a) { return a >= base && a < base + (npages << PAGE_SHIFT); }
uintptr_t pmm_ram_base(void) { return base; }
uintptr_t pmm_ram_end(void) { return base + (npages << PAGE_SHIFT); }

int pmm_reserve(uintptr_t start, uintptr_t end)
{
    if (end <= start) return -1;
    uintptr_t ram_end = pmm_ram_end();
    if (start >= ram_end || end <= base) return 0;       /* entirely outside managed RAM: nothing to reserve */
    if (start < base) start = base;                      /* partially outside: clamp BEFORE rounding (no overflow) */
    if (end > ram_end) end = ram_end;
    uintptr_t s = start & ~(PAGE_SIZE - 1), e = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uintptr_t a = s; a < e; a += PAGE_SIZE) {
        uint64_t i = (a - base) >> PAGE_SHIFT;
        if (TST(rsv_bm, i)) continue;                    /* overlapping reservations are fine */
        if (TST(used_bm, i)) return -2;                  /* already allocated: cannot retroactively reserve */
        SET(used_bm, i); SET(rsv_bm, i); nfree--; nrsv++;
    }
    return 0;
}

/* Overflow-checked variant for untrusted (device-tree) input. */
int pmm_reserve_range(uint64_t addr, uint64_t size)
{
    if (size == 0 || addr + size < addr) return -3;
    return pmm_reserve((uintptr_t)addr, (uintptr_t)(addr + size));
}

uintptr_t pmm_alloc_page(void)
{
    for (uint64_t n = 0; n < npages; n++) {
        uint64_t i = hint + n; if (i >= npages) i -= npages;
        /* Skip whole fully-used 64-page words quickly when aligned. */
        if ((i & 63) == 0 && i + 64 <= npages && used_bm[i >> 6] == ~0ULL) { n += 63; continue; }
        if (!TST(used_bm, i)) {
            SET(used_bm, i); nfree--; hint = i + 1;
            volatile uint64_t *p = (volatile uint64_t *)(base + (i << PAGE_SHIFT));
            for (uint64_t k = 0; k < PAGE_SIZE / 8; k++) p[k] = 0;
            return base + (i << PAGE_SHIFT);
        }
    }
    alloc_fail++;
    return 0;
}

int pmm_free_page(uintptr_t addr)
{
    if (addr & (PAGE_SIZE - 1)) { free_err++; return PMM_ERR_ALIGN; }
    if (!pmm_in_ram(addr))      { free_err++; return PMM_ERR_RANGE; }
    uint64_t i = (addr - base) >> PAGE_SHIFT;
    if (TST(rsv_bm, i))         { free_err++; return PMM_ERR_RESERVED; }
    if (!TST(used_bm, i))       { free_err++; return PMM_ERR_DOUBLE; }
    CLR(used_bm, i); nfree++;
    if (i < hint) hint = i;
    return PMM_OK;
}

int pmm_is_reserved(uintptr_t a)  { return pmm_in_ram(a) && TST(rsv_bm, (a - base) >> PAGE_SHIFT); }
int pmm_is_allocated(uintptr_t a) { if (!pmm_in_ram(a)) return 0; uint64_t i = (a - base) >> PAGE_SHIFT; return TST(used_bm, i) && !TST(rsv_bm, i); }

void pmm_get_stats(struct pmm_stats *s)
{
    s->total_pages = npages; s->free_pages = nfree; s->reserved_pages = nrsv;
    s->allocated_pages = npages - nfree - nrsv; s->alloc_failures = alloc_fail; s->free_errors = free_err;
}
