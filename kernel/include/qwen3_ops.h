/*
 * kernel/include/qwen3_ops.h - Quantized Qwen3 transformer-layer primitives (AI-05).
 *
 * Pure C, freestanding, no floating point, no libc, no dynamic allocation, no globals.
 * Implements bounded fixed-point building blocks for Qwen3-style decoder layers:
 *   1. qwen3_rmsnorm(): int8 RMS normalization with int64 sum-of-squares, integer isqrt, and int8 gamma
 *   2. qwen3_silu() / qwen3_swiglu(): fixed-point SiLU activation and SwiGLU gating
 *   3. qwen3_rope_pair(): fixed-point Q15 Rotary Position Embedding pair rotation
 *   4. qwen3_kv_cache + qwen3_attn_head(): bounded KV-cache and causal single-head attention
 */
#ifndef AURORA_QWEN3_OPS_H
#define AURORA_QWEN3_OPS_H

#include <stddef.h>
#include <stdint.h>

#define Q3_MAX_DIM      128u
#define Q3_MAX_HEAD_DIM 32u
#define Q3_MAX_SEQ      32u

enum q3_err {
    Q3_OK = 0,
    Q3_E_ARG = -1,
    Q3_E_DIMS = -2,
    Q3_E_FULL = -3
};

static inline int8_t q3_clamp_i8(int32_t v)
{
    if (v > 127) return 127;
    if (v < -128) return -128;
    return (int8_t)v;
}

static inline uint32_t q3_isqrt64(uint64_t x)
{
    if (x == 0) return 0;
    uint64_t r = 0;
    uint64_t bit = 1ULL << 62;
    while (bit > x) bit >>= 2;
    while (bit != 0) {
        if (x >= r + bit) {
            x -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* RMSNorm: out[i] = clamp( (x[i] * gamma[i]) / rms, -128, 127 ), where rms = isqrt(mean(x^2) + eps).
 * Scale factor Q7 (gamma in [-128..127] representing ~[-1..1]) keeps output in int8 range. */
static inline int qwen3_rmsnorm(const int8_t *x, const int8_t *gamma, size_t dim, uint32_t eps, int8_t *out)
{
    if (!x || !gamma || !out) return Q3_E_ARG;
    if (dim == 0 || dim > Q3_MAX_DIM) return Q3_E_DIMS;

    uint64_t sq_sum = 0;
    for (size_t i = 0; i < dim; i++) {
        int32_t v = (int32_t)x[i];
        sq_sum += (uint64_t)((int64_t)v * (int64_t)v);
    }
    uint64_t mean_sq = sq_sum / (uint64_t)dim;
    uint32_t rms = q3_isqrt64(mean_sq + (uint64_t)eps);
    if (rms == 0) rms = 1u;

    for (size_t i = 0; i < dim; i++) {
        int64_t num = (int64_t)x[i] * (int64_t)gamma[i];
        int32_t scaled = (int32_t)(num / (int64_t)rms);
        out[i] = q3_clamp_i8(scaled);
    }
    return Q3_OK;
}

/* Fixed-point piecewise-linear SiLU: x * sigmoid(x) for int8 input in [-128..127].
 * Approximates sigmoid(x / 32) in Q7 [0..128], so:
 *   x <= -128 -> 0; x == 0 -> 0; x >= 127 -> ~125. */
static inline int8_t qwen3_silu_i8(int8_t x)
{
    int32_t xi = (int32_t)x;
    int32_t sig_q7;
    if (xi <= -96) {
        sig_q7 = 0;
    } else if (xi >= 96) {
        sig_q7 = 128;
    } else {
        /* Linear ramp from 0 at -96 to 128 at +96: (xi + 96) * 128 / 192 */
        sig_q7 = ((xi + 96) * 2) / 3;
    }
    int32_t res = (xi * sig_q7) >> 7;
    return q3_clamp_i8(res);
}

/* SwiGLU elementwise: out[i] = clamp((silu(gate[i]) * up[i]) >> shift, -128, 127). */
static inline int qwen3_swiglu(const int8_t *gate, const int8_t *up, size_t dim, uint32_t shift, int8_t *out)
{
    if (!gate || !up || !out) return Q3_E_ARG;
    if (dim == 0 || dim > Q3_MAX_DIM || shift > 15u) return Q3_E_DIMS;
    for (size_t i = 0; i < dim; i++) {
        int32_t s = (int32_t)qwen3_silu_i8(gate[i]);
        int32_t prod = (s * (int32_t)up[i]) >> shift;
        out[i] = q3_clamp_i8(prod);
    }
    return Q3_OK;
}

/* RoPE pair rotation with Q15 cos/sin (cos_q15, sin_q15 in [-32767..32767]):
 *   out0 = (x0 * cos - x1 * sin) >> 15
 *   out1 = (x0 * sin + x1 * cos) >> 15 */
static inline void qwen3_rope_pair(int8_t x0, int8_t x1, int16_t cos_q15, int16_t sin_q15, int8_t *out0, int8_t *out1)
{
    if (!out0 || !out1) return;
    int32_t r0 = ((int32_t)x0 * (int32_t)cos_q15 - (int32_t)x1 * (int32_t)sin_q15) >> 15;
    int32_t r1 = ((int32_t)x0 * (int32_t)sin_q15 + (int32_t)x1 * (int32_t)cos_q15) >> 15;
    *out0 = q3_clamp_i8(r0);
    *out1 = q3_clamp_i8(r1);
}

/* Bounded KV cache for single-head causal attention. */
struct qwen3_kv_cache {
    uint32_t head_dim;
    uint32_t seq_len;
    int8_t k[Q3_MAX_SEQ][Q3_MAX_HEAD_DIM];
    int8_t v[Q3_MAX_SEQ][Q3_MAX_HEAD_DIM];
};

static inline int qwen3_kv_init(struct qwen3_kv_cache *c, uint32_t head_dim)
{
    if (!c) return Q3_E_ARG;
    if (head_dim == 0 || head_dim > Q3_MAX_HEAD_DIM) return Q3_E_DIMS;
    c->head_dim = head_dim;
    c->seq_len = 0;
    for (size_t t = 0; t < Q3_MAX_SEQ; t++) {
        for (size_t d = 0; d < Q3_MAX_HEAD_DIM; d++) {
            c->k[t][d] = 0;
            c->v[t][d] = 0;
        }
    }
    return Q3_OK;
}

static inline int qwen3_kv_push(struct qwen3_kv_cache *c, const int8_t *k_vec, const int8_t *v_vec)
{
    if (!c || !k_vec || !v_vec) return Q3_E_ARG;
    if (c->head_dim == 0 || c->head_dim > Q3_MAX_HEAD_DIM) return Q3_E_DIMS;
    if (c->seq_len >= Q3_MAX_SEQ) return Q3_E_FULL;
    for (uint32_t d = 0; d < c->head_dim; d++) {
        c->k[c->seq_len][d] = k_vec[d];
        c->v[c->seq_len][d] = v_vec[d];
    }
    c->seq_len++;
    return Q3_OK;
}

/* Causal integer attention over cached [0..seq_len) keys/values using ReLU-squared / shifted
 * non-negative score normalization (no exp/float needed, deterministic integer weights). */
static inline int qwen3_attn_head(const struct qwen3_kv_cache *c, const int8_t *q, int8_t *out)
{
    if (!c || !q || !out) return Q3_E_ARG;
    if (c->head_dim == 0 || c->head_dim > Q3_MAX_HEAD_DIM || c->seq_len == 0 || c->seq_len > Q3_MAX_SEQ)
        return Q3_E_DIMS;

    int32_t scores[Q3_MAX_SEQ];
    int32_t max_s = INT32_MIN;
    for (uint32_t t = 0; t < c->seq_len; t++) {
        int32_t dot = 0;
        for (uint32_t d = 0; d < c->head_dim; d++) {
            dot += (int32_t)q[d] * (int32_t)c->k[t][d];
        }
        scores[t] = dot;
        if (dot > max_s) max_s = dot;
    }

    /* Convert relative scores (shifted to >= 0 with floor 1) into integer weights. */
    uint64_t w_sum = 0;
    uint32_t w[Q3_MAX_SEQ];
    for (uint32_t t = 0; t < c->seq_len; t++) {
        int32_t rel = scores[t] - max_s + 256;
        w[t] = rel > 0 ? (uint32_t)rel : 1u;
        w_sum += w[t];
    }
    if (w_sum == 0) w_sum = 1;

    for (uint32_t d = 0; d < c->head_dim; d++) {
        int64_t acc = 0;
        for (uint32_t t = 0; t < c->seq_len; t++) {
            acc += (int64_t)w[t] * (int64_t)c->v[t][d];
        }
        out[d] = q3_clamp_i8((int32_t)(acc / (int64_t)w_sum));
    }
    return Q3_OK;
}

#endif /* AURORA_QWEN3_OPS_H */
