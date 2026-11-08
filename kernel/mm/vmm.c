/*
 * kernel/mm/vmm.c — AArch64 stage-1 translation at EL1.
 *
 * Configuration (ARM DDI 0487, D13/D8): 4 KiB granule, 39-bit virtual
 * addresses (T0SZ=25) so the walk starts at level 1 (512 x 1 GiB entries),
 * TTBR0 only (EPD1=1 disables TTBR1 walks). MAIR_EL1 attr0 = Device-nGnRnE,
 * attr1 = Normal Write-Back. SCTLR_EL1.WXN makes writable memory
 * non-executable regardless of the page tables.
 *
 * Identity map: UART and GIC windows as Device; RAM as Normal with
 * per-section permissions (text RX, rodata RO, data/bss/stack RW+NX),
 * the stack guard page left unmapped. Tables come from the page allocator.
 */
#include <mm.h>

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

static uint64_t *l1;

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

/* Return the next-level table for entry *e, allocating it if empty. NULL on failure or if *e is a block. */
static uint64_t *next_table(uint64_t *e)
{
    if (*e & PTE_VALID) {
        if (!(*e & PTE_TABLE)) return 0;                     /* existing block mapping: refuse to split */
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
    if (!l1 || !bits || size == 0) return -1;
    if ((va | pa | size) & (PAGE_SIZE - 1)) return -2;
    if (va + size < va || va + size > (1ULL << VA_BITS)) return -3;

    while (size) {
        uint64_t *e1 = &l1[(va >> 30) & 0x1FF];
        uint64_t *l2 = next_table(e1);
        if (!l2) return -4;
        uint64_t *e2 = &l2[(va >> 21) & 0x1FF];
        if (!(*e2 & PTE_VALID) && !((va | pa) & 0x1FFFFFULL) && size >= 0x200000ULL) {
            *e2 = pa | bits | PTE_VALID;                      /* 2 MiB block (bit1 clear) */
            va += 0x200000ULL; pa += 0x200000ULL; size -= 0x200000ULL;
            continue;
        }
        uint64_t *l3 = next_table(e2);
        if (!l3) return -4;
        uint64_t *e3 = &l3[(va >> 12) & 0x1FF];
        if (*e3 & PTE_VALID) return -5;                      /* never silently remap */
        *e3 = pa | bits | PTE_TABLE | PTE_VALID;              /* 4 KiB page (bit1 set at level 3) */
        va += PAGE_SIZE; pa += PAGE_SIZE; size -= PAGE_SIZE;
    }
    if (vmm_mmu_enabled()) {
        __asm__ volatile ("dsb ishst\n tlbi vmalle1is\n dsb ish\n isb" ::: "memory");
    }
    return 0;
}

static int map_ident(uintptr_t s, uintptr_t e, int attr)
{
    if (e <= s) return 0;
    return vmm_map(s, s, e - s, attr);
}

int vmm_init(void)
{
    uintptr_t t = pmm_alloc_page();
    if (!t) return -1;
    l1 = (uint64_t *)t;

    /* Devices (QEMU virt): GIC window 0x08000000 (2 MiB) and the PL011 UART page. */
    if (vmm_map(0x08000000ULL, 0x08000000ULL, 0x200000ULL, VMM_ATTR_DEVICE)) return -2;
    if (vmm_map(0x09000000ULL, 0x09000000ULL, PAGE_SIZE, VMM_ATTR_DEVICE)) return -3;

    /* RAM, section by section. The stack guard page is intentionally not mapped. */
    uintptr_t rb = pmm_ram_base(), re = pmm_ram_end();
    if (map_ident(rb, (uintptr_t)__text_start, VMM_ATTR_NORMAL_RW)) return -4;
    if (map_ident((uintptr_t)__text_start, (uintptr_t)__text_end, VMM_ATTR_NORMAL_RX)) return -5;
    if (map_ident((uintptr_t)__rodata_start, (uintptr_t)__rodata_end, VMM_ATTR_NORMAL_RO)) return -6;
    if (map_ident((uintptr_t)__data_start, (uintptr_t)__data_end, VMM_ATTR_NORMAL_RW)) return -7;
    if (map_ident((uintptr_t)__stack_bottom, (uintptr_t)__kernel_end, VMM_ATTR_NORMAL_RW)) return -8;
    if (map_ident((uintptr_t)__kernel_end, re, VMM_ATTR_NORMAL_RW)) return -9;
    return 0;
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
                 | (1ULL << 23)              /* EPD1: no TTBR1 walks */
                 | (ips << 32);              /* IPS */
    __asm__ volatile ("dsb sy" ::: "memory");                /* table writes visible to the walker */
    __asm__ volatile ("msr mair_el1, %0" :: "r"(MAIR_VALUE));
    __asm__ volatile ("msr tcr_el1, %0" :: "r"(tcr));
    __asm__ volatile ("msr ttbr0_el1, %0" :: "r"((uint64_t)(uintptr_t)l1));
    __asm__ volatile ("isb\n tlbi vmalle1\n dsb nsh\n isb" ::: "memory");
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
