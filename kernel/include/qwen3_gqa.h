/* Bounded fixed-point causal grouped-query attention for EL0 inference. */
#ifndef AURORA_QWEN3_GQA_H
#define AURORA_QWEN3_GQA_H

#include <qwen3_qk.h>

#define Q3_GQA_MAX_HEADS 32u
#define Q3_GQA_MAX_TOKENS 256u

struct q3_gqa_shape {
    uint32_t query_heads;
    uint32_t kv_heads;
    uint32_t width;
    uint32_t tokens;
};

/* exp(-delta / 4096) in Q30. Each factor is round(2^30 * exp(-2^i/4096)).
 * Past 16, the maximum discarded weight is less than 1.13e-7. */
static inline uint32_t q3_gqa_weight(uint32_t delta)
{
    static const uint32_t factors[] = {
        1073479712u, 1073217664u, 1072693760u, 1071646719u,
        1069555701u, 1065385899u, 1057095000u, 1040706261u,
        1008687096u, 947573834u, 836230973u, 651257337u,
        395007542u, 145315154u, 19666268u, 360200u
    };
    if (delta >= 16u * 4096u) return 0;
    uint64_t weight = 1u << 30;
    for (uint32_t bit = 0; delta; ++bit, delta >>= 1) {
        if (delta & 1u)
            weight = (weight * factors[bit] + (1u << 29)) >> 30;
    }
    return (uint32_t)weight;
}

static inline int32_t q3_gqa_score(const int16_t *query, const int16_t *key,
                                   uint32_t width, uint32_t scale)
{
    int64_t dot = 0;
    for (uint32_t i = 0; i < width; ++i)
        dot += (int32_t)query[i] * key[i];
    return q3_qk_round(dot, scale);
}

/*
 * Compute the context for a single query position. All vectors are signed Q12.
 * Query and output are [query_head][channel]. Key/value are
 * [token][kv_head][channel]. Key and value share a length, expressed in
 * elements. Input buffers and output must be disjoint.
 *
 * The caller owns the cache and supplies its actual lengths. Only tokens
 * 0..position are read. Head groups use the same KV-head ordering as Qwen3.
 * Scratch is one channel accumulator array, independent of context length.
 */
static inline int q3_gqa_context(const struct q3_gqa_shape *shape, uint32_t position,
                                 const int16_t *query, size_t query_len,
                                 const int16_t *keys, const int16_t *values, size_t kv_len,
                                 int16_t *output, size_t output_len)
{
    if (!shape || !query || !keys || !values || !output) return Q3_E_ARG;
    uint32_t width = shape->width;
    if (!width || width > Q3_QK_MAX_WIDTH || !shape->query_heads ||
        shape->query_heads > Q3_GQA_MAX_HEADS || !shape->kv_heads ||
        shape->query_heads % shape->kv_heads || shape->tokens > Q3_GQA_MAX_TOKENS ||
        position >= shape->tokens)
        return Q3_E_DIMS;

    size_t stride = (size_t)shape->kv_heads * width;
    size_t query_elements = (size_t)shape->query_heads * width;
    if (query_len < query_elements || output_len < query_elements ||
        kv_len < (size_t)shape->tokens * stride)
        return Q3_E_DIMS;

    uint32_t group = shape->query_heads / shape->kv_heads;
    /* The Q24 dot product divided by sqrt(width) in Q12 yields Q12 scores. */
    uint32_t scale = q3_isqrt64((uint64_t)width << 24);
    for (uint32_t head = 0; head < shape->query_heads; ++head) {
        size_t offset = (size_t)(head / group) * width;
        const int16_t *q = query + (size_t)head * width;
        int32_t peak = INT32_MIN;
        for (uint32_t token = 0; token <= position; ++token) {
            const int16_t *k = keys + token * stride + offset;
            int32_t score = q3_gqa_score(q, k, width, scale);
            if (score > peak) peak = score;
        }

        int64_t accum[Q3_QK_MAX_WIDTH];
        for (uint32_t i = 0; i < width; ++i) accum[i] = 0;
        int64_t total = 0;
        for (uint32_t token = 0; token <= position; ++token) {
            size_t start = token * stride + offset;
            int32_t score = q3_gqa_score(q, keys + start, width, scale);
            uint32_t weight = q3_gqa_weight((uint32_t)(peak - score));
            total += weight;
            for (uint32_t i = 0; i < width; ++i)
                accum[i] += (int64_t)weight * values[start + i];
        }
        /* At least one score equals the peak, so total is positive. */
        for (uint32_t i = 0; i < width; ++i) {
            int64_t rounding = accum[i] < 0 ? -total / 2 : total / 2;
            output[(size_t)head * width + i] = (int16_t)((accum[i] + rounding) / total);
        }
    }
    return Q3_OK;
}

#endif
