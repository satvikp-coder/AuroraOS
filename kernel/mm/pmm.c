/*
 * kernel/mm/pmm.c — bitmap physical page allocator (4 KiB pages).
 *
 * Two bitmaps over managed RAM (capped at PMM_MAX_RAM):
 *   used  - page is not available for allocation (allocated OR reserved)
 *   rsv   - page is permanently reserved (kernel image, DTB, ...)
 * A page that is used && !rsv is allocated. Frees are fully checked, so
 * misuse is reported rather than silently corrupting accounting.
 * Single core, interrupts masked: no locking yet (add one with SMP/IRQ use).
 *
 * Scalable ownership: bitmaps are dynamically allocated in pmm_init() based on
 * actual RAM size (npages), not the compile-time PMM_MAX_PAGES cap. This keeps
 * memory overhead proportional to physical RAM (2 bits per page = 1 KiB per GiB).
 *
 * The bitmap arrays are statically allocated in .bss (linker.ld) sized for
 * PMM_MAX_PAGES; pmm_init() uses only the first bm_words entries.
 */
#include <mm.h>
#include <proc.h>

/* Linker-defined bitmap arrays (sized for PMM_MAX_PAGES in linker.ld) */
extern uint64_t __pmm_used_bm_start[];
extern uint64_t __pmm_rsv_bm_start[];

static uint64_t *used_bm;
static uint64_t *rsv_bm;
static uintptr_t base;
static uint64_t npages, nfree, nrsv, hint, alloc_fail, free_err;
static uint64_t bm_words;   /* number of uint64_t words in each bitmap (ceil(npages/64)) */

#define TST(bm, i) (((bm)[(i) >> 6] >> ((i) & 63)) & 1ULL)
#define SET(bm, i) ((bm)[(i) >> 6] |= 1ULL << ((i) & 63))
#define CLR(bm, i) ((bm)[(i) >> 6] &= ~(1ULL << ((i) & 63)))

/* ---- Bounded large allocation region (MEM-03) ----------------------- */
/* Maximum number of bounded regions we can track. */
#define BOUNDED_MAX_REGIONS  64u

static struct bounded_region bounded_regions[BOUNDED_MAX_REGIONS];
static uint64_t bounded_alloc_fail = 0;
static uint64_t bounded_free_err = 0;

/* Initialize bounded allocator state. Called from pmm_init(). */
void pmm_bounded_init(void)
{
    for (uint32_t i = 0; i < BOUNDED_MAX_REGIONS; i++) {
        bounded_regions[i].in_use = 0;
    }
    bounded_alloc_fail = 0;
    bounded_free_err = 0;
}

/* Count pages currently allocated by a specific process in bounded regions. */
static uint32_t bounded_proc_pages(uint32_t pid)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < BOUNDED_MAX_REGIONS; i++) {
        if (bounded_regions[i].in_use && bounded_regions[i].pid == pid) {
            total += bounded_regions[i].pages;
        }
    }
    return total;
}

/* Count pages currently allocated by all processes with a given principal. */
static uint32_t bounded_principal_pages(uint32_t principal)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < BOUNDED_MAX_REGIONS; i++) {
        if (bounded_regions[i].in_use && bounded_regions[i].principal == principal) {
            total += bounded_regions[i].pages;
        }
    }
    return total;
}

/* Find a free slot in the bounded regions array. Returns index or BOUNDED_MAX_REGIONS if full. */
static uint32_t bounded_find_free_slot(void)
{
    for (uint32_t i = 0; i < BOUNDED_MAX_REGIONS; i++) {
        if (!bounded_regions[i].in_use) return i;
    }
    return BOUNDED_MAX_REGIONS;
}

/* Find a bounded region by physical address. Returns index or BOUNDED_MAX_REGIONS if not found. */
static uint32_t bounded_find_by_addr(uintptr_t addr)
{
    for (uint32_t i = 0; i < BOUNDED_MAX_REGIONS; i++) {
        if (bounded_regions[i].in_use && bounded_regions[i].pa == addr) {
            return i;
        }
    }
    return BOUNDED_MAX_REGIONS;
}

/* Allocate a contiguous run of free pages. Returns base physical address or 0. */
static uintptr_t pmm_alloc_contiguous(uint32_t pages)
{
    if (pages == 0 || pages > BOUNDED_MAX_PAGES) return 0;
    if (nfree < pages + BOUNDED_RESERVE_PAGES) return 0;

    uint64_t max_start = npages - pages;
    for (uint64_t i = 0; i <= max_start; i++) {
        uint8_t ok = 1;
        for (uint32_t k = 0; k < pages; k++) {
            if (TST(used_bm, i + k)) { ok = 0; break; }
        }
        if (ok) {
            for (uint32_t k = 0; k < pages; k++) {
                SET(used_bm, i + k);
            }
            nfree -= pages;
            uintptr_t pa = base + (i << PAGE_SHIFT);
            /* Zero the allocated pages. */
            volatile uint64_t *p = (volatile uint64_t *)pa;
            for (uint32_t k = 0; k < pages * (PAGE_SIZE / 8); k++) p[k] = 0;
            return pa;
        }
    }
    bounded_alloc_fail++;
    return 0;
}

/* Free a contiguous run of pages. Returns BOUNDED_OK or error code. */
static int pmm_free_contiguous(uintptr_t addr, uint32_t pages)
{
    if (addr & (PAGE_SIZE - 1)) { bounded_free_err++; return BOUNDED_ERR_ALIGN; }
    if (!pmm_in_ram(addr))      { bounded_free_err++; return BOUNDED_ERR_RANGE; }
    uint64_t i = (addr - base) >> PAGE_SHIFT;
    if (i + pages > npages)     { bounded_free_err++; return BOUNDED_ERR_RANGE; }

    for (uint32_t k = 0; k < pages; k++) {
        uint64_t idx = i + k;
        if (TST(rsv_bm, idx))    { bounded_free_err++; return BOUNDED_ERR_RESERVED; }
        if (!TST(used_bm, idx))  { bounded_free_err++; return BOUNDED_ERR_DOUBLE; }
        CLR(used_bm, idx);
    }
    nfree += pages;
    return BOUNDED_OK;
}

int pmm_init(uintptr_t ram_base, uint64_t ram_size)
{
    if ((ram_base & (PAGE_SIZE - 1)) || ram_size < PAGE_SIZE) return -1;
    if (ram_size > PMM_MAX_RAM) ram_size = PMM_MAX_RAM;

    npages = ram_size >> PAGE_SHIFT;
    bm_words = (npages + 63) >> 6;   /* ceil(npages/64) */

    /* Allocate bitmaps from the kernel's early static buffer (before pmm is up).
     * The kernel image reserves space for PMM_MAX_PAGES/64 words per bitmap in .bss;
     * we only use the first bm_words entries. This avoids a chicken-and-egg allocator
     * while keeping runtime overhead proportional to actual RAM. */
    used_bm = (uint64_t *)(uintptr_t)__pmm_used_bm_start;
    rsv_bm  = (uint64_t *)(uintptr_t)__pmm_rsv_bm_start;

    for (uint64_t i = 0; i < bm_words; i++) used_bm[i] = rsv_bm[i] = 0;

    base = ram_base;
    nfree = npages; nrsv = 0; hint = 0; alloc_fail = free_err = 0;

    /* Initialize bounded allocation region. */
    pmm_bounded_init();

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

/* ---- Bounded large allocation region (MEM-03) public API ------------ */

uintptr_t pmm_alloc_bounded(uint64_t size, uint32_t principal, uint32_t pid, int perm, uint64_t *va_out)
{
    if (!base || size == 0 || size > BOUNDED_MAX_BYTES) return 0;
    if (perm != VMM_ATTR_NORMAL_RW && perm != VMM_ATTR_NORMAL_RO) return 0;

    /* Round up to pages. */
    uint32_t pages = (uint32_t)((size + PAGE_SIZE - 1) >> PAGE_SHIFT);
    if (pages > BOUNDED_MAX_PAGES) return 0;

    /* Check per-process budget (defense in depth: caller also checks). */
    if (pid != 0) {
        uint32_t proc_pages = bounded_proc_pages(pid);
        if (proc_pages + pages > BOUNDED_PROC_BUDGET_PAGES) {
            bounded_alloc_fail++;
            return 0;
        }
    }

    /* Check per-principal budget. */
    uint32_t principal_pages = bounded_principal_pages(principal);
    if (principal_pages + pages > BOUNDED_PRINCIPAL_BUDGET_PAGES) {
        bounded_alloc_fail++;
        return 0;
    }

    /* Check global reserve. */
    struct pmm_stats st; pmm_get_stats(&st);
    if (st.free_pages < pages + BOUNDED_RESERVE_PAGES) {
        bounded_alloc_fail++;
        return 0;
    }

    /* Find a free slot. */
    uint32_t slot = bounded_find_free_slot();
    if (slot >= BOUNDED_MAX_REGIONS) {
        bounded_alloc_fail++;
        return 0;
    }

    /* Allocate contiguous pages. */
    uintptr_t pa = pmm_alloc_contiguous(pages);
    if (!pa) return 0;

    /* Record the allocation. */
    bounded_regions[slot].pa = pa;
    bounded_regions[slot].va = va_out ? *va_out : 0;
    bounded_regions[slot].pages = pages;
    bounded_regions[slot].perm = (uint32_t)perm;
    bounded_regions[slot].principal = principal;
    bounded_regions[slot].pid = pid;
    bounded_regions[slot].in_use = 1;

    return pa;
}

int pmm_free_bounded(uintptr_t addr, uint32_t caller_principal)
{
    if (!base) return BOUNDED_ERR_STATE;

    uint32_t slot = bounded_find_by_addr(addr);
    if (slot >= BOUNDED_MAX_REGIONS) return BOUNDED_ERR_NOT_FOUND;

    struct bounded_region *r = &bounded_regions[slot];

    /* Permission check: kernel (principal 0) can free anything; otherwise
     * the caller must match the owning principal. */
    if (caller_principal != 0 && caller_principal != r->principal) {
        bounded_free_err++;
        return BOUNDED_ERR_PERM;
    }

    /* Free the physical pages. */
    int rc = pmm_free_contiguous(r->pa, r->pages);
    if (rc != BOUNDED_OK) return rc;

    /* Clear the region entry. */
    r->in_use = 0;
    r->pa = 0;
    r->pages = 0;
    r->perm = 0;
    r->principal = 0;
    r->pid = 0;

    return BOUNDED_OK;
}

uint64_t pmm_region_budget(uint32_t principal)
{
    if (!base) return 0;

    uint32_t used = bounded_principal_pages(principal);
    if (used >= BOUNDED_PRINCIPAL_BUDGET_PAGES) return 0;
    return BOUNDED_PRINCIPAL_BUDGET_PAGES - used;
}

int pmm_bounded_lookup(uintptr_t addr, struct bounded_region *out)
{
    if (!base || !out) return 0;

    uint32_t slot = bounded_find_by_addr(addr);
    if (slot >= BOUNDED_MAX_REGIONS) return 0;

    *out = bounded_regions[slot];
    return 1;
}

void pmm_bounded_cleanup_proc(uint32_t pid, struct aspace *as)
{
    if (!base || pid == 0) return;

    for (uint32_t i = 0; i < BOUNDED_MAX_REGIONS; i++) {
        if (bounded_regions[i].in_use && bounded_regions[i].pid == pid) {
            /* Unmap from process address space if VA is tracked and aspace provided. */
            if (as && bounded_regions[i].va != 0) {
                for (uint32_t p = 0; p < bounded_regions[i].pages; p++) {
                    (void)aspace_unmap(as, bounded_regions[i].va + p * PAGE_SIZE);
                }
            }
            /* Free the physical pages without permission check (kernel cleanup). */
            (void)pmm_free_contiguous(bounded_regions[i].pa, bounded_regions[i].pages);
            bounded_regions[i].in_use = 0;
            bounded_regions[i].pa = 0;
            bounded_regions[i].va = 0;
            bounded_regions[i].pages = 0;
            bounded_regions[i].perm = 0;
            bounded_regions[i].principal = 0;
            bounded_regions[i].pid = 0;
        }
    }
}

uint32_t pmm_bounded_proc_pages(uint32_t pid)
{
    if (!base || pid == 0) return 0;
    return bounded_proc_pages(pid);
}
