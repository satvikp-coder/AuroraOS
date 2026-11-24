/*
 * kernel/arch/aarch64/mmu.S — ARM64 MMU enable assembly
 *
 * Configures stage-1 translation for EL2 (the exception level at which
 * QEMU starts the kernel when using -kernel).  Programs MAIR_EL2,
 * TCR_EL2, TTBR0_EL2, then enables the MMU via SCTLR_EL2.
 *
 * After this code runs, the CPU continues executing at the same virtual
 * address (identity mapping), so no address switch is needed.
 */

.section .text.mmu
.align 4
.global mmu_enable
.type mmu_enable, %function

mmu_enable:
    /* 1. Program MAIR_EL2:
        - Index 0: Device memory, non-cacheable (0x00)
        - Index 1: Normal memory, non-cacheable (0x44)
        - Index 7: Normal memory, write-back (0xFF)
        MAIR_EL2 = 0xFF << 56 | 0x44 << 8 | 0x00 = 0xFF00000000000444 */
    mov x0, #0x44
    mov x1, #0xFF
    mov x1, x1, lsl #56
    orr x0, x0, x1
    msr mair_el2, x0

    /* 2. Program TCR_EL2:
        - T0SZ = 16 (48-bit VA)
        - TG0 = 4 KiB
        - SH0 = Inner shareable (3)
        - ORGN0 = WB (1)
        - IRGN0 = WB (0) */
    mov x0, #(16 << 0)       /* T0SZ = 16 */
    orr x0, x0, #(3 << 12)   /* SH0 = Inner shareable */
    orr x0, x0, #(1 << 8)    /* ORGN0 = WB */
    orr x0, x0, #(1 << 23)   /* EPD1 = 1 (disable TTBR1 translations) */
    orr x0, x0, #(4 << 32)   /* PS = 48-bit physical address */
    msr tcr_el2, x0

    /* 3. Program TTBR0_EL2 with the L0 table physical address */
    adr x0, mmu_l0_table_storage
    ldr x0, [x0]
    msr ttbr0_el2, x0

    /* 4. Ensure all table writes are visible before enabling MMU */
    dsb ish
    isb

    /* 5. Read SCTLR_EL2, set M (MMU enable) and C (data cache) */
    mrs x0, sctlr_el2
    orr x0, x0, #(1 << 0)    /* M = MMU enable */
    orr x0, x0, #(1 << 2)    /* C = Data cache enable */
    orr x0, x0, #(1 << 12)   /* I = Instruction cache enable */
    msr sctlr_el2, x0

    /* 6. ISB to ensure the MMU enable takes effect immediately */
    isb

    ret

.section .data.mmu
.align 3
.global mmu_l0_table_storage
mmu_l0_table_storage:
    .quad 0   /* filled in by mmu_set_l0_table() before calling mmu_enable */

/* C-callable: set the L0 table physical address */
.global mmu_set_l0_table
.type mmu_set_l0_table, %function
mmu_set_l0_table:
    adr x1, mmu_l0_table_storage
    str x0, [x1]
    ret

/* C-callable: set VBAR_EL2 to the exception vector table address */
.global mmu_set_vbar
.type mmu_set_vbar, %function
mmu_set_vbar:
    msr vbar_el2, x0
    isb
    ret