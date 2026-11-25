# AI-02 — Tensor sizes and INT8 quantization scheme (read-only documentation)

Status: read-only assessment. Documents tensor dimensions and the INT8 quantization scheme used
by the bounded primitives (AI-03..AI-08). No host inference, no conversion, no benchmark.

## Tensor dimensions (primitive scale, not model scale)

The primitives operate on small, bounded, caller-sized int8 tensors:

- Vector: length N, N chosen by the caller, bounded by the tensor capacity (TOK_TENSOR_CAP,
  default 4096 elements).
- Matrix: rows x cols, stored row-major; rows and cols bounded by caller-provided lengths.
- Activation buffer: one int8 vector per layer output, bounded by hidden size (max 1024 in the
  Qwen3 shape, but here caller-bounded).

All sizes are explicit lengths passed by the caller; no dynamic allocation.

## INT8 quantization scheme

Symmetric affine quantization with per-tensor scale and optional zero point:

```
q = clamp(round((f - zero_point) / scale), -128, 127)     // int8
f = (q - zero_point) * scale                              // float32
```

- `scale` > 0, float32.
- `zero_point` in [-128, 127], int8 (symmetric: zero_point = 0 for symmetric quantization).
- Quantization rounds to nearest (ties away from zero is not required; round-half-away or
  round-half-to-even both acceptable, documented per function).
- Clamping to int8 range [-128, 127] guarantees no overflow on storage.

## Accumulator safety

- int8 x int8 product: [-16384, 16129] (i.e. -128*128 .. 127*127).
- Dot product / matrix-vector row: int32 accumulator. Worst case for N elements:
  N * 16384. Overflow impossible while N * 16384 <= INT32_MAX, i.e. N <= 131072.
  Primitive callers cap N at TOK_TENSOR_CAP (4096) => max |acc| = 4096 * 16384 = 2^26, far
  inside int32.
- Requantization (acc * mult >> shift) uses int64 for the product; worst case well below
  2^58 (see existing matvec primitive, tests/programs/matvec_core.h).

## What this is NOT

- Not the Qwen3-0.6B weight layout (48 layers, hidden 1024).
- Not Q4_K/Q8_0 dequantization (grouped scales, blocks).
- Not a claim that Qwen3-0.6B fits or runs on AuroraOS.
