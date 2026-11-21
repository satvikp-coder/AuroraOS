# Offline AI on AuroraOS: native inference prerequisites (assessment, not an implementation)

Status: **NOT STARTED natively.** No model forward pass runs inside AuroraOS. The earlier Qwen3-0.6B work was a host-only baseline (outside the OS), and nothing measured there is evidence about AuroraOS speed, memory or safety. This document lists what the OS lacks, measured from this repository, and the order in which the gaps could be closed.

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
| Pages one process may own (tables included) | 24 (`ASPACE_MAX_PAGES`) = 96 KiB | `kernel/include/proc.h` |
| Concurrent processes | 8 (`PROC_MAX`) | `kernel/include/proc.h` |
| User address space | one 1 GiB slot per process, but only the fixed code/data/stack pages are mapped; no system call maps more memory | `kernel/include/proc.h` |
| RAM in the QEMU run scripts | 256 MB, one Cortex-A53 CPU (`-smp 1`), TCG emulation | `run-qemu.sh`, `scripts/package-qemu.sh` |
| FP/SIMD in EL0 | available and context-switched per process (Cycle 22) | `docs/kernel/userspace.md` |
| Kernel numeric code | none: the kernel is built `-mgeneral-regs-only` | `build.sh` |

## 3. The gaps, in dependency order

1. **Weights cannot be stored.** A 24 KiB file limit and 32 files cannot hold even one quantised layer of the model. Needs either a large-object store (extent-based files or a separate read-only model volume with its own bounded format and integrity check) or a streaming reader over a raw disk region. Whichever is chosen must keep the per-principal quota and permission model.
2. **Processes cannot hold the weights.** A process owns 96 KiB. Needs a bounded, quota-checked way to map many more pages (an explicit map/unmap call with a per-principal page budget, W^X preserved, guard pages, no access to physical memory or the framebuffer, clean reclamation on exit or fault). Alternatively map the weights read-only and shared; that needs an explicit sharing design and must not weaken isolation.
3. **RAM.** At 256 MB the weights do not fit next to the kernel in any usable format. The VM needs a larger `-m` (and the `highmem` setting checked), which also changes what "works under QEMU" means for a phone with limited RAM.
4. **Disk throughput.** The virtio block path was built for correctness tests, not for streaming hundreds of megabytes. Needs measurement before any claim.
5. **Compute.** FP/SIMD state is now saved per process, but nothing uses NEON yet. The first native workload would be a small, hand-written matrix-vector kernel with a bit-exact test against a reference, run under QEMU TCG. TCG timing says nothing about real hardware and must never be reported as performance.
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

Verdict: **NOT STARTED.** The OS gained one prerequisite this cycle (per-process FP/SIMD); the storage and memory prerequisites are still open.
