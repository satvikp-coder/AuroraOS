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
    .equ SYS_SEND,  6
    .equ SYS_RECV,  7
    .equ SYS_FS,    8

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
    cmp     x0, #60
    b.eq    role_send3
    cmp     x0, #61
    b.eq    role_recv_n
    cmp     x0, #62
    b.eq    role_ipc_probe
    cmp     x0, #63
    b.eq    role_send_fault
    cmp     x0, #64
    b.eq    role_recv_once
    cmp     x0, #65
    b.eq    role_flood
    cmp     x0, #66
    b.eq    role_recv_flags
    cmp     x0, #67
    b.eq    role_tryall
    cmp     x0, #68
    b.eq    role_send_two
    cmp     x0, #70
    b.eq    role_fs_create
    cmp     x0, #71
    b.eq    role_fs_intruder
    cmp     x0, #72
    b.eq    role_fs_reader
    cmp     x0, #73
    b.eq    role_fs_hostile
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

/* ---- IPC programs. Data page: results in 8-byte slots; receive buffer at +0x200. ---- */

/* role 60: send "m1","m2","m3" to pid x1; store each send's return value at [0x20+8i] */
role_send3:
    ldr     x20, =DATA_VA
    mov     x19, x1
    mov     x21, #0
    adr     x22, msg_m
s3_loop:
    mov     x0, x19
    add     x1, x22, x21, lsl #1
    mov     x2, #2
    sys     SYS_SEND
    add     x3, x20, x21, lsl #3
    str     x0, [x3, #0x20]
    add     x21, x21, #1
    cmp     x21, #3
    b.lt    s3_loop
    mov     x0, #0
    sys     SYS_EXIT
    b       .

/* role 61: BLOCKING receive of x1 messages (0 means 3). Per message i: length [0x20+8i], sender [0x50+8i], first 2 bytes [0x80+8i]. */
role_recv_n:
    ldr     x20, =DATA_VA
    mov     x19, x1
    cbnz    x19, 1f
    mov     x19, #3
1:  mov     x21, #0
    add     x22, x20, #0x200
rn_loop:
    mov     x0, x22
    mov     x1, #64
    mov     x2, #0
    sys     SYS_RECV
    add     x3, x20, x21, lsl #3
    str     x0, [x3, #0x20]
    str     x1, [x3, #0x50]
    ldrh    w4, [x22]
    str     x4, [x3, #0x80]
    add     x21, x21, #1
    cmp     x21, x19
    b.lt    rn_loop
    ldr     x3, =0x1CC
    str     x3, [x20, #8]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

/* role 62: probe every rejection path. x1 = a kernel address, x2 = a pid this process is NOT allowed to send to. */
role_ipc_probe:
    ldr     x20, =DATA_VA
    mov     x19, x1
    mov     x23, x2
    sys     SYS_GETPID
    mov     x21, x0
    adr     x22, msg_m
    mov     x0, x21                     /* (a) message too long */
    mov     x1, x22
    mov     x2, #65
    sys     SYS_SEND
    str     x0, [x20, #0x20]
    mov     x0, #0                      /* (b) pid 0 */
    mov     x1, x22
    mov     x2, #2
    sys     SYS_SEND
    str     x0, [x20, #0x28]
    mov     x0, #99                     /* (c) pid out of range */
    mov     x1, x22
    mov     x2, #2
    sys     SYS_SEND
    str     x0, [x20, #0x30]
    mov     x0, x21                     /* (d) source buffer inside the kernel */
    mov     x1, x19
    mov     x2, #4
    sys     SYS_SEND
    str     x0, [x20, #0x38]
    mov     x0, x21                     /* (e) a valid send to itself */
    mov     x1, x22
    mov     x2, #2
    sys     SYS_SEND
    str     x0, [x20, #0x48]
    mov     x0, x19                     /* (f) receive into a kernel address: refused, the message stays queued */
    mov     x1, #64
    mov     x2, #1
    sys     SYS_RECV
    str     x0, [x20, #0x50]
    add     x0, x20, #0x200             /* (g) receive into its own buffer: the message that stayed queued arrives */
    mov     x1, #64
    mov     x2, #1
    sys     SYS_RECV
    str     x0, [x20, #0x58]
    add     x0, x20, #0x200             /* (h) empty mailbox, non-blocking */
    mov     x1, #64
    mov     x2, #1
    sys     SYS_RECV
    str     x0, [x20, #0x60]
    mov     x0, x23                     /* (i) destination it has no authority for */
    mov     x1, x22
    mov     x2, #2
    sys     SYS_SEND
    str     x0, [x20, #0x68]
    mov     x24, #0                     /* (j) fill its own mailbox: 4 accepted, then "full" */
pr_fill:
    mov     x0, x21
    mov     x1, x22
    mov     x2, #2
    sys     SYS_SEND
    add     x3, x20, x24, lsl #3
    str     x0, [x3, #0x70]
    add     x24, x24, #1
    cmp     x24, #5
    b.lt    pr_fill
    add     x0, x20, #0x200             /* (k) capacity smaller than the queued message */
    mov     x1, #1
    mov     x2, #1
    sys     SYS_RECV
    str     x0, [x20, #0x98]
    add     x0, x20, #0x200             /* (l) zero capacity */
    mov     x1, #0
    mov     x2, #1
    sys     SYS_RECV
    str     x0, [x20, #0xa0]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

/* role 63: send two messages to pid x1, THEN fault. The queued messages must survive the sender's fault. */
role_send_fault:
    mov     x19, x1
    mov     x0, x19
    adr     x1, msg_m
    mov     x2, #2
    sys     SYS_SEND
    mov     x0, x19
    adr     x1, msg_m + 2
    mov     x2, #2
    sys     SYS_SEND
    ldr     x2, =0xDEAD0000
    ldr     x3, [x2]
    after_fault

/* role 64: ONE blocking receive; exit code = message length. Slots: length [0x20], sender [0x50]. */
role_recv_once:
    ldr     x20, =DATA_VA
    add     x0, x20, #0x200
    mov     x1, #64
    mov     x2, #0
    sys     SYS_RECV
    str     x0, [x20, #0x20]
    str     x1, [x20, #0x50]
    sys     SYS_EXIT
    b       .

/* role 65: send six messages to pid x1 as fast as possible: the first four fit, the rest report "full". */
role_flood:
    ldr     x20, =DATA_VA
    mov     x19, x1
    mov     x21, #0
    adr     x22, msg_m
fl_loop:
    mov     x0, x19
    mov     x1, x22
    mov     x2, #2
    sys     SYS_SEND
    add     x3, x20, x21, lsl #3
    str     x0, [x3, #0x20]
    add     x21, x21, #1
    cmp     x21, #6
    b.lt    fl_loop
    mov     x0, #0
    sys     SYS_EXIT
    b       .

/* role 68: send "ab" to pid x1 and "cd" to pid x1+1 */
role_send_two:
    ldr     x20, =DATA_VA
    mov     x19, x1
    mov     x0, x19
    adr     x1, msg_ab
    mov     x2, #2
    sys     SYS_SEND
    str     x0, [x20, #0x20]
    add     x0, x19, #1
    adr     x1, msg_cd
    mov     x2, #2
    sys     SYS_SEND
    str     x0, [x20, #0x28]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

/* role 66: one NON-BLOCKING-or-not receive with the flags in x1 (anything but 0 or 1 must be refused); result at [0x20] */
role_recv_flags:
    ldr     x20, =DATA_VA
    mov     x2, x1
    add     x0, x20, #0x200
    mov     x1, #64
    sys     SYS_RECV
    str     x0, [x20, #0x20]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

/* role 67: try every unknown system call number 8..40 (all must be -ENOSYS: there is no way to grant oneself authority),
 * then send to the pid in x1 which this process was never authorised to message. [0x20] = number of -ENOSYS, [0x28] = send result. */
role_tryall:
    ldr     x20, =DATA_VA
    mov     x19, x1
    mov     x21, #9
    mov     x22, #0
ta_loop:
    mov     x0, #2
    mov     x1, #0
    mov     x2, #0
    mov     x8, x21
    svc     #0
    cmn     x0, #4                      /* x0 == -4 (AE_ENOSYS)? */
    cset    x3, eq
    add     x22, x22, x3
    add     x21, x21, #1
    cmp     x21, #41
    b.lt    ta_loop
    str     x22, [x20, #0x20]
    mov     x0, x19
    adr     x1, msg_m
    mov     x2, #2
    sys     SYS_SEND
    str     x0, [x20, #0x28]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

/* ---- file-access programs (SYS_FS; operations: 1 create 2 lookup 3 read 4 write 5 stat 6 list 7 delete 8 set_perm) ---- */
    .macro fsop op, nr_slot
    mov     x0, #\op
    sys     SYS_FS
    str     x0, [x20, #\nr_slot]
    .endm

/* role 70: program A. Creates the private file "note" (perm 3) and the other-readable file "pub" (perm 7), writes 16 bytes into each, reads "note"
 * back. Results: [0x20] id of note  [0x28] write rc  [0x30] read rc (bytes read lands at +0x400)  [0x38] id of pub  [0x50] write rc for pub */
role_fs_create:
    ldr     x20, =DATA_VA
    adr     x1, fs_note
    mov     x2, #4
    mov     x3, #3
    fsop    1, 0x20
    mov     x19, x0
    mov     x0, #4
    mov     x1, x19
    mov     x2, #0
    adr     x3, fs_data
    mov     x4, #16
    sys     SYS_FS
    str     x0, [x20, #0x28]
    mov     x0, #3
    mov     x1, x19
    mov     x2, #0
    add     x3, x20, #0x400
    mov     x4, #64
    sys     SYS_FS
    str     x0, [x20, #0x30]
    adr     x1, fs_pub
    mov     x2, #3
    mov     x3, #7
    fsop    1, 0x38
    mov     x19, x0
    mov     x0, #4
    mov     x1, x19
    mov     x2, #0
    adr     x3, fs_data
    mov     x4, #16
    sys     SYS_FS
    str     x0, [x20, #0x50]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

/* role 71: program B (intruder). x1 = a file id it GUESSED for "note". Tries lookup, read, write, delete, chmod on it, then reads/writes the
 * other-readable "pub". Results: [0x20] lookup note  [0x28] read  [0x30] write  [0x38] delete  [0x50] chmod  [0x58] lookup pub  [0x60] read pub
 * [0x68] write pub */
role_fs_intruder:
    ldr     x20, =DATA_VA
    mov     x19, x1
    adr     x1, fs_note
    mov     x2, #4
    fsop    2, 0x20
    mov     x0, #3
    mov     x1, x19
    mov     x2, #0
    add     x3, x20, #0x400
    mov     x4, #16
    sys     SYS_FS
    str     x0, [x20, #0x28]
    mov     x0, #4
    mov     x1, x19
    mov     x2, #0
    adr     x3, fs_data
    mov     x4, #16
    sys     SYS_FS
    str     x0, [x20, #0x30]
    mov     x0, #7
    mov     x1, x19
    sys     SYS_FS
    str     x0, [x20, #0x38]
    mov     x0, #8
    mov     x1, x19
    mov     x2, #15
    sys     SYS_FS
    str     x0, [x20, #0x50]
    adr     x1, fs_pub
    mov     x2, #3
    fsop    2, 0x58
    mov     x19, x0
    mov     x0, #3
    mov     x1, x19
    mov     x2, #0
    add     x3, x20, #0x500
    mov     x4, #16
    sys     SYS_FS
    str     x0, [x20, #0x60]
    mov     x0, #4
    mov     x1, x19
    mov     x2, #0
    adr     x3, fs_data
    mov     x4, #16
    sys     SYS_FS
    str     x0, [x20, #0x68]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

/* role 72: reader. Looks up "note", reads 64 bytes, stats it, lists the directory. Results: [0x20] id  [0x28] read rc (data at +0x400)
 * [0x30] stat rc (struct at +0x500)  [0x38] list rc (entries at +0x600) */
role_fs_reader:
    ldr     x20, =DATA_VA
    adr     x1, fs_note
    mov     x2, #4
    fsop    2, 0x20
    mov     x19, x0
    mov     x0, #3
    mov     x1, x19
    mov     x2, #0
    add     x3, x20, #0x400
    mov     x4, #64
    sys     SYS_FS
    str     x0, [x20, #0x28]
    mov     x0, #5
    mov     x1, x19
    add     x2, x20, #0x500
    sys     SYS_FS
    str     x0, [x20, #0x30]
    mov     x0, #6
    add     x1, x20, #0x600
    mov     x2, #8
    sys     SYS_FS
    str     x0, [x20, #0x38]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

/* role 73: hostile arguments. x1 = a KERNEL address, x2 = a VALID file id this process may use. Every call must fail safely.
 * [0x20] read into kernel ptr  [0x28] create with name at kernel ptr  [0x30] write from kernel ptr  [0x38] read len=-1  [0x50] read offset=-1
 * [0x58] unknown op 99  [0x60] op "FORM"  [0x68] create name len 40  [0x70] create name len 0  [0x78] list into kernel ptr  [0x80] stat into kernel ptr */
role_fs_hostile:
    ldr     x20, =DATA_VA
    mov     x19, x1
    mov     x18, x2
    mov     x0, #3
    mov     x1, x18
    mov     x2, #0
    mov     x3, x19
    mov     x4, #16
    sys     SYS_FS
    str     x0, [x20, #0x20]
    mov     x0, #1
    mov     x1, x19
    mov     x2, #4
    mov     x3, #3
    sys     SYS_FS
    str     x0, [x20, #0x28]
    mov     x0, #4
    mov     x1, x18
    mov     x2, #0
    mov     x3, x19
    mov     x4, #16
    sys     SYS_FS
    str     x0, [x20, #0x30]
    mov     x0, #3
    mov     x1, x18
    mov     x2, #0
    add     x3, x20, #0x400
    mov     x4, #-1
    sys     SYS_FS
    str     x0, [x20, #0x38]
    mov     x0, #3
    mov     x1, x18
    mov     x2, #-1
    add     x3, x20, #0x400
    mov     x4, #16
    sys     SYS_FS
    str     x0, [x20, #0x50]
    mov     x0, #99
    sys     SYS_FS
    str     x0, [x20, #0x58]
    ldr     x0, =0x464F524D
    sys     SYS_FS
    str     x0, [x20, #0x60]
    mov     x0, #1
    adr     x1, fs_note
    mov     x2, #40
    mov     x3, #3
    sys     SYS_FS
    str     x0, [x20, #0x68]
    mov     x0, #1
    adr     x1, fs_note
    mov     x2, #0
    mov     x3, #3
    sys     SYS_FS
    str     x0, [x20, #0x70]
    mov     x0, #6
    mov     x1, x19
    mov     x2, #4
    sys     SYS_FS
    str     x0, [x20, #0x78]
    mov     x0, #5
    mov     x1, x18
    mov     x2, x19
    sys     SYS_FS
    str     x0, [x20, #0x80]
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

fs_note: .ascii "note"
fs_pub:  .ascii "pub"
fs_data: .ascii "AURORA-EL0-FILE!"
    .balign 4
msg_a:  .ascii "A"
msg_b:  .ascii "B"
msg_ok: .ascii "ok"
msg_m:  .ascii "m1m2m3"
msg_ab: .ascii "ab"
msg_cd: .ascii "cd"
    .balign 8
    .ltorg
    .balign 16
user_blob_end:
