/*
 * kernel/mm/vmm.c — AArch64 stage-1 translation at EL1.
 *
 * Configuration (ARM DDI 0487, D8 "The AArch64 Virtual Memory System Architecture"; see
 * docs/kernel/mmu-configuration.md for the field-by-field review): 4 KiB granule, 39-bit
 * virtual addresses (T0SZ=25) so the walk starts at level 1 (512 x 1 GiB entries), TTBR0
 * only (EPD1=1 disables TTBR1 walks). MAIR_EL1 attr0 = Device-nGnRnE, attr1 = Normal
 * Write-Back. SCTLR_EL1.WXN makes writable memory non-executable regardless of the tables.
 *
 * Identity map: UART and GIC windows as Device; RAM as Normal with per-section permissions
 * (text RX, rodata RO, data/bss/stack RW+NX), the stack guard page and any device-tree
 * `no-map` regions left unmapped. Tables come from the page allocator.
 *
 * vmm_map() is all-or-nothing: the whole request is validated (arguments, ranges, conflicts
 * with existing mappings, and that enough free pages exist for every page table it could
 * need) BEFORE any descriptor is written. A failed call changes nothing.
 *
 * Every mapping (including the boot identity map) is checked against the physical-target policy in
 * mmpolicy.c: no writable alias of kernel text/rodata, no executable memory outside text, no mapping of
 * device-tree no-map memory, Normal attributes only on RAM, Device attributes only inside registered
 * MMIO windows. There is deliberately no bypass flag; a driver registers its window with
 * vmm_register_mmio() first.
 */
#include <mm.h>
#include <mmpolicy.h>

#define PTE_VALID    (1ULL << 0)
#define PTE_TABLE    (1ULL << 1)            /* level 1/2: table descriptor; level 3: page */
#define PTE_ATTRIDX(n) ((uint64_t)(n) << 2)
#define PTE_AP_RO    (1ULL << 7)            /* AP[2]=1: read-only at EL1 (AP[1]=0: no EL0 access) */
#define PTE_SH_OUTER (2ULL << 8)
#define PTE_SH_INNER (3ULL << 8)
#define PTE_AF       (1ULL << 10)
#define PTE_PXN      (1ULL << 53)
#define PTE_UXN      (1ULL << 54)
#define ADDR_MASK    0x0000FFFFFFFFF000ULL

#define MAIR_VALUE   ((0x00ULL << 0) | (0xFFULL << 8))   /* idx0 Device-nGnRnE, idx1 Normal WB RA/WA */
#define VA_BITS      39
#define BLOCK_2M     0x200000ULL

static uint64_t *l1;
static struct mm_policy policy;
static int last_denial;

struct hole { uint64_t start, end; };
static struct hole nomap[FDT_MAX_RESERVED];
static int nnomap;

static uint64_t attr_bits(int attr)
{
    switch (attr) {
    case VMM_ATTR_NORMAL_RW: return PTE_ATTRIDX(1) | PTE_SH_INNER | PTE_AF | PTE_PXN | PTE_UXN;
    case VMM_ATTR_NORMAL_RO: return PTE_ATTRIDX(1) | PTE_SH_INNER | PTE_AF | PTE_PXN | PTE_UXN | PTE_AP_RO;
    case VMM_ATTR_NORMAL_RX: return PTE_ATTRIDX(1) | PTE_SH_INNER | PTE_AF | PTE_UXN | PTE_AP_RO;
    case VMM_ATTR_DEVICE:    return PTE_ATTRIDX(0) | PTE_SH_OUTER | PTE_AF | PTE_PXN | PTE_UXN;
    default: return 0;
    }
}

/* Physical address width implemented (ID_AA64MMFR0_EL1.PARange). */
int vmm_pa_bits(void)
{
    static const int bits[] = { 32, 36, 40, 42, 44, 48, 52 };
    uint64_t v; __asm__ volatile ("mrs %0, id_aa64mmfr0_el1" : "=r"(v));
    uint64_t r = v & 0xF;
    return r < 7 ? bits[r] : 0;
}

#define IDX1(v) (((v) >> 30) & 0x1FF)
#define IDX2(v) (((v) >> 21) & 0x1FF)
#define IDX3(v) (((v) >> 12) & 0x1FF)

/*
 * Validate a mapping request without modifying anything. On success returns VMM_OK and an upper
 * bound on the number of page-table pages it may need in *tables_needed.
 */
static int preflight(uint64_t va, uint64_t pa, uint64_t size, uint64_t *tables_needed)
{
    uint64_t need = 0, end = va + size, v = va;
    while (v < end) {
        uint64_t e1 = l1[IDX1(v)];
        uint64_t slot_end = (v | 0x3FFFFFFFULL) + 1;
        uint64_t step_end = slot_end < end ? slot_end : end;
        if (!(e1 & PTE_VALID)) {                       /* empty 1 GiB slot: needs an L2 table + up to one L3 per 2 MiB touched */
            need += 1 + (((step_end - 1) >> 21) - (v >> 21) + 1);
            v = step_end; continue;
        }
        if (!(e1 & PTE_TABLE)) return VMM_ERR_CONFLICT; /* level-1 block: refuse to split */
        uint64_t *l2 = (uint64_t *)(uintptr_t)(e1 & ADDR_MASK);
        uint64_t e2 = l2[IDX2(v)];
        uint64_t sub_end = (v | (BLOCK_2M - 1)) + 1;
        if (sub_end > end) sub_end = end;
        if (!(e2 & PTE_VALID)) { need += 1; v = sub_end; continue; }   /* empty 2 MiB entry: block or one L3 table */
        if (!(e2 & PTE_TABLE)) return VMM_ERR_CONFLICT; /* existing 2 MiB block overlaps the request */
        uint64_t *l3 = (uint64_t *)(uintptr_t)(e2 & ADDR_MASK);
        for (; v < sub_end; v += PAGE_SIZE)
            if (l3[IDX3(v)] & PTE_VALID) return VMM_ERR_CONFLICT;   /* never silently remap */
    }
    (void)pa;
    *tables_needed = need;
    return VMM_OK;
}

/* Return the next-level table for entry *e, allocating it if empty. NULL on failure or if *e is a block. */
static uint64_t *next_table(uint64_t *e)
{
    if (*e & PTE_VALID) {
        if (!(*e & PTE_TABLE)) return 0;
        return (uint64_t *)(uintptr_t)(*e & ADDR_MASK);
    }
    uintptr_t t = pmm_alloc_page();                          /* zeroed */
    if (!t) return 0;
    *e = (uint64_t)t | PTE_TABLE | PTE_VALID;
    return (uint64_t *)t;
}

int vmm_map(uint64_t va, uint64_t pa, uint64_t size, int attr)
{
    uint64_t bits = attr_bits(attr);
    if (!l1 || !bits || size == 0) return VMM_ERR_ARG;
    if ((va | pa | size) & (PAGE_SIZE - 1)) return VMM_ERR_ALIGN;
    if (va + size < va || va + size > (1ULL << VA_BITS)) return VMM_ERR_RANGE;
    int pab = vmm_pa_bits();
    if (pa + size < pa || (pab < 64 && pab > 0 && pa + size > (1ULL << pab))) return VMM_ERR_RANGE;

    /* Physical-target policy, before anything is allocated or written. */
    int pr = mm_policy_check(&policy, pa, size, attr);
    if (pr) { last_denial = pr; return VMM_ERR_POLICY; }

    /* Phase 1: validate everything; nothing is written yet. */
    uint64_t tables = 0;
    int rc = preflight(va, pa, size, &tables);
    if (rc) return rc;
    struct pmm_stats st; pmm_get_stats(&st);
    if (st.free_pages < tables) return VMM_ERR_NOMEM;       /* guarantees phase 2 cannot run out of tables */

    /* Phase 2: install descriptors. */
    uint64_t v = va, p = pa, left = size;
    while (left) {
        uint64_t *e1 = &l1[IDX1(v)];
        uint64_t *l2 = next_table(e1);
        if (!l2) return VMM_ERR_NOMEM;                       /* unreachable after phase 1; fail loudly if it ever happens */
        uint64_t *e2 = &l2[IDX2(v)];
        if (!(*e2 & PTE_VALID) && !((v | p) & (BLOCK_2M - 1)) && left >= BLOCK_2M) {
            *e2 = p | bits | PTE_VALID;                      /* 2 MiB block (bit1 clear) */
            v += BLOCK_2M; p += BLOCK_2M; left -= BLOCK_2M;
            continue;
        }
        uint64_t *l3 = next_table(e2);
        if (!l3) return VMM_ERR_NOMEM;
        l3[IDX3(v)] = p | bits | PTE_TABLE | PTE_VALID;      /* 4 KiB page (bit1 set at level 3) */
        v += PAGE_SIZE; p += PAGE_SIZE; left -= PAGE_SIZE;
    }
    /* Make the new descriptors visible to the table walker. Invalid->valid needs no TLB maintenance in
     * the architecture, but we invalidate anyway as a conservative measure once the MMU is on. */
    if (vmm_mmu_enabled()) {
        __asm__ volatile ("dsb ishst\n tlbi vmalle1is\n dsb ish\n isb" ::: "memory");
    } else {
        __asm__ volatile ("dsb sy" ::: "memory");
    }
    return VMM_OK;
}

int vmm_last_denial(void) { return last_denial; }

int vmm_register_mmio(uint64_t addr, uint64_t size)
{
    if ((addr | size) & (PAGE_SIZE - 1)) return VMM_ERR_ALIGN;
    return mm_policy_add_mmio(&policy, addr, size) ? VMM_ERR_POLICY : VMM_OK;
}

int vmm_add_nomap(uint64_t addr, uint64_t size)
{
    if (size == 0 || addr + size < addr || nnomap >= FDT_MAX_RESERVED) return VMM_ERR_ARG;
    nomap[nnomap].start = addr & ~(PAGE_SIZE - 1);
    nomap[nnomap].end = (addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (nomap[nnomap].end < nomap[nnomap].start) return VMM_ERR_RANGE;
    nnomap++;
    return VMM_OK;
}

static int map_ident(uintptr_t s, uintptr_t e, int attr)
{
    if (e <= s) return VMM_OK;
    return vmm_map(s, s, e - s, attr);
}

int vmm_init(void)
{
    uintptr_t t = pmm_alloc_page();
    if (!t) return VMM_ERR_NOMEM;
    l1 = (uint64_t *)t;

    /* Build the physical-target policy from the kernel layout and the device tree. */
    policy.nnomap = 0; policy.nmmio = 0;
    policy.ram.start = pmm_ram_base(); policy.ram.end = pmm_ram_end();
    policy.text.start = (uint64_t)(uintptr_t)__text_start; policy.text.end = (uint64_t)(uintptr_t)__text_end;
    policy.rodata.start = (uint64_t)(uintptr_t)__rodata_start; policy.rodata.end = (uint64_t)(uintptr_t)__rodata_end;
    for (int i = 0; i < nnomap; i++)
        if (mm_policy_add_nomap(&policy, nomap[i].start, nomap[i].end - nomap[i].start)) return VMM_ERR_POLICY;
    /* Only the console UART is a boot-time window. Other devices (the interrupt controller, ...) are
     * declared by their drivers via vmm_register_mmio() using addresses from the device tree. */
    if (vmm_register_mmio(0x09000000ULL, PAGE_SIZE)) return VMM_ERR_POLICY;

    /* Boot console: the PL011 UART page (QEMU virt). */
    if (vmm_map(0x09000000ULL, 0x09000000ULL, PAGE_SIZE, VMM_ATTR_DEVICE)) return -3;

    /* RAM, section by section. The stack guard page is intentionally not mapped. */
    uintptr_t rb = pmm_ram_base(), re = pmm_ram_end();
    for (int i = 0; i < nnomap; i++)                         /* no-map regions inside the boot region/image are unsupported */
        if (nomap[i].start < (uint64_t)(uintptr_t)__kernel_end && nomap[i].end > rb) return VMM_ERR_CONFLICT;
    if (map_ident(rb, (uintptr_t)__text_start, VMM_ATTR_NORMAL_RW)) return -4;
    if (map_ident((uintptr_t)__text_start, (uintptr_t)__text_end, VMM_ATTR_NORMAL_RX)) return -5;
    if (map_ident((uintptr_t)__rodata_start, (uintptr_t)__rodata_end, VMM_ATTR_NORMAL_RO)) return -6;
    if (map_ident((uintptr_t)__data_start, (uintptr_t)__data_end, VMM_ATTR_NORMAL_RW)) return -7;
    /* Kernel stack and exception stack; both guard pages (below each) stay unmapped. */
    if (map_ident((uintptr_t)__stack_bottom, (uintptr_t)__stack_top, VMM_ATTR_NORMAL_RW)) return -8;
    if (map_ident((uintptr_t)__exc_stack_bottom, (uintptr_t)__kernel_end, VMM_ATTR_NORMAL_RW)) return -8;

    /* The rest of RAM, skipping device-tree no-map holes (kept unmapped so nothing can touch them). */
    uintptr_t cur = (uintptr_t)__kernel_end;
    for (;;) {
        int best = -1;
        for (int i = 0; i < nnomap; i++) {
            if (nomap[i].end <= cur || nomap[i].start >= re) continue;
            if (best < 0 || nomap[i].start < nomap[best].start) best = i;
        }
        if (best < 0) { if (map_ident(cur, re, VMM_ATTR_NORMAL_RW)) return -9; break; }
        if (nomap[best].start > cur && map_ident(cur, (uintptr_t)nomap[best].start, VMM_ATTR_NORMAL_RW)) return -9;
        if (nomap[best].end > cur) cur = (uintptr_t)nomap[best].end;
        if (cur >= re) break;
    }
    return VMM_OK;
}

int vmm_mmu_enabled(void)
{
    uint64_t v; __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(v));
    return (int)(v & 1);
}

void vmm_enable(void)
{
    uint64_t mmfr0; __asm__ volatile ("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t ips = mmfr0 & 0xF; if (ips > 5) ips = 5;        /* PARange, capped at 48-bit */
    uint64_t tcr = (64 - VA_BITS)            /* T0SZ */
                 | (1ULL << 8)               /* IRGN0 = WB RA WA */
                 | (1ULL << 10)              /* ORGN0 = WB RA WA */
                 | (3ULL << 12)              /* SH0 = inner shareable */
                 | (0ULL << 14)              /* TG0 = 4 KiB */
                 | ((uint64_t)(64 - VA_BITS) << 16) /* T1SZ (unused: EPD1=1) */
                 | (1ULL << 23)              /* EPD1: no TTBR1 walks */
                 | (2ULL << 30)              /* TG1 = 4 KiB (0b00 is a reserved encoding) */
                 | (ips << 32);              /* IPS */
    __asm__ volatile ("dsb sy" ::: "memory");                /* table writes visible to the walker */
    __asm__ volatile ("msr mair_el1, %0" :: "r"(MAIR_VALUE));
    __asm__ volatile ("msr tcr_el1, %0" :: "r"(tcr));
    __asm__ volatile ("msr ttbr0_el1, %0" :: "r"((uint64_t)(uintptr_t)l1));
    __asm__ volatile ("isb\n tlbi vmalle1\n dsb nsh\n isb" ::: "memory");
    __asm__ volatile ("ic iallu\n dsb nsh\n isb" ::: "memory");   /* no stale instruction lines when I-cache is enabled */
    uint64_t sctlr; __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0)    /* M   */
          |  (1ULL << 2)    /* C   */
          |  (1ULL << 12)   /* I   */
          |  (1ULL << 19);  /* WXN */
    __asm__ volatile ("msr sctlr_el1, %0\n isb" :: "r"(sctlr) : "memory");
}

int vmm_hw_translate(uint64_t va, int write, uint64_t *pa)
{
    uint64_t par;
    if (write) __asm__ volatile ("at s1e1w, %0" :: "r"(va));
    else       __asm__ volatile ("at s1e1r, %0" :: "r"(va));
    __asm__ volatile ("isb\n mrs %0, par_el1" : "=r"(par) :: "memory");
    if (par & 1) return -1;
    if (pa) *pa = (par & ADDR_MASK) | (va & (PAGE_SIZE - 1));
    return 0;
}
