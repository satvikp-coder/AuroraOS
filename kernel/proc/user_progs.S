/*
 * kernel/proc/user_progs.S — native AArch64 programs that run at EL0.
 *
 * One position-independent blob, copied into each process's code page (USER_CODE_VA). It dispatches on x0 (the
 * "role", set by the kernel when the process is created); x1 carries one kernel-chosen argument (e.g. an address the
 * program must try to touch). These are real EL0 instructions: they are not kernel C functions.
 *
 * Process data page (USER_DATA_VA = 0x4000001000), 8-byte slots, offsets in hex:
 *   00 loop counter    08 completion marker    10 register-mismatch count    18 pid    20 ticks
 *   20..58 syscall results (role 14)    40 "executed after the fault" marker    80 stale-memory result
 * Syscall ABI: svc #0, number in x8, arguments x0-x5, result in x0 (see kernel/include/proc.h).
 */
    .equ DATA_VA,   0x4000001000
    .equ SYS_WRITE, 1
    .equ SYS_YIELD, 2
    .equ SYS_GETPID, 3
    .equ SYS_EXIT,  4
    .equ SYS_TICKS, 5

    .macro sys nr
    mov     x8, #\nr
    svc     #0
    .endm

    /* After a fault the process must be dead: if it ever gets here, it records the fact and exits. */
    .macro after_fault
    ldr     x20, =DATA_VA
    mov     x3, #0x666
    str     x3, [x20, #0x40]
    mov     x0, #0
    sys     SYS_EXIT
    b       .
    .endm

    .section .user_blob, "a"
    .global user_blob_start
    .global user_blob_end
    .balign 16
user_blob_start:
    cmp     x0, #1
    b.eq    role_a
    cmp     x0, #2
    b.eq    role_b
    cmp     x0, #10
    b.eq    role_read
    cmp     x0, #11
    b.eq    role_write
    cmp     x0, #13
    b.eq    role_mmio
    cmp     x0, #14
    b.eq    role_syscalls
    cmp     x0, #15
    b.eq    role_unmapped
    cmp     x0, #16
    b.eq    role_priv
    cmp     x0, #17
    b.eq    role_write_code
    cmp     x0, #18
    b.eq    role_exec_data
    cmp     x0, #19
    b.eq    role_guard
    cmp     x0, #20
    b.eq    role_secret
    cmp     x0, #21
    b.eq    role_stale
    cmp     x0, #22
    b.eq    role_fp
    cmp     x0, #31
    b.eq    role_spin
    cmp     x0, #41
    b.eq    role_exit7
    cmp     x0, #42
    b.eq    role_badsvc
    cmp     x0, #50
    b.eq    role_cpu
    mov     x0, #99
    sys     SYS_EXIT
    b       .

/* ---- Program A: counts, writes "A", yields; registers must survive every context switch ---- */
role_a:
    ldr     x20, =DATA_VA
    mov     x19, #0                     /* mismatch count */
    mov     x21, #0                     /* iteration */
    ldr     x23, =0xA1A1A1A1A1A1A123
    ldr     x24, =0xA2A2A2A2A2A2A224
    ldr     x25, =0xA3A3A3A3A3A3A325
    ldr     x26, =0xA4A4A4A4A4A4A426
    ldr     x27, =0xA5A5A5A5A5A5A527
    ldr     x28, =0xA6A6A6A6A6A6A628
a_loop:
    ldr     x2, [x20]
    add     x2, x2, #1
    str     x2, [x20]
    adr     x0, msg_a
    mov     x1, #1
    sys     SYS_WRITE
    sys     SYS_YIELD
    ldr     x2, =0xA1A1A1A1A1A1A123
    cmp     x23, x2
    b.eq    1f
    add     x19, x19, #1
1:  ldr     x2, =0xA2A2A2A2A2A2A224
    cmp     x24, x2
    b.eq    2f
    add     x19, x19, #1
2:  ldr     x2, =0xA3A3A3A3A3A3A325
    cmp     x25, x2
    b.eq    3f
    add     x19, x19, #1
3:  ldr     x2, =0xA4A4A4A4A4A4A426
    cmp     x26, x2
    b.eq    4f
    add     x19, x19, #1
4:  ldr     x2, =0xA5A5A5A5A5A5A527
    cmp     x27, x2
    b.eq    5f
    add     x19, x19, #1
5:  ldr     x2, =0xA6A6A6A6A6A6A628
    cmp     x28, x2
    b.eq    6f
    add     x19, x19, #1
6:  add     x21, x21, #1
    cmp     x21, #5
    b.lt    a_loop
    str     x19, [x20, #0x10]
    ldr     x3, =0xAAAA
    str     x3, [x20, #8]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

/* ---- Program B: counts to 7, uses getpid/ticks, yields; its own register patterns ---- */
role_b:
    ldr     x20, =DATA_VA
    mov     x19, #0
    mov     x21, #0
    ldr     x23, =0xB1B1B1B1B1B1B123
    ldr     x24, =0xB2B2B2B2B2B2B224
    ldr     x25, =0xB3B3B3B3B3B3B325
    ldr     x26, =0xB4B4B4B4B4B4B426
    ldr     x27, =0xB5B5B5B5B5B5B527
    ldr     x28, =0xB6B6B6B6B6B6B628
b_loop:
    ldr     x2, [x20]
    add     x2, x2, #1
    str     x2, [x20]
    sys     SYS_GETPID
    str     x0, [x20, #0x18]
    sys     SYS_TICKS
    str     x0, [x20, #0x20]
    adr     x0, msg_b
    mov     x1, #1
    sys     SYS_WRITE
    sys     SYS_YIELD
    ldr     x2, =0xB1B1B1B1B1B1B123
    cmp     x23, x2
    b.eq    1f
    add     x19, x19, #1
1:  ldr     x2, =0xB2B2B2B2B2B2B224
    cmp     x24, x2
    b.eq    2f
    add     x19, x19, #1
2:  ldr     x2, =0xB3B3B3B3B3B3B325
    cmp     x25, x2
    b.eq    3f
    add     x19, x19, #1
3:  ldr     x2, =0xB4B4B4B4B4B4B426
    cmp     x26, x2
    b.eq    4f
    add     x19, x19, #1
4:  ldr     x2, =0xB5B5B5B5B5B5B527
    cmp     x27, x2
    b.eq    5f
    add     x19, x19, #1
5:  ldr     x2, =0xB6B6B6B6B6B6B628
    cmp     x28, x2
    b.eq    6f
    add     x19, x19, #1
6:  add     x21, x21, #1
    cmp     x21, #7
    b.lt    b_loop
    str     x19, [x20, #0x10]
    ldr     x3, =0xBBBB
    str     x3, [x20, #8]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

/* ---- fault programs: each performs ONE forbidden access; the process must be stopped by the kernel ---- */
role_read:                              /* read an address the kernel passed in x1 (kernel data / another process's page) */
    ldr     x2, [x1]
    after_fault
role_write:                             /* write to an address the kernel passed in x1 (kernel text) */
    mov     x2, #0x41
    str     x2, [x1]
    after_fault
role_mmio:                              /* read the UART data register: kernel MMIO */
    movz    x1, #0x900, lsl #16
    ldr     x2, [x1]
    after_fault
role_unmapped:                          /* read an address that is mapped nowhere */
    movz    x1, #0xdead, lsl #16
    ldr     x2, [x1]
    after_fault
role_priv:                              /* privileged system-register read: EL0 must not execute it */
    mrs     x2, sctlr_el1
    after_fault
role_write_code:                        /* the process's own code page is read-only */
    adr     x2, user_blob_start
    mov     x3, #0x41
    str     x3, [x2]
    after_fault
role_exec_data:                         /* writable data is never executable */
    ldr     x2, =DATA_VA
    br      x2
    after_fault
role_guard:                             /* the unmapped guard page below the stack */
    ldr     x2, =0x4000002008
    ldr     x3, [x2]
    after_fault
role_fp:                                /* FP/SIMD is not available to user processes (the kernel does not save it) */
    fmov    d0, xzr
    after_fault
role_badsvc:                            /* only "svc #0" is the syscall instruction */
    svc     #5
    after_fault

/* ---- syscall validation: bad arguments must be rejected, the process must survive ---- */
role_syscalls:
    ldr     x20, =DATA_VA
    mov     x19, x1                     /* a kernel address */
    mov     x0, x19                     /* (a) pointer into kernel memory */
    mov     x1, #8
    sys     SYS_WRITE
    str     x0, [x20, #0x20]
    ldr     x0, =0x403FFFFC             /* (b) range that runs past the end of the user region */
    mov     x1, #16
    sys     SYS_WRITE
    str     x0, [x20, #0x28]
    mov     x0, #-16                    /* (c) pointer + length overflows 64 bits */
    mov     x1, #32
    sys     SYS_WRITE
    str     x0, [x20, #0x30]
    adr     x0, msg_ok                  /* (d) length above the limit */
    mov     x1, #4096
    sys     SYS_WRITE
    str     x0, [x20, #0x38]
    mov     x8, #99                     /* (e) unknown system call number */
    svc     #0
    str     x0, [x20, #0x48]
    ldr     x0, =0x4000002000           /* (f) inside the user region but unmapped (guard page) */
    mov     x1, #4
    sys     SYS_WRITE
    str     x0, [x20, #0x50]
    adr     x0, msg_ok                  /* (g) a valid call still works */
    mov     x1, #2
    sys     SYS_WRITE
    str     x0, [x20, #0x58]
    adr     x0, msg_ok                  /* (h) zero length */
    mov     x1, #0
    sys     SYS_WRITE
    str     x0, [x20, #0x60]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

/* ---- stale-memory pair: one process leaves secrets, the next must see only zeros ---- */
role_secret:
    ldr     x20, =DATA_VA
    ldr     x3, =0x5EC4E75EC4E7
    str     x3, [x20]
    str     x3, [x20, #8]
    ldr     x4, =0x4000003000           /* bottom of the stack: also write a secret there */
    str     x3, [x4]
    mov     x0, #0
    sys     SYS_EXIT
    b       .
role_stale:
    ldr     x20, =DATA_VA
    ldr     x2, [x20]
    ldr     x3, [x20, #8]
    ldr     x4, =0x4000003000
    ldr     x5, [x4]
    orr     x2, x2, x3
    orr     x2, x2, x5
    str     x2, [x20, #0x80]
    mov     x0, #0
    cbz     x2, 1f
    mov     x0, #1
1:  sys     SYS_EXIT
    b       .

/*
 * ---- Program CPU: never calls yield. x1 = how many scheduler ticks to run for. ----
 * It can only share the CPU with other processes if the kernel PREEMPTS it from a timer interrupt. Every burst it checks that
 *   - x23-x28 still hold its own pattern (seed derived from its pid, so another process's registers would be detected), and
 *   - SP is still the value it started with (SP_EL0 is saved and restored per process).
 * Results: [0] iterations, [8] completion marker 0xCCCC, [0x10] register mismatches, [0x28] stack-pointer mismatches.
 */
role_cpu:
    mov     x19, sp
    ldr     x20, =DATA_VA
    mov     x12, x1
    sys     SYS_GETPID
    lsl     x22, x0, #56
    ldr     x2, =0x12345678AB000000
    orr     x22, x22, x2                /* seed = pid<<56 | constant */
    add     x23, x22, #1
    add     x24, x22, #2
    add     x25, x22, #3
    add     x26, x22, #4
    add     x27, x22, #5
    add     x28, x22, #6
    mov     x9, #0                      /* register mismatches */
    mov     x10, #0                     /* stack-pointer mismatches */
    sys     SYS_TICKS
    add     x21, x0, x12                /* end = now + duration */
cpu_loop:
    mov     x11, #3000
cpu_inner:
    ldr     x2, [x20]
    add     x2, x2, #1
    str     x2, [x20]
    subs    x11, x11, #1
    b.ne    cpu_inner
    add     x2, x22, #1
    cmp     x23, x2
    cset    x3, ne
    add     x9, x9, x3
    add     x2, x22, #2
    cmp     x24, x2
    cset    x3, ne
    add     x9, x9, x3
    add     x2, x22, #3
    cmp     x25, x2
    cset    x3, ne
    add     x9, x9, x3
    add     x2, x22, #4
    cmp     x26, x2
    cset    x3, ne
    add     x9, x9, x3
    add     x2, x22, #5
    cmp     x27, x2
    cset    x3, ne
    add     x9, x9, x3
    add     x2, x22, #6
    cmp     x28, x2
    cset    x3, ne
    add     x9, x9, x3
    mov     x2, sp
    cmp     x2, x19
    cset    x3, ne
    add     x10, x10, x3
    sys     SYS_TICKS
    cmp     x0, x21
    b.lt    cpu_loop
    str     x9, [x20, #0x10]
    str     x10, [x20, #0x28]
    ldr     x3, =0xCCCC
    str     x3, [x20, #8]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

role_spin:                              /* never yields: only preemption or the watchdog can stop it */
    ldr     x20, =DATA_VA
1:  ldr     x2, [x20]
    add     x2, x2, #1
    str     x2, [x20]
    b       1b
role_exit7:
    mov     x0, #7
    sys     SYS_EXIT
    b       .

msg_a:  .ascii "A"
msg_b:  .ascii "B"
msg_ok: .ascii "ok"
    .balign 8
    .ltorg
    .balign 16
user_blob_end:
