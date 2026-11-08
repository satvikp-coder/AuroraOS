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

## Not done (issue #4 stays open)

* **Timer preemption** is implemented in the scheduler (`sched_set_preempt`, tick-driven `need_resched`, applied on the
  exception-return path) but is **not tested yet**; only the cooperative scheduler and the watchdog are.
* **IPC** and **blocking** (so `BLOCKED` is reachable only by the table, not by any syscall) are not implemented.
* No TTBR1/higher-half kernel and no KPTI: kernel addresses are present (EL1-only) in every user table. That is correct for
  EL0 access control but offers no protection against speculative side channels. L1 slots beyond 0-1 are not shared, so new
  kernel mappings outside those slots would not appear in existing processes.
* No `PAN` (not present on Cortex-A53); `copy_from_user` is the only sanctioned user-memory path, but the kernel *could*
  dereference a user pointer by mistake.
* Single CPU only; no locking; 8 processes; one fixed program image; no `exec`/`fork`, no per-process kernel resources beyond
  memory; no file descriptors.
* Nothing has run on hardware.
