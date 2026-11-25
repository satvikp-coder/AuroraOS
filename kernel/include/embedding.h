#ifndef AURORA_EMBEDDING_H
#define AURORA_EMBEDDING_H

/*
 * kernel/include/embedding.h — bounded embedding table lookup (AI-08).
 *
 * Looks up the row for a token id in a vocab_size x dim int8 embedding table,
 * with index bounds checking and output-bounds checking. No allocation, no libc,
 * no global state. EL0 / unprivileged safe.
 */

#include <stdint.h>
#include <stddef.h>

/* Copy table[token_id * dim .. (token_id+1)*dim) into out (capacity out_cap).
 * Returns 0 on success, -1 if token_id >= vocab_size, out_cap < dim, or any
 * pointer is NULL. */
int embedding_lookup(const int8_t *table, size_t vocab_size, size_t dim,
                     uint32_t token_id, int8_t *out, size_t out_cap);

/* Look up multiple tokens: token_ids[n] rows of `dim` each, written contiguously
 * into out (capacity out_cap >= n*dim). Returns 0 on success, -1 on bad args or
 * if any token id is out of range. */
int embedding_lookup_batch(const int8_t *table, size_t vocab_size, size_t dim,
                           const uint32_t *token_ids, size_t n,
                           int8_t *out, size_t out_cap);

#endif /* AURORA_EMBEDDING_H */
