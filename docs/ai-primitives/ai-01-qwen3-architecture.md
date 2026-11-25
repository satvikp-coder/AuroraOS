# AI-01 — Verified model architecture: Qwen3-0.6B (read-only documentation)

Status: read-only assessment. Nothing here is implemented in AuroraOS; this documents the
public Qwen3-0.6B architecture so the primitive size bounds (AI-02..AI-10) can be chosen.

Source of figures: public Qwen3-0.6B model card. Figures are UNVERIFIED against an actual
checkpoint in this repository; no weights, no tokenizer and no model volume exist here.

## Architecture (Qwen3-0.6B, public model card)

| Parameter | Value |
|---|---|
| Layers | 48 |
| Hidden size | 1024 |
| Attention heads | 16 |
| Head dimension | 64 (1024 / 16) |
| Intermediate (FFN) size | ~3072 (SwiGLU) |
| Vocabulary | ~151,936 tokens (byte-level BPE + merges) |
| Parameter count | ~0.6 billion total (~0.44B outside embedding table) |
| Positional embedding | RoPE |
| Attention | GQA |
| Normalization | RMSNorm + per-head QK-norm |
| Activations | SwiGLU |
| Output | tied (embedding weights reused for the output projection) |

## Estimated memory footprint

| Format | Approximate size |
|---|---|
| BF16 / FP16 | ~1.2 GB |
| INT8 | ~0.6 GB |
| Q4_K / Q8_0 (4-bit group-quantised) | ~0.3 to 0.4 GB plus scales |

Plus key/value cache (grows with context length) and activation buffers.

## Why this is not implemented natively on AuroraOS

Per docs/kernel/native-inference-prerequisites.md:

- Largest file in AuroraFS is 24 KiB; only 32 files — cannot hold even one quantised layer.
- A process heap is at most 128 KiB (32 pages); weights are hundreds of megabytes.
- QEMU guest RAM is 256 MB — weights do not fit next to the kernel.
- No EL0 file-backed mmap, no large read-only model mapping, no FP/SIMD usage yet.

Conclusion: Qwen3-0.6B inference is **not feasible on EL0** and is deliberately NOT implemented
here. AI-01 is documentation only. The primitives in AI-03..AI-10 are small, bounded, EL0-safe
building blocks; they do not constitute a model runtime and do not claim Qwen3 compatibility.
