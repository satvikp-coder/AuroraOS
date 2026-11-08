# Storage: virtio block driver (issue #6, milestone A)

Status: block I/O works in QEMU only. There is no filesystem yet (milestone B), no EL0 file access (C), no loader (D) and no
settings registry (E). Nothing here has run on hardware.

## Device selection (verified, not assumed)

QEMU `virt` exposes **32 virtio-mmio transports**, found by dumping the machine's own device tree
(`-machine virt,dumpdtb=...`): nodes `virtio_mmio@a000000` ... `virtio_mmio@a003e00`, 0x200 bytes each, `compatible =
"virtio,mmio"`, `interrupts = <0 N 1>` (SPI N, GIC ID 32+N). QEMU puts a `-device virtio-blk-device` on the **last free**
transport (observed: `0xa003e00`, SPI 47), so the driver never hard-codes an address: `fdt_virtio_mmio()` enumerates the
nodes from the validated device tree and the driver probes each one's `MagicValue` ("virt"), `DeviceID` (2 = block) and
`Version`.

* **Transport:** modern virtio-mmio (**Version 2**, requires `VIRTIO_F_VERSION_1`). QEMU defaults to the legacy transport for
  these devices, so the tests pass `-global virtio-mmio.force-legacy=false`. A legacy (Version 1) block device is detected,
  reported with that remedy, and **refused**; the kernel keeps booting. Supporting legacy would be a separate, tested
  driver path, not a silent fallback.
* **Written from the VIRTIO 1.x specification** (device status 2.1, split virtqueues 2.7, initialisation 3.1, MMIO 4.2, block
  5.2). No code copied from Linux or another driver.

## Driver (`kernel/drivers/virtio_blk.c`, interface `kernel/include/blk.h`)

* **Mapping:** the pages named by the device tree are declared to the MMU policy (`vmm_register_mmio`) and mapped
  **Device-nGnRnE, no-execute**, read/write, EL1 only.
* **Initialisation:** reset (wait for Status 0, bounded) -> ACKNOWLEDGE -> DRIVER -> read both feature words -> require
  `VERSION_1` -> accept only `VERSION_1`, `BLK_F_RO`, `BLK_F_FLUSH` -> FEATURES_OK and **re-read** it (the device may refuse)
  -> one queue of 8 descriptors, `QueueReady` -> DRIVER_OK. Capacity is read from config space with the `ConfigGeneration`
  consistency loop.
* **Requests:** one in flight. Chain = 16-byte header, data (omitted for flush), 1 status byte. `dsb sy` before the available
  index update and before reading the used index. Data goes through a kernel-owned bounce page (8 sectors max per request;
  longer transfers loop). Caller buffers are never given to the device.
* **Completion:** polled with a **bounded wait** (default 1 s, timer-based). On timeout the driver resets and re-initialises
  the device and returns `BLK_ETIMEOUT`; a device reporting `NEEDS_RESET`/`FAILED` or an unexpected used-ring id fails the
  request (`BLK_EIO`/`BLK_EPROTO`) and disables the device rather than guessing. The status byte is pre-set to a value the
  device never writes, so a request cannot "complete" without the device having answered. Interrupts are deliberately not
  used yet.
* **Checks before any device access:** not present -> `BLK_ENODEV`; NULL buffer or zero count -> `BLK_EARG`; range outside
  the capacity (overflow-safe) -> `BLK_ERANGE`; write to a read-only device -> `BLK_ERO`. Rejected requests never reach the
  device (the request counter proves it).
* **No disk is normal:** `BLK_ENODEV` is a supported situation and the rest of the boot is unaffected.
* A normal kernel **never writes** to an attached disk. The destructive test suite (`kernel/blktest.c`) exists only in
  kernels built with `AURORA_BLK_TEST=1` and is run only against image files created by the test script.

## Tests

`tests/qemu/storage-tests.sh` (53 checks; also in CI) and 11 new host checks in `tests/kernel-host/fdt_pmm_test.c`:

| Scenario | What is proven |
|---|---|
| no disk | normal boot, "no block device attached", userspace suite still 79/79 |
| destructive driver tests (13 checks) | identify, 1/3/20-sector round trips, neighbours untouched, 40 rewrite cycles, first/last sector, bounds + argument errors, rejected requests never reach the device, flush, **lost doorbell -> timeout -> reset -> recovery with old data intact** |
| persistence | boot 1 writes a marker; a **new QEMU process** (boot 2) reads all 512 bytes back; the **host** independently finds the marker at byte 51200 of the image file and a non-zero last sector |
| normal kernel with a disk | image checksum unchanged |
| read-only disk | reads work, write refused with `BLK_ERO`, image checksum unchanged |
| legacy transport | reported + refused, boot continues, image unchanged |
| capacities 512 / 16384 sectors | reported capacity is the real one |

Mutation check: weakening the range check (`sector + count` no longer validated) makes the test kernel overrun its buffer
and panic, which the harness reports as a failure.

## Limits

Single outstanding request; polled (a 1 s worst-case stall blocks the CPU); no interrupts, no multi-queue, no discard/zeroes,
no scatter-gather from callers; only block size 512; modern transport only; the lost-doorbell timeout is simulated by skipping
the doorbell, not by a hung real device; behaviour on I/O error mid-multi-sector write is "stops at the failing chunk" (no
atomicity); nothing is claimed about crash consistency. QEMU only.
