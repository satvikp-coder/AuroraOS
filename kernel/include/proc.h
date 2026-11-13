/*
 * kernel/include/proc.h — user processes (EL0), address spaces, scheduler and system-call ABI.
 *
 * ARCHITECTURE (see docs/kernel/userspace.md for the reasoning):
 *   - Kernel boot context: runs at EL1 on SP_EL0 (EL1t), as before.
 *   - User processes: run at EL0 on their own SP_EL0 (a user stack in their private address space).
 *   - ALL exception handling (syscalls, faults, IRQs, the scheduler) runs on the shared SP_EL1 exception stack.
 *     A process's CPU state lives in a saved struct exc_frame; a context switch copies the next frame into the
 *     live frame and returns through the normal ERET path (which now also restores SP_EL0).
 *   - Every address space shares the kernel's L1 slots 0-1 by reference (EL1-only pages: EL0 accesses fault with a
 *     permission fault) and owns a private L1 slot for user memory (non-global pages tagged with the process ASID).
 */
#ifndef AURORA_PROC_H
#define AURORA_PROC_H
#include <arch.h>
#include <stddef.h>
#include <stdint.h>

/* ---- user virtual-address layout (identical in every process, different physical pages) ---- */
#define USER_VA_BASE   0x4000000000ULL          /* 256 GiB: L1 slot 256, disjoint from the kernel's slots 0-1 */
#define USER_VA_LIMIT  0x4040000000ULL          /* one 1 GiB slot */
#define USER_CODE_VA   (USER_VA_BASE + 0x0000)  /* RX, one page */
#define USER_DATA_VA   (USER_VA_BASE + 0x1000)  /* RW, one page of private data */
/* 0x2000 is an unmapped guard page */
#define USER_STACK_VA  (USER_VA_BASE + 0x3000)  /* RW, two pages */
#define USER_STACK_TOP (USER_VA_BASE + 0x5000)

/* ---- address spaces ---- */
enum { UPERM_RX = 1, UPERM_RW = 2, UPERM_RO = 3 };
#define ASPACE_MAX_PAGES 24
struct aspace {
    uint64_t l1_pa;
    uint32_t asid;                              /* 8-bit ASID (TCR_EL1.AS = 0); 0 is the kernel */
    uint64_t pages[ASPACE_MAX_PAGES];           /* every physical page owned: tables and user pages */
    int npages;
    int live;
};
int      aspace_create(struct aspace *as, uint32_t asid);
int      aspace_map(struct aspace *as, uint64_t va, int perm, const void *init, size_t len, uint64_t *pa_out);
void     aspace_destroy(struct aspace *as);     /* flushes the ASID's TLB entries, frees every owned page */
uint64_t aspace_ttbr(const struct aspace *as);

/* ---- processes ---- */
#define PROC_MAX 8
enum proc_state { PROC_UNUSED = 0, PROC_NEW, PROC_READY, PROC_RUNNING, PROC_BLOCKED, PROC_TERMINATED, PROC_FAULTED };
/* ---- bounded IPC: each process OWNS one fixed-capacity mailbox; messages are copied into kernel storage ---- */
#define IPC_MSG_MAX 64
#define IPC_QUEUE_DEPTH 4
struct ipc_msg { uint8_t from, len; uint8_t data[IPC_MSG_MAX]; };
struct mailbox { struct ipc_msg q[IPC_QUEUE_DEPTH]; uint8_t head, count; };

struct proc {
    int pid;                                    /* 1..PROC_MAX; also selects the ASID */
    enum proc_state state;
    struct exc_frame ctx;                       /* saved CPU context (valid unless RUNNING) */
    struct aspace as;
    uint64_t entry;
    int exit_code;
    uint64_t fault_esr, fault_far, fault_elr, fault_spsr;
    uint64_t code_pa, data_pa, stack_pa;
    uint64_t syscalls, ticks_run, switches_in, el0_exceptions, el0_confirmed;   /* el0_confirmed: SPSR said EL0t */
    struct mailbox mbox;                        /* messages addressed TO this process (owned by it; freed when it exits) */
    uint32_t principal;                         /* filesystem identity: set by kernel code only (proc_set_principal); PRINCIPAL_NONE until then */
    uint64_t gen;                               /* unique per process INSTANCE (never reused, unlike the pid/slot) */
    uint64_t ipc_grant_gen[PROC_MAX + 1];       /* [pid] = the generation of the recipient instance this process may send to; 0 = no grant */
    int rx_pending; uint64_t rx_buf, rx_max;    /* a blocking receive in progress (state BLOCKED) */
    uint64_t yields, preemptions;               /* voluntary switches (SYS_YIELD) vs timer-driven ones: never conflated */
    char name[16];
};
int  proc_create(int role, uint64_t arg1, const char *name);   /* pid > 0, or a negative error */
int  proc_create_image(int role, uint64_t arg1, const char *name, const void *image, size_t len);   /* same, with kernel-supplied code (<= 1 page) instead of the built-in blob */
/* Create a process from an executable file image (kernel/proc/elf.c validates it; see docs/kernel/loader.md). The principal is chosen by the CALLER, which
 * is always kernel code. Returns pid > 0 or PROC_ELOAD_*; for PROC_ELOAD_INVALID *elf_err is the elf_validate error. */
enum { PROC_ELOAD_ARG = -1, PROC_ELOAD_INVALID = -2, PROC_ELOAD_NOSLOT = -3, PROC_ELOAD_NOMEM = -4 };
int  proc_create_elf(const uint8_t *file, size_t len, const char *name, uint32_t principal, uint64_t arg, int *elf_err);   /* arg: kernel-chosen number passed in x0 */
struct proc *proc_get(int pid);
int  proc_destroy(int pid);                      /* NEW/READY/TERMINATED/FAULTED only; frees all memory */
const char *proc_state_name(enum proc_state s);
int  proc_transition_ok(enum proc_state from, enum proc_state to);

/* ---- scheduler ---- */
enum { SCHED_ALL_DONE = 0, SCHED_DEADLOCK = 1, SCHED_TIMEOUT = 2 };
uint64_t sched_run(uint64_t limit_ticks);        /* from kernel context: run processes until done/blocked/timeout; returns SCHED_* */
void     sched_set_preempt(int enabled, uint32_t quantum_ticks);
uint64_t sched_ticks(void);
int      sched_handle_el0(struct exc_frame *f);  /* exceptions from EL0 (kinds 8/9); returns 0 when handled */
int      sched_handle_kernel_svc(struct exc_frame *f, uint32_t imm);  /* SVC from kernel code; returns 0 if consumed */
void     sched_after_irq(struct exc_frame *f);   /* called after an IRQ was dispatched */
int      sched_active(void);

/* Evidence counters (single CPU). ttbr_*: on every exception from EL0 TTBR0_EL1 is compared with the running process's
 * expected value (table address + ASID). preempt_elr_in_user: a timer preemption interrupted an instruction inside the
 * process's code page (so the switch really happened in the middle of user code, not at a syscall). */
struct sched_stats { uint64_t ttbr_checks, ttbr_mismatch, preemptions, preempt_elr_in_user, switches, timeouts; };
const struct sched_stats *sched_get_stats(void);
void sched_stats_reset(void);

/* Observation log for tests: first byte of each user write, in order. */
#define SCHED_TRACE_MAX 128
const char *sched_trace(int *len);
void sched_trace_reset(void);

/* ---- system calls (SVC #0 from EL0): number in x8, arguments in x0-x5, result in x0 ---- */
#define SYS_WRITE  1   /* x0 = user buffer, x1 = length (<= SYS_WRITE_MAX): print one tagged line */
#define SYS_YIELD  2
#define SYS_GETPID 3
#define SYS_EXIT   4   /* x0 = exit code */
#define SYS_TICKS  5   /* scheduler ticks since the scheduler started */
#define SYS_SEND   6   /* x0 = destination pid, x1 = user buffer, x2 = length (<= IPC_MSG_MAX): 0 or an error */
#define SYS_RECV   7   /* x0 = user buffer, x1 = capacity, x2 = flags (1 = do not block): x0 = length or error, x1 = sender pid */
#define IPC_NONBLOCK 1
#define SYS_FS     8   /* x0 = operation (FSOP_*), x1..x4 arguments: see kernel/proc/fssys.c */
#define SYS_SETTINGS 9   /* x0 = operation (SETOP_*), x1 = setting id, x2 = value: see kernel/settings.c */
#define SYS_WRITE_MAX 256
#define AE_OK      0
#define AE_EINVAL (-1)
#define AE_EFAULT (-2)   /* pointer outside user memory, or not mapped for EL0 */
#define AE_E2BIG  (-3)
#define AE_ENOSYS (-4)
#define AE_EAGAIN (-5)   /* mailbox full (send) / empty (non-blocking receive) */
#define AE_ESRCH  (-6)   /* no such live process */
#define AE_EPERM  (-7)   /* sender not authorised for that destination */

/* ---- file system call (kernel/proc/fssys.c) ---- */
#define PRINCIPAL_NONE 0xFFFFFFFFu   /* no filesystem identity: every file operation is refused */
#define AE_FS_BASE (-100)            /* filesystem errors are returned as AE_FS_BASE + FS_E* (e.g. -107 = permission denied) */
enum { FSOP_CREATE = 1, FSOP_LOOKUP, FSOP_READ, FSOP_WRITE, FSOP_STAT, FSOP_LIST, FSOP_DELETE, FSOP_SET_PERM };
#define SYS_FS_IO_MAX   4096
#define SYS_FS_LIST_MAX 16
struct user_fs_stat { uint64_t id; uint32_t size, owner, perm, reserved; char name[32]; };   /* 56 bytes, written to user memory */
int  proc_set_principal(int pid, uint32_t principal);   /* kernel-only; 0 (the kernel) and PRINCIPAL_NONE are refused */
int64_t fs_syscall(struct proc *p, uint64_t op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
int64_t settings_syscall(struct proc *p, uint64_t op, uint64_t id, uint64_t value);   /* kernel/settings.c */

/* ---- safe user memory access ---- */
int copy_from_user(void *dst, uint64_t uva, size_t n);   /* AE_OK / AE_EINVAL / AE_EFAULT; never faults the kernel */
int user_range_ok(uint64_t uva, size_t n);
int copy_to_user(uint64_t uva, const void *src, size_t n);   /* same guarantees; needs write permission for EL0 */
int user_writable(uint64_t uva, size_t n);

/* ---- IPC (kernel/proc/ipc.c) ---- */
int  proc_ipc_allow(int from, int to);                                /* kernel-only grant: 0, or -1 unless BOTH are live processes; bound to that recipient instance */
int  ipc_send(struct proc *from, uint64_t dest, uint64_t ubuf, uint64_t len);   /* AE_OK or error; caller wakes a blocked receiver */
int  ipc_dequeue(struct proc *p, uint64_t ubuf, uint64_t max, uint64_t *from);  /* length, or AE_EAGAIN/AE_E2BIG/AE_EFAULT (message stays queued) */
void ipc_cleanup(struct proc *p);

/* ---- user programs (assembly blob copied into each process's code page) ---- */
extern uint8_t user_blob_start[], user_blob_end[];

#endif
