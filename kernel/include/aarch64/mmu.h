/*
 * kernel/include/aarch64/mmu.h — ARM64 MMU definitions
 *
 * Register encodings and memory attribute constants for configuring
 * stage-1 address translation on AArch64.  All bit fields follow the
 * ARM Architecture Reference Manual (ARM DDI 0487).
 *
 * Target: ARM Cortex-A53 (ARMv8-A), 4 KiB translation granule.
 */

#ifndef AURORA_AARCH64_MMU_H
#define AURORA_AARCH64_MMU_H

#include <stdint.h>

/* --- MAIR_ELx memory attribute indices ------------------------------- */

#define MAIR_ATTR_DEVICE_nGnRnE 0x00  /* Device memory, non-cacheable      */
#define MAIR_ATTR_NORMAL_NC     0x44  /* Normal memory, non-cacheable      */
#define MAIR_ATTR_NORMAL_WB     0xFF  /* Normal memory, cacheable WB RA WA */

/* MAIR_ELx register value: index 0 = device, 1 = normal NC, 7 = normal WB */
#define MAIR_VALUE                                          \
    ((MAIR_ATTR_DEVICE_nGnRnE << 0)  |                      \
     (MAIR_ATTR_NORMAL_NC     << 8)  |                      \
     (MAIR_ATTR_NORMAL_WB     << 56))

/* --- TCR_EL1/EL2 bit fields ------------------------------------------ */

#define TCR_T0SZ_48BIT   ((64 - 48) << 0)   /* VA size: 48-bit            */
#define TCR_T1SZ_48BIT   ((64 - 48) << 16)  /* VA size: 48-bit            */
#define TCR_TG0_4K       (0 << 14)          /* 4 KiB granule              */
#define TCR_SH0_INNER    (3 << 12)          /* Inner shareable            */
#define TCR_ORGN0_WB     (1 << 8)           /* Outer WB                   */
#define TCR_IRGN0_WB     (0 << 6)           /* Inner WB                   */
#define TCR_T0SZ         TCR_T0SZ_48BIT
#define TCR_TG0          TCR_TG0_4K
#define TCR_SH0          TCR_SH0_INNER
#define TCR_ORGN0        TCR_ORGN0_WB
#define TCR_IRGN0        TCR_IRGN0_WB

/* --- SCTLR_ELx bit fields -------------------------------------------- */

#define SCTLR_M          (1 << 0)   /* MMU enable                    */
#define SCTLR_A          (1 << 1)   /* Alignment check enable        */
#define SCTLR_C          (1 << 2)   /* Data cache enable             */
#define SCTLR_I          (1 << 12)  /* Instruction cache enable      */
#define SCTLR_SA         (1 << 3)   /* Stack alignment check         */
#define SCTLR_SA0        (1 << 4)   /* SP0 alignment check           */

/* --- Exception syndrome registers ----------------------------------- */

#define ESR_ELx_EC_SHIFT 26
#define ESR_ELx_EC_MASK  (0x3F << ESR_ELx_EC_SHIFT)

/* ESR_ELx EC values */
#define ESR_EC_INSTR_ABORT_LOWER  (0x20)
#define ESR_EC_DATA_ABORT_LOWER   (0x24)
#define ESR_EC_INSTR_ABORT_SAME   (0x21)
#define ESR_EC_DATA_ABORT_SAME    (0x25)

/* Data Fault Status Register (DFSC) bits within ESR */
#define ESR_DFSC_FO   (1 << 10)   /* Fault on outer cacheable */
#define ESR_DFSC_STATUS_MASK 0x3F

/* --- Page table entry attributes ------------------------------------- */

#define PTE_VALID     (1ULL << 0)   /* Valid bit                    */
#define PTE_TABLE     (1ULL << 1)   /* Points to next-level table   */
#define PTE_BLOCK     (0ULL << 1)   /* Block mapping                */
#define PTE_PXN       (1ULL << 53)  /* Privileged execute never     */
#define PTE_UXN       (1ULL << 54)  /* User execute never           */
#define PTE_SHARED    (3ULL << 8)   /* Inner shareable              */
#define PTE_AF        (1ULL << 10)  /* Access flag                  */
#define PTE_NG        (1ULL << 11)  /* Not global                   */
#define PTE_MEM_ATTR_SHIFT 2
#define PTE_ATTR_DEVICE (MAIR_ATTR_DEVICE_nGnRnE << PTE_MEM_ATTR_SHIFT)
#define PTE_ATTR_NORMAL_WB (MAIR_ATTR_NORMAL_WB << PTE_MEM_ATTR_SHIFT)

/* AP[2:1] bits (bits [7:6]): access permissions */
#define PTE_AP_RW_RW   (0ULL << 6)   /* EL0: RW, EL1: RW             */
#define PTE_AP_RO_RO   (1ULL << 6)   /* EL0: RO, EL1: RO             */
#define PTE_AP_RW_RO   (2ULL << 6)   /* EL0: No, EL1: RW             */
#define PTE_AP_RO_NONE (3ULL << 6)   /* EL0: No, EL1: RO             */

/* --- Page table constants ------------------------------------------- */

#define PAGE_TABLE_ENTRIES 512
#define PAGE_TABLE_SIZE    AURORA_PAGE_SIZE

/* VA -> page table index helpers */
#define VA_L0_INDEX(va) (((va) >> 39) & 0x1FF)
#define VA_L1_INDEX(va) (((va) >> 30) & 0x1FF)
#define VA_L2_INDEX(va) (((va) >> 21) & 0x1FF)
#define VA_L3_INDEX(va) (((va) >> 12) & 0x1FF)

/* Block sizes */
#define L2_BLOCK_SIZE (2ULL * 1024 * 1024)   /* 2 MiB block              */
#define L3_PAGE_SIZE  AURORA_PAGE_SIZE       /* 4 KiB page               */

/* --- Exception-level-specific system register access ----------------- */

#if defined(__aarch64__)

static inline uint64_t read_ttbr0_el2(void)
{
    uint64_t val;
    __asm__ volatile ("mrs %0, ttbr0_el2" : "=r"(val));
    return val;
}

static inline void write_ttbr0_el2(uint64_t val)
{
    __asm__ volatile ("msr ttbr0_el2, %0" :: "r"(val));
}

static inline uint64_t read_tcr_el2(void)
{
    uint64_t val;
    __asm__ volatile ("mrs %0, tcr_el2" : "=r"(val));
    return val;
}

static inline void write_tcr_el2(uint64_t val)
{
    __asm__ volatile ("msr tcr_el2, %0" :: "r"(val));
}

static inline uint64_t read_mair_el2(void)
{
    uint64_t val;
    __asm__ volatile ("mrs %0, mair_el2" : "=r"(val));
    return val;
}

static inline void write_mair_el2(uint64_t val)
{
    __asm__ volatile ("msr mair_el2, %0" :: "r"(val));
}

static inline uint64_t read_sctlr_el2(void)
{
    uint64_t val;
    __asm__ volatile ("mrs %0, sctlr_el2" : "=r"(val));
    return val;
}

static inline void write_sctlr_el2(uint64_t val)
{
    __asm__ volatile ("msr sctlr_el2, %0" :: "r"(val));
}

static inline uint64_t read_esr_el2(void)
{
    uint64_t val;
    __asm__ volatile ("mrs %0, esr_el2" : "=r"(val));
    return val;
}

static inline uint64_t read_far_el2(void)
{
    uint64_t val;
    __asm__ volatile ("mrs %0, far_el2" : "=r"(val));
    return val;
}

static inline void dsb_ish(void)
{
    __asm__ volatile ("dsb ish" ::: "memory");
}

static inline void isb(void)
{
    __asm__ volatile ("isb" ::: "memory");
}

#endif /* __aarch64__ */

#endif /* AURORA_AARCH64_MMU_H */
