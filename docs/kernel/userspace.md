# AuroraOS kernel: EL0 processes, system calls and the scheduler

Verified on QEMU 11.1 `virt`, `-cpu cortex-a53`, one CPU. **Not run on hardware.** Issue #4 (partially: see "Not done").

## The stack problem and the chosen architecture

Before this work the kernel ran on `SP_EL0` (EL1t) so that every exception switched to a dedicated `SP_EL1` stack (see
[interrupts-timers.md](interrupts-timers.md)). A real EL0 program also needs `SP_EL0` for its own stack, so the old
convention could not simply be reused. The new one:

| Context | Exception level / stack |
|---|---|
| Kernel boot context (`kernel_main`, tests) | EL1t, `SP_EL0` = kernel stack (unchanged, still has an unmapped guard page) |
| User process | EL0t, `SP_EL0` = the process's user stack |
| **All exception handling** (syscalls, user faults, IRQs, scheduling) | EL1h on the shared `SP_EL1` exception stack |

* A process's CPU state lives in a saved `struct exc_frame` (x0-x30, ELR, SPSR, SP_EL0). **A context switch copies the
  next frame over the live frame and returns through the normal `ERET` path**; no kernel function "returns on another
  stack", and no per-process kernel stacks are needed (a blocked process is just a saved frame).
* The exception epilogue (`vectors.S`) now restores `SP_EL0` from the frame, so returning to a user stack, to the kernel
  stack, or to another process is the same code. (It previously only recorded it.)
* The kernel context enters the scheduler with `svc #0x5c4e` (`sched_run`): its frame is parked, and restored with the
  result in `x0` when nothing is runnable or the watchdog tick limit expires. This SVC arrives as kind 0 (EL1), which EL0 can
  never produce.
* Exceptions are distinguished by vector kind: 0/1 = kernel code (EL1t), 8/9 = from EL0 (synchronous / IRQ), 4-7 = taken while
  already on the exception stack (nested: panic), everything else panics. For kind 8 the saved `SPSR` must say EL0t or the
  kernel treats it as a bug.
* FP/SIMD: `CPACR_EL1.FPEN=01` (EL0 use traps). The kernel does not save FP state, so a user floating-point instruction is a
  fault (EC 0x07), not a silent cross-process register leak.

## Address spaces

* 39-bit VA, TTBR0 only. Each process has its own level-1 table. L1 slots 0 and 1 (devices and all RAM, i.e. the kernel image,
  stacks, exception stack, MMIO) are copied **by reference** from the kernel's master table; those descriptors are EL1-only
  (`AP[1]=0`), so any EL0 access is a permission fault, and they are never executable at EL0.
* Slot 256 (`USER_VA_BASE` = 0x4000000000) is private: code (EL0 read/execute, read-only), data (read/write, never
  executable), a guard page (unmapped), a two-page stack (read/write, never executable). Same virtual layout in every process,
  different physical pages. User pages are `nG` and tagged with the process ASID (8-bit, `TCR_EL1.AS=0`); the ASID is the pid.
* User pages can only reference pages handed out by the physical allocator (`pmm_is_allocated`), so a user mapping can never
  name kernel text, the device tree, `no-map` memory or MMIO. Every page (tables included) is tracked and freed by
  `aspace_destroy()`, which first flushes the ASID (`tlbi aside1is`). The allocator zeroes pages, and a test proves a new
  process reusing a dead process's pages sees only zeros.
* TTBR0 is switched in C on the exception stack, which is mapped identically in every table.

## System calls (`svc #0`, number in x8, args x0-x5, result x0)

`SYS_WRITE` (print one kernel-tagged line, <= 256 bytes), `SYS_YIELD`, `SYS_GETPID`, `SYS_EXIT`, `SYS_TICKS`. Errors are
negative (`AE_EINVAL -1`, `AE_EFAULT -2`, `AE_E2BIG -3`, `AE_ENOSYS -4`). Only `svc #0` is a syscall: any other SVC immediate is a
fault. Unknown numbers return `AE_ENOSYS` and the process continues.

**User pointers** never reach kernel code directly. `copy_from_user` rejects anything outside the user slot or with
`ptr+len` overflow, then for each page asks the hardware (`AT S1E0R`, using the caller's live tables) whether EL0 can read it and
copies through the kernel's physical identity map. A bad pointer therefore cannot fault the kernel, and kernel-only pages are
refused because their EL0 translation fails. Console output from a process is always one line starting with a tag the kernel
writes (`[user pN] `), control bytes are replaced, so user text cannot forge kernel or test lines.

The test-only SVC hook (`#0x7e57`, register round trip) is compiled only when `AURORA_SELFTEST_HOOKS` is defined (default; absent
with `AURORA_PRODUCTION=1`) and is reachable only from kernel code.

## States

`NEW -> READY -> RUNNING -> {READY, BLOCKED, TERMINATED, FAULTED}`; `BLOCKED -> READY`; dead states only go to `UNUSED`. The table
is enforced by `proc_transition_ok`; an illegal transition is a kernel bug and panics. A `FAULTED` process keeps its frame for
inspection, is never scheduled again, and a test checks it executed nothing after the fault.

## Evidence (QEMU, real EL0 accesses and real syndromes)

`[proc-test]` has 35 checks; `tests/qemu/process-tests.sh` runs them at 128/256/512 MB and with EL2 entry. Highlights: every
exception from the programs arrives with SPSR=EL0t; a user read of kernel data / write to kernel text / read of the UART /
read of another process's page by physical address are permission faults with the exact `FAR`; `0xDEAD0000` is a translation
fault; user code is not writable; user data is not executable (instruction abort); the guard page faults; `mrs sctlr_el1` is
UNDEFINED (EC 0x00); FP traps (EC 0x07); in each case a healthy partner process finishes correctly. Two processes alternate
strictly and their x23-x28 survive every yield. A kernel fault after processes ran still panics (fault case 8).

## Timer preemption (tested)

`sched_set_preempt(1, quantum)` makes the scheduler request a switch every `quantum` timer ticks (1 kHz). The request is applied
in `sched_after_irq`, i.e. inside the IRQ exception, by saving the interrupted frame and installing the next process's frame, so
the switch completes through the normal `ERET`. Counters keep cooperative and timer switches apart (`yields` vs `preemptions`).
Tests use a program that never yields and verify its registers (pattern derived from its pid) and SP_EL0 after every burst, that
TTBR0/ASID matched the running process on every EL0 exception, that each preemption interrupted real user code, shorter quanta switch
proportionally more, the watchdog still fires, and 12 rounds of create/run/destroy leak nothing. Disabling the resched request fails
5 of those checks. Assumptions: single CPU, one 1 kHz scheduler tick shared with the watchdog, IRQs masked throughout the kernel's
exception handling (no kernel preemption).

## Bounded IPC and blocking

* **Mailboxes:** each process owns one fixed mailbox (4 messages x 64 bytes) in the kernel's process table. `SYS_SEND(dest, buf, len)`
  validates length, that dest is a live process, that the sender is authorised (an explicit bit granted with `proc_ipc_allow`; self
  is always allowed), that the mailbox is not full, and only then copies the bytes with `copy_from_user`. No user pointer is kept.
  `SYS_RECV(buf, cap, flags)` can only dequeue from the caller's OWN mailbox (no syscall names another process's queue), copies with
  `copy_to_user`, and returns the length in x0 and the sender's pid in x1. If the copy fails or the buffer is too small the message
  stays queued. New errors: `-EAGAIN` (full / empty non-blocking), `-ESRCH` (no live process), `-EPERM` (not authorised).
* **Blocking receive without busy-waiting:** with an empty mailbox the process is parked as `BLOCKED` (its saved frame; no kernel
  stack is held). A send to a blocked receiver makes it `READY`; its receive completes when it is next scheduled, with ITS page tables
  live, so the copy into its buffer uses the normal hardware EL0 check. If nothing is runnable and some process is blocked,
  `sched_run` returns `SCHED_DEADLOCK` to the kernel instead of hanging.
* **Authorisation lifetime (pid reuse):** a grant records the **generation** of the recipient instance (a 64-bit counter assigned at
  process creation and never reused), not just its pid. When a recipient exits, faults or is destroyed and its pid is reused, the
  new process has a different generation, so every old grant fails to match (`-EPERM`); `ipc_cleanup` also erases grants that
  named the ended process. `proc_ipc_allow` is kernel-only (there is no system call: numbers 8-40 are all `-ENOSYS`), records
  nothing unless BOTH processes are live, and a new process starts with no grants of its own (the process slot is zeroed).
  Receive flags other than `IPC_NONBLOCK` are `-EINVAL`; a zero-length message is legal.
* **Lifetime:** queued messages survive their sender; a receiver's mailbox is wiped when it exits, faults or is destroyed, and later
  sends to it return `-ESRCH`.
* Limits: no capability to receive from a specific sender, no timeouts on blocking receive, no message priorities, no shared memory.

## Not done (issue #4 stays open)

* No TTBR1/higher-half kernel and no KPTI: kernel addresses are present (EL1-only) in every user table. That is correct for
  EL0 access control but offers no protection against speculative side channels. L1 slots beyond 0-1 are not shared, so new
  kernel mappings outside those slots would not appear in existing processes.
* No `PAN` (not present on Cortex-A53); `copy_from_user` is the only sanctioned user-memory path, but the kernel *could*
  dereference a user pointer by mistake.
* Single CPU only; no locking; 8 processes; one fixed program image; no `exec`/`fork`, no per-process kernel resources beyond
  memory; no file descriptors.
* Nothing has run on hardware.

## File access from EL0 (SYS_FS = 8, issue #6 milestone C)

Depends on AuroraFS (PR #28). Implemented in `kernel/proc/fssys.c`; tests in `kernel/fsproctest.c` (run by `tests/qemu/fs-tests.sh`).

**Identity model.** The filesystem principal of a caller is `struct proc::principal`, assigned only by kernel code (`proc_set_principal`) when a process is created. EL0 never supplies, reads or changes it; no system call touches it; `0` (the kernel) and `PRINCIPAL_NONE` (`0xFFFFFFFF`, the default for every new process) can never be assigned, and a process without an identity gets `AE_EPERM` for every file operation. The principal is a durable **application identity**, i.e. the number stored in a file's owner field on disk. It is deliberately *not* the pid, the ASID or the process generation: pids are reused and generations restart at 1 on every boot, so an owner derived from them would pass to the next occupant of a pid, or to a different program after a reboot. Whoever creates a process chooses its identity (today the kernel tests; milestone D's loader will take it from the owner of the executable's file, so the identity follows the program file, which is itself permission-protected). A process that is given the *same* identity as a previous one legitimately has the same access; a process in a recycled pid slot that is given a different identity (or none) has none (tested).

**ABI.** `x8 = 8`, `x0 = operation`, `x1..x4` arguments, result in `x0`. Operations: 1 CREATE(name, len, perm) -> id, 2 LOOKUP(name, len) -> id, 3 READ(id, off, buf, len<=4096), 4 WRITE(id, off, buf, len<=4096), 5 STAT(id, struct user_fs_stat*), 6 LIST(struct user_fs_stat*, max<=16) -> count, 7 DELETE(id), 8 SET_PERM(id, perm). Ids are used directly (no handle table, hence no close): they are not secrets, every call checks permissions, and an id carries a generation so a stale id never names a newer file. Results `>= 0` are values; negative values are the generic `AE_*` codes (`AE_EFAULT` bad user pointer, `AE_EINVAL` bad argument or unknown operation, `AE_E2BIG`, `AE_EPERM` no identity) or `AE_FS_BASE (-100) + FS_E*` (e.g. `-107` permission denied, `-102` not found). Names are copied to the kernel, 1-31 bytes, no NUL, then validated by the filesystem. There is no format, mount or raw-block operation. Syscalls 9-40 are still `-ENOSYS`.

**Memory safety.** User buffers are touched only through `copy_from_user` / `copy_to_user` / `user_writable`; destination buffers are checked writable *before* the file is read; lengths and offsets are bounded before any arithmetic; file data goes through a single 4 KiB kernel bounce buffer (syscalls run with interrupts masked on one CPU).

**Verified in QEMU (real EL0 programs):** program A (identity 21) creates `note` (private) and `pub` (other-readable), writes and reads back; program B (identity 22), even holding the exact id, is refused lookup/read/write/delete/chmod on `note`, may read but not write `pub`; a process with no identity is refused everything; hostile arguments (kernel pointers for read/name/write/list/stat, length -1, offset -1, unknown/"format"-like operation numbers, 40-byte and empty names) fail safely with the documented codes and leave the file and kernel secret untouched; PID reuse inherits nothing; after restarting QEMU on the same disk a new process with identity 21 reads the persisted bytes. Mutation check: treating every caller as identity 21 fails 8 of the 16 checks.

**Not done / limits.** Identities are assigned by kernel code only; there is no login, user database, or capability passing. One 4 KiB transfer per call; 24 KiB files; whole-file rewrite on every write; no handles, no directories, no timestamps; QEMU only.
