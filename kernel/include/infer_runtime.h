/*
 * kernel/include/infer_runtime.h - Reusable EL0-safe quantized inference runtime (AI-04).
 *
 * Separates:
 *   1. Model format validation (dense-ReLU-dense quantized MLP descriptor & bounds)
 *   2. Tokenization (via tokenizer.h)
 *   3. Embedding & tensor loading (via tensor.h, bounded, no allocation)
 *   4. Numerical execution (via quant_ops.h: quant_matvec + quant_relu + argmax)
 *   5. Output validation (bounded logits, argmax index, explicit status)
 *
 * Pure C, freestanding, no libc, no dynamic allocation, no global state.
 * Note: This executes bounded synthetic/small quantized MLP graphs; it is NOT Qwen3-0.6B.
 */
#ifndef AURORA_INFER_RUNTIME_H
#define AURORA_INFER_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <tokenizer.h>
#include <tensor.h>
#include <quant_ops.h>

#define IR_MAX_VOCAB_EMBED 256u
#define IR_MAX_DIM         64u
#define IR_MAX_HIDDEN      64u
#define IR_MAX_CLASSES     32u

enum ir_err {
    IR_OK = 0,
    IR_E_ARG = -1,
    IR_E_DIMS = -2,
    IR_E_SCALE = -3,
    IR_E_TOK = -4,
    IR_E_TENSOR = -5,
    IR_E_EXEC = -6,
    IR_E_OUTPUT = -7
};

/* Bounded 2-layer dense-ReLU-dense quantized MLP model descriptor. */
struct ir_mlp_model {
    uint32_t vocab_size;              /* <= IR_MAX_VOCAB_EMBED */
    uint32_t embed_dim;               /* <= IR_MAX_DIM */
    uint32_t hidden_dim;              /* <= IR_MAX_HIDDEN */
    uint32_t num_classes;             /* <= IR_MAX_CLASSES */
    const int8_t *embed_table;        /* [vocab_size * embed_dim] */
    const int8_t *w1;                 /* [hidden_dim * embed_dim] */
    int32_t w1_mult;
    uint32_t w1_shift;
    int8_t w1_zp_out;
    const int8_t *w2;                 /* [num_classes * hidden_dim] */
    int32_t w2_mult;
    uint32_t w2_shift;
    int8_t w2_zp_out;
};

struct ir_result {
    uint32_t token_count;
    uint32_t tokens[TOK_MAX_TOKENS];
    int8_t logits[IR_MAX_CLASSES];
    uint32_t num_classes;
    uint32_t argmax;
    int32_t status;
};

/* 1. Validate model descriptor dimensions, pointers, and quantization parameters. */
static inline int ir_validate_model(const struct ir_mlp_model *m)
{
    if (!m) return IR_E_ARG;
    if (!m->embed_table || !m->w1 || !m->w2) return IR_E_ARG;
    if (m->vocab_size == 0 || m->vocab_size > IR_MAX_VOCAB_EMBED) return IR_E_DIMS;
    if (m->embed_dim == 0 || m->embed_dim > IR_MAX_DIM) return IR_E_DIMS;
    if (m->hidden_dim == 0 || m->hidden_dim > IR_MAX_HIDDEN) return IR_E_DIMS;
    if (m->num_classes == 0 || m->num_classes > IR_MAX_CLASSES) return IR_E_DIMS;
    if (m->w1_mult <= 0 || m->w1_shift > 31u) return IR_E_SCALE;
    if (m->w2_mult <= 0 || m->w2_shift > 31u) return IR_E_SCALE;
    return IR_OK;
}

/* 2. Pool token embeddings (saturating int32 sum -> int8 mean-like shift/clamp) into tok_tensor. */
static inline int ir_embed_tokens(const struct ir_mlp_model *m,
                                  const uint32_t *tokens, size_t ntok,
                                  struct tok_tensor *out)
{
    int rc = ir_validate_model(m);
    if (rc != IR_OK) return rc;
    if (!tokens || !out || ntok == 0) return IR_E_ARG;
    tensor_init(out);

    int32_t acc[IR_MAX_DIM];
    for (uint32_t d = 0; d < m->embed_dim; d++) acc[d] = 0;

    for (size_t i = 0; i < ntok; i++) {
        uint32_t tid = tokens[i];
        uint32_t idx = (tid < m->vocab_size) ? tid : (tid % m->vocab_size);
        const int8_t *row = m->embed_table + (size_t)idx * m->embed_dim;
        for (uint32_t d = 0; d < m->embed_dim; d++) {
            acc[d] += (int32_t)row[d];
        }
    }

    for (uint32_t d = 0; d < m->embed_dim; d++) {
        int32_t v = acc[d] / (int32_t)ntok;
        if (v > 127) v = 127;
        if (v < -128) v = -128;
        if (tensor_push(out, (int8_t)v) != 0) return IR_E_TENSOR;
    }
    return IR_OK;
}

/* 3. Execute dense-ReLU-dense forward pass on a loaded embedding tensor. */
static inline int ir_forward_mlp(const struct ir_mlp_model *m,
                                 const struct tok_tensor *input,
                                 struct ir_result *res)
{
    int rc = ir_validate_model(m);
    if (rc != IR_OK) return rc;
    if (!input || !res) return IR_E_ARG;
    if (tensor_len(input) != m->embed_dim) return IR_E_DIMS;

    int8_t x[IR_MAX_DIM];
    for (uint32_t d = 0; d < m->embed_dim; d++) {
        if (tensor_get(input, d, &x[d]) != 0) return IR_E_TENSOR;
    }

    int8_t h1[IR_MAX_HIDDEN];
    int8_t h1_relu[IR_MAX_HIDDEN];
    if (quant_matvec(m->w1, m->hidden_dim, m->embed_dim, x, m->w1_mult, m->w1_shift, m->w1_zp_out, h1) != 0)
        return IR_E_EXEC;
    if (quant_relu(h1, m->hidden_dim, h1_relu) != 0)
        return IR_E_EXEC;
    if (quant_matvec(m->w2, m->num_classes, m->hidden_dim, h1_relu, m->w2_mult, m->w2_shift, m->w2_zp_out, res->logits) != 0)
        return IR_E_EXEC;

    res->num_classes = m->num_classes;
    uint32_t best = 0;
    for (uint32_t c = 1; c < m->num_classes; c++) {
        if (res->logits[c] > res->logits[best]) best = c;
    }
    res->argmax = best;
    return IR_OK;
}

/* 4. Validate output result invariants. */
static inline int ir_validate_output(const struct ir_result *res, uint32_t expected_classes)
{
    if (!res) return IR_E_ARG;
    if (res->status != IR_OK) return IR_E_OUTPUT;
    if (res->num_classes == 0 || res->num_classes != expected_classes || res->num_classes > IR_MAX_CLASSES)
        return IR_E_OUTPUT;
    if (res->argmax >= res->num_classes) return IR_E_OUTPUT;
    for (uint32_t c = 0; c < res->num_classes; c++) {
        if (res->logits[c] > res->logits[res->argmax]) return IR_E_OUTPUT;
    }
    return IR_OK;
}

/* 5. End-to-end runtime: tokenize -> embed -> forward -> validate output. */
static inline int ir_run_text(struct tokenizer *tok,
                              const struct ir_mlp_model *m,
                              const char *text, size_t text_len,
                              struct ir_result *res)
{
    if (!res) return IR_E_ARG;
    for (size_t i = 0; i < sizeof(*res); i++) ((uint8_t *)res)[i] = 0;
    res->status = IR_E_ARG;

    if (!tok || !m || !text || text_len == 0 || text_len > TOK_MAX_INPUT) return IR_E_ARG;
    int rc = ir_validate_model(m);
    if (rc != IR_OK) { res->status = rc; return rc; }

    int ntok_i = tokenizer_encode(tok, text, text_len, res->tokens, TOK_MAX_TOKENS, 0, 0);
    if (ntok_i <= 0) {
        res->status = IR_E_TOK;
        return IR_E_TOK;
    }
    size_t ntok = (size_t)ntok_i;
    res->token_count = (uint32_t)ntok;

    struct tok_tensor emb;
    rc = ir_embed_tokens(m, res->tokens, ntok, &emb);
    if (rc != IR_OK) { res->status = rc; return rc; }

    rc = ir_forward_mlp(m, &emb, res);
    if (rc != IR_OK) { res->status = rc; return rc; }

    res->status = IR_OK;
    return ir_validate_output(res, m->num_classes);
}

#endif /* AURORA_INFER_RUNTIME_H */
