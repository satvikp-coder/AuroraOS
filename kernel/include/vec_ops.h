#ifndef AURORA_VEC_OPS_H
#define AURORA_VEC_OPS_H

/*
 * kernel/include/vec_ops.h — int8 vector operations (AI-05).
 *
 * Element-wise int8 addition with saturation. Pure C: no libc, no dynamic
 * allocation, no global state. Buffers bounded by caller-provided length.
 * EL0 / unprivileged safe.
 */

#include <stdint.h>
#include <stddef.h>

/* out[i] = clamp(a[i] + b[i], -128, 127) for n elements.
 * The sum is computed in int32 so no overflow can occur. Returns 0 on success,
 * -1 if any pointer is NULL. */
int vec_add_sat(const int8_t *a, const int8_t *b, size_t n, int8_t *out);

/* out[i] = clamp(a[i] + b[i], -128, 127) in-place: out may alias a or b. */
int vec_add_sat_alias(const int8_t *a, const int8_t *b, size_t n, int8_t *out);

#endif /* AURORA_VEC_OPS_H */
