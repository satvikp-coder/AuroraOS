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
 */
#include <mm.h>
#include <proc.h>
#include <pte.h>

#define IDX1(v) (((v) >> 30) & 0x1FF)
#define IDX2(v) (((v) >> 21) & 0x1FF)
#define IDX3(v) (((v) >> 12) & 0x1FF)

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

static uint64_t take_page(struct aspace *as)
{
    if (as->npages >= ASPACE_MAX_PAGES) return 0;
    uint64_t pa = pmm_alloc_page();                 /* zeroed */
    if (!pa) return 0;
    as->pages[as->npages++] = pa;
    return pa;
}

int aspace_create(struct aspace *as, uint32_t asid)
{
    if (!as || asid == 0 || asid > 255) return -1;
    as->l1_pa = 0; as->asid = asid; as->npages = 0; as->live = 0;
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
    if (as->npages + need > ASPACE_MAX_PAGES || st.free_pages < (uint64_t)need) return -4;

    if (!have_l2) { uint64_t t = take_page(as); if (!t) return -4; *e1 = t | PTE_TABLE | PTE_VALID; l2 = (uint64_t *)(uintptr_t)t; }
    uint64_t *e2 = &l2[IDX2(va)];
    if (!have_l3) { uint64_t t = take_page(as); if (!t) return -4; *e2 = t | PTE_TABLE | PTE_VALID; l3 = (uint64_t *)(uintptr_t)t; }
    uint64_t *e3 = &l3[IDX3(va)];

    uint64_t pa = take_page(as);
    if (!pa) return -4;
    if (!pmm_is_allocated(pa) || pmm_is_reserved(pa)) return -6;   /* user memory only from allocator pages */
    const uint8_t *src = (const uint8_t *)init; volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)pa;
    for (size_t i = 0; i < len; i++) d[i] = src[i];
    if (perm == UPERM_RX) __asm__ volatile ("dc cvau, %0\n dsb ish\n ic iallu\n dsb ish\n isb" :: "r"(pa) : "memory");   /* code written as data must be visible to instruction fetch */
    *e3 = pa | attr | PTE_TABLE | PTE_VALID;
    __asm__ volatile ("dsb ishst" ::: "memory");
    if (pa_out) *pa_out = pa;
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
    as->npages = 0; as->l1_pa = 0; as->live = 0;
}
