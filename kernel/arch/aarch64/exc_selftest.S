/*
 * kernel/arch/aarch64/exc_selftest.S — exception entry/exit register-preservation self-test.
 *
 * uint64_t exc_roundtrip_test(void)
 * Loads a distinct pattern into x1-x28 (except x19, which holds SP, and x29/x30, used by the test itself),
 * takes a real synchronous exception (SVC #0x7e57, handled by a test hook in exceptions.c that rewrites x0 and
 * x27 inside the SAVED FRAME), and checks on return that:
 *   - every pattern register is unchanged (the frame was saved and restored faithfully),
 *   - x0 and x27 carry the values the handler wrote into the frame (so the registers really are restored FROM the
 *     frame, not merely left alone),
 *   - SP_EL0 is unchanged and SPSel is still 0.
 * Returns a bitmask: bit n = register xn wrong, bit 62 = SP_EL0 changed, bit 61 = SPSel != 0. 0 means success.
 */
    .text
    .global exc_roundtrip_test
    .balign 4
exc_roundtrip_test:
    sub     sp, sp, #112
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    mov     x19, sp

    .irp    n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,20,21,22,23,24,25,26,28
    ldr     x\n, =(0x5A5A5A5A00000000 + \n)
    .endr
    mov     x27, #0
    mov     x0, #0
    mov     x29, xzr

    svc     #0x7e57

    .irp    n, 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,20,21,22,23,24,25,26,28
    ldr     x30, =(0x5A5A5A5A00000000 + \n)
    cmp     x\n, x30
    b.eq    1f
    orr     x29, x29, #(1 << \n)
1:
    .endr
    ldr     x30, =0xAAAA5555
    cmp     x0, x30
    b.eq    2f
    orr     x29, x29, #1
2:
    ldr     x30, =0xC0DE0027
    cmp     x27, x30
    b.eq    3f
    orr     x29, x29, #(1 << 27)
3:
    mov     x30, sp
    cmp     x30, x19
    b.eq    4f
    mov     x30, #1
    orr     x29, x29, x30, lsl #62
4:
    mrs     x30, spsel
    cbz     x30, 5f
    mov     x30, #1
    orr     x29, x29, x30, lsl #61
5:
    mov     x0, x29
    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #112
    ret
