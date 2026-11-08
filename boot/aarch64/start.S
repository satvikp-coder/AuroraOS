/*
 * boot/aarch64/start.S — AuroraOS ARM64 boot entry
 *
 * QEMU "virt" loads the kernel at 0x40080000 and enters with the MMU off.
 * x0 holds the device-tree blob address. Depending on the machine options
 * the CPU starts at EL2 or EL1; we always continue at EL1 (EL2 is left
 * with an ERET). Interrupts stay masked.
 */
.section .text.boot
/*
 * Linux arm64 "Image" header (Documentation/arch/arm64/booting.rst). It makes QEMU treat
 * build/kernel.bin as an arm64 Image: it loads it at RAM base + text_offset (0x40080000,
 * matching linker.ld) and passes the device-tree address in x0. The ELF still boots, but
 * QEMU then provides no DTB. This is only a boot-protocol header; no Linux code is used.
 */
.global _head
_head:
    b       _start                      /* code0 */
    .long   0                           /* code1 */
    .quad   0x80000                     /* text_offset */
    .quad   0x100000                    /* image_size upper bound (checked by linker.ld ASSERT) */
    .quad   0x2                         /* flags: little endian, 4 KiB pages */
    .quad   0, 0, 0                     /* reserved */
    .long   0x644d5241                  /* magic "ARM\x64" */
    .long   0

.global _start
_start:
    msr     daifset, #0xf
    mov     x19, x0                     /* preserve DTB pointer */
    mrs     x21, CurrentEL
    lsr     x21, x21, #2                /* x21 = exception level we were entered at (recorded for diagnostics) */

.ifdef AURORA_TEST_ENTRY_SPSEL0
    /* TEST ONLY (build.sh AURORA_TEST_ENTRY=spsel0): when entered at EL1, deliberately arrive the way the
     * normal QEMU entry does NOT: SPSel=0, with both stack pointers holding junk. The initialisation
     * below must still produce correct stacks, because it selects each stack pointer explicitly. */
    cmp     x21, #1
    b.ne    1f
    msr     spsel, #1
    mov     x1, #0x1000
    mov     sp, x1                      /* junk SP_EL1 */
    msr     spsel, #0
    mov     x1, #0x2000
    mov     sp, x1                      /* junk SP_EL0, and SP_EL0 is the selected stack */
    isb
1:
.endif

    cmp     x21, #2
    b.ne    el1_entry

    /* EL2 -> EL1 (AArch64) */
    mrs     x2, midr_el1                /* EL1 reads of MIDR/MPIDR come from the virtualization registers */
    msr     vpidr_el2, x2
    mrs     x2, mpidr_el1
    msr     vmpidr_el2, x2
    mov     x2, #(1 << 31)              /* HCR_EL2.RW: EL1 is AArch64 (all other HCR_EL2 bits 0: no trapping, IRQs stay at EL1) */
    msr     hcr_el2, x2
    mov     x2, #3
    msr     cnthctl_el2, x2             /* EL1 may use physical counter/timer */
    msr     cntvoff_el2, xzr
    mov     x2, #0x33ff                 /* CPTR_EL2: RES1 bits, no FP/SIMD trap */
    msr     cptr_el2, x2
.ifdef AURORA_TEST_ENTRY_EL1T
    mov     x2, #0x3c4                  /* TEST ONLY (AURORA_TEST_ENTRY=el1t): SPSR_EL2 = DAIF masked, EL1t, i.e. SPSel=0 at EL1 entry */
.else
    mov     x2, #0x3c5                  /* SPSR_EL2: DAIF masked, EL1h */
.endif
    msr     spsr_el2, x2
    adr     x2, el1_entry
    msr     elr_el2, x2
    eret

el1_entry:
    mrs     x20, spsel                  /* SPSel as we arrived (recorded for diagnostics; never assumed below) */
    ldr     x2, =0x30d00800             /* SCTLR_EL1: RES1 bits only (MMU, caches off) */
    msr     sctlr_el1, x2
    mov     x2, #(3 << 20)              /* CPACR_EL1.FPEN: no FP/SIMD trap */
    msr     cpacr_el1, x2
    isb

    /* Two stacks: kernel code runs on SP_EL0 (SPSel=0); every exception taken to EL1 switches to SP_EL1
     * automatically (Arm ARM D1.6: the target EL's stack pointer is selected on exception entry), so
     * exception entry never touches possibly-exhausted kernel stack memory.
     * The incoming SPSel is NOT assumed: select SP_EL1 explicitly before writing it. */
    msr     spsel, #1                   /* "sp" now means SP_EL1 */
    ldr     x0, =__exc_stack_top        /* 16-byte aligned by linker.ld */
    mov     sp, x0
    ldr     x0, =__stack_top            /* 16-byte aligned by linker.ld */
    msr     sp_el0, x0                  /* SP_EL0 is writable by name from EL1 */
    msr     spsel, #0                   /* kernel code runs on SP_EL0 */
    isb

    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
clear_bss:
    cmp     x0, x1
    b.hs    bss_done
    str     xzr, [x0], #8
    b       clear_bss
bss_done:
    ldr     x1, =boot_entry_el          /* record how we were entered (BSS is cleared now, so this sticks) */
    str     x21, [x1]
    ldr     x1, =boot_entry_spsel
    str     x20, [x1]
    mov     x0, x19
    bl      kernel_main
halt:
    wfi
    b       halt
