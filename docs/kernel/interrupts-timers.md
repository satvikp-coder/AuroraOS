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
| FIQ, SError, EL0, AArch32 | panic (none expected yet) | by construction |

## Limits (honest)

* One CPU: no per-CPU exception stacks, no locking, no IPIs. IRQ priorities exist but nesting is not supported (handlers run with IRQs masked).
* **SGIs cannot be disabled on a GICv2** (their enable bits are fixed), so the "unknown source is disabled" protection only applies to PPIs/SPIs; tests use an SPI for that.
* SPI trigger configuration (`GICD_ICFGR`) is left at its reset value; no SPI-based device exists yet.
* An overflow of the *exception* stack itself is detected only if the overflowing access faults in a way the (already corrupted) handler can still print; it cannot be made fully reliable without a third stack.
* `timer_oneshot`/`timer_start_periodic` use the physical timer only; the virtual timer ID is parsed but unused.
* Tests assert ordering properties (never early, exactly one delivery, masked means pending) and use 2 s of *counter* time only as an upper bound for "it happened at all". They do not assert wall-clock accuracy.
