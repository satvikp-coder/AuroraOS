# Storage: virtio block driver (issue #6, milestone A)

Status (current): block I/O (milestone A), the AuroraFS filesystem (B) and EL0 file access (C) are merged and tested in QEMU. The program
loader (D) and the native settings registry (E) do not exist yet. Nothing here has run on hardware. (This file keeps the milestone A text
below as written; later sections describe the filesystem and its hardening.)

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

Kernel filesystem over `blk_*` (`kernel/fs/afs.c`, `kernel/include/fs.h`). EL0 programs reach it only through `SYS_FS` (milestone C,
docs/kernel/userspace.md); authorization is enforced in the filesystem API per *principal* (0 = kernel, never given to a process; others
identify owners).

**On-disk format** (512-byte sectors, little-endian): sector 0 superblock (`AURORAFS`, version 1, geometry, CRC-32); sectors 1-16
inode table, 32 inodes x 256 B (state, generation, owner, permission bits, size, block count, data CRC-32, name, 48 block
pointers, inode CRC-32); data from sector 17. There is **no stored allocation bitmap**: free space is rebuilt from the inodes at
mount, so it cannot disagree with them. Flat namespace, names `[A-Za-z0-9._-]` of 1-31 characters, files up to 24 KiB.

**Ids and authorization:** a file id is `(generation << 8) | slot`. Deleting keeps the slot's generation, so an old id never names
a later file (the PID-reuse lesson applied again). Ids are *not* secrets: every operation checks permission bits (owner r/w, other
r/w); a file the caller cannot discover is hidden from `lookup` (indistinguishable from absent) and, since the cycle-10 hardening below, from
every id-taking operation too. Only the owner or the kernel may delete or change permissions.

**Names are unique per owner.** Two principals may each own a file called `config`. `create` only refuses a name the SAME principal already
owns (`FS_EEXIST`); a stranger's file of that name, hidden or not, neither blocks the create nor shows through its error code (an earlier
design returned `FS_EEXIST` for any file, which let a principal probe whether a hidden name exists and squat on names). `lookup` returns the
caller's own file of that name first, otherwise the first file it may see. (The two limits this section used to list, the file-id existence
difference and the missing quotas, are fixed: see "Hardening" below.)

**Mount** validates and writes nothing: magic, version (`FS_EVERSION`), CRCs, geometry, per-inode sanity, block ranges, no sector used twice,
no duplicate names *within one owner*, never-used slots all zero. Any failure leaves the filesystem unmounted. No automatic repair or format (`fs_format`
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

**Tests:** `tests/qemu/fs-tests.sh` (161 checks, in CI, including the EL0 file-access scenarios of docs/kernel/userspace.md): three boots on one image (format + 69 in-kernel checks and 26 EL0 checks; then 23 + 24 each on mount without
formatting incl. injected corruption), host-side proof that the bytes are in the image file, host-damaged images (flipped inode bit, wrong
version, foreign content, flipped superblock bit) reported precisely with the image checksum unchanged, read-only disk (2 + 1 checks), no disk (normal kernel, and a test kernel running the EL0 file calls).

## Hardening (cycle 10): existence policy and per-principal quotas

**Existence policy.** A file id is not a secret, so what a caller learns from an id must not depend on whether a file it may not discover sits
behind it. `resolve_for(principal, id)` runs first in `read`, `write`, `stat`, `set_perm` and `delete`, and every argument check that does not
depend on the file (NULL buffer, invalid permission bits, NULL stat pointer, mount state) is made *before* it, so no argument error can be used
as a probe either:

| Caller | File it can discover (owner, or has r or w permission) | File it cannot discover, or no such file |
|---|---|---|
| kernel (principal 0) | the true result | `FS_EBADID` only if there really is no such file |
| owner / has r or w | operation allowed, else `FS_EACCES` | n/a |
| anyone else | n/a | `FS_EBADID` (byte for byte what a nonexistent, stale-generation or deleted id returns) |

An owner who cleared every permission bit on its own file still discovers it (it can `chmod`/`delete` it; `read`/`stat` are `FS_EACCES`). Revoking
other-read on a file immediately turns it into `FS_EBADID` for others. Not claimed: any protection against *timing* or other side channels (not
analyzed or tested). The aggregate-resource errors `FS_ENFILE` and `FS_ENOSPC` still reflect the usage of the whole filesystem by design (a low-bandwidth
channel about total load); they name no file.

**Quotas.** Per principal (the file's *owner*): at most `FS_QUOTA_FILES` = 8 files and `FS_QUOTA_BLOCKS` = 96 data blocks (48 KiB, two maximum-size
files). Rationale: 8 is a quarter of the 32-slot table, so four principals can always coexist and no single one can exhaust the table; 96 blocks
lets an application keep two full-size files while leaving a normal test disk (2031 data blocks) to others. Usage is **derived** from the inode
table on demand (`usage_of`), never stored, so it cannot drift: it is exact after delete, overwrite, a failed write (nothing is written before the
check), an uncertain commit (the cache is untouched or the filesystem unmounted, and the next mount recomputes from the disk), remount and reboot.
Only growth is charged (`nb > nblocks`), so overwriting in place at the limit still works and nothing about quotas affects reads. Arithmetic is on
counts of at most 32 x 48, with the comparison written as `nb > QUOTA - others` so it cannot wrap. The quota is charged to the owner whoever writes
(a world-writable file spends its owner's quota, including a write by the kernel principal); this is deliberate and documented because the owner
chose to expose the file. The temporary second copy that whole-file copy-on-write needs is physical space, not charged to quota. A disk written by
an older kernel that already exceeds a quota still mounts (mount does not enforce quotas); such a principal simply cannot grow until it deletes.
Over-quota is `FS_EQUOTA` (-18; `-118` from EL0). `fs_usage()` is a kernel diagnostic, not a system call. A generation-reused slot is an ordinary
create and counts like any other file.

## Uncertain metadata updates (fail-safe recovery, review fix for PR #28)

Every metadata change (create, delete, chmod, and the inode publication of a write) goes through one function, `commit_inode`, with three outcomes:

| Outcome | Meaning | Filesystem state |
|---|---|---|
| `FS_OK` | inode sector written and flushed; cache updated | mounted |
| definite error (`FS_EIO`, ...) | the block layer rejected the write before it reached the device (`blk_error_definite`), or a failure happened before any metadata was touched (data-block write, data flush) | mounted, cache still exact, retry allowed |
| `FS_EUNCERTAIN` | the inode write was submitted and failed or timed out, or the flush after a submitted write failed: durability unknown | **unmounted**: every operation returns `FS_EMOUNT` until `fs_mount()` |

Why: after an uncertain inode write the disk may hold the new inode while the in-memory table and allocation map still describe the old file, so a later write could be handed sectors the new inode references. Unmounting makes that impossible; the next mount rebuilds the inode table and the allocation map from the disk and validates them (checksums, ranges, no sector owned twice, no duplicate names). The operation that returned `FS_EUNCERTAIN` **may or may not have committed**; the mount shows which, and (under the documented assumptions: in-order device, flush honoured, 512-byte sector writes atomic) the result is exactly the old or exactly the new state. Nothing is repaired or formatted automatically. Not claimed: recovery from torn sectors, reordered writes, or real power loss.

Tests (test-only injection in the block driver: reject / lost-with-error / landed-but-reported-as-error, for writes and flushes): inode write rejected, lost, landed; the dangerous follow-up (a second write after an uncertain commit is refused and the sectors of the new inode stay byte-for-byte untouched; after remount both files are exact and own disjoint sectors); inode-flush failure; data-block write failure; data-flush failure; the same three cases for create, delete and chmod. The new tests were run against the previous `commit_inode` first: 8 of them failed, including the sector-overwrite case.

Driver audit: `blk_init` is now idempotent (no remap, no allocation on a second call) and frees its three pages on every failure path after resetting the device. Timeout recovery re-initialises the device and the request returns `BLK_ETIMEOUT` (an uncertain outcome for writes, which the filesystem treats as `FS_EUNCERTAIN` when it concerns an inode). Failure paths *during* queue setup are covered by code review only; they have no injection test.
