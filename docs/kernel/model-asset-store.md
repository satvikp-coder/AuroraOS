# Model asset store (design, with a tested on-disk format; no device or runtime yet)

Status: the **format, the SHA-256 and the validator exist and are host-tested** (`kernel/include/modelstore.h`, `kernel/fs/modelstore.c`, `kernel/include/sha256.h`, `kernel/crypto/sha256.c`, `tests/kernel-host/modelstore_test.c`). Nothing in the kernel links them yet. There is no block-device attachment, no syscall, no installer and no inference runtime. This document says what exists, what does not, and the order to build the rest. See also `native-inference-prerequisites.md` and `docs/aura/qwen3-local-feasibility.md`.

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
The block driver supports one device. The volume would be a **second virtio-blk device attached with `readonly=on`**, which needs driver work (multiple devices, role selection by an unforgeable identity such as the device's serial, never by probe order). The alternative, a reserved region of the single AuroraFS disk, couples model updates to filesystem writes and is not recommended.

## Output is untrusted
A model reads and writes text. Its output is data: it is shown to the user, and it is never executed, never turned into a syscall, a capability grant, a file path or a command without going through the same Action Broker, Passport and review rules as any other request. Model quality, speed and memory use on this machine are unmeasured; nothing here claims any.

## Build order (each step is its own PR with negative tests)
1. Installer tool (host) that writes a volume from files and refuses what `ms_parse` refuses. Round-trip test against the kernel parser.
2. Signature scheme and trusted-key placement (design review first; no home-made cryptography without a vetted implementation and test vectors).
3. Block driver: second device, read-only, identified by serial; negative tests (missing, wrong serial, writable).
4. Kernel object read path with per-object quota and a separate memory class; hostile-argument tests.
5. EL0 runtime against a tiny synthetic model (to prove plumbing and bounds), then a real one. Only then any speed or quality statement, measured.

## Not done, stated plainly
No device attachment, no installer, no signature, no downgrade protection, no memory class, no syscall, no runtime, no model, no performance data. The format and its checks are what is real today.
