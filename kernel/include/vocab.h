#ifndef AURORA_VOCAB_H
#define AURORA_VOCAB_H

/*
 * kernel/include/vocab.h — bounded vocab/merge-table loader from a byte buffer (AI-10).
 *
 * Loads a vocabulary and a BPE merge table from a caller-supplied byte buffer
 * (NOT from host files). Every read is bounds-checked against buf_len; every write
 * is bounds-checked against caller-provided output capacities. No allocation, no
 * libc, no filesystem, no global state. EL0 / unprivileged safe.
 *
 * On-disk/in-buffer format (little-endian, packed, no padding):
 *   vocab:  u32 count
 *           count x { u32 id; u16 piece_len; piece_len bytes of piece text (no NUL) }
 *   merges: u32 count
 *           count x { u32 left; u32 right; u32 out; u32 rank }
 */

#include <stdint.h>
#include <stddef.h>

#include <tokenizer.h>   /* struct tok_entry and struct tok_merge are defined here */

/* Load a vocab from `buf` (length `buf_len`) into `out` (capacity `out_cap`).
 * Piece text is NUL-terminated into `piece_store` (capacity `piece_store_cap`);
 * out[i].piece points into piece_store. Returns the number of entries loaded, or
 * a negative error: -1 bad args, -2 buffer truncated/malformed, -3 output too
 * small (entries or piece store). */
int vocab_load(const uint8_t *buf, size_t buf_len,
               struct tok_entry *out, size_t out_cap,
               char *piece_store, size_t piece_store_cap);

/* Load a merge table from `buf` (length `buf_len`) into `out` (capacity `out_cap`).
 * Returns the number of merges loaded, or a negative error (same codes as above). */
int merges_load(const uint8_t *buf, size_t buf_len,
                struct tok_merge *out, size_t out_cap);

/* Read a little-endian u32 from `p`; returns 0 and sets *out on success, -1 if
 * fewer than 4 bytes remain at `base + limit`. */
static inline int rd_u32(const uint8_t *buf, size_t buf_len, size_t *pos, uint32_t *out)
{
    if (!buf || !pos || !out) return -1;
    if (*pos + 4 > buf_len) return -2;
    *out = (uint32_t)buf[*pos]       |
           ((uint32_t)buf[*pos+1] << 8) |
           ((uint32_t)buf[*pos+2] << 16) |
           ((uint32_t)buf[*pos+3] << 24);
    *pos += 4;
    return 0;
}

/* Read a little-endian u16; same error convention. */
static inline int rd_u16(const uint8_t *buf, size_t buf_len, size_t *pos, uint32_t *out)
{
    if (!buf || !pos || !out) return -1;
    if (*pos + 2 > buf_len) return -2;
    *out = (uint32_t)buf[*pos] | ((uint32_t)buf[*pos+1] << 8);
    *pos += 2;
    return 0;
}

#endif /* AURORA_VOCAB_H */
