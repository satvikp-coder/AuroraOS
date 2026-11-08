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

# AuroraFS v1 (issue #6, milestone B)

Kernel-internal filesystem over `blk_*` (`kernel/fs/afs.c`, `kernel/include/fs.h`). No EL0 access yet (milestone C): authorization
is already enforced in the filesystem API per *principal* (0 = kernel, never given to a process; others identify owners).

**On-disk format** (512-byte sectors, little-endian): sector 0 superblock (`AURORAFS`, version 1, geometry, CRC-32); sectors 1-16
inode table, 32 inodes x 256 B (state, generation, owner, permission bits, size, block count, data CRC-32, name, 48 block
pointers, inode CRC-32); data from sector 17. There is **no stored allocation bitmap**: free space is rebuilt from the inodes at
mount, so it cannot disagree with them. Flat namespace, names `[A-Za-z0-9._-]` of 1-31 characters, files up to 24 KiB.

**Ids and authorization:** a file id is `(generation << 8) | slot`. Deleting keeps the slot's generation, so an old id never names
a later file (the PID-reuse lesson applied again). Ids are *not* secrets: every operation checks permission bits (owner r/w, other
r/w); owner-only files are hidden from `lookup` (indistinguishable from absent) and a principal that guesses the exact id still gets
`FS_EACCES`. Only the owner or the kernel may delete or change permissions.

**Mount** validates and writes nothing: magic, version (`FS_EVERSION`), CRCs, geometry, per-inode sanity, block ranges, no sector used twice,
no duplicate names, never-used slots all zero. Any failure leaves the filesystem unmounted. No automatic repair or format (`fs_format`
is an explicit call, used only by test kernels). File data is checked against its CRC on every read, so silent corruption returns an
error, never wrong bytes.

**Write ordering:** whole-file copy-on-write: new data to free sectors, flush, one inode sector, flush. Tested by dropping every device
write after the k-th for every k (and for create and delete): after remount the file is exactly the old or exactly the new content, and
the filesystem always mounts. **This is simulated power loss with in-order writes, not a real power cut, and it assumes the device honours
flush and 512-byte writes are atomic.** Mutation checks: writing the inode before the data fails the sweep; disabling the data CRC fails
the corruption test.

**Limits:** 32 files, 24 KiB per file, flat namespace, whole-file rewrite on every write (so a file larger than half the free space
cannot be appended to: `FS_ENOSPC`, documented and tested), 32 MiB maximum filesystem (RAM allocation map), single mount, no
timestamps, no concurrency control (single CPU, callers are not preempted inside the filesystem), QEMU only.

**Tests:** `tests/qemu/fs-tests.sh` (81 checks, in CI): three boots on one image (format + 39 checks; then 17 checks each on mount without
formatting incl. injected corruption), host-side proof that the bytes are in the image file, host-damaged images (flipped inode bit, wrong
version, foreign content, flipped superblock bit) reported precisely with the image checksum unchanged, read-only disk, no disk.
