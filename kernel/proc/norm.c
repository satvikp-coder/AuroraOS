/*
 * kernel/proc/norm.c — layer normalization primitive (AI-06).
 *
 * Bounded layer norm: no allocation, no libc, no global state. Integer square root
 * implemented in pure C. See kernel/include/norm.h.
 */
#include <norm.h>

/* Integer square root (floor) of a 64-bit value, bit-by-bit, pure C, no libc. */
static uint64_t isqrt_u64(uint64_t n)
{
    uint64_t res = 0;
    uint64_t bit = 1ULL << 62;  /* highest power of four <= 2^62 */
    while (bit > n) bit >>= 2;
    while (bit != 0) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

int norm_layer(const int8_t *x, size_t n, int8_t gamma, int8_t beta, int8_t *out)
{
    if (!x || !out || n == 0) return -1;
    /* Cap n so widened accumulators cannot overflow: |sum| <= 128*n and
     * varsum <= 65536*n; with n <= 1<<20 both fit in int64 with huge margin. */
    if (n > (size_t)1 << 20) return -1;

    /* Mean: int64 accumulator. */
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += (int64_t)(int32_t)x[i];
    int32_t mean = (int32_t)(sum / (int64_t)n);

    /* Variance: int64 accumulator, (x-mean) in [-256,256], squared <= 65536,
     * varsum <= 65536*n <= 2^38 for n <= 1<<20, fits in int64. */
    int64_t varsum = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t d = (int32_t)x[i] - mean;
        varsum += (int64_t)d * (int64_t)d;
    }
    int64_t var = varsum / (int64_t)n;

    /* Normalization factor with EPS=1 to avoid divide-by-zero. */
    uint64_t r = isqrt_u64((uint64_t)(var + 1));
    if (r == 0) r = 1;
    int64_t inv_r = (int64_t)1 << 16;  /* Q16 */
    inv_r /= (int64_t)r;

    for (size_t i = 0; i < n; i++) {
        int32_t d = (int32_t)x[i] - mean;               /* int32 */
        int64_t scaled = ((int64_t)d * inv_r) >> 16;    /* back to int8-ish range */
        /* gamma as Q7: scaled * gamma / 128 */
        int64_t g = (scaled * (int64_t)gamma) >> 7;
        int32_t v = (int32_t)g + (int32_t)beta;
        if (v < -128) v = -128;
        if (v > 127)  v = 127;
        out[i] = (int8_t)v;
    }
    return 0;
}
