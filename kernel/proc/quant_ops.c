/*
 * kernel/proc/quant_ops.c — quantized numerical primitives (AI-04).
 *
 * int8 dot product and matrix-vector multiply with int32/int64 accumulators, ReLU,
 * and float32 <-> int8 quantization/dequantization with scale and zero point.
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
    if (n > (size_t)131072) return -1;
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
    if (cols > (size_t)131072) return -1;

    for (size_t j = 0; j < rows; j++) {
        int32_t acc = 0;
        const int8_t *row = mat + j * cols;
        for (size_t i = 0; i < cols; i++) {
            acc += (int32_t)row[i] * (int32_t)x[i];
        }
        /* Requantize: t = acc * mult in int64, then arithmetic shift with rounding. */
        int64_t t = (int64_t)acc * (int64_t)mult;
        int32_t q;
        if (shift == 0u) {
            q = (int32_t)t;               /* no scaling */
        } else {
            const int64_t half = (int64_t)1 << (shift - 1);
            int64_t rounded = t + half;
            /* arithmetic shift (floor toward negative inf) */
            q = (int32_t)(rounded >> shift);
        }
        /* Clamp to int8 and apply output zero point. */
        int32_t v = q + (int32_t)zp_out;
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

/* ---- quantization / dequantization (scalar) ---- */
int quant_quantize(float f, float scale, int8_t zp, int8_t *out)
{
    if (!out || scale <= 0.0f) return -1;
    float qf = (f - (float)zp) / scale;
    /* round half away from zero */
    int32_t q;
    if (qf >= 0.0f) q = (int32_t)(qf + 0.5f);
    else            q = (int32_t)(qf - 0.5f);
    if (q < -128) q = -128;
    if (q > 127)  q = 127;
    *out = (int8_t)q;
    return 0;
}

int quant_dequantize(int8_t q, float scale, int8_t zp, float *out)
{
    if (!out || scale <= 0.0f) return -1;
    *out = ((float)q - (float)zp) * scale;
    return 0;
}

/* ---- vector quantization / dequantization ---- */
int quant_quantize_vec(const float *f, size_t n, float scale, int8_t zp, int8_t *out)
{
    if (!f || !out || scale <= 0.0f) return -1;
    for (size_t i = 0; i < n; i++) {
        int8_t q;
        if (quant_quantize(f[i], scale, zp, &q) != 0) return -1;
        out[i] = q;
    }
    return 0;
}

int quant_dequantize_vec(const int8_t *q, size_t n, float scale, int8_t zp, float *out)
{
    if (!q || !out || scale <= 0.0f) return -1;
    for (size_t i = 0; i < n; i++) {
        float v;
        if (quant_dequantize(q[i], scale, zp, &v) != 0) return -1;
        out[i] = v;
    }
    return 0;
}
