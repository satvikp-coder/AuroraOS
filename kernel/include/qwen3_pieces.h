/* Direct lookup in an AURQTP1 byte-piece table, using a SYS_MODEL-style reader. */
#ifndef AURORA_QWEN3_PIECES_H
#define AURORA_QWEN3_PIECES_H

#include <qwen3_model.h>

#define Q3P_SPECIAL UINT32_C(0x80000000)
#define Q3P_MAX_PIECE 4096u

/* Returns the piece byte length or a negative q3m_err. Pass the exact object
 * size. UTF-8 decoding belongs after concatenation. The caller may skip pieces
 * marked special. No terminator is added; a piece may contain arbitrary bytes. */
static inline int q3p_read_piece(q3m_read_fn rd, void *ctx, uint64_t size,
                                 uint32_t id, uint8_t *out, size_t cap, int *special)
{
    if (!rd || !out || !special) return Q3M_E_ARG;
    uint8_t header[12];
    if (size < sizeof(header)) return Q3M_E_TRUNC;
    int rc = q3m_read_exact(rd, ctx, 0, header, sizeof(header));
    if (rc != Q3M_OK) return rc;
    static const uint8_t magic[8] = {'A','U','R','Q','T','P','1',0};
    for (unsigned i = 0; i < sizeof(magic); ++i)
        if (header[i] != magic[i]) return Q3M_E_MAGIC;
    uint32_t count = q3m_le32(header + 8);
    if (!count || count > Q3M_MAX_VOCAB) return Q3M_E_FORMAT;
    uint64_t pool = sizeof(header) + 4u * (count + 1u);
    if (pool > size || size - pool >= Q3P_SPECIAL) return Q3M_E_BOUNDS;
    if (id >= count) return Q3M_E_NOENT;
    uint8_t offsets[8];
    rc = q3m_read_exact(rd, ctx, sizeof(header) + 4u * id, offsets, sizeof(offsets));
    if (rc != Q3M_OK) return rc;
    uint32_t first = q3m_le32(offsets), next = q3m_le32(offsets + 4);
    uint32_t start = first & ~Q3P_SPECIAL, end = next & ~Q3P_SPECIAL;
    if ((id == 0 && start != 0) || (id + 1 == count && next != size - pool) ||
        end <= start || end > size - pool) return Q3M_E_BOUNDS;
    uint32_t length = end - start;
    if (length > Q3P_MAX_PIECE || length > cap) return Q3M_E_BOUNDS;
    rc = q3m_read_exact(rd, ctx, pool + start, out, length);
    if (rc != Q3M_OK) return rc;
    *special = (first & Q3P_SPECIAL) != 0;
    return (int)length;
}
#endif
