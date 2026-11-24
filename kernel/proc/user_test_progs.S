/*
 * kernel/proc/user_test_progs.S — EL0 test programs for the file-access boundary tests. Assembled ONLY into AURORA_FS_TEST kernels
 * (build.sh); a normal or production kernel does not contain these bytes at all.
 *
 * Unlike the shared blob in user_progs.S (which must fit one 4 KiB code page together with every other role), each program here is its own
 * image: the test harness loads it into a fresh process with proc_create_image(). Each image is self-contained, including its literal pool.
 * The programs are unprivileged EL0 code that only use SYS_FS; the process identity (principal) is assigned by kernel code, never by them.
 */
    .equ DATA_VA,   0x4000001000
    .equ SYS_EXIT,  4
    .equ SYS_FS,    8

    .macro sys nr
    mov     x8, #\nr
    svc     #0
    .endm

    .section .user_blob, "a"

/*
 * Program "bounds": system-call BOUNDARY matrix. x1 = id of a 16-byte file this process owns, x2 = id of a file it must NOT be able to
 * discover, x3 = a stale (deleted) id, x4 = a KERNEL address. Every call must fail (or, for the misaligned stat, succeed) safely.
 * Results are 8-byte slots in the data page; the data page ends at +0xFFF and the page after it is the unmapped guard page.
 *   100 lookup, name straddles the guard page   108 create, name straddles the guard   110 read, buffer straddles   118 write, buffer straddles
 *   120 create name with an embedded NUL   128 create name with a slash   130 create perm 0x10   138 create perm -1   140 set_perm 0x10   148 set_perm -1
 *   150 list max 0   158 list max 17   160 list max -1   168 write at offset size+1 (a hole)   170 read INTO the read-only code page
 *   178 read into a kernel address   180 stat into a buffer straddling the guard   188 stat into a MISALIGNED user address (must work)
 *   190 stat into the code page   198 read offset 0xFFFFFFFFFFFFFFF0   1A0 read len 2^32+16   1A8 write offset 2^32+1   1B0 write len 2^32+4
 *   1B8 lookup with a pointer that wraps past 2^64   1C0 read into a wrapping pointer   1C8 lookup with name length 2^32+4
 *   200..220 read/write/stat/delete/set_perm on the HIDDEN id     240..260 the same five calls on the STALE id
 */
    .macro fscall slot, op, a1, a2, a3, a4
    mov     x0, \op
    mov     x1, \a1
    mov     x2, \a2
    mov     x3, \a3
    mov     x4, \a4
    mov     x8, #SYS_FS
    svc     #0
    str     x0, [x20, #\slot]
    .endm

    .balign 16
    .global fs_bounds_start
    .global fs_bounds_end
fs_bounds_start:
    ldr     x20, =DATA_VA
    mov     x19, x1
    mov     x18, x2
    mov     x17, x3
    mov     x16, x4
    sub     x21, x20, #0x1000              /* the read-only code page */
    add     x22, x20, #0xFFE               /* 2 bytes before the guard page */
    add     x23, x20, #0xFF8               /* 8 bytes before the guard page */
    add     x24, x20, #0xFE0               /* 32 bytes before it: a 56-byte stat record straddles */
    add     x25, x20, #0x501               /* misaligned */
    adr     x26, fs_nul
    adr     x27, fs_slash
    add     x28, x20, #0x600               /* an ordinary valid buffer */
    ldr     x10, =0x100000010
    ldr     x11, =0x100000001
    ldr     x12, =0xFFFFFFFFFFFFFFFC
    ldr     x13, =0x100000004
    ldr     x14, =0xFFFFFFFFFFFFFFF0
    ldr     x15, =0x5A5A5A5A5A5A5A5A
    str     x15, [x20, #0xFF8]             /* sentinel in the last 8 bytes of the data page: no call may change it */
    fscall  0x100, #2, x22, #4, #0, #0
    fscall  0x108, #1, x22, #4, #3, #0
    fscall  0x110, #3, x19, #0, x23, #16
    fscall  0x118, #4, x19, #0, x23, #16
    fscall  0x120, #1, x26, #3, #3, #0
    fscall  0x128, #1, x27, #3, #3, #0
    fscall  0x130, #1, x26, #1, #16, #0
    fscall  0x138, #1, x26, #1, x14, #0
    fscall  0x140, #8, x19, #16, #0, #0
    fscall  0x148, #8, x19, x14, #0, #0
    fscall  0x150, #6, x28, #0, #0, #0
    fscall  0x158, #6, x28, #17, #0, #0
    fscall  0x160, #6, x28, x14, #0, #0
    fscall  0x168, #4, x19, #17, x28, #4
    fscall  0x170, #3, x19, #0, x21, #16
    fscall  0x178, #3, x19, #0, x16, #16
    fscall  0x180, #5, x19, x24, #0, #0
    fscall  0x188, #5, x19, x25, #0, #0
    fscall  0x190, #5, x19, x21, #0, #0
    fscall  0x198, #3, x19, x14, x28, #16
    fscall  0x1A0, #3, x19, #0, x28, x10
    fscall  0x1A8, #4, x19, x11, x28, #4
    fscall  0x1B0, #4, x19, #0, x28, x13
    fscall  0x1B8, #2, x12, #8, #0, #0
    fscall  0x1C0, #3, x19, #0, x12, #16
    fscall  0x1C8, #2, x26, x13, #0, #0
    fscall  0x200, #3, x18, #0, x28, #16
    fscall  0x208, #4, x18, #0, x28, #4
    fscall  0x210, #5, x18, x28, #0, #0
    fscall  0x218, #7, x18, #0, #0, #0
    fscall  0x220, #8, x18, #3, #0, #0
    fscall  0x240, #3, x17, #0, x28, #16
    fscall  0x248, #4, x17, #0, x28, #4
    fscall  0x250, #5, x17, x28, #0, #0
    fscall  0x258, #7, x17, #0, #0, #0
    fscall  0x260, #8, x17, #3, #0, #0
    mov     x0, #0
    sys     SYS_EXIT
    b       .
fs_nul:   .byte 0x61, 0, 0x62
fs_slash: .ascii "a/b"
    .balign 8
    .ltorg
    .balign 16
fs_bounds_end:

/*
 * Program "quota": creates ten one-letter files, then fills the first two with 24 KiB each (12 writes of 4096 bytes taken from its own
 * readable code page) and writes one byte to the third. Results: ids/errors of the ten creates at +0x400 + 8*i, the 12 write results at
 * +0x500 + 8*i, the final write at +0x5F8.
 */
    .global fs_quota_start
    .global fs_quota_end
fs_quota_start:
    ldr     x20, =DATA_VA
    sub     x24, x20, #0x1000
    add     x23, x20, #0x400
    mov     x21, #0
    mov     x22, #97
1:  strb    w22, [x20, #0x300]
    mov     x0, #1
    add     x1, x20, #0x300
    mov     x2, #1
    mov     x3, #3
    sys     SYS_FS
    str     x0, [x23, x21, lsl #3]
    add     x22, x22, #1
    add     x21, x21, #1
    cmp     x21, #10
    b.lt    1b
    add     x25, x20, #0x500
    mov     x28, #0
    mov     x26, #0
2:  ldr     x19, [x23, x26, lsl #3]
    mov     x27, #0
3:  mov     x0, #4
    mov     x1, x19
    mov     x2, x27
    mov     x3, x24
    mov     x4, #4096
    sys     SYS_FS
    str     x0, [x25, x28, lsl #3]
    add     x28, x28, #1
    add     x27, x27, #4096
    cmp     x27, #24576
    b.lt    3b
    add     x26, x26, #1
    cmp     x26, #2
    b.lt    2b
    ldr     x19, [x23, #16]
    mov     x0, #4
    mov     x1, x19
    mov     x2, #0
    mov     x3, x24
    mov     x4, #1
    sys     SYS_FS
    str     x0, [x20, #0x5F8]
    mov     x0, #0
    sys     SYS_EXIT
    b       .
    .balign 8
    .ltorg
    .balign 16
fs_quota_end:
