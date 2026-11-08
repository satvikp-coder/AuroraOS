# AuroraOS kernel: MMU configuration review

Scope: `boot/aarch64/start.S`, `kernel/arch/aarch64/vectors.S`, `kernel/mm/vmm.c`. Reference: Arm Architecture
Reference Manual for A-profile (DDI 0487), chapters D8 (VMSA), D17 (system registers). Verified on QEMU
`virt` with `-cpu cortex-a53` only. **Nothing here has been run on real hardware.**

## Register by register

| Register | Value | Why |
|---|---|---|
| `MAIR_EL1` | `Attr0=0x00`, `Attr1=0xFF` | Attr0 = Device-nGnRnE (UART, GIC). Attr1 = Normal, Inner and Outer Write-Back, non-transient, read/write-allocate. |
| `TCR_EL1.T0SZ` | 25 | 39-bit VA; a 4 KiB-granule walk with T0SZ 25..33 starts at level 1 (512 x 1 GiB entries). |
| `TCR_EL1.TG0` | `0b00` | 4 KiB granule for TTBR0 (TG0 encoding differs from TG1). |
| `TCR_EL1.IRGN0/ORGN0/SH0` | `0b01 / 0b01 / 0b11` | Walks are Write-Back cacheable, inner shareable; matches the Normal attribute used for the tables' memory. |
| `TCR_EL1.EPD1` | 1 | No TTBR1 walks; the kernel is identity-mapped in TTBR0 for now. |
| `TCR_EL1.TG1` / `T1SZ` | `0b10` (4 KiB) / 25 | `TG1 = 0b00` is a reserved encoding; set a valid one even though EPD1 disables walks (review finding). |
| `TCR_EL1.IPS` | `ID_AA64MMFR0_EL1.PARange`, capped at 48-bit | Never larger than the implemented physical width. |
| `TTBR0_EL1` | physical address of the L1 table | Page allocator returns zeroed, 4 KiB-aligned pages. |
| `SCTLR_EL1` | start `0x30d00800` (RES1 only), then `M|C|I|WXN` | MMU on, data and instruction caches on, and `WXN` makes any writable page non-executable even if a table is wrong. `A` (alignment check) stays 0; the build uses `-mstrict-align` because data accesses are Device memory until the MMU is on. |

## Enable sequence

1. Descriptors written with the MMU off (Device-type stores), then `dsb sy`.
2. `msr mair_el1; msr tcr_el1; msr ttbr0_el1; isb`.
3. `tlbi vmalle1; dsb nsh; isb`; `ic iallu; dsb nsh; isb` (no stale instruction lines when `SCTLR.I` is set).
4. `mrs/orr/msr sctlr_el1; isb`.

Single core, so non-broadcast (`nsh`) barriers are sufficient; revisit when SMP is added.

## Descriptors

Level 1/2 table = `0b11`; level 2 block = `0b01` (bit 1 clear); level 3 page = `0b11`. `AF=1` (hardware
AF/DBM is not enabled). `AP[2]=1, AP[1]=0` = read-only at EL1, no EL0 access; `AP=0b00` = read/write EL1 only.
`UXN=1` on every kernel mapping, `PXN=1` on everything except text. Output address mask `[47:12]`.
Hierarchical `APTable/XNTable/PXNTable` bits are 0. Section permissions:

| Region | Attributes |
|---|---|
| boot region below the image, data, bss, stack, rest of RAM | Normal WB, RW, `PXN|UXN` |
| text (incl. boot code and vectors) | Normal WB, RO, executable at EL1 |
| rodata | Normal WB, RO, `PXN|UXN` |
| stack guard page | **unmapped** (level-3 translation fault) |
| device-tree `no-map` regions | **unmapped** |
| GIC window (2 MiB), UART page | Device-nGnRnE, `PXN|UXN` |

## Physical-target policy (`kernel/mm/mmpolicy.c`)

Page-table construction (`vmm.c`) is separate from the question of *which physical memory may be mapped how*.
Every `vmm_map()` call, including the boot identity map, is checked by the pure function `mm_policy_check()`
before anything is allocated or written:

1. size 0 or `pa+size` overflow: denied.
2. any overlap with a device-tree **no-map** region: denied for every attribute.
3. **Device** attributes: only wholly inside a registered MMIO window (UART, GIC, plus anything registered with
   `vmm_register_mmio()`), never on RAM.
4. **Normal** attributes: only wholly inside RAM (never MMIO or unknown space); RW may not overlap kernel text or
   rodata; RX only inside kernel text (so no executable alias of writable memory); RO is always fine in RAM.

A request mixing protected and unprotected pages is judged as a whole. There is intentionally **no bypass flag**: a
future driver registers its window, it does not skip the check.

Found by review: before this policy a writable alias of text, rodata and a no-map page, and a Normal mapping of the
UART, all succeeded (observed on QEMU: `rc=0`, and a write through the alias translated). The goal is correct
mapping contracts, not protection against malicious EL1 code, which can rewrite its own page tables. Not yet
protected: aliases of the page-table pages themselves.

## EL2 to EL1

QEMU `virt` enters at EL1 unless started with `virtualization=on`; both are tested. At EL2 we set
`VPIDR_EL2/VMPIDR_EL2` from `MIDR/MPIDR` (otherwise EL1 reads UNKNOWN values), `HCR_EL2.RW=1` with every other
HCR bit 0 (no trapping; physical IRQ/FIQ/SError stay routed to EL1), `CNTHCTL_EL2=3`, `CNTVOFF_EL2=0`,
`CPTR_EL2=0x33ff`, `SPSR_EL2=0x3c5` (EL1h, DAIF masked), then `ERET`. `SCTLR_EL1` is set to its RES1 value
and `CPACR_EL1.FPEN=0b11`.

## Exception vectors

`VBAR_EL1` bits [10:0] are RES0, so the table is `.balign 2048`; each of the 16 entries is `.balign 128` and
only 16 bytes long. The handler saves a 288-byte frame (x0-x30, ELR, SPSR, ESR, FAR, kind) keeping SP 16-byte
aligned. All vectors currently end in a panic; there is no interrupt handling yet (issue #5).

## Assumptions specific to QEMU / Cortex-A53

* PARange is 40 bits (printed at boot); caches and TLBs are architecturally modelled loosely, so a missing
  cache or TLB maintenance would **not** be caught here.
* On real hardware, D-cache contents at reset are UNKNOWN; firmware normally invalidates them. We rely on that
  and do not run a set/way invalidate. Revisit before any hardware port.
* The DTB is passed in `x0` only because `kernel.bin` carries an arm64 Image header. The ELF gets no DTB, and
  the kernel then refuses to boot (tested).
* No hardware AF/dirty management, no PAN/UAO, no pointer authentication, no MTE: Cortex-A53 is plain ARMv8.0.

## Known limits (honest)

* **Stack guard page.** An unmapped guard page proves an access to it faults. Reliable *diagnosis* of a real overflow
  needs the exception entry to run on a different stack, which is now the case: see
  [interrupts-timers.md](interrupts-timers.md) (exception stack on SP_EL1, tested with a genuine recursion overflow).
* Page-table pages are never freed; there is no `vmm_unmap` yet.
* `vmm_map` is all-or-nothing against *validation* (conflicts, ranges, free pages for tables). It is not
  concurrent-safe; locking comes with interrupts/SMP.
* RAM is capped at 1 GiB; only one `/memory` region is supported (a device tree with more is rejected).
* `/reserved-memory` children that only give `size` (dynamic pools) are not reserved because they have no
  fixed address and the kernel never uses them.
