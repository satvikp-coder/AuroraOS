#ifndef AURORA_NORM_H
#define AURORA_NORM_H

/*
 * kernel/include/norm.h — layer normalization primitive (AI-06).
 *
 * Bounded layer norm with no allocation and no libc. Mean and variance are computed
 * in widened integers (int32 / int64) so overflow cannot occur; an integer square
 * root (pure C) provides the normalization factor. Output is int8 with saturation.
 * EL0 / unprivileged safe.
 */

#include <stdint.h>
#include <stddef.h>

/* Layer norm over n int8 elements:
 *
 *   mean   = sum(x) / n                                          (int32)
 *   var    = sum((x - mean)^2) / n                               (int64)
 *   r      = isqrt(var + EPS)         (EPS = 1, avoids divide-by-zero)
 *   inv_r  = (1 << 16) / max(r,1)     (Q16 reciprocal)
 *   out[i] = clamp( (((x[i]-mean) * inv_r) >> 16) * gamma / 128 + beta, -128, 127 )
 *
 * gamma is applied as Q7 (gamma/128, so 127 ~= 1.0), beta is added as an int8 offset.
 * Returns 0 on success, -1 if any pointer is NULL or n == 0.
 */
int norm_layer(const int8_t *x, size_t n, int8_t gamma, int8_t beta, int8_t *out);

#endif /* AURORA_NORM_H */
