/* Per-head Qwen3 query/key normalization and split-half rotary embedding. */
#ifndef AURORA_QWEN3_QK_H
#define AURORA_QWEN3_QK_H

#include <qwen3_ops.h>

#define Q3_QK_MAX_WIDTH 128u
#define Q3_QK_EPSILON_Q32 4295u /* round(1e-6 * 2^32) */

static inline int32_t q3_qk_round(int64_t value, uint32_t divisor)
{
    int64_t half = divisor / 2u;
    return (int32_t)((value < 0 ? value - half : value + half) / divisor);
}

static inline int16_t q3_qk_saturate(int32_t value)
{
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return (int16_t)value;
}

/* One projected head, in channel order. Real values are raw / 2^8 (input),
 * raw / 2^12 (gain and output), and raw / 2^15 (cosine/sine).
 * Each trig vector has width/2 entries for this token's position. The
 * rotation pairs channel i with i+width/2. Output may alias input exactly.
 * Values beyond the signed output range saturate after rotation. */
static inline int q3_qk_prepare(const int16_t *input, const int16_t *gain,
                                const int16_t *cosine, const int16_t *sine,
                                uint32_t width, int16_t *output)
{
    if (!input || !gain || !cosine || !sine || !output) return Q3_E_ARG;
    if (!width || width > Q3_QK_MAX_WIDTH || width % 2u) return Q3_E_DIMS;

    uint64_t squares = 0;
    for (uint32_t i = 0; i < width; ++i) {
        int32_t x = input[i];
        squares += (uint32_t)(x * x);
    }
    /* squares <= 128 * 2^30. Retain fractional mean before the square root. */
    uint32_t rms = q3_isqrt64((squares << 16) / width + Q3_QK_EPSILON_Q32);
    uint32_t half = width / 2u;
    for (uint32_t i = 0; i < half; ++i) {
        uint32_t j = i + half;
        int32_t x = q3_qk_round((int64_t)input[i] * gain[i] * 256, rms);
        int32_t y = q3_qk_round((int64_t)input[j] * gain[j] * 256, rms);
        int64_t real = (int64_t)x * cosine[i] - (int64_t)y * sine[i];
        int64_t imag = (int64_t)y * cosine[i] + (int64_t)x * sine[i];
        output[i] = q3_qk_saturate(q3_qk_round(real, 32768));
        output[j] = q3_qk_saturate(q3_qk_round(imag, 32768));
    }
    return Q3_OK;
}

#endif
