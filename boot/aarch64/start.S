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

    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    el1_entry

    /* EL2 -> EL1 (AArch64) */
    mov     x2, #(1 << 31)              /* HCR_EL2.RW: EL1 is AArch64 */
    msr     hcr_el2, x2
    mov     x2, #3
    msr     cnthctl_el2, x2             /* EL1 may use physical counter/timer */
    msr     cntvoff_el2, xzr
    mov     x2, #0x33ff                 /* CPTR_EL2: RES1 bits, no FP/SIMD trap */
    msr     cptr_el2, x2
    mov     x2, #0x3c5                  /* SPSR_EL2: DAIF masked, EL1h */
    msr     spsr_el2, x2
    adr     x2, el1_entry
    msr     elr_el2, x2
    eret

el1_entry:
    ldr     x2, =0x30d00800             /* SCTLR_EL1: RES1 bits only (MMU, caches off) */
    msr     sctlr_el1, x2
    mov     x2, #(3 << 20)              /* CPACR_EL1.FPEN: no FP/SIMD trap */
    msr     cpacr_el1, x2
    isb

    ldr     x0, =__stack_top
    mov     sp, x0

    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
clear_bss:
    cmp     x0, x1
    b.hs    bss_done
    str     xzr, [x0], #8
    b       clear_bss
bss_done:
    mov     x0, x19
    bl      kernel_main
halt:
    wfi
    b       halt
