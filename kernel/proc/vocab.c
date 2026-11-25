/*
 * kernel/proc/vocab.c — bounded vocab/merge-table loader from a byte buffer (AI-10).
 *
 * Pure C: no libc, no dynamic allocation, no filesystem, no global state. All reads
 * bounds-checked against buf_len; all writes bounds-checked against caller capacities.
 * See kernel/include/vocab.h.
 */
#include <vocab.h>

int vocab_load(const uint8_t *buf, size_t buf_len,
               struct tok_entry *out, size_t out_cap,
               char *piece_store, size_t piece_store_cap)
{
    if (!buf || !out || !piece_store) return -1;
    size_t pos = 0;
    uint32_t count = 0;
    if (rd_u32(buf, buf_len, &pos, &count) != 0) return -2;
    if ((size_t)count > out_cap) return -3;
    size_t piece_used = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = 0, piece_len = 0;
        if (rd_u32(buf, buf_len, &pos, &id) != 0) return -2;
        if (rd_u16(buf, buf_len, &pos, &piece_len) != 0) return -2;
        if (pos + (size_t)piece_len > buf_len) return -2;
        /* Need piece_len bytes + NUL in the piece store. */
        if (piece_used + (size_t)piece_len + 1 > piece_store_cap) return -3;
        out[i].id = id;
        out[i].piece = piece_store + piece_used;
        for (uint32_t k = 0; k < piece_len; k++) {
            piece_store[piece_used++] = (char)buf[pos++];
        }
        piece_store[piece_used++] = 0; /* NUL-terminate */
    }
    return (int)count;
}

int merges_load(const uint8_t *buf, size_t buf_len,
                struct tok_merge *out, size_t out_cap)
{
    if (!buf || !out) return -1;
    size_t pos = 0;
    uint32_t count = 0;
    if (rd_u32(buf, buf_len, &pos, &count) != 0) return -2;
    if ((size_t)count > out_cap) return -3;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t left = 0, right = 0, outid = 0, rank = 0;
        if (rd_u32(buf, buf_len, &pos, &left) != 0) return -2;
        if (rd_u32(buf, buf_len, &pos, &right) != 0) return -2;
        if (rd_u32(buf, buf_len, &pos, &outid) != 0) return -2;
        if (rd_u32(buf, buf_len, &pos, &rank) != 0) return -2;
        out[i].left = left;
        out[i].right = right;
        out[i].out = outid;
        out[i].rank = rank;
    }
    return (int)count;
}
