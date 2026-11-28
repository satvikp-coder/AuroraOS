/* QEMU virt loads this ELF directly. Semihosted exit reports the C test status. */
.section .text.boot,"ax"
.global _start
_start:
    ldr x0, =stack_top
    mov sp, x0
    bl main
    cbnz x0, exit_probe
    mov x0, #4
    ldr x1, =passed
    hlt #0xf000
    mov x0, #0
exit_probe:
    mov x2, #0x26
    movk x2, #2, lsl #16
    stp x2, x0, [sp, #-16]!
    mov x1, sp
    mov x0, #0x20
    hlt #0xf000
1:  wfe
    b 1b

.section .rodata
passed: .asciz "AURORA-SIGNED-PROBE-PASS\n"
