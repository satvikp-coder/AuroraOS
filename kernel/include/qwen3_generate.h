/*
 * kernel/include/qwen3_generate.h - Bounded autoregressive multi-layer Qwen3 token generation (AI-07).
 *
 * Pure C, freestanding, no allocation, no floating point, no libc, no globals.
 * Connects streamed embeddings -> N layers of q3l_forward() -> final q3l_norm() ->
 * q3m_select_token() in an autoregressive loop up to Q3L_TOKENS (8) total tokens,
 * stopping on EOS or max_new_tokens.
 */
#ifndef AURORA_QWEN3_GENERATE_H
#define AURORA_QWEN3_GENERATE_H

#include <qwen3_small_layer.h>
#include <qwen3_token_select.h>

#define Q3G_MAX_LAYERS 4u

struct q3g_model {
    uint32_t hidden;
    uint32_t inter;
    uint32_t num_layers;                    /* 1..Q3G_MAX_LAYERS */
    struct q3_gqa_shape attn;
    struct q3m_tensor_desc embed;           /* [vocab_size, hidden] */
    struct q3m_tensor_desc lm_head;         /* [vocab_size, hidden] (may equal embed) */
    struct q3l_weights layers[Q3G_MAX_LAYERS];
    const int16_t *final_norm_gain;         /* [hidden] signed Q12 */
};

struct q3g_work {
    struct q3l_work layer_work;
    int8_t act_a[Q3L_TOKENS * Q3L_WIDTH];
    int8_t act_b[Q3L_TOKENS * Q3L_WIDTH];
    int8_t final_norm[Q3L_WIDTH];
    int8_t select_scratch[Q3M_CHUNK];
};

static inline int q3g_embed_span(const struct q3m_tensor_desc *embed,
                                 q3m_read_fn read,
                                 void *ctx,
                                 const uint32_t *tokens,
                                 uint32_t ntok,
                                 uint32_t hidden,
                                 int8_t *out)
{
    if (!embed || !read || !tokens || !out) return Q3M_E_ARG;
    if (embed->role != Q3M_T_EMBED || embed->cols != hidden) return Q3M_E_FORMAT;
    for (uint32_t t = 0; t < ntok; ++t) {
        if (tokens[t] >= embed->rows) return Q3M_E_BOUNDS;
        uint64_t off = embed->offset + (uint64_t)tokens[t] * (uint64_t)hidden;
        int rc = q3m_read_exact(read, ctx, off, (uint8_t *)(out + (size_t)t * hidden), hidden);
        if (rc != Q3M_OK) return rc;
    }
    return Q3M_OK;
}

/* Autoregressively generate up to `max_new_tokens` starting from `prompt_len` tokens in `tokens[0..cap)`.
 * Writes total sequence length to `*out_total_len`. Stops early on `eos_token`. */
static inline int q3g_generate(const struct q3g_model *m,
                               q3m_read_fn read,
                               void *read_ctx,
                               const int16_t *cosine,
                               const int16_t *sine,
                               size_t trig_len,
                               uint32_t *tokens,
                               uint32_t prompt_len,
                               uint32_t cap,
                               uint32_t max_new_tokens,
                               uint32_t eos_token,
                               q3m_token_filter_fn filter,
                               void *filter_ctx,
                               struct q3g_work *w,
                               uint32_t *out_total_len)
{
    if (out_total_len) *out_total_len = 0;
    if (!m || !read || !cosine || !sine || !tokens || !w || !out_total_len || !m->final_norm_gain)
        return Q3M_E_ARG;
    if (m->num_layers == 0 || m->num_layers > Q3G_MAX_LAYERS) return Q3M_E_DIMS;
    if (m->hidden == 0 || m->hidden > Q3L_WIDTH || m->inter == 0 || m->inter > Q3L_WIDTH)
        return Q3M_E_DIMS;
    if (prompt_len == 0 || prompt_len > Q3L_TOKENS || cap < prompt_len || cap > Q3L_TOKENS)
        return Q3M_E_BOUNDS;

    uint32_t seq_len = prompt_len;
    for (uint32_t step = 0; step < max_new_tokens && seq_len < cap; ++step) {
        int rc = q3g_embed_span(&m->embed, read, read_ctx, tokens, seq_len, m->hidden, w->act_a);
        if (rc != Q3M_OK) return rc;

        struct q3_gqa_shape shape = m->attn;
        shape.tokens = seq_len;

        int8_t *cur_in = w->act_a;
        int8_t *cur_out = w->act_b;
        for (uint32_t l = 0; l < m->num_layers; ++l) {
            rc = q3l_forward(&shape, m->hidden, m->inter, &m->layers[l], read, read_ctx,
                             cosine, sine, trig_len, cur_in, cur_out,
                             (size_t)seq_len * m->hidden, &w->layer_work);
            if (rc != Q3M_OK) return rc;
            int8_t *tmp = cur_in;
            cur_in = cur_out;
            cur_out = tmp;
        }

        const int8_t *last_hidden = cur_in + (size_t)(seq_len - 1u) * m->hidden;
        q3l_norm(last_hidden, m->final_norm_gain, m->hidden, w->final_norm);

        uint32_t next_tok = 0;
        rc = q3m_select_token(&m->lm_head, read, read_ctx, w->final_norm, m->hidden,
                              w->select_scratch, sizeof(w->select_scratch),
                              filter, filter_ctx, &next_tok);
        if (rc != Q3M_OK) return rc;

        tokens[seq_len++] = next_tok;
        if (next_tok == eos_token) break;
    }

    *out_total_len = seq_len;
    return Q3M_OK;
}

#endif /* AURORA_QWEN3_GENERATE_H */
