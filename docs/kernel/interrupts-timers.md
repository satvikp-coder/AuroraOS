# AuroraOS kernel: interrupts, timer and exception stack

Verified on QEMU 11.1 `virt`, `-cpu cortex-a53`, single CPU. **Not run on hardware.** Issue #5.

## Audit of the starting point (main at b68853d)

| Item | State |
|---|---|
| Exception level | EL1. With `-machine virt,virtualization=on` the CPU starts at EL2 and `start.S` ERETs to EL1 (both tested). |
| Masking at boot | `msr daifset, #0xf`: D, A, I, F all masked. Nothing unmasks them until the kernel does. |
| Vectors | `vectors.S`: 16 entries, 2 KiB-aligned table, `VBAR_EL1` set by `exceptions_init()`. All entries saved a frame and called one handler that always panicked. |
| Stack | One kernel stack used for both kernel code and exception entry, so a stack overflow could not be diagnosed. |
| Device tree | QEMU `virt` DTB (passed in `x0` thanks to the arm64 Image header). |

What the QEMU DTB actually says (dumped with `-machine virt,dumpdtb=`, read with `dtc`; nothing is assumed):

* `intc@8000000`, `compatible = "arm,cortex-a15-gic"` = **GICv2**. `reg` = distributor `0x08000000` (64 KiB), CPU interface `0x08010000` (64 KiB). (`gic-version=3` would be a different node; this kernel recognises it and refuses to start rather than guess.)
* `timer`, `compatible = "arm,armv8-timer"`, `interrupts` = four `<1 N 0x104>` PPI triples for N = 13, 14, 11, 10: secure physical, **non-secure physical**, virtual, hypervisor. GIC interrupt ID = 16 + N, so the EL1 physical timer is **ID 30** and the virtual timer ID 27.
* `CNTFRQ_EL0` reads 62,500,000 Hz.

## Design

```
vectors.S  --kind 1 (IRQ from kernel code on SP_EL0)-->  exceptions.c  -->  irq.c (generic dispatch)
                                                                               |  irq_controller_ops
                                                                               v
                                                           gicv2.c (GICv2)    timer.c (ARM generic timer)
```

* **Generic vs architecture code.** `irq.c` owns the handler table, statistics and the spurious/unknown policy and knows nothing about GIC registers; `gicv2.c` supplies an `irq_controller_ops` (ack, eoi, enable, disable). Swapping in a GICv3 driver means a new ops table.
* **GICv2** (ARM IHI 0048B): distributor and CPU interface bases/sizes come from the DT; both windows are registered with the physical-target mapping policy (`vmm_register_mmio`) and mapped as Device memory. The boot-time 2 MiB GIC mapping was **removed**: the driver now declares exactly what it uses, and the policy refuses cacheable aliases of those windows or Device mappings next to them (tested). Priority mask 0xF0, every enabled interrupt at priority 0xA0, SPIs targeted at CPU 0.
* **IRQ dispatch:** acknowledge (`GICC_IAR`), run the handler with IRQs masked, end-of-interrupt (`GICC_EOIR` with the raw IAR value). *Spurious* (ID >= 1020): counted, no EOI. *Unknown* (valid ID, no handler): counted, the line is **disabled** so a level-triggered source cannot storm the CPU, then EOI.
* **Timer:** EL1 physical timer (`CNTP_CVAL_EL0` absolute compare, `CNTP_CTL_EL0`). Periodic ticks rearm from the *previous deadline* (no drift); if more than a period late, the missed periods are counted and the deadline moves to now+period instead of bursting. Comparisons use signed 64-bit differences, so counter wraparound cannot mis-order values.
* **Exception stack (the review's open item):** kernel code runs on **SP_EL0** (`SPSel=0`). Any exception to EL1 switches to **SP_EL1**, which points at a dedicated 8 KiB exception stack with its own unmapped guard page. Entry therefore never writes to the (possibly exhausted) kernel stack, and no register is touched before it is saved. The frame (304 bytes, 16-byte aligned) also records the interrupted `SP_EL0`.

## What is supported, precisely

| Case | Behaviour | Tested by |
|---|---|---|
| IRQ from kernel code (kind 1) | dispatched, then ERET to the interrupted code | irq-tests: timer one-shot/periodic, SGI |
| Synchronous exception from kernel code (kind 0) | decoded diagnostics, panic | mm-tests faults 1-5 |
| Kernel stack overflow | diagnostics from the exception stack, "STACK OVERFLOW detected", FAR in the guard page | irq-tests fault 6 (real recursion) |
| Exception while already on the exception stack (kinds 4-7), e.g. a fault in a handler | reported as **NESTED**, panic | irq-tests fault 7 (fault inside the timer handler) |
| Misaligned/out-of-range frame | reported, panic | (guarded in code; no test can provoke it without corrupting SP_EL1) |
| SVC #0x7e57 from kernel code | self-test hook: handler edits the saved frame and returns | irq-tests: register round trip |
| FIQ, SError, EL0, AArch32 | panic (none expected yet) | by construction |

## Limits (honest)

* One CPU: no per-CPU exception stacks, no locking, no IPIs. IRQ priorities exist but nesting is not supported (handlers run with IRQs masked).
* **SGIs cannot be disabled on a GICv2** (their enable bits are fixed), so the "unknown source is disabled" protection only applies to PPIs/SPIs; tests use an SPI for that.
* SPI trigger configuration (`GICD_ICFGR`) is left at its reset value; no SPI-based device exists yet.
* An overflow of the *exception* stack itself is detected only if the overflowing access faults in a way the (already corrupted) handler can still print; it cannot be made fully reliable without a third stack.
* `timer_oneshot`/`timer_start_periodic` use the physical timer only; the virtual timer ID is parsed but unused.
* Tests assert ordering properties (never early, exactly one delivery, masked means pending) and use 2 s of *counter* time only as an upper bound for "it happened at all". They do not assert wall-clock accuracy.

## Hardening round (independent review of the first PR head)

### Stack initialisation no longer assumes how the CPU was entered

`start.S` used `mov sp, x0` to set the exception stack, which writes *whichever* stack pointer `SPSel` selects. That
was only correct because QEMU enters with `SPSel=1`. Entered with `SPSel=0`, `SP_EL0` would have been set to the
exception stack and `SP_EL1` left at an UNKNOWN value: the first exception would run on garbage. Now (Arm ARM D1.6,
"Exception entry": the target EL's stack pointer is selected on entry; `SPSel` chooses `SP_EL0`/`SP_ELx` for code
running at ELx):

```
msr spsel, #1 ; mov sp, <exception stack top>   ; "sp" is SP_EL1, explicitly
msr sp_el0, <kernel stack top>                    ; SP_EL0 by name
msr spsel, #0 ; isb                               ; kernel runs on SP_EL0
```
Both tops are 16-byte aligned by `linker.ld`; both guard pages stay unmapped (tested). The entry EL and `SPSel` are
recorded (`[info] boot entry: ...`) so tests assert what QEMU **actually** did.

What each test really exercised in QEMU:

| Entry | How | Observed |
|---|---|---|
| default, EL1 | plain `-kernel` | `EL1, SPSel=1` |
| default, EL2 | `virtualization=on`, `ERET` to EL1h | `EL2, SPSel=1` |
| **controlled** `AURORA_TEST_ENTRY=spsel0` | build-time test shim: at EL1 entry select `SPSel=0` and put junk in both stack pointers | `EL1, SPSel=0`; full suite passes |
| **controlled** `AURORA_TEST_ENTRY=el1t` | build-time test shim: `SPSR_EL2` = EL1t | `EL2, SPSel=0`; full suite passes |

Negative control (not committed): with the old assume-`SPSel=1` code, the default entry still passes everything, but
the `spsel0` variant produces no kernel output at all. So the default tests alone could never have caught this.
Entry states the emulator did not exercise (e.g. EL3, firmware-specific stack states) are **not** claimed.

Exception entry preserving interrupted state is tested by `exc_selftest.S`: it loads a distinct pattern into x1-x28
(x19/x29/x30 are used by the test), takes a real `SVC` from kernel code, lets the handler rewrite x0 and x27 **in the
saved frame**, and checks every register on return, that x0/x27 carry the handler's values (so registers are restored
*from the frame*), and that `SP_EL0` and `SPSel` are unchanged.

### Timer reprogramming is safe against its own interrupt

`period`, `deadline`, `periodic` and `CNTP_*` are shared with the tick handler. Every `timer_start_periodic`,
`timer_oneshot` and `timer_stop` now runs inside `irq_save()`/`irq_restore()`:

1. validate the request (an invalid request returns `TIMER_ERR_ARG` and changes nothing; a running timer keeps running);
2. `irq_save()` masks IRQs and remembers the caller's mask state;
3. `CNTP_CTL=0` (stops the timer and deasserts the level-sensitive line), clear any pending interrupt for the OLD
   deadline at the GIC, write the new deadline, enable last;
4. `irq_restore()` unmasks **only if the caller had IRQs enabled**. Called with IRQs masked, or from inside the tick
   handler itself, they stay masked.

Intervals: a one-shot must be 1..2^62 ticks; a periodic rate must give a period of at least 10 microseconds
(`TIMER_MIN_PERIOD_HZ`, to stop an interrupt storm) and at most 2^62 ticks. Deadlines are compared with signed 64-bit
differences, which is only valid below 2^63 ticks, so 2^62 leaves a full 2^62 of margin.

Tests (real interrupts): mask state preserved by every call; a probe proving the register updates run with IRQs
masked (removing the critical sections fails 8 tests); replacing periodic by one-shot **from inside the tick handler**;
periodic to faster periodic; periodic to one-shot (fires exactly once more); one-shot to periodic (the old long
deadline is discarded: the first tick is at the new deadline); stop with a stale pending condition; 100 rapid
start/one-shot/stop cycles; replacing a timer exactly after its deadline passed (no stale tick leaks); invalid and
excessive intervals leave a running timer untouched; the limit values are accepted.

Honest note from mutation testing: on QEMU's GICv2 model, writing `CNTP_CTL=0` already deasserts the level line, so
the explicit `irq_clear_pending()` is **defence in depth** that these tests cannot distinguish from its absence. It is
kept because real GICs may latch a pending state, but no test proves it is necessary.

### Single-CPU assumptions, and what SMP will need

* `irq_save()` protects only against the handler on **this** CPU. It is not a lock.
* `CNTP_*` registers are per-CPU, so the timer state (`period`, `deadline`, `periodic`, history) must become **per-CPU**
  (or be protected by a spinlock taken with IRQs masked) before a second CPU is started.
* If one CPU may ever reprogram another's timer, that needs an IPI-based protocol; the current API does not allow it.
* `timer_history()` returns a pointer into live state; callers must read it with IRQs masked or with the timer stopped.
* The GIC distributor writes (`ISENABLER`, `ICPENDR`) would need serialisation between CPUs, and `ITARGETSR` routing.
