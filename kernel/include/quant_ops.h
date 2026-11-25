#ifndef AURORA_QUANT_OPS_H
#define AURORA_QUANT_OPS_H

/*
 * kernel/include/quant_ops.h — quantized numerical primitives (AI-04).
 *
 * int8 dot product and matrix-vector multiply with int32 accumulators, ReLU, and
 * float32 <-> int8 quantization/dequantization with scale and zero point.
 *
 * Pure C: no libc, no dynamic allocation, no global state. All buffers bounded by
 * caller-provided lengths. EL0 / unprivileged safe.
 */

#include <stdint.h>
#include <stddef.h>

/* int8 dot product: acc = sum(a[i]*b[i]) over n elements, int32 accumulator.
 * Overflow-safe while n * 16384 <= INT32_MAX (n <= 131072). Returns 0 on success,
 * -1 if n == 0 or either pointer is NULL. Result written to *out (int32). */
int quant_dot(const int8_t *a, const int8_t *b, size_t n, int32_t *out);

/* int8 matrix-vector multiply (row-major matrix, rows x cols):
 * y[j] = requantize( sum_i A[j*cols+i] * x[i] ), j in [0, rows).
 *
 * Each row is a dot product in an int32 accumulator, then requantized:
 *   acc = sum_i A[j*cols+i] * x[i]            (int32)
 *   t   = (int64)acc * mult                   (int64)
 *   y[j] = clamp( round(t / 2^shift) + zp_out, -128, 127 )  (int8)
 *
 * `mult` in [1, 2^31-1], `shift` in [0, 31]. mat and x are int8; y and zp_out are int8.
 * Returns 0 on success, -1 on bad args (NULL, rows==0, cols==0, mult==0,
 * shift>31, or a row would overflow the int32 accumulator given cols).
 */
int quant_matvec(const int8_t *mat, size_t rows, size_t cols,
                 const int8_t *x, int32_t mult, uint32_t shift,
                 int8_t zp_out, int8_t *y);

/* ReLU: y[i] = max(0, x[i]) for n int8 elements. Returns 0 on success, -1 on bad args. */
int quant_relu(const int8_t *x, size_t n, int8_t *y);

/* Quantize float32 f to int8: q = clamp(round((f - zp) / scale), -128, 127).
 * scale must be > 0. Returns 0 on success, -1 on bad args (NULL or scale <= 0). */
int quant_quantize(float f, float scale, int8_t zp, int8_t *out);

/* Dequantize int8 q to float32: f = (q - zp) * scale. Returns 0 on success, -1 on bad args. */
int quant_dequantize(int8_t q, float scale, int8_t zp, float *out);

/* Quantize an entire float vector to int8 (bounded by n). */
int quant_quantize_vec(const float *f, size_t n, float scale, int8_t zp, int8_t *out);

/* Dequantize an entire int8 vector to float (bounded by n). */
int quant_dequantize_vec(const int8_t *q, size_t n, float scale, int8_t zp, float *out);

#endif /* AURORA_QUANT_OPS_H */
