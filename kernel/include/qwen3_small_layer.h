/* Small-shape streamed decoder-layer reference. Fixed Q5 residual/MLP activations. */
#ifndef AURORA_QWEN3_SMALL_LAYER_H
#define AURORA_QWEN3_SMALL_LAYER_H

#include <qwen3_gqa.h>
#include <qwen3_projection.h>

#define Q3L_WIDTH 32u   /* maximum hidden, intermediate, or concatenated Q width */
#define Q3L_TOKENS 8u

struct q3l_weights {
    struct q3m_tensor_desc q, k, v, o, gate, up, down;
    /* Caller-provided signed Q12 vectors: hidden, hidden, head width, head width. */
    const int16_t *input_gain, *post_gain, *q_gain, *k_gain;
};

struct q3l_work {
    int16_t keys[Q3L_TOKENS * Q3L_WIDTH], values[Q3L_TOKENS * Q3L_WIDTH];
    int16_t queries[Q3L_WIDTH], context[Q3L_WIDTH];
    int8_t norm[Q3L_WIDTH], projected[Q3L_WIDTH], scratch[Q3L_WIDTH];
    int8_t residual[Q3L_WIDTH], gate[Q3L_WIDTH], up[Q3L_WIDTH], product[Q3L_WIDTH];
};

static inline void q3l_norm(const int8_t *x, const int16_t *gain, uint32_t n, int8_t *y)
{
    uint64_t squares = 0;
    for (uint32_t i = 0; i < n; ++i) {
        int32_t value = x[i];
        squares += (uint32_t)(value * value);
    }
    uint32_t rms_q16 = q3_isqrt64((squares << 22) / n + Q3_QK_EPSILON_Q32);
    for (uint32_t i = 0; i < n; ++i) {
        int64_t num = (int64_t)x[i] * gain[i] * 16;
        y[i] = q3_clamp_i8(q3_qk_round(num, rms_q16));
    }
}

static inline int q3l_project(const struct q3m_tensor_desc *d, q3m_read_fn read,
                             void *ctx, struct q3l_work *w, const int8_t *x,
                             uint32_t cols, uint32_t rows, int8_t *y)
{
    if (d->rows != rows || d->cols != cols) return Q3M_E_DIMS;
    return q3m_project_i8(d, read, ctx, x, cols, y, rows, w->scratch, sizeof(w->scratch));
}

static inline int q3l_prepare(const int8_t *x, const int16_t *gain, const int16_t *cosine,
                             const int16_t *sine, uint32_t heads, uint32_t dim, int16_t *out)
{
    for (uint32_t h = 0; h < heads; ++h) {
        for (uint32_t i = 0; i < dim; ++i) out[i] = x[i] * 8; /* Q5 -> Q8 */
        int rc = q3_qk_prepare(out, gain, cosine, sine, dim, out);
        if (rc) return rc;
        x += dim;
        out += dim;
    }
    return 0;
}

/* Evaluate an entire causal span. Input/output hold tokens * hidden elements;
 * both trig arrays hold tokens * (width/2) Q15 elements. All activation and
 * scratch buffers are disjoint. Gains are predecoded by the caller. Matrix
 * entries are signed int8 / 2^descriptor.shift. Discard output on error.
 * qwen3_swiglu uses its existing piecewise sigmoid approximation; the Q5
 * product returns to Q5 with shift 5. All Q5 transitions saturate to int8. */
static inline int q3l_forward(const struct q3_gqa_shape *s, uint32_t hidden, uint32_t inter,
                              const struct q3l_weights *m, q3m_read_fn read, void *ctx,
                              const int16_t *cosine, const int16_t *sine, size_t trig_len,
                              const int8_t *input, int8_t *output, size_t activation_len,
                              struct q3l_work *w)
{
    if (!s || !m || !read || !cosine || !sine || !input || !output || !w ||
        !m->input_gain || !m->post_gain || !m->q_gain || !m->k_gain) return Q3M_E_ARG;
    if (!hidden || hidden > Q3L_WIDTH || !inter || inter > Q3L_WIDTH ||
        !s->width || s->width % 2 || !s->query_heads || !s->kv_heads ||
        s->query_heads % s->kv_heads || s->query_heads > Q3L_WIDTH / s->width ||
        !s->tokens || s->tokens > Q3L_TOKENS) return Q3M_E_DIMS;
    uint32_t qwidth = s->query_heads * s->width, kvwidth = s->kv_heads * s->width;
    if (trig_len < s->tokens * (s->width / 2) || activation_len < s->tokens * hidden)
        return Q3M_E_BOUNDS;

    for (uint32_t t = 0; t < s->tokens; ++t) {
        q3l_norm(input, m->input_gain, hidden, w->norm);
        int rc = q3l_project(&m->q, read, ctx, w, w->norm, hidden, qwidth, w->projected);
        if (rc) return rc;
        rc = q3l_prepare(w->projected, m->q_gain, cosine, sine, s->query_heads, s->width, w->queries);
        if (rc) return rc;
        rc = q3l_project(&m->k, read, ctx, w, w->norm, hidden, kvwidth, w->projected);
        if (rc) return rc;
        rc = q3l_prepare(w->projected, m->k_gain, cosine, sine, s->kv_heads, s->width,
                         w->keys + t * kvwidth);
        if (rc) return rc;
        rc = q3l_project(&m->v, read, ctx, w, w->norm, hidden, kvwidth, w->projected);
        if (rc) return rc;
        for (uint32_t i = 0; i < kvwidth; ++i) w->values[t * kvwidth + i] = w->projected[i] * 128;
        rc = q3_gqa_context(s, t, w->queries, qwidth, w->keys, w->values,
                            s->tokens * kvwidth, w->context, qwidth);
        if (rc) return rc;
        for (uint32_t i = 0; i < qwidth; ++i)
            w->projected[i] = q3_clamp_i8(q3_qk_round(w->context[i], 128));
        rc = q3l_project(&m->o, read, ctx, w, w->projected, qwidth, hidden, w->residual);
        if (rc) return rc;
        for (uint32_t i = 0; i < hidden; ++i)
            w->residual[i] = q3_clamp_i8((int32_t)w->residual[i] + input[i]);
        q3l_norm(w->residual, m->post_gain, hidden, w->norm);
        rc = q3l_project(&m->gate, read, ctx, w, w->norm, hidden, inter, w->gate);
        if (rc) return rc;
        rc = q3l_project(&m->up, read, ctx, w, w->norm, hidden, inter, w->up);
        if (rc) return rc;
        rc = qwen3_swiglu(w->gate, w->up, inter, 5, w->product);
        if (rc) return rc;
        rc = q3l_project(&m->down, read, ctx, w, w->product, inter, hidden, w->projected);
        if (rc) return rc;
        for (uint32_t i = 0; i < hidden; ++i)
            output[i] = q3_clamp_i8((int32_t)w->residual[i] + w->projected[i]);
        input += hidden;
        output += hidden;
        cosine += s->width / 2;
        sine += s->width / 2;
    }
    return 0;
}
#endif
