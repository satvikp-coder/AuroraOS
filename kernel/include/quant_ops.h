#ifndef AURORA_QUANT_OPS_H
#define AURORA_QUANT_OPS_H

/*
 * kernel/include/quant_ops.h — quantized numerical primitives (AI-04).
 *
 * int8 dot product and matrix-vector multiply with int32 accumulators and ReLU.
 *
 * Pure C with no libc, dynamic allocation, or global state. The caller supplies
 * valid buffers with at least the stated lengths.
 */

#include <stdint.h>
#include <stddef.h>

/* int8 dot product: acc = sum(a[i]*b[i]) over n elements, int32 accumulator.
 * Overflow-safe while n * 16384 <= INT32_MAX (n <= 131071). Returns 0 on success,
 * -1 if n is outside that range or a pointer is NULL. Result written to *out. */
int quant_dot(const int8_t *a, const int8_t *b, size_t n, int32_t *out);

/* int8 matrix-vector multiply (row-major matrix, rows x cols):
 * y[j] = requantize( sum_i A[j*cols+i] * x[i] ), j in [0, rows).
 *
 * Each row is a dot product in an int32 accumulator, then requantized:
 *   acc = sum_i A[j*cols+i] * x[i]            (int32)
 *   t   = (int64)acc * mult                   (int64)
 *   y[j] = clamp( floor(t / 2^shift + 0.5) + zp_out, -128, 127 )  (int8)
 *
 * `mult` in [1, 2^31-1], `shift` in [0, 31]. mat and x are int8; y and zp_out are int8.
 * Supply rows * cols matrix elements, cols input elements, and rows output
 * elements. The output must not overlap either input.
 * Returns 0 on success or -1 for null pointers, zero dimensions, nonpositive
 * mult, shift > 31, or unsafe shape or accumulator arithmetic.
 */
int quant_matvec(const int8_t *mat, size_t rows, size_t cols,
                 const int8_t *x, int32_t mult, uint32_t shift,
                 int8_t zp_out, int8_t *y);

/* ReLU: y[i] = max(0, x[i]) for n int8 elements. Returns 0 on success, -1 on bad args. */
int quant_relu(const int8_t *x, size_t n, int8_t *y);

#endif /* AURORA_QUANT_OPS_H */
