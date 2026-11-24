/*
 * kernel/include/aarch64/mmu_asm.h — ARM64 MMU assembly constants
 *
 * Numeric constant definitions for the assembler.  Values must match
 * kernel/include/aarch64/mmu.h.
 */

#ifndef AURORA_AARCH64_MMU_ASM_H
#define AURORA_AARCH64_MMU_ASM_H

/* MAIR_ELx memory attribute indices */
#define MAIR_ATTR_DEVICE_nGnRnE 0x00
#define MAIR_ATTR_NORMAL_NC     0x44
#define MAIR_ATTR_NORMAL_WB     0xFF

#define MAIR_VALUE                                          \
    ((MAIR_ATTR_DEVICE_nGnRnE << 0)  |                      \
     (MAIR_ATTR_NORMAL_NC     << 8)  |                      \
     (MAIR_ATTR_NORMAL_WB     << 56))

/* TCR_EL2 bit fields */
#define TCR_T0SZ_48BIT   ((64 - 48) << 0)
#define TCR_TG0_4K       (0 << 14)
#define TCR_SH0_INNER    (3 << 12)
#define TCR_ORGN0_WB     (1 << 8)
#define TCR_IRGN0_WB     (0 << 6)

#define TCR_T0SZ         TCR_T0SZ_48BIT
#define TCR_TG0          TCR_TG0_4K
#define TCR_SH0          TCR_SH0_INNER
#define TCR_ORGN0        TCR_ORGN0_WB
#define TCR_IRGN0        TCR_IRGN0_WB

/* SCTLR_ELx bit fields */
#define SCTLR_M          (1 << 0)
#define SCTLR_C          (1 << 2)
#define SCTLR_I          (1 << 12)

/* Page table entry attributes */
#define PTE_VALID     (1ULL << 0)
#define PTE_TABLE     (1ULL << 1)
#define PTE_BLOCK     (0ULL << 1)
#define PTE_PXN       (1ULL << 53)
#define PTE_UXN       (1ULL << 54)
#define PTE_SHARED    (3ULL << 8)
#define PTE_AF        (1ULL << 10)
#define PTE_NG        (1ULL << 11)
#define PTE_MEM_ATTR_SHIFT 2
#define PTE_ATTR_DEVICE (MAIR_ATTR_DEVICE_nGnRnE << PTE_MEM_ATTR_SHIFT)
#define PTE_ATTR_NORMAL_WB (MAIR_ATTR_NORMAL_WB << PTE_MEM_ATTR_SHIFT)

/* Page table constants */
#define PAGE_TABLE_ENTRIES 512
#define PAGE_TABLE_SIZE    4096

/* Block sizes */
#define L2_BLOCK_SIZE (2 * 1024 * 1024)
#define L3_PAGE_SIZE  4096

/* VA -> page table index helpers */
#define VA_L0_INDEX(va) (((va) >> 39) & 0x1FF)
#define VA_L1_INDEX(va) (((va) >> 30) & 0x1FF)
#define VA_L2_INDEX(va) (((va) >> 21) & 0x1FF)
#define VA_L3_INDEX(va) (((va) >> 12) & 0x1FF)

/* --- Exception-level-specific system register access ----------------- */

.macro mrs_ttbr0_el2, reg
    mrs \reg, ttbr0_el2
.endm

.macro msr_ttbr0_el2, reg
    msr ttbr0_el2, \reg
.endm

.macro mrs_tcr_el2, reg
    mrs \reg, tcr_el2
.endm

.macro msr_tcr_el2, reg
    msr tcr_el2, \reg
.endm

.macro mrs_mair_el2, reg
    mrs \reg, mair_el2
.endm

.macro msr_mair_el2, reg
    msr mair_el2, \reg
.endm

.macro mrs_sctlr_el2, reg
    mrs \reg, sctlr_el2
.endm

.macro msr_sctlr_el2, reg
    msr sctlr_el2, \reg
.endm

.macro mrs_vbar_el2, reg
    mrs \reg, vbar_el2
.endm

.macro msr_vbar_el2, reg
    msr vbar_el2, \reg
.endm

.macro mrs_esr_el2, reg
    mrs \reg, esr_el2
.endm

.macro mrs_far_el2, reg
    mrs \reg, far_el2
.endm

.macro dsb_ish
    dsb ish
.endm

.macro isb
    isb
.endm

#endif /* AURORA_AARCH64_MMU_ASM_H */
