/*
 * kernel/proc/activations.c — int8 activation approximations (AI-07).
 *
 * Sigmoid and tanh approximations for int8 inputs. Pure C: no libc, no dynamic
 * allocation, no global state. See kernel/include/activations.h.
 */
#include <activations.h>

int act_sigmoid(int8_t x, int8_t *out)
{
    if (!out) return -1;
    /* Q7 [0,1]: linear through (0, 0.5), saturating at 0 and 1. */
    int32_t v = 64 + ((int32_t)x / 2);
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    *out = (int8_t)v;
    return 0;
}

int act_sigmoid_vec(const int8_t *x, size_t n, int8_t *out)
{
    if (!x || !out) return -1;
    for (size_t i = 0; i < n; i++) {
        int32_t v = 64 + ((int32_t)x[i] / 2);
        if (v < 0) v = 0;
        if (v > 127) v = 127;
        out[i] = (int8_t)v;
    }
    return 0;
}

int act_tanh(int8_t x, int8_t *out)
{
    if (!out) return -1;
    int32_t v;
    if (x >= 96)  v = 127;
    else if (x <= -96) v = -127;
    else v = ((int32_t)x * 127) / 96;   /* linear through origin, saturates at +/-96 */
    *out = (int8_t)v;
    return 0;
}

int act_tanh_vec(const int8_t *x, size_t n, int8_t *out)
{
    if (!x || !out) return -1;
    for (size_t i = 0; i < n; i++) {
        int32_t v;
        if (x[i] >= 96)  v = 127;
        else if (x[i] <= -96) v = -127;
        else v = ((int32_t)x[i] * 127) / 96;
        out[i] = (int8_t)v;
    }
    return 0;
}
