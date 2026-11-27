#ifndef AURORA_QWEN3_PROJECTION_H
#define AURORA_QWEN3_PROJECTION_H

#include <quant_ops.h>
#include <qwen3_model.h>

/*
 * Multiply a validated row-major AURQ3V1 int8 tensor by one input vector.
 * The scratch buffer holds one or more rows and may be reused across tensors.
 * Scratch, input and output buffers must be disjoint. Discard the output on
 * error: a read failure may leave a prefix of the projection in the buffer.
 */
static inline int q3m_project_i8(const struct q3m_tensor_desc *tensor,
                                 q3m_read_fn read, void *ctx,
                                 const int8_t *input, size_t input_len,
                                 int8_t *output, size_t output_len,
                                 int8_t *scratch, size_t scratch_len)
{
    if (!tensor || !read || !input || !output || !scratch) return Q3M_E_ARG;

    size_t rows = tensor->rows;
    size_t cols = tensor->cols;
    if (!rows || !cols || cols > Q3M_MAX_INTER || tensor->shift > 31u)
        return Q3M_E_DIMS;
    if (cols != input_len || rows > output_len || cols > scratch_len)
        return Q3M_E_BOUNDS;

    uint64_t bytes = (uint64_t)rows * cols;
    if (tensor->bytes != bytes || bytes > UINT64_MAX - tensor->offset)
        return Q3M_E_BOUNDS;

    /* Batch whole rows within one model read when their width allows it. */
    size_t batch_rows = scratch_len / cols;
    size_t rows_per_read = Q3M_CHUNK / cols;
    if (rows_per_read == 0) rows_per_read = 1;
    if (batch_rows > rows_per_read) batch_rows = rows_per_read;

    for (size_t row = 0; row < rows;) {
        size_t count = rows - row;
        if (count > batch_rows) count = batch_rows;
        uint64_t offset = tensor->offset + (uint64_t)row * cols;
        int rc = q3m_read_exact(read, ctx, offset, (uint8_t *)scratch, count * cols);
        if (rc != Q3M_OK) return rc;
        if (quant_matvec(scratch, count, cols, input, 1, tensor->shift, 0,
                         output + row) != 0)
            return Q3M_E_DIMS;
        row += count;
    }
    return Q3M_OK;
}

#endif /* AURORA_QWEN3_PROJECTION_H */
