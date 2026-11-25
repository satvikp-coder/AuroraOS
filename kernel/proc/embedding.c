/*
 * kernel/proc/embedding.c — bounded embedding table lookup (AI-08).
 *
 * Pure C: no libc, no dynamic allocation, no global state. Index and output
 * bounds are checked before any write. See kernel/include/embedding.h.
 */
#include <embedding.h>

int embedding_lookup(const int8_t *table, size_t vocab_size, size_t dim,
                     uint32_t token_id, int8_t *out, size_t out_cap)
{
    if (!table || !out) return -1;
    if (vocab_size == 0 || dim == 0) return -1;
    if ((size_t)token_id >= vocab_size) return -1;
    if (out_cap < dim) return -1;
    const int8_t *row = table + (size_t)token_id * dim;
    for (size_t i = 0; i < dim; i++) out[i] = row[i];
    return 0;
}

int embedding_lookup_batch(const int8_t *table, size_t vocab_size, size_t dim,
                           const uint32_t *token_ids, size_t n,
                           int8_t *out, size_t out_cap)
{
    if (!table || !token_ids || !out) return -1;
    if (vocab_size == 0 || dim == 0) return -1;
    if (n > (size_t)1 << 20) return -1;            /* keep n*dim from overflowing */
    if (dim > (size_t)1 << 20) return -1;
    if (out_cap / dim < n) return -1;              /* overflow-safe n*dim <= out_cap */
    size_t w = 0;
    for (size_t k = 0; k < n; k++) {
        if ((size_t)token_ids[k] >= vocab_size) return -1;
        const int8_t *row = table + (size_t)token_ids[k] * dim;
        for (size_t i = 0; i < dim; i++) out[w++] = row[i];
    }
    return 0;
}
