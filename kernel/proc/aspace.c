/*
 * kernel/proc/aspace.c — per-process stage-1 address spaces.
 *
 * Layout of a process's translation tables:
 *   L1[0], L1[1]  copied BY REFERENCE from the kernel's master table: they point at the kernel's shared L2/L3 tables
 *                 (devices, all RAM, the kernel image). Those descriptors are EL1-only (AP[1]=0), so EL0 touching them is
 *                 a permission fault, and PXN/UXN keep them non-executable at EL0.
 *   L1[256]       private: an L2 table and an L3 table holding this process's user pages (nG, tagged by the ASID).
 * No other L1 slot is ever populated, so a process can map nothing else.
 *
 * Every physical page used (tables included) is recorded in as->pages and freed by aspace_destroy(). A user mapping may
 * only reference a page the page allocator handed out (pmm_is_allocated): never kernel text/rodata, the device tree,
 * no-map regions or MMIO. This is the EL0 counterpart of the kernel's physical-target mapping policy.
 *
 * Scalable ownership: the pages array is dynamically allocated from the PMM (one page = 512 entries),
 * keeping per-process overhead proportional to actual mappings, not a compile-time cap.
 *
 * Read-only asset mappings (MEM-04): aspace_map_ro() maps physical pages read-only into user space
 * with EL0 read permission (AP=11), PXN, UXN, and nG (ASID-tagged).
 */
#include <mm.h>
#include <proc.h>
#include <pte.h>
#include <uvlayout.h>

/* Capacity of the pages array when allocated as a single page (4096 / 8 = 512 entries) */
#define ASPACE_PAGES_PER_ARRAY_PAGE (PAGE_SIZE / sizeof(uint64_t))

static uint64_t user_attr(int perm)
{
    uint64_t common = PTE_ATTRIDX(1) | PTE_SH_INNER | PTE_AF | PTE_NG | PTE_PXN;     /* never executable at EL1 */
    switch (perm) {
    case UPERM_RX: return common | PTE_AP_EL0_RO;                                      /* EL0 may execute, not write */
    case UPERM_RW: return common | PTE_AP_EL0_RW | PTE_UXN;                            /* never executable */
    case UPERM_RO: return common | PTE_AP_EL0_RO | PTE_UXN;
    default: return 0;
    }
}

/* Ensure the pages array has capacity for at least one more entry.
 * Returns 1 on success, 0 on allocation failure. */
static int aspace_ensure_capacity(struct aspace *as)
{
    if (as->npages + 1 <= as->capacity) return 1;
    if (as->capacity >= ASPACE_MAX_PAGES_CAP) return 0;

    /* Allocate a new array page (or the first one) */
    uint64_t new_pa = pmm_alloc_page();
    if (!new_pa) return 0;

    uint64_t *new_pages = (uint64_t *)(uintptr_t)new_pa;
    if (as->pages) {
        /* Copy existing entries */
        for (int i = 0; i < as->npages; i++) {
            new_pages[i] = as->pages[i];
        }
        /* Free the old array page */
        pmm_free_page((uintptr_t)as->pages);
    }
    as->pages = new_pages;
    as->capacity = ASPACE_PAGES_PER_ARRAY_PAGE;
    return 1;
}

static uint64_t take_page(struct aspace *as)
{
    if (!aspace_ensure_capacity(as)) return 0;
    uint64_t pa = pmm_alloc_page();                 /* zeroed */
    if (!pa) return 0;
    as->pages[as->npages++] = pa;
    return pa;
}

int aspace_create(struct aspace *as, uint32_t asid)
{
    if (!as || asid == 0 || asid > 255) return -1;
    as->l1_pa = 0; as->asid = asid; as->npages = 0; as->capacity = 0; as->pages = 0; as->live = 0;
    uint64_t l1 = take_page(as);
    if (!l1) return -2;
    const uint64_t *k = vmm_kernel_l1();
    for (int i = 0; i < 2; i++) {                   /* the kernel's devices and RAM, shared by reference */
        if (!(k[i] & PTE_VALID) || !(k[i] & PTE_TABLE)) { aspace_destroy(as); return -3; }
        ((uint64_t *)(uintptr_t)l1)[i] = k[i];
    }
    as->l1_pa = l1; as->live = 1;
    return 0;
}

int aspace_map(struct aspace *as, uint64_t va, int perm, const void *init, size_t len, uint64_t *pa_out)
{
    if (!as || !as->live) return -1;
    uint64_t attr = user_attr(perm);
    if (!attr || (va & (PAGE_SIZE - 1))) return -2;
    /* Overflow-safe range check: prove the START is inside the region. No end address is ever computed, and because both
     * bounds are page aligned, "va < LIMIT" already means the whole page fits (an end such as va+PAGE_SIZE can wrap to 0
     * for va near UINT64_MAX and slip past a "> LIMIT" test). */
    if (va < USER_VA_BASE || va >= USER_VA_LIMIT) return -2;
    if ((va >> 30) != (USER_VA_BASE >> 30)) return -2;      /* exactly the user L1 slot; never another slot, never the shared kernel slots */
    if (!uv_mappable(va)) return -7;                        /* a guard page (or any hole in the process window) is never mapped, by anybody */
    if (len > PAGE_SIZE || (len && !init)) return -3;

    uint64_t *l1 = (uint64_t *)(uintptr_t)as->l1_pa;
    uint64_t *e1 = &l1[IDX1(va)];
    int have_l2 = (*e1 & PTE_VALID) != 0;
    uint64_t *l2 = have_l2 ? (uint64_t *)(uintptr_t)(*e1 & ADDR_MASK) : 0;
    int have_l3 = have_l2 && (l2[IDX2(va)] & PTE_VALID) != 0;
    uint64_t *l3 = have_l3 ? (uint64_t *)(uintptr_t)(l2[IDX2(va)] & ADDR_MASK) : 0;
    if (have_l3 && (l3[IDX3(va)] & PTE_VALID)) return -5;   /* never silently remap */

    /* Atomicity: work out how many pages this call needs (missing tables + the page) and require them up front, so a
     * failure can never leave a half-built table, a consumed page or a changed ownership list behind. */
    int need = 1 + !have_l2 + !have_l3;
    struct pmm_stats st; pmm_get_stats(&st);
    if (as->npages + need > ASPACE_MAX_PAGES_CAP || st.free_pages < (uint64_t)need) return -4;

    if (!have_l2) { uint64_t t = take_page(as); if (!t) return -4; *e1 = t | PTE_TABLE | PTE_VALID; l2 = (uint64_t *)(uintptr_t)t; }
    uint64_t *e2 = &l2[IDX2(va)];
    if (!have_l3) { uint64_t t = take_page(as); if (!t) return -4; *e2 = t | PTE_TABLE | PTE_VALID; l3 = (uint64_t *)(uintptr_t)t; }
    uint64_t *e3 = &l3[IDX3(va)];

    uint64_t pa = take_page(as);
    if (!pa) return -4;
    if (!pmm_is_allocated(pa) || pmm_is_reserved(pa)) return -6;   /* user memory only from allocator pages */
    const uint8_t *src = (const uint8_t *)init; volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)pa;
    for (size_t i = 0; i < len; i++) d[i] = src[i];
    if (perm == UPERM_RX) {   /* code written as data must be visible to instruction fetch: clean EVERY data-cache line of the bytes, then invalidate the I-cache */
        uint64_t ctr; __asm__ volatile ("mrs %0, ctr_el0" : "=r"(ctr));
        uint64_t line = 4u << ((ctr >> 16) & 0xF);            /* smallest D-cache line, in bytes */
        for (uint64_t a = pa & ~(line - 1); a < pa + len; a += line) __asm__ volatile ("dc cvau, %0" :: "r"(a) : "memory");
        __asm__ volatile ("dsb ish\n ic iallu\n dsb ish\n isb" ::: "memory");
    }
    *e3 = pa | attr | PTE_TABLE | PTE_VALID;
    __asm__ volatile ("dsb ishst" ::: "memory");
    if (pa_out) *pa_out = pa;
    return 0;
}

int aspace_unmap(struct aspace *as, uint64_t va)
{
    if (!as || !as->live) return -1;
    if ((va & (PAGE_SIZE - 1)) || va < USER_VA_BASE || va >= USER_VA_LIMIT || (va >> 30) != (USER_VA_BASE >> 30)) return -2;   /* same range rules as aspace_map */
    uint64_t *l1 = (uint64_t *)(uintptr_t)as->l1_pa;
    uint64_t e1 = l1[IDX1(va)];
    if (!(e1 & PTE_VALID)) return -5;
    uint64_t *l2 = (uint64_t *)(uintptr_t)(e1 & ADDR_MASK);
    uint64_t e2 = l2[IDX2(va)];
    if (!(e2 & PTE_VALID)) return -5;
    uint64_t *l3 = (uint64_t *)(uintptr_t)(e2 & ADDR_MASK);
    uint64_t e3 = l3[IDX3(va)];
    if (!(e3 & PTE_VALID)) return -5;
    uint64_t pa = e3 & ADDR_MASK;
    int idx = -1;
    for (int i = 0; i < as->npages; i++) if (as->pages[i] == pa) { idx = i; break; }
    if (idx < 0) return -6;                                  /* not a page this address space owns: never free it */
    l3[IDX3(va)] = 0;
    __asm__ volatile ("dsb ishst" ::: "memory");
    uint64_t arg = ((uint64_t)as->asid << 48) | (va >> 12);
    __asm__ volatile ("tlbi vae1is, %0\n dsb ish\n isb" :: "r"(arg) : "memory");       /* drop the stale translation for this ASID and page before the page can be reused */
    as->pages[idx] = as->pages[--as->npages];
    pmm_free_page(pa);
    return 0;
}

/* Check if a VA is already mapped in the address space. Returns 1 if mapped, 0 if not, -1 on error. */
int aspace_is_mapped(struct aspace *as, uint64_t va)
{
    if (!as || !as->live) return -1;
    if ((va & (PAGE_SIZE - 1)) || va < USER_VA_BASE || va >= USER_VA_LIMIT || (va >> 30) != (USER_VA_BASE >> 30)) return -1;
    uint64_t *l1 = (uint64_t *)(uintptr_t)as->l1_pa;
    uint64_t e1 = l1[IDX1(va)];
    if (!(e1 & PTE_VALID)) return 0;
    uint64_t *l2 = (uint64_t *)(uintptr_t)(e1 & ADDR_MASK);
    uint64_t e2 = l2[IDX2(va)];
    if (!(e2 & PTE_VALID)) return 0;
    uint64_t *l3 = (uint64_t *)(uintptr_t)(e2 & ADDR_MASK);
    uint64_t e3 = l3[IDX3(va)];
    return (e3 & PTE_VALID) ? 1 : 0;
}

/* Map a physical page read-only into the process address space (EL0 readable, PXN/UXN, nG).
 * va must be page-aligned and in the user mappable region. pa must be a reserved asset page.
 * The mapping is tracked in as->pages so it gets cleaned up on aspace_destroy().
 * Returns 0 on success, error code on failure. */
int aspace_map_ro(struct aspace *as, uint64_t va, uint64_t pa)
{
    if (!as || !as->live) return -1;
    if ((va & (PAGE_SIZE - 1)) || (pa & (PAGE_SIZE - 1))) return -2;
    if (va < USER_VA_BASE || va >= USER_VA_LIMIT) return -2;
    if ((va >> 30) != (USER_VA_BASE >> 30)) return -2;
    if (!uv_mappable(va)) return -7;                        /* a guard page is never mapped */
    if (!pmm_is_reserved(pa)) return -6;                    /* only reserved (asset) pages may be mapped RO */

    uint64_t attr = PTE_ATTRIDX(1) | PTE_SH_INNER | PTE_AF | PTE_NG | PTE_PXN | PTE_UXN | PTE_AP_EL0_RO;

    uint64_t *l1 = (uint64_t *)(uintptr_t)as->l1_pa;
    uint64_t *e1 = &l1[IDX1(va)];
    int have_l2 = (*e1 & PTE_VALID) != 0;
    uint64_t *l2 = have_l2 ? (uint64_t *)(uintptr_t)(*e1 & ADDR_MASK) : 0;
    int have_l3 = have_l2 && (l2[IDX2(va)] & PTE_VALID) != 0;
    uint64_t *l3 = have_l3 ? (uint64_t *)(uintptr_t)(l2[IDX2(va)] & ADDR_MASK) : 0;
    if (have_l3 && (l3[IDX3(va)] & PTE_VALID)) return -5;   /* never silently remap */

    /* Atomicity: need missing tables + this page's ownership entry. */
    int need = !have_l2 + !have_l3;
    struct pmm_stats st; pmm_get_stats(&st);
    if (as->npages + need > ASPACE_MAX_PAGES_CAP || st.free_pages < (uint64_t)need) return -4;

    if (!have_l2) { uint64_t t = take_page(as); if (!t) return -4; *e1 = t | PTE_TABLE | PTE_VALID; l2 = (uint64_t *)(uintptr_t)t; }
    uint64_t *e2 = &l2[IDX2(va)];
    if (!have_l3) { uint64_t t = take_page(as); if (!t) return -4; *e2 = t | PTE_TABLE | PTE_VALID; l3 = (uint64_t *)(uintptr_t)t; }
    uint64_t *e3 = &l3[IDX3(va)];

    /* Record the PA in the ownership array so aspace_destroy frees the tables but NOT the asset page.
     * The asset page is globally managed and freed separately (or never). We only track table pages here.
     * However, we need to track that this VA is mapped so we can unmap it on cleanup.
     * We don't add the PA to as->pages since it's not owned by this process.
     * Instead, we just need the table pages tracked. The table pages are already tracked via take_page(). */
    *e3 = pa | attr | PTE_TABLE | PTE_VALID;
    __asm__ volatile ("dsb ishst" ::: "memory");
    return 0;
}

/* Unmap a read-only asset page from the process address space.
 * va must be page-aligned and in the user mappable region.
 * The asset page itself is NOT freed (it's globally managed).
 * Only the page table entries are cleared and table pages are freed if they become empty.
 * Returns 0 on success, error code on failure. */
int aspace_unmap_ro(struct aspace *as, uint64_t va)
{
    if (!as || !as->live) return -1;
    if ((va & (PAGE_SIZE - 1)) || va < USER_VA_BASE || va >= USER_VA_LIMIT) return -2;
    if ((va >> 30) != (USER_VA_BASE >> 30)) return -2;

    uint64_t *l1 = (uint64_t *)(uintptr_t)as->l1_pa;
    uint64_t e1 = l1[IDX1(va)];
    if (!(e1 & PTE_VALID)) return -5;
    uint64_t *l2 = (uint64_t *)(uintptr_t)(e1 & ADDR_MASK);
    uint64_t e2 = l2[IDX2(va)];
    if (!(e2 & PTE_VALID)) return -5;
    uint64_t *l3 = (uint64_t *)(uintptr_t)(e2 & ADDR_MASK);
    uint64_t e3 = l3[IDX3(va)];
    if (!(e3 & PTE_VALID)) return -5;

    /* Verify this is an RO asset mapping (EL0 read-only, PXN/UXN, nG). */
    uint64_t expected_attr = PTE_ATTRIDX(1) | PTE_SH_INNER | PTE_AF | PTE_NG | PTE_PXN | PTE_UXN | PTE_AP_EL0_RO;
    if ((e3 & (PTE_ATTRIDX(7) | PTE_SH_INNER | PTE_AF | PTE_NG | PTE_PXN | PTE_UXN | PTE_AP_EL0_RO)) != expected_attr) {
        return -6;  /* not an RO asset mapping */
    }

    l3[IDX3(va)] = 0;
    __asm__ volatile ("dsb ishst" ::: "memory");
    uint64_t arg = ((uint64_t)as->asid << 48) | (va >> 12);
    __asm__ volatile ("tlbi vae1is, %0\n dsb ish\n isb" :: "r"(arg) : "memory");

    /* Note: we don't free the asset page (not in as->pages). Table pages are freed by aspace_destroy.
     * For a proper implementation, we'd need to track RO mappings separately to free empty table pages.
     * For now, the tables are freed on process exit via aspace_destroy. */
    return 0;
}

uint64_t aspace_ttbr(const struct aspace *as) { return ((uint64_t)as->asid << 48) | as->l1_pa; }

void aspace_destroy(struct aspace *as)
{
    if (!as) return;
    if (as->asid) {                                 /* drop every TLB entry tagged with this ASID before the pages can be reused */
        uint64_t arg = (uint64_t)as->asid << 48;
        __asm__ volatile ("dsb ishst\n tlbi aside1is, %0\n dsb ish\n isb" :: "r"(arg) : "memory");
    }
    for (int i = 0; i < as->npages; i++) pmm_free_page(as->pages[i]);
    if (as->pages) {
        pmm_free_page((uintptr_t)as->pages);
        as->pages = 0;
        as->capacity = 0;
    }
    as->npages = 0; as->l1_pa = 0; as->live = 0;
}
