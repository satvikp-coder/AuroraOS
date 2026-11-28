#ifndef AURORA_QWEN3_TOKEN_SELECT_H
#define AURORA_QWEN3_TOKEN_SELECT_H

#include <quant_ops.h>
#include <qwen3_model.h>

/* A positive result permits the token. Zero masks it. A negative result aborts
 * selection and propagates the error. The caller owns tokenizer and EOS policy. */
typedef int (*q3m_token_filter_fn)(void *ctx, uint32_t token);

/* Greedy selection from an int8 output matrix or tied embedding matrix. Scores
 * use the unrounded dot product because all rows have the same scale. This
 * preserves ordering even when an int8 projection would saturate. Returns the
 * lowest ID on a tie and writes the result only on success. */
static inline int q3m_select_token(const struct q3m_tensor_desc *tensor,
                                   q3m_read_fn read, void *read_ctx,
                                   const int8_t *input, size_t input_len,
                                   int8_t *scratch, size_t scratch_len,
                                   q3m_token_filter_fn filter, void *filter_ctx,
                                   uint32_t *token)
{
    if (!tensor || !read || !input || !scratch || !token) return Q3M_E_ARG;
    if (tensor->role != Q3M_T_EMBED && tensor->role != Q3M_T_LM_HEAD)
        return Q3M_E_FORMAT;
    size_t rows = tensor->rows, cols = tensor->cols;
    if (!rows || rows > Q3M_MAX_VOCAB || !cols || cols > Q3M_MAX_HIDDEN ||
        tensor->shift > 31u)
        return Q3M_E_DIMS;
    uint64_t bytes = (uint64_t)rows * cols;
    if (input_len != cols || scratch_len < cols || bytes != tensor->bytes ||
        bytes > UINT64_MAX - tensor->offset)
        return Q3M_E_BOUNDS;

    size_t batch_rows = scratch_len / cols;
    if (batch_rows > Q3M_CHUNK / cols) batch_rows = Q3M_CHUNK / cols;
    uint32_t best = UINT32_MAX;
    int32_t best_score = INT32_MIN;
    for (size_t row = 0; row < rows;) {
        size_t count = rows - row;
        if (count > batch_rows) count = batch_rows;
        int rc = q3m_read_exact(read, read_ctx, tensor->offset + (uint64_t)row * cols,
                                (uint8_t *)scratch, count * cols);
        if (rc != Q3M_OK) return rc;
        for (size_t i = 0; i < count; ++i) {
            uint32_t id = (uint32_t)(row + i);
            if (filter) {
                int allowed = filter(filter_ctx, id);
                if (allowed < 0) return allowed;
                if (!allowed) continue;
            }
            int32_t score;
            if (quant_dot(scratch + i * cols, input, cols, &score) != 0)
                return Q3M_E_DIMS;
            if (best == UINT32_MAX || score > best_score) {
                best_score = score;
                best = id;
            }
        }
        row += count;
    }
    if (best == UINT32_MAX) return Q3M_E_NOENT;
    *token = best;
    return Q3M_OK;
}

#endif
