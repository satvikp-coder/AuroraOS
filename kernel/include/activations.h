#ifndef AURORA_ACTIVATIONS_H
#define AURORA_ACTIVATIONS_H

/*
 * kernel/include/activations.h — int8 activation approximations (AI-07).
 *
 * Sigmoid and tanh approximations for int8 inputs, producing int8 outputs.
 * Pure C: no libc, no dynamic allocation, no global state. Bounded by caller
 * length. EL0 / unprivileged safe.
 *
 * Output convention: int8 in [-127,127] as Q7 ([-1,1]) for tanh; int8 in [0,127]
 * as Q7 ([0,1]) for sigmoid.
 */

#include <stdint.h>
#include <stddef.h>

/* Scalar sigmoid: out = clamp(64 + x/2, 0, 127)  (Q7 [0,1]). */
int act_sigmoid(int8_t x, int8_t *out);
/* Vector sigmoid. */
int act_sigmoid_vec(const int8_t *x, size_t n, int8_t *out);

/* Scalar tanh: piecewise-linear, saturates at +/-127 beyond +/-96. */
int act_tanh(int8_t x, int8_t *out);
/* Vector tanh. */
int act_tanh_vec(const int8_t *x, size_t n, int8_t *out);

#endif /* AURORA_ACTIVATIONS_H */
