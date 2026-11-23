# Model asset store (tested format, read-only device verified at boot, bounded EL0 read call; no runtime yet)

Status: the **format, the SHA-256 and the validator exist and are host-tested** (`kernel/include/modelstore.h`, `kernel/fs/modelstore.c`, `kernel/include/sha256.h`, `kernel/crypto/sha256.c`, `tests/kernel-host/modelstore_test.c`). The kernel finds an optional second, read-only virtio-blk device by its `AURMODEL` magic and verifies it once at boot (`kernel/fs/modelvol.c`, `tests/qemu/storage-tests.sh` section 8). A process holding `PRINCIPAL_INFER` can read verified objects through the bounded `SYS_MODEL` call (below, `tests/qemu/model-tests.sh`). There is no signature, no installer beyond the host test tool `tests/tools/mkmodel.c`, and no inference runtime. This document says what exists, what does not, and the order to build the rest. See also `native-inference-prerequisites.md` and `docs/aura/qwen3-local-feasibility.md`.

## Why a separate store, and why not small files
AuroraFS files are at most 24 KiB (`FS_MAX_FILE`) and there are 32 files. The eventual model, Qwen3-0.6B, is a ~639 MB file at Q8_0 (about 1.25 million 512-byte sectors, `qwen3-local-feasibility.md`). Splitting it into 24 KiB AuroraFS files would need roughly 26,000 files, which is more than 800 times the filesystem's file limit and would also put weights under per-principal quotas, name rules and the inode table for no benefit. That is a hack, and it is rejected. The store is a **separate read-only volume** of large immutable objects.

## What a volume is
One header sector, a table of up to 16 objects, then the objects' sectors (`AURMODEL` v1; exact layout in `modelstore.h`). Properties the validator enforces on every parse:
- every byte of the header sector and the table has exactly the value the format gives it (a flipped byte anywhere in those 1,024 bytes (header sector plus table sector in the test volume) is rejected: 3,072 mutations tested, 0 accepted);
- extents lie inside the data area, never overlap, never wrap 64-bit arithmetic (sizes of 2^63 and 2^64-1, a first sector of 2^64-1, and a 2^41-sector volume are tested);
- names are 1..31 characters of `[A-Za-z0-9._-]`, unique;
- an object's bytes are checked against its recorded SHA-256 while streaming sector by sector, and the zero tail of its last sector is required (6,656 single-byte data mutations tested, 0 accepted).

## What a digest does and does not give
SHA-256 here is **unkeyed**. It detects accidental damage, truncation and mix-ups. It does **not** authenticate: anyone who can rewrite the volume can rewrite the weights and recompute every digest, and the test suite asserts exactly that (`KNOWN LIMIT`). So:
- the installer must be the only writer, and the volume is attached **read-only** to the VM;
- authenticity needs a **signature over the header digest**, verified against a public key that is already trusted (compiled into the kernel or installer). Not implemented; until it is, a model on this store must be described as "integrity-checked", never "authentic" or "tamper-proof";
- downgrade (an old, vulnerable model presented as current) needs a version or monotonic counter inside the signed header. Not implemented.

## Memory (why this is not just a bigger file)
- `-m 256M` QEMU RAM cannot hold 639 MB. Either the runtime streams weights from the volume (slow under TCG, unmeasured) or the VM gets at least 1 GiB, which changes the supported-RAM statement and the kernel's reserve arithmetic. No claim is made about either until measured.
- The per-principal heap budget is 64 pages (256 KiB) and the per-process budget 32 pages. Weights must never be charged to a heap: a model needs its own accounting class (read-only mapped pages owned by the kernel, charged to nobody's heap) so the heap budget keeps meaning what it says.
- The model is never in the kernel. The EL0 runtime gets read access to one named object through a kernel-mediated, read-only mapping or read call bound to the object index; there is no write path and no way to name another object or another device.

## Device
The volume is a **second virtio-blk device**, meant to be attached with `readonly=on`. The driver selects it by its `AURMODEL` magic (content, not probe order; `storage-tests.sh` section 7); selection by an unforgeable identity such as the device's serial is not implemented, so anything that can attach a disk with that magic can offer a volume (it must still pass the boot check). The alternative, a reserved region of the single AuroraFS disk, couples model updates to filesystem writes and is not recommended.

## EL0 read path: `SYS_MODEL` (11)
`kernel/proc/modelsys.c`, ABI in `kernel/include/modelsys.h`. A bounded, read-only window onto the objects the boot check **verified**; nothing else.
- **Ops.** `MODELOP_INFO(index, user_ptr)` writes `struct user_model_obj { u64 size; u32 kind; u32 reserved; char name[32]; u8 digest[32]; }` (80 bytes) and returns 0; `index == 0xFFFFFFFF` (exactly; a 64-bit compare) returns the object count instead. `MODELOP_READ(index, offset, user_buf, len)` copies `min(len, size - offset)` bytes and returns that count; 0 at `offset == size`; `len` must be 1..4096. The table `kind` is opaque to the kernel; kind 1 is used by the int8 matvec test tensor (`tests/programs/matvec_core.h`, `docs/kernel/native-inference-prerequisites.md` section 6), and `tests/tools/mkmodel` sets it with `name:kind=file` (default 0).
- **Who.** Only a process whose kernel-chosen principal is `PRINCIPAL_INFER` (0x7F000006, reserved; see `principals.md`). It is put on a process only by `proc_set_service_principal()`, kernel code; no registration, grant or EL0 path can name it. Everyone else gets `AE_EPERM` for every op, checked before anything else, so an unauthorised process cannot even learn whether a volume exists. A process holding `PRINCIPAL_INFER` has no file access (`fssys.c` refuses every reserved identity).
- **When.** Only when `modelvol_state() == MODELVOL_VALID`; an absent volume or one that failed its boot check is `AE_ENOSYS`.
- **Bounds.** EL0 names an object index and a byte range, never a sector or a device. The extent is the verified table entry (`modelvol_object()`); the range rule is `model_read_clamp()` (pure, header-only, no addition that can wrap; host-tested with 2^64-1, offset = size, len 0 / 4096 / 4097, a 24^3 sweep against a 128-bit reference and 2,000,000 random triples, `tests/kernel-host/modelsys_test.c`), and every sector is re-checked against `[first_sector, first_sector + sectors)` before `blkm_read()`. Unknown op, bad index, `len 0` or `offset > size`: `AE_EINVAL`; `len > 4096`: `AE_E2BIG`; a destination EL0 may not write (NULL, unmapped, kernel, read-only code or data, crossing the end of user memory): `AE_EFAULT`, checked for the whole range before any device read; a device failure: `AE_EIO`.
- **Copies.** `copy_to_user` only, through one static 512-byte bounce sector used inside a single system call (system calls run to completion on the one CPU with interrupts masked). There is **no per-process state**: a process that faults or exits between reads holds nothing (`tests/qemu/model-tests.sh` runs 5 crash + 5 mid-object-exit rounds, each followed by a passing full run, and checks every page comes back).
- **What it does not give.** The volume is verified **once, at boot**. Reads are re-bounded every time, but object bytes are **not re-hashed** on read: a host that rewrites the image file while the VM runs is not detected (attaching it `readonly=on` stops the guest writing it, not the host). And the check is **integrity, not authenticity** (unkeyed SHA-256, above). No mapping, no per-object quota, no separate memory class yet: reads go into the caller's own memory, at most 4096 bytes per call.

## Output is untrusted
A model reads and writes text. Its output is data: it is shown to the user, and it is never executed, never turned into a syscall, a capability grant, a file path or a command without going through the same Action Broker, Passport and review rules as any other request. Model quality, speed and memory use on this machine are unmeasured; nothing here claims any.

## Build order (each step is its own PR with negative tests)
1. Installer tool (host) that writes a volume from files and refuses what `ms_parse` refuses. Round-trip test against the kernel parser.
2. Signature scheme and trusted-key placement: designed in `model-trust-design.md` (not implemented; no home-made cryptography without a vetted implementation and test vectors).
3. Block driver: second device, read-only, identified by serial; negative tests (missing, wrong serial, writable).
4. Kernel object read path with hostile-argument tests: **done as `SYS_MODEL`** (a bounded read call, above). Still open: per-object quota and a separate memory class for a mapped (rather than copied) path.
5. EL0 runtime against a tiny synthetic model (to prove plumbing and bounds), then a real one. Only then any speed or quality statement, measured.

## Not done, stated plainly
No signature, no downgrade protection, no memory class, no mapping, no runtime, no model, no performance data. What is real today: the format and its checks, the read-only second device verified once at boot (`kernel/fs/modelvol.c`), the host image tool (`tests/tools/mkmodel.c`) and the bounded `SYS_MODEL` read call for `PRINCIPAL_INFER`.
