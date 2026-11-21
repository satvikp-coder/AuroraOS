/*
 * kernel/proc/user_mem_progs.S - EL0 programs that exercise SYS_MEM, the bounded user heap (test kernels only: assembled when AURORA_SELFTEST_HOOKS is set).
 * A separate image from user_progs.S (which must fit one code page); started with proc_create_image(role, arg, name, user_mem_blob_start, ...).
 * Data page (DATA_VA = 0x4000001000), results in 8-byte slots. Heap at HEAP_VA = USER_HEAP_VA (kernel/include/proc.h), budget 32 pages.
 *
 *  role 90: the boundary probe. Every call below stores its result at the offset shown; the program ends by touching an UNMAPPED (shrunk) heap page and faulting.
 *     0x10 info            0x18 grow(0)         0x20 grow(2^63)      0x28 grow(33)        0x30 unknown op      0x38 shrink(1) on an empty heap
 *     0x40 grow(1)         0x48 first heap word 0x50 pattern readback 0x58 grow(31)       0x60 last heap word  0x68 grow(1) over the budget
 *     0x70 shrink(33)      0x78 shrink(32)      [8] marker 0x9999 just before the final fault
 *  role 91: grow 1 page, write a RET instruction into it and branch there: the heap is never executable (instruction abort, permission fault).
 *  role 92 (x1 = duration in ticks): grow 8 pages, fill all 4096 doublewords with a pid-derived pattern, keep verifying them for the duration (it never yields, so the
 *     timer preempts it), then exit WITHOUT shrinking (exit must reclaim the heap). [0] iterations, [8] 0xDDDD, [0x10] mismatches.
 *  role 93: try grow(1) twice around nothing: [0x10] the result of grow(1), [0x18] info. Used to test refusal at the memory reserve and success just above it.
 *  role 94 (x1 = probe 0..5): grow the heap, then touch ONE address in a guard region and fault. The fault address and class prove which guard it was:
 *     0 load  HEAP_VA - 8            (last word of the guard below the heap)         3 load  HEAP_VA + 0x2FFF8 (last word of the guard above a FULL heap)
 *     1 load  HEAP_VA + 0x1000       (the page after a 1-page heap)                   4 load  HEAP_VA - 0x10000 (the first page of the guard below)
 *     2 store HEAP_VA + 0x20000      (the first page past a full 32-page heap)        5 branch to HEAP_VA - 0x1000 (instruction fetch from the guard)
 *     [8] marker 0x9999 just before the access; [0x40] 0x666 if the access (wrongly) completed.
 *  role 96 (x1 = pages): grow(x1) once, [0x10] its result, [0x18] info; then exit 0 (the process stays TERMINATED, and keeps its pages, until it is destroyed).
 *  role 95: 8 times {grow(32), count every non-zero doubleword (a regrown heap must be ALL ZERO), fill all 16384 doublewords with a pattern, shrink(32)}.
 *     [0x10] total non-zero words seen (must be 0), [0x18] 0xBAD if grow or shrink returned something unexpected, [8] 0xCCCC at the end.
 */
    .equ DATA_VA,   0x4000001000
    .equ HEAP_VA,   0x4000110000          /* USER_HEAP_VA: ELF_VA_MAX + a 16-page guard (kernel/include/uvlayout.h) */
    .equ SYS_GETPID, 3
    .equ SYS_EXIT,  4
    .equ SYS_TICKS, 5
    .equ SYS_MEM,   10

    .macro sys nr
    mov     x8, #\nr
    svc     #0
    .endm

    .macro memcall op, n
    mov     x0, #\op
    mov     x1, #\n
    sys     SYS_MEM
    .endm

    .macro after_fault
    ldr     x20, =DATA_VA
    mov     x3, #0x666
    str     x3, [x20, #0x40]
    mov     x0, #0
    sys     SYS_EXIT
    b       .
    .endm

    .section .user_mem_blob, "a"
    .global user_mem_blob_start
    .global user_mem_blob_end
    .balign 16
user_mem_blob_start:
    cmp     x0, #90
    b.eq    role_memprobe
    cmp     x0, #91
    b.eq    role_heapexec
    cmp     x0, #92
    b.eq    role_heappat
    cmp     x0, #93
    b.eq    role_memtry
    cmp     x0, #94
    b.eq    role_guard
    cmp     x0, #95
    b.eq    role_cycle
    cmp     x0, #96
    b.eq    role_grow
    mov     x0, #99
    sys     SYS_EXIT
    b       .

role_memprobe:
    ldr     x20, =DATA_VA
    ldr     x21, =HEAP_VA
    memcall 3, 0
    str     x0, [x20, #0x10]
    memcall 1, 0
    str     x0, [x20, #0x18]
    mov     x0, #1
    mov     x1, #1
    lsl     x1, x1, #63
    sys     SYS_MEM
    str     x0, [x20, #0x20]
    memcall 1, 33
    str     x0, [x20, #0x28]
    memcall 77, 1
    str     x0, [x20, #0x30]
    memcall 2, 1
    str     x0, [x20, #0x38]
    memcall 1, 1
    str     x0, [x20, #0x40]
    ldr     x2, [x21]                   /* a fresh heap page is zero */
    str     x2, [x20, #0x48]
    ldr     x3, =0xA5A5C3C35A5A3C3C
    str     x3, [x21]
    ldr     x4, [x21]
    str     x4, [x20, #0x50]
    memcall 1, 31
    str     x0, [x20, #0x58]
    ldr     x5, =(HEAP_VA + 0x20000 - 8)
    str     x3, [x5]                    /* the last doubleword of the last heap page */
    ldr     x6, [x5]
    str     x6, [x20, #0x60]
    memcall 1, 1
    str     x0, [x20, #0x68]
    memcall 2, 33
    str     x0, [x20, #0x70]
    memcall 2, 32
    str     x0, [x20, #0x78]
    ldr     x3, =0x9999
    str     x3, [x20, #8]
    ldr     x2, [x21]                   /* the heap was shrunk to nothing: this page is unmapped, so this must fault */
    after_fault

role_heapexec:
    ldr     x20, =DATA_VA
    ldr     x21, =HEAP_VA
    memcall 1, 1
    ldr     x2, =0xD65F03C0             /* ret */
    str     w2, [x21]
    br      x21                         /* never executable: instruction abort */
    after_fault

role_heappat:
    ldr     x20, =DATA_VA
    mov     x12, x1
    sys     SYS_GETPID
    lsl     x22, x0, #56
    ldr     x2, =0x0123456789000000
    orr     x22, x22, x2                /* seed = pid<<56 | constant */
    ldr     x21, =HEAP_VA
    memcall 1, 8
    mov     x4, #0
1:  add     x5, x22, x4
    str     x5, [x21, x4, lsl #3]
    add     x4, x4, #1
    cmp     x4, #4096
    b.lt    1b
    mov     x9, #0                      /* mismatches */
    sys     SYS_TICKS
    add     x13, x0, x12
2:  mov     x4, #0
3:  ldr     x2, [x21, x4, lsl #3]
    add     x5, x22, x4
    cmp     x2, x5
    cset    x3, ne
    add     x9, x9, x3
    add     x4, x4, #1
    cmp     x4, #4096
    b.lt    3b
    ldr     x2, [x20]
    add     x2, x2, #1
    str     x2, [x20]
    sys     SYS_TICKS
    cmp     x0, x13
    b.lt    2b
    str     x9, [x20, #0x10]
    ldr     x3, =0xDDDD
    str     x3, [x20, #8]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

role_memtry:
    ldr     x20, =DATA_VA
    memcall 1, 1
    str     x0, [x20, #0x10]
    memcall 3, 0
    str     x0, [x20, #0x18]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

role_guard:
    ldr     x20, =DATA_VA
    ldr     x21, =HEAP_VA
    mov     x13, #1                     /* pages to grow first */
    mov     x14, #0                     /* 0 load, 1 store, 2 branch */
    sub     x2, x21, #8                 /* probe 0 */
    cmp     x1, #0
    b.eq    g_go
    add     x2, x21, #0x1000            /* probe 1 */
    cmp     x1, #1
    b.eq    g_go
    mov     x13, #32
    mov     x14, #1
    add     x2, x21, #0x20, lsl #12     /* probe 2: HEAP_VA + 0x20000 */
    cmp     x1, #2
    b.eq    g_go
    mov     x14, #0
    ldr     x3, =0x2FFF8
    add     x2, x21, x3                 /* probe 3 */
    cmp     x1, #3
    b.eq    g_go
    mov     x13, #1
    sub     x2, x21, #0x10, lsl #12     /* probe 4: HEAP_VA - 0x10000 */
    cmp     x1, #4
    b.eq    g_go
    mov     x14, #2
    sub     x2, x21, #0x1000            /* probe 5 */
    cmp     x1, #5
    b.eq    g_go
    mov     x0, #99
    sys     SYS_EXIT
    b       .
g_go:
    mov     x0, #1
    mov     x1, x13
    sys     SYS_MEM
    ldr     x3, =0x9999
    str     x3, [x20, #8]
    cmp     x14, #1
    b.eq    g_store
    cmp     x14, #2
    b.eq    g_branch
    ldr     x4, [x2]
    after_fault
g_store:
    str     x4, [x2]
    after_fault
g_branch:
    br      x2
    after_fault

role_cycle:
    ldr     x20, =DATA_VA
    ldr     x21, =HEAP_VA
    ldr     x5, =0xBADC0FFEE0DDF00D
    mov     x9, #0                      /* non-zero words seen after a regrow */
    mov     x10, #8                     /* cycles */
c_loop:
    memcall 1, 32
    cmp     x0, #32
    b.ne    c_bad
    mov     x4, #0
    mov     x6, #16384
c_walk:
    ldr     x2, [x21, x4, lsl #3]
    cmp     x2, #0
    cset    x3, ne
    add     x9, x9, x3
    str     x5, [x21, x4, lsl #3]
    add     x4, x4, #1
    cmp     x4, x6
    b.lt    c_walk
    memcall 2, 32
    cmp     x0, #0
    b.ne    c_bad
    subs    x10, x10, #1
    b.ne    c_loop
    str     x9, [x20, #0x10]
    ldr     x3, =0xCCCC
    str     x3, [x20, #8]
    mov     x0, #0
    sys     SYS_EXIT
    b       .
c_bad:
    ldr     x3, =0xBAD
    str     x3, [x20, #0x18]
    mov     x0, #1
    sys     SYS_EXIT
    b       .

role_grow:
    ldr     x20, =DATA_VA
    mov     x0, #1
    sys     SYS_MEM
    str     x0, [x20, #0x10]
    memcall 3, 0
    str     x0, [x20, #0x18]
    mov     x0, #0
    sys     SYS_EXIT
    b       .

    .ltorg                              /* literals (ldr x, =imm) must lie INSIDE the copied image */
    .balign 4
user_mem_blob_end:
