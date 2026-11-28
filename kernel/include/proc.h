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
#include <fpsimd.h>

/* ---- user virtual-address layout: kernel/include/uvlayout.h (constants, guards and the one mappability predicate) ---- */
#include <uvlayout.h>

/* ---- address spaces ---- */
enum { UPERM_RX = 1, UPERM_RW = 2, UPERM_RO = 3 };
/* Maximum pages an address space can own: tables (L1+L2+L3=3) + code+data+stack(4) + heap(USER_HEAP_MAX_PAGES) + ELF segments.
 * We cap it at a reasonable bound; the actual pages array is dynamically allocated from the PMM. */
#define ASPACE_MAX_PAGES_CAP (24 + (int)USER_HEAP_MAX_PAGES + 32)  /* 88 pages max, fits in one page (512 entries) */

struct aspace {
    uint64_t l1_pa;
    uint32_t asid;                              /* 8-bit ASID (TCR_EL1.AS = 0); 0 is the kernel */
    uint64_t *pages;                            /* dynamically allocated array of owned physical pages */
    int npages;
    int capacity;                               /* allocated capacity of pages array */
    int live;
};
int      aspace_create(struct aspace *as, uint32_t asid);
int      aspace_map(struct aspace *as, uint64_t va, int perm, const void *init, size_t len, uint64_t *pa_out);
int      aspace_map_ro(struct aspace *as, uint64_t va, uint64_t pa);   /* map reserved asset page read-only (EL0 RO, PXN/UXN, nG) */
int      aspace_unmap_ro(struct aspace *as, uint64_t va);             /* unmap read-only asset page (does not free the asset page) */
int      aspace_map_at(struct aspace *as, uint64_t va, uint64_t pa, int perm); /* map a specific physical page at VA (permissions: UPERM_RW/RO), no data init, tracks table pages only */
int      aspace_unmap_at(struct aspace *as, uint64_t va);             /* unmap a page mapped via aspace_map_at: clears PTE, flushes TLB, does NOT free the physical page */
int      aspace_unmap(struct aspace *as, uint64_t va);                /* unmap and free one mapped user page: 0, or -2 (bad address) / -5 (not mapped); flushes its TLB entry */
int      aspace_ro_asset_mapped(struct aspace *as, uint64_t va);
int      aspace_is_mapped(struct aspace *as, uint64_t va);            /* check if VA is mapped: 1=mapped, 0=not, -1=error */
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
    struct fpstate fp;                          /* saved FP/SIMD registers (valid unless this process owns the live registers); zero for a new process */
    struct aspace as;
    uint64_t entry;
    int exit_code;
    uint32_t heap_pages;                        /* pages currently mapped at USER_HEAP_VA (SYS_MEM), 0..USER_HEAP_MAX_PAGES */
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

    /* ---- Bounded large allocation region (MEM-03) ------------------- */
    uint32_t bounded_pages;                     /* total pages in bounded regions owned by this process */
    uint32_t bounded_count;                     /* number of bounded regions owned by this process */
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
/* The interactive shell's entry: run READY processes for at most `slice_ticks` (1..1000) scheduler ticks (1000 Hz while it runs), then return to the
 * kernel context with SCHED_TIMEOUT (a process was still runnable; it is READY again and resumes in the next slice), SCHED_ALL_DONE or SCHED_DEADLOCK.
 * Unlike sched_run, the tick count (SYS_TICKS) and the round-robin position persist across calls, so every runnable process gets its turn. Like
 * sched_run it stops the timer on return: the caller restarts its own. */
uint64_t sched_run_slice(uint64_t slice_ticks);
int      sched_has_ready(void);                  /* 1 if any process is READY (so a slice would do work) */
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
#define SYS_MEM    10  /* x0 = operation (MEMOP_*), x1 = page count: a bounded user heap; see kernel/proc/memsys.c */
enum { MEMOP_GROW = 1, MEMOP_SHRINK, MEMOP_INFO,
       MEMOP_BOUNDED_ALLOC = 10,  /* x1 = size | (perm << 48), returns virtual address or error */
       MEMOP_BOUNDED_FREE = 11,   /* x1 = allocation base virtual address, returns 0 or error */
       MEMOP_BOUNDED_INFO = 12    /* returns (budget_pages << 32) | allocated_pages */
};
#define SYS_SETTINGS 9   /* x0 = operation (SETOP_*), x1 = setting id, x2 = value: see kernel/settings.c */
#define SYS_RO_MAP   12  /* x0 = operation (RO_*), x1..x2 arguments: read-only asset mapping; see kernel/proc/rosys.c */
enum { ROOP_MAP = 1,      /* x1 = asset_id, x2 = requested VA (0 = kernel-chosen); returns VA or error */
       ROOP_UNMAP = 2,    /* x1 = VA, x2 = size (pages); returns 0 or error */
       ROOP_INFO = 3      /* x1 = asset_id; returns (asset_size_pages << 32) | asset_flags or error */
};
#define SYS_WRITE_MAX 256
#define AE_OK      0
#define AE_EINVAL (-1)
#define AE_EFAULT (-2)   /* pointer outside user memory, or not mapped for EL0 */
#define AE_E2BIG  (-3)
#define AE_ENOSYS (-4)
#define AE_EAGAIN (-5)   /* mailbox full (send) / empty (non-blocking receive) */
#define AE_ESRCH  (-6)   /* no such live process */
#define AE_EPERM  (-7)   /* sender not authorised for that destination */
#define AE_ENOMEM (-8)   /* out of memory, over the process's budget, or the kernel's reserve would be used */
#define AE_EIO    (-9)   /* the device failed a read the kernel had already bounded (SYS_MODEL) */

/* ---- file system call (kernel/proc/fssys.c) ---- */
#include <principal.h>
#define PRINCIPAL_NONE 0xFFFFFFFFu   /* no filesystem identity: every file operation is refused */
#define AE_FS_BASE (-100)            /* filesystem errors are returned as AE_FS_BASE + FS_E* (e.g. -107 = permission denied) */
enum { FSOP_CREATE = 1, FSOP_LOOKUP, FSOP_READ, FSOP_WRITE, FSOP_STAT, FSOP_LIST, FSOP_DELETE, FSOP_SET_PERM };
#define SYS_FS_IO_MAX   4096
#define SYS_FS_LIST_MAX 16
struct user_fs_stat { uint64_t id; uint32_t size, owner, perm, reserved; char name[32]; };   /* 56 bytes, written to user memory */
int  proc_set_principal(int pid, uint32_t principal);   /* kernel-only; 0 (the kernel) and PRINCIPAL_NONE are refused */
int  proc_set_service_principal(int pid, uint32_t principal);   /* kernel-only; accepts ONLY principal_service_holdable() (PRINCIPAL_INFER), else -1 */
int64_t fs_syscall(struct proc *p, uint64_t op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
int64_t mem_syscall(struct proc *p, uint64_t op, uint64_t n);   /* SYS_MEM (kernel/proc/memsys.c) */
int64_t settings_syscall(struct proc *p, uint64_t op, uint64_t id, uint64_t value);   /* kernel/settings.c */
int64_t ro_syscall(struct proc *p, uint64_t op, uint64_t a1, uint64_t a2);   /* SYS_RO_MAP (kernel/proc/rosys.c) */

/* ---- safe user memory access ---- */
int copy_from_user(void *dst, uint64_t uva, size_t n);   /* AE_OK / AE_EINVAL / AE_EFAULT; never faults the kernel */
int user_range_ok(uint64_t uva, size_t n);
int copy_to_user(uint64_t uva, const void *src, size_t n);   /* same guarantees; needs write permission for EL0 */
int user_writable(uint64_t uva, size_t n);

/* ---- current process principal (for capability checks from kernel subsystems) ---- */
/* Use kernel_identity only when no process owns the CPU. An unassigned or
 * missing process with a nonzero scheduler PID always yields PRINCIPAL_NONE. */
uint32_t proc_current_principal_or(uint32_t kernel_identity);

/* ---- IPC (kernel/proc/ipc.c) ---- */
int  proc_ipc_allow(int from, int to);                                /* kernel-only grant: 0, or -1 unless BOTH are live processes; bound to that recipient instance */
int  ipc_send(struct proc *from, uint64_t dest, uint64_t ubuf, uint64_t len);   /* AE_OK or error; caller wakes a blocked receiver */
int  ipc_dequeue(struct proc *p, uint64_t ubuf, uint64_t max, uint64_t *from);  /* length, or AE_EAGAIN/AE_E2BIG/AE_EFAULT (message stays queued) */
void ipc_cleanup(struct proc *p);

/* ---- user programs (assembly blob copied into each process's code page) ---- */
extern uint8_t user_blob_start[], user_blob_end[];
#ifdef AURORA_SELFTEST_HOOKS
extern uint8_t user_fp_blob_start[], user_fp_blob_end[];   /* kernel/proc/user_fp_progs.S: FP/SIMD test programs (test kernels only) */
extern uint8_t user_mem_blob_start[], user_mem_blob_end[];   /* kernel/proc/user_mem_progs.S: SYS_MEM test programs (test kernels only) */
#endif

#endif
