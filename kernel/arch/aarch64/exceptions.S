/*
 * kernel/arch/aarch64/exceptions.S — ARM64 exception vectors
 *
 * Minimal exception handling for synchronous faults.  When the MMU is
 * enabled and an access targets an unmapped address, the CPU takes a
 * synchronous exception.  We read ESR_EL2 and FAR_EL2 to diagnose the
 * fault, call the C fault handler for diagnostics, and halt.
 *
 * Target: ARM Cortex-A53 (ARMv8-A), EL2.
 */

.section .text.exceptions
.align 4
.global exception_sync
.type exception_sync, %function

/* Synchronous exception entry point (EL2) */
exception_sync:
    /* Save x0-x2 (scratch registers we'll use) */
    stp x0, x1, [sp, #-16]!

    /* Read ESR_EL2 to determine the exception class */
    mrs x0, esr_el2

    /* Read FAR_EL2 for the faulting address */
    mrs x1, far_el2

    /* Call C fault handler: x0=esr, x1=far */
    bl fault_handler

    /* If fault_handler ever returns (it shouldn't), halt */
    ldp x0, x1, [sp], #16
    b fault_halt

fault_halt:
    /* Halt forever — fault is fatal at this stage */
    wfi
    b fault_halt

.section .data.exceptions
.align 11
.global exception_vector_table
.type exception_vector_table, %object

/* Exception vector table for EL2 (current exception level) */
exception_vector_table:
    .quad exception_sync   /* 0x000: Synchronous exception (current EL) */
    .quad exception_sync   /* 0x080: IRQ (current EL)                 */
    .quad exception_sync   /* 0x100: FIQ                                */
    .quad exception_sync   /* 0x180: SError                             */
    .quad exception_sync   /* 0x200: Synchronous exception (lower EL)   */
    .quad exception_sync   /* 0x280: IRQ (lower EL)                     */
    .quad exception_sync   /* 0x300: FIQ (lower EL)                     */
    .quad exception_sync   /* 0x380: SError (lower EL)                  */
