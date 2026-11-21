/*
 * kernel/proc/user_fp_progs.S - EL0 programs that exercise FP/SIMD context switching (test kernels only: assembled when AURORA_SELFTEST_HOOKS is set).
 * A separate image from user_progs.S (which must fit one code page); started with proc_create_image(role, arg, name, user_fp_blob_start, ...). Same syscall ABI
 * and data-page layout as user_progs.S (DATA_VA = 0x4000001000; svc #0, number in x8).
 */
    .equ DATA_VA,   0x4000001000
    .equ SYS_GETPID, 3
    .equ SYS_EXIT,  4
    .equ SYS_TICKS, 5

    .macro sys nr
    mov     x8, #\nr
    svc     #0
    .endm

    .macro after_fault
    ldr     x20, =DATA_VA
    mov     x3, #0x666
    str     x3, [x20, #0x40]
    mov     x0, #0
    sys     SYS_EXIT
    b       .
    .endm

    .section .user_fp_blob, "a"
    .global user_fp_blob_start
    .global user_fp_blob_end
    .balign 16
user_fp_blob_start:
    cmp     x0, #80
    b.eq    role_fpcheck
    cmp     x0, #81
    b.eq    role_fpzero
    cmp     x0, #82
    b.eq    role_fpdirty
    mov     x0, #99
    sys     SYS_EXIT
    b       .

/* ---- FP/SIMD programs (kernel/include/fpsimd.h). Data page: 0x400 pattern buffer (64 x 8 bytes), 0x800 read-back buffer.
 *  role 80 (x1 = duration in ticks): load V0-V31 with a pattern derived from its pid (and FPCR RMode / FPSR QC from its pid), keep running for the duration (it never
 *          yields, so the timer preempts it), and verify every one of the 64 doublewords, FPCR and FPSR after each pass.
 *          Results: [0] iterations, [8] marker 0xDDDD, [0x10] register mismatches, [0x28] FPCR/FPSR mismatches.
 *  role 81: read V0-V31, FPCR and FPSR at start-up and count the non-zero ones: a new process must see ZERO (no value left by an earlier process). [8] 0xEEEE, [0x10] count.
 *  role 82: dirty V0-V31, FPCR and FPSR with a recognisable pattern, then fault (an unmapped read): the next process in the slot must still see zero. */
role_fpcheck:
    ldr     x20, =DATA_VA
    mov     x12, x1
    sys     SYS_GETPID
    mov     x13, x0
    lsl     x22, x0, #56
    ldr     x2, =0x0F1E2D3C4B000000
    orr     x22, x22, x2                /* seed = pid<<56 | constant */
    add     x14, x20, #0x400            /* pattern buffer */
    add     x15, x20, #0x800            /* read-back buffer */
    mov     x4, #0
1:  add     x5, x22, x4
    str     x5, [x14, x4, lsl #3]
    add     x4, x4, #1
    cmp     x4, #64
    b.lt    1b
    ldp     q0, q1, [x14, #0]
    ldp     q2, q3, [x14, #32]
    ldp     q4, q5, [x14, #64]
    ldp     q6, q7, [x14, #96]
    ldp     q8, q9, [x14, #128]
    ldp     q10, q11, [x14, #160]
    ldp     q12, q13, [x14, #192]
    ldp     q14, q15, [x14, #224]
    ldp     q16, q17, [x14, #256]
    ldp     q18, q19, [x14, #288]
    ldp     q20, q21, [x14, #320]
    ldp     q22, q23, [x14, #352]
    ldp     q24, q25, [x14, #384]
    ldp     q26, q27, [x14, #416]
    ldp     q28, q29, [x14, #448]
    ldp     q30, q31, [x14, #480]
    and     x5, x13, #3
    lsl     x5, x5, #22
    msr     fpcr, x5                    /* RMode = pid & 3 */
    and     x6, x13, #1
    lsl     x6, x6, #27
    msr     fpsr, x6                    /* QC = pid & 1 */
    mov     x9, #0                      /* register mismatches */
    mov     x10, #0                     /* FPCR / FPSR mismatches */
    sys     SYS_TICKS
    add     x21, x0, x12
fp_loop:
    mov     x11, #3000
2:  ldr     x2, [x20]
    add     x2, x2, #1
    str     x2, [x20]
    subs    x11, x11, #1
    b.ne    2b
    stp     q0, q1, [x15, #0]
    stp     q2, q3, [x15, #32]
    stp     q4, q5, [x15, #64]
    stp     q6, q7, [x15, #96]
    stp     q8, q9, [x15, #128]
    stp     q10, q11, [x15, #160]
    stp     q12, q13, [x15, #192]
    stp     q14, q15, [x15, #224]
    stp     q16, q17, [x15, #256]
    stp     q18, q19, [x15, #288]
    stp     q20, q21, [x15, #320]
    stp     q22, q23, [x15, #352]
    stp     q24, q25, [x15, #384]
    stp     q26, q27, [x15, #416]
    stp     q28, q29, [x15, #448]
    stp     q30, q31, [x15, #480]
    mov     x4, #0
3:  ldr     x2, [x15, x4, lsl #3]
    ldr     x3, [x14, x4, lsl #3]
    cmp     x2, x3
    cset    x3, ne
    add     x9, x9, x3
    add     x4, x4, #1
    cmp     x4, #64
    b.lt    3b
    and     x5, x13, #3
    lsl     x5, x5, #22
    mrs     x2, fpcr
    and     x2, x2, #0xC00000
    cmp     x2, x5
    cset    x3, ne
    add     x10, x10, x3
    and     x6, x13, #1
    lsl     x6, x6, #27
    mrs     x2, fpsr
    and     x2, x2, #0x8000000
    cmp     x2, x6
    cset    x3, ne
    add     x10, x10, x3
    sys     SYS_TICKS
    cmp     x0, x21
    b.lt    fp_loop
    str     x9, [x20, #0x10]
    str     x10, [x20, #0x28]
    ldr     x3, =0xDDDD
    str     x3, [x20, #8]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

role_fpzero:
    ldr     x20, =DATA_VA
    add     x15, x20, #0x800
    stp     q0, q1, [x15, #0]
    stp     q2, q3, [x15, #32]
    stp     q4, q5, [x15, #64]
    stp     q6, q7, [x15, #96]
    stp     q8, q9, [x15, #128]
    stp     q10, q11, [x15, #160]
    stp     q12, q13, [x15, #192]
    stp     q14, q15, [x15, #224]
    stp     q16, q17, [x15, #256]
    stp     q18, q19, [x15, #288]
    stp     q20, q21, [x15, #320]
    stp     q22, q23, [x15, #352]
    stp     q24, q25, [x15, #384]
    stp     q26, q27, [x15, #416]
    stp     q28, q29, [x15, #448]
    stp     q30, q31, [x15, #480]
    mov     x9, #0
    mov     x4, #0
1:  ldr     x2, [x15, x4, lsl #3]
    cmp     x2, #0
    cset    x3, ne
    add     x9, x9, x3
    add     x4, x4, #1
    cmp     x4, #64
    b.lt    1b
    mrs     x2, fpcr
    cmp     x2, #0
    cset    x3, ne
    add     x9, x9, x3
    mrs     x2, fpsr
    cmp     x2, #0
    cset    x3, ne
    add     x9, x9, x3
    str     x9, [x20, #0x10]
    ldr     x3, =0xEEEE
    str     x3, [x20, #8]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

role_fpdirty:
    ldr     x20, =DATA_VA
    add     x14, x20, #0x400
    ldr     x22, =0xDEADBEEFCAFE0000
    mov     x4, #0
1:  add     x5, x22, x4
    str     x5, [x14, x4, lsl #3]
    add     x4, x4, #1
    cmp     x4, #64
    b.lt    1b
    ldp     q0, q1, [x14, #0]
    ldp     q2, q3, [x14, #32]
    ldp     q4, q5, [x14, #64]
    ldp     q6, q7, [x14, #96]
    ldp     q8, q9, [x14, #128]
    ldp     q10, q11, [x14, #160]
    ldp     q12, q13, [x14, #192]
    ldp     q14, q15, [x14, #224]
    ldp     q16, q17, [x14, #256]
    ldp     q18, q19, [x14, #288]
    ldp     q20, q21, [x14, #320]
    ldp     q22, q23, [x14, #352]
    ldp     q24, q25, [x14, #384]
    ldp     q26, q27, [x14, #416]
    ldp     q28, q29, [x14, #448]
    ldp     q30, q31, [x14, #480]
    mov     x5, #(3 << 22)
    msr     fpcr, x5
    mov     x6, #(1 << 27)
    msr     fpsr, x6
    ldr     x2, =0xDEAD0000
    ldr     x3, [x2]                    /* unmapped: faults */
    after_fault


    .ltorg                              /* literals (ldr x, =imm) must lie INSIDE the copied image */
    .balign 4
user_fp_blob_end:
