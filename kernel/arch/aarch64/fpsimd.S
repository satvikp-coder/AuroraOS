/*
 * kernel/arch/aarch64/fpsimd.S - save and restore the FP/SIMD register file (V0-V31, FPSR, FPCR) of an EL0 process.
 *
 * The kernel itself is built -mgeneral-regs-only and never touches these registers, so between a process leaving EL0 and the scheduler
 * switching away they still hold exactly that process's values. The layout is struct fpstate in kernel/include/fpsimd.h:
 * 32 x 16 bytes (V0..V31), then FPSR and FPCR as 32-bit values (528 bytes, 16-byte aligned).
 */
    .text
    .global fp_save
    .global fp_restore

fp_save:                                /* x0 = struct fpstate * */
    stp     q0,  q1,  [x0, #0]
    stp     q2,  q3,  [x0, #32]
    stp     q4,  q5,  [x0, #64]
    stp     q6,  q7,  [x0, #96]
    stp     q8,  q9,  [x0, #128]
    stp     q10, q11, [x0, #160]
    stp     q12, q13, [x0, #192]
    stp     q14, q15, [x0, #224]
    stp     q16, q17, [x0, #256]
    stp     q18, q19, [x0, #288]
    stp     q20, q21, [x0, #320]
    stp     q22, q23, [x0, #352]
    stp     q24, q25, [x0, #384]
    stp     q26, q27, [x0, #416]
    stp     q28, q29, [x0, #448]
    stp     q30, q31, [x0, #480]
    mrs     x1, fpsr
    mrs     x2, fpcr
    str     w1, [x0, #512]
    str     w2, [x0, #516]
    ret

fp_restore:                             /* x0 = struct fpstate * */
    ldp     q0,  q1,  [x0, #0]
    ldp     q2,  q3,  [x0, #32]
    ldp     q4,  q5,  [x0, #64]
    ldp     q6,  q7,  [x0, #96]
    ldp     q8,  q9,  [x0, #128]
    ldp     q10, q11, [x0, #160]
    ldp     q12, q13, [x0, #192]
    ldp     q14, q15, [x0, #224]
    ldp     q16, q17, [x0, #256]
    ldp     q18, q19, [x0, #288]
    ldp     q20, q21, [x0, #320]
    ldp     q22, q23, [x0, #352]
    ldp     q24, q25, [x0, #384]
    ldp     q26, q27, [x0, #416]
    ldp     q28, q29, [x0, #448]
    ldp     q30, q31, [x0, #480]
    ldr     w1, [x0, #512]
    ldr     w2, [x0, #516]
    msr     fpsr, x1
    msr     fpcr, x2
    ret
