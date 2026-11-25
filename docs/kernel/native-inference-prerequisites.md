# Offline AI on AuroraOS: native inference prerequisites (assessment, not an implementation)

Status: **No model runs inside AuroraOS.** No forward pass, no layer, no tokenizer. What exists natively is ONE validated primitive (section 6): an EL0 program that reads an int8 tensor object from the verified model volume through `SYS_MODEL` and computes a quantized matrix-vector product, checked bit-exactly against a host reference. The earlier Qwen3-0.6B work was a host-only baseline (outside the OS), and nothing measured there is evidence about AuroraOS speed, memory or safety. This document lists what the OS lacks, measured from this repository, and the order in which the gaps could be closed.

Selected model: Qwen3-0.6B. The figures below about the model come from its public model card and are **UNVERIFIED here**; check them before sizing anything: roughly 0.6 billion parameters (about 0.44 billion outside the embedding table), 28 layers, hidden size 1024, a vocabulary of about 152,000 tokens.

## 1. Size arithmetic (estimates)

| Weights format | Approximate size |
|---|---|
| 16-bit (BF16/FP16) | about 1.2 GB |
| 8-bit | about 0.6 GB |
| 4-bit (group-quantised) | about 0.3 to 0.4 GB plus scales |

Add the key/value cache (grows with context length) and activation buffers. Whatever the format, the weights are hundreds of megabytes.

## 2. What AuroraOS has today (from the code)

| Resource | Current limit | Where |
|---|---|---|
| Largest file in AuroraFS | 24 KiB (`FS_MAX_FILE` = 48 blocks x 512 B) | `kernel/include/fs.h` |
| Files in the whole filesystem | 32 (`FS_MAX_FILES`) | `kernel/include/fs.h` |
| Executable size | 12 pages of image, file at most 24 KiB | `kernel/include/elf.h` |
| Pages one process may own (tables included) | 24 fixed + `USER_HEAP_MAX_PAGES` (32) = 56 (`ASPACE_MAX_PAGES`) | `kernel/include/proc.h` |
| Concurrent processes | 8 (`PROC_MAX`) | `kernel/include/proc.h` |
| User address space | one 1 GiB slot per process; fixed code/data/stack plus a `SYS_MEM` heap of at most 32 pages (128 KiB) per process and 64 pages (256 KiB) per principal, with a 256-page kernel reserve; guards are never mapped; nothing larger or shared can be mapped | `kernel/include/proc.h` |
| RAM in the QEMU run scripts | 256 MB, one Cortex-A53 CPU (`-smp 1`), TCG emulation | `run-qemu.sh`, `scripts/package-qemu.sh` |
| FP/SIMD in EL0 | available and context-switched per process (Cycle 22) | `docs/kernel/userspace.md` |
| Kernel numeric code | none: the kernel is built `-mgeneral-regs-only` | `build.sh` |

## 3. The gaps, in dependency order

1. **Weights cannot be stored.** A 24 KiB file limit and 32 files cannot hold even one quantised layer of the model. Needs either a large-object store (extent-based files or a separate read-only model volume with its own bounded format and integrity check) or a streaming reader over a raw disk region. Whichever is chosen must keep the per-principal quota and permission model.
2. **Processes cannot hold the weights.** The `SYS_MEM` heap (Cycle 22) gives a process at most 128 KiB, enough for a small tensor kernel and test vectors but not for model weights; raising that ceiling would not solve this, because the weights are hundreds of megabytes. What is missing is a read-only, kernel-validated mapping of a verified model volume (see gap 1). Earlier text: a process owned 96 KiB. Needs a bounded, quota-checked way to map many more pages (an explicit map/unmap call with a per-principal page budget, W^X preserved, guard pages, no access to physical memory or the framebuffer, clean reclamation on exit or fault). Alternatively map the weights read-only and shared; that needs an explicit sharing design and must not weaken isolation.
3. **RAM.** At 256 MB the weights do not fit next to the kernel in any usable format. The VM needs a larger `-m` (and the `highmem` setting checked), which also changes what "works under QEMU" means for a phone with limited RAM.
4. **Disk throughput.** The virtio block path was built for correctness tests, not for streaming hundreds of megabytes. Needs measurement before any claim.
5. **Compute.** FP/SIMD state is now saved per process, but nothing uses NEON yet. A scalar integer int8 matrix-vector kernel now exists with a bit-exact test against a reference, run under QEMU TCG (section 6); a NEON version does not. TCG timing says nothing about real hardware and must never be reported as performance.
6. **Tokenizer and model runtime.** Byte-pair tokenizer, the model's layer arithmetic, sampling, all inside a process with no libc assumptions. Large, separately testable pieces; none exist in the repository.
7. **Time limits and scheduling.** A long computation must be preemptible (it is: timer preemption works) and must not starve the UI; it needs a CPU-time budget per principal.

## 4. Safety rules (fixed)

- Model output is untrusted text. It is never executed and never turned directly into an OS action. Any suggested action passes through the same typed, validated command path as keyboard input, then the broker and the trusted review for anything sensitive. The previous host baseline showed weak intent safety, so no direct action path is allowed.
- The inference process holds no ambient authority: no file access beyond its weights and a scratch area, no network, no timers beyond a bounded one, no right to launch other apps.
- No cloud fallback for speech or text: offline means offline.
- Performance and accuracy numbers from a host machine or from TCG are never reported as AuroraOS or phone results.

## 5. Suggested smallest next steps

1. Memory primitive: explicit, quota-checked map/unmap for a process, with hostile-client tests (overflow, exhaustion, partial failure, guard pages, cross-process isolation). This is independently useful for richer apps.
2. A NEON matrix-vector test program with a bit-exact check, to prove the numeric path end to end under QEMU.
3. Only then a model-volume format and a streaming reader.

Verdict: **No model runs natively.** Since this assessment was written the OS gained per-process FP/SIMD, a verified read-only model volume with bounded EL0 reads (`SYS_MODEL`, `docs/kernel/model-asset-store.md`) and the integer matvec primitive below. The memory prerequisite (mapping weights larger than the 128 KiB heap) is still open.

### Low-memory fallback (R12-3)

Typed Aura Quick Commands already run without an inference process or model volume. `kernel/ui/aura_exec.c` passes the original text to `aura_parse()` and submits the typed result to the broker. Supported navigation and timer commands use the existing services. Settings changes still require trusted review and healthy records. Unknown input ends at the parser's rejection. This is the available offline fallback today.

If a runtime is added, treat model absence, failed boot integrity checks and workspace allocation failure as inference unavailable. Keep the original typed input and offer the same Quick Commands path. Discard incomplete inference output before submitting anything to the broker. Test the same commands with the model missing and with allocation failure before enabling this handoff. `SYS_MODEL` already supports reads of up to 4 KiB, so mapping the full weights is optional for a streaming design. A full runtime still needs measured working memory, including its activations and context cache.

## 6. What exists now: a validated int8 matvec primitive (Milestone I, first component)

Exactly this, and nothing more:

- **Format.** One model-volume object of table kind 1 holds an `AURQMV1` tensor: a 32-byte header (magic, version, rows, cols, shift, two zero reserved words), `cols` int8 activations, then per row a 32-bit multiplier (1 .. 2^31-1) and `cols` int8 weights. The object size must equal `32 + cols + rows * (4 + cols)` exactly. Defined in `tests/programs/matvec_core.h`.
- **Arithmetic (integer only, no floating point, EL0 built `-mgeneral-regs-only`).** `acc = sum(w * x)` in int32; `q = clamp(floor((acc * mult + 2^(shift-1)) / 2^shift), -128, 127)` with the product in int64. Symmetric quantization, no zero points, no bias, no activation function.
- **Limits that make overflow impossible.** rows 1..1024, cols 1..4096, shift 0..31. The worst accumulator is 4096 x 128 x 128 = 2^26, far inside int32 (a static assert enforces `MV_MAX_COLS * 16384 <= INT32_MAX`); the requantization product stays below 2^58.
- **Bounded memory.** The program streams the payload through one 4096-byte buffer (`MODEL_READ_MAX`), rows may straddle reads, and every read must return exactly the bytes asked for. Static state is about 13 KiB; it uses no `SYS_MEM` heap (whose limit is 32 pages, 128 KiB).
- **Validation before output.** Wrong kind, an object shorter than the header, a header whose dimensions do not describe exactly the object (larger or smaller), zero or oversized dimensions, a bad magic, version or reserved word, an out-of-range shift or a zero/negative row multiplier are rejected; results are printed only after the whole object validated. Under an identity other than `PRINCIPAL_INFER` the kernel refuses every call (EPERM); without a valid volume it returns ENOSYS; the program exits cleanly in both cases.
- **Tests.** `tests/kernel-host/matvec_test.c` (host, ASan/UBSan) against an independently written reference (`matvec_ref.h`: whole arrays, int64, division-based rounding) and hand-computed vectors, including the maximum dimensions, truncated, failing and over-long reads, and random chunking; `tests/kernel-host/mutate_matvec.py` shows that 24 injected defects (limits off by one, missing checks, wrong rounding, lost sign) each make the suite fail. `tests/qemu/infer-tests.sh` runs the EL0 program in QEMU on 3 valid and 8 hostile tensor objects (generated by `tests/tools/mktensor`, packed by `tests/tools/mkmodel` with `name:kind=file`) and compares every printed row with the host reference.

What this is NOT: not Qwen3, not a model, not a forward pass, not a performance result (QEMU TCG timing means nothing for hardware), not NEON. The tensor object is only **integrity**-checked (unkeyed SHA-256 at boot): whoever can rewrite the volume can produce any numbers. **The output is untrusted data**: it is printed for comparison by a test script and nothing in the OS acts on it. The streaming truncation checks are exercised on the host only: a volume that passed its boot check cannot produce a short read in QEMU, so there those objects are rejected earlier, by the size check.
