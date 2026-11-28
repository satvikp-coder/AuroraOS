/* Binary search over a trusted, exporter-sorted AURQTM1 merge table. */
#ifndef AURORA_QWEN3_MERGES_H
#define AURORA_QWEN3_MERGES_H

#include <qwen3_model.h>

/* Caller supplies the exact object length. Reads at most one header plus 18
 * 16-byte records for the declared maximum count (short reads may be retried).
 * Returns Q3M_E_NOENT for absent pairs, including non-base token IDs. */
static inline int q3m_find_merge(q3m_read_fn read, void *ctx, uint64_t size,
                                 uint32_t left, uint32_t right,
                                 uint32_t *result, uint32_t *rank)
{
    if (!read || !result || !rank) return Q3M_E_ARG;
    if (size < 16) return Q3M_E_TRUNC;
    uint8_t buf[16];
    int rc = q3m_read_exact(read, ctx, 0, buf, sizeof(buf));
    if (rc != Q3M_OK) return rc;
    static const uint8_t magic[8] = {'A','U','R','Q','T','M','1',0};
    for (unsigned i = 0; i < sizeof(magic); ++i)
        if (buf[i] != magic[i]) return Q3M_E_MAGIC;
    uint32_t vocab = q3m_le32(buf + 8), count = q3m_le32(buf + 12);
    if (!vocab || vocab > Q3M_MAX_VOCAB || count > Q3M_MAX_VOCAB) return Q3M_E_FORMAT;
    if (size != 16u + 16u * count) return Q3M_E_BOUNDS;
    if (left >= vocab || right >= vocab) return Q3M_E_NOENT;
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        rc = q3m_read_exact(read, ctx, 16u + 16u * mid, buf, sizeof(buf));
        if (rc != Q3M_OK) return rc;
        uint32_t a = q3m_le32(buf), b = q3m_le32(buf + 4);
        uint32_t output = q3m_le32(buf + 8), priority = q3m_le32(buf + 12);
        if (a >= vocab || b >= vocab || output >= vocab || priority >= count)
            return Q3M_E_FORMAT;
        if (a == left && b == right) {
            *result = output;
            *rank = priority;
            return Q3M_OK;
        }
        if (a < left || (a == left && b < right)) lo = mid + 1;
        else hi = mid;
    }
    return Q3M_E_NOENT;
}

#endif
