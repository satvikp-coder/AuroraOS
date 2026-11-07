/*
 * boot/aarch64/start.S — AuroraOS ARM64 boot entry
 *
 * QEMU ARM64 virt board loads the kernel at physical address 0x40080000
 * via the -kernel option. The CPU starts in EL2 (or EL1 depending on
 * machine type) with the MMU off. We set up a stack and branch to the
 * C kernel entry.
 *
 * Target: QEMU ARM64 "virt" machine
 */

.section .text.boot
.global _start
.start = 0x40080000

_start:
    /* Disable all interrupts */
    msr daifset, #0xf

    /* Set up the stack pointer at the top of our reserved stack area */
    adr x0, _stack_top
    mov sp, x0

    /* Clear the BSS section (zero-initialized globals) */
    adr x0, __bss_start
    adr x1, __bss_end
    sub x1, x1, x0
    mov x2, #0
1:
    cmp x2, x1
    b.ge 2f
    str x2, [x0], #8
    add x2, x2, #8
    b 1b
2:

    /* Branch to the C kernel entry point */
    bl kernel_main

    /* If kernel_main returns, halt forever */
3:
    wfi
    b 3b

.section .bss
.align 4
_stack:
    .skip 4096                /* 4 KiB kernel stack */
_stack_top:

/* BSS boundaries (set by linker) */
.globl __bss_start, __bss_end
__bss_start:
__bss_end:
