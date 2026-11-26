/*
 * kernel/proc/quant_ops.c — quantized numerical primitives (AI-04).
 *
 * int8 dot product and matrix-vector multiply with int32/int64 accumulators and ReLU.
 *
 * Pure C: no libc, no dynamic allocation, no global state. All buffers bounded by
 * caller-provided lengths. EL0 / unprivileged safe.
 */
#include <quant_ops.h>

/* ---- int8 dot product, int32 accumulator ---- */
int quant_dot(const int8_t *a, const int8_t *b, size_t n, int32_t *out)
{
    if (!a || !b || !out || n == 0) return -1;
    /* Worst |a[i]*b[i]| = 128*128 = 16384. Overflow impossible while
     * n * 16384 <= INT32_MAX. */
    if (n > (size_t)(INT32_MAX / 16384)) return -1;
    int32_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc += (int32_t)a[i] * (int32_t)b[i];
    }
    *out = acc;
    return 0;
}

/* ---- int8 matrix-vector multiply, row-major ---- */
int quant_matvec(const int8_t *mat, size_t rows, size_t cols,
                 const int8_t *x, int32_t mult, uint32_t shift,
                 int8_t zp_out, int8_t *y)
{
    if (!mat || !x || !y || rows == 0 || cols == 0) return -1;
    if (mult <= 0 || shift > 31u) return -1;
    /* Per-row accumulator overflow: cols * 16384 <= INT32_MAX required. */
    if (cols > (size_t)(INT32_MAX / 16384) || rows > (size_t)PTRDIFF_MAX / cols) return -1;

    for (size_t j = 0; j < rows; j++) {
        int32_t acc = 0;
        const int8_t *row = mat + j * cols;
        for (size_t i = 0; i < cols; i++) {
            acc += (int32_t)row[i] * (int32_t)x[i];
        }
        /* Requantize: t = acc * mult in int64, then arithmetic shift with rounding. */
        int64_t t = (int64_t)acc * (int64_t)mult;
        if (shift) {
            const int64_t scale = (int64_t)1 << shift;
            const int64_t rounded = t + scale / 2;
            t = rounded / scale;
            if (rounded < 0 && rounded % scale) t--;
        }
        int64_t v = t + zp_out;
        if (v < -128) v = -128;
        if (v > 127) v = 127;
        y[j] = (int8_t)v;
    }
    return 0;
}

/* ---- ReLU: y[i] = max(0, x[i]) ---- */
int quant_relu(const int8_t *x, size_t n, int8_t *y)
{
    if (!x || !y) return -1;
    for (size_t i = 0; i < n; i++) {
        int32_t v = (int32_t)x[i];
        if (v < 0) v = 0;
        y[i] = (int8_t)v;
    }
    return 0;
}
