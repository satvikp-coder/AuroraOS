/*
 * kernel/include/qwen3_model.h - Qwen3 quantized model metadata & weight-streaming loader (AI-06).
 *
 * Pure C, freestanding, no allocation, no libc, no floating point, no globals.
 * Parses an explicit little-endian "AURQ3V1\0" model header + tensor directory via a
 * SYS_MODEL-compatible read callback (chunks <= Q3M_CHUNK = 4096 bytes) and streams
 * individual tensor payloads into caller-supplied bounded buffers.
 */
#ifndef AURORA_QWEN3_MODEL_H
#define AURORA_QWEN3_MODEL_H

#include <stddef.h>
#include <stdint.h>

#define Q3M_HDR_SIZE       48u
#define Q3M_ENTRY_SIZE     32u
#define Q3M_CHUNK          4096u
#define Q3M_MAX_LAYERS     32u
#define Q3M_MAX_TENSORS    256u
#define Q3M_MAX_VOCAB      151936u
#define Q3M_MAX_HIDDEN     2048u
#define Q3M_MAX_INTER      8192u
#define Q3M_MAX_HEAD_DIM   128u

enum q3m_err {
    Q3M_OK = 0,
    Q3M_E_ARG = -1,
    Q3M_E_MAGIC = -2,
    Q3M_E_VERSION = -3,
    Q3M_E_DIMS = -4,
    Q3M_E_FORMAT = -5,
    Q3M_E_BOUNDS = -6,
    Q3M_E_READ = -7,
    Q3M_E_TRUNC = -8,
    Q3M_E_NOENT = -9
};

enum q3m_tensor_role {
    Q3M_T_EMBED = 1,
    Q3M_T_RMSNORM = 2,
    Q3M_T_WQ = 3,
    Q3M_T_WK = 4,
    Q3M_T_WV = 5,
    Q3M_T_WO = 6,
    Q3M_T_WGATE = 7,
    Q3M_T_WUP = 8,
    Q3M_T_WDOWN = 9,
    Q3M_T_LM_HEAD = 10
};

struct q3m_header {
    uint32_t version;
    uint32_t vocab_size;
    uint32_t hidden_dim;
    uint32_t intermediate_dim;
    uint32_t num_layers;
    uint32_t num_heads;
    uint32_t head_dim;
    uint32_t num_tensors;
    uint64_t total_bytes;
};

struct q3m_tensor_desc {
    uint16_t layer;
    uint16_t role;
    uint32_t rows;
    uint32_t cols;
    uint32_t shift;
    uint64_t offset;
    uint64_t bytes;
};

typedef int64_t (*q3m_read_fn)(void *ctx, uint64_t off, uint8_t *buf, uint64_t len);

static inline uint16_t q3m_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t q3m_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t q3m_le64(const uint8_t *p)
{
    return (uint64_t)q3m_le32(p) | ((uint64_t)q3m_le32(p + 4) << 32);
}

static inline int q3m_read_exact(q3m_read_fn rd, void *ctx, uint64_t off, uint8_t *buf, uint64_t len)
{
    uint64_t done = 0;
    while (done < len) {
        uint64_t want = len - done;
        if (want > Q3M_CHUNK) want = Q3M_CHUNK;
        int64_t got = rd(ctx, off + done, buf + done, want);
        if (got < 0) return Q3M_E_READ;
        if (got == 0 || (uint64_t)got > want) return Q3M_E_TRUNC;
        done += (uint64_t)got;
    }
    return Q3M_OK;
}

/* Parse and validate 48-byte Qwen3 model header. */
static inline int q3m_parse_header(const uint8_t *buf, size_t len, uint64_t obj_size, struct q3m_header *out)
{
    if (!buf || !out) return Q3M_E_ARG;
    if (len < Q3M_HDR_SIZE || obj_size < Q3M_HDR_SIZE) return Q3M_E_TRUNC;
    if (buf[0] != 'A' || buf[1] != 'U' || buf[2] != 'R' || buf[3] != 'Q' ||
        buf[4] != '3' || buf[5] != 'V' || buf[6] != '1' || buf[7] != 0) return Q3M_E_MAGIC;

    uint32_t ver = q3m_le32(buf + 8);
    if (ver != 1u) return Q3M_E_VERSION;

    uint32_t vocab  = q3m_le32(buf + 12);
    uint32_t hidden = q3m_le32(buf + 16);
    uint32_t inter  = q3m_le32(buf + 20);
    uint32_t layers = q3m_le32(buf + 24);
    uint32_t heads  = q3m_le32(buf + 28);
    uint32_t hdim   = q3m_le32(buf + 32);
    uint32_t ntens  = q3m_le32(buf + 36);
    uint64_t total  = q3m_le64(buf + 40);

    if (vocab == 0 || vocab > Q3M_MAX_VOCAB) return Q3M_E_DIMS;
    if (hidden == 0 || hidden > Q3M_MAX_HIDDEN) return Q3M_E_DIMS;
    if (inter == 0 || inter > Q3M_MAX_INTER) return Q3M_E_DIMS;
    if (layers == 0 || layers > Q3M_MAX_LAYERS) return Q3M_E_DIMS;
    if (heads == 0 || hdim == 0 || hdim > Q3M_MAX_HEAD_DIM) return Q3M_E_DIMS;
    if ((uint64_t)heads * (uint64_t)hdim > (uint64_t)hidden) return Q3M_E_DIMS;
    if (ntens == 0 || ntens > Q3M_MAX_TENSORS) return Q3M_E_FORMAT;
    if (total != obj_size) return Q3M_E_BOUNDS;

    uint64_t dir_bytes = (uint64_t)ntens * Q3M_ENTRY_SIZE;
    if (dir_bytes > total - Q3M_HDR_SIZE) return Q3M_E_BOUNDS;

    out->version = ver;
    out->vocab_size = vocab;
    out->hidden_dim = hidden;
    out->intermediate_dim = inter;
    out->num_layers = layers;
    out->num_heads = heads;
    out->head_dim = hdim;
    out->num_tensors = ntens;
    out->total_bytes = total;
    return Q3M_OK;
}

/* Read and validate tensor descriptor at `index` from the directory. */
static inline int q3m_read_tensor_desc(const struct q3m_header *h,
                                       uint32_t index,
                                       q3m_read_fn rd,
                                       void *ctx,
                                       struct q3m_tensor_desc *out)
{
    if (!h || !rd || !out) return Q3M_E_ARG;
    if (index >= h->num_tensors) return Q3M_E_NOENT;

    uint8_t raw[Q3M_ENTRY_SIZE];
    uint64_t off = (uint64_t)Q3M_HDR_SIZE + (uint64_t)index * Q3M_ENTRY_SIZE;
    int rc = q3m_read_exact(rd, ctx, off, raw, Q3M_ENTRY_SIZE);
    if (rc != Q3M_OK) return rc;

    uint16_t layer = q3m_le16(raw + 0);
    uint16_t role  = q3m_le16(raw + 2);
    uint32_t rows  = q3m_le32(raw + 4);
    uint32_t cols  = q3m_le32(raw + 8);
    uint32_t shift = q3m_le32(raw + 12);
    uint64_t toff  = q3m_le64(raw + 16);
    uint64_t tbytes = q3m_le64(raw + 24);

    if (layer >= h->num_layers) return Q3M_E_FORMAT;
    if (role < Q3M_T_EMBED || role > Q3M_T_LM_HEAD) return Q3M_E_FORMAT;
    if (rows == 0 || cols == 0 || shift > 31u) return Q3M_E_DIMS;

    uint64_t expected_elems = (uint64_t)rows * (uint64_t)cols;
    if (tbytes != expected_elems) return Q3M_E_BOUNDS;

    uint64_t data_start = (uint64_t)Q3M_HDR_SIZE + (uint64_t)h->num_tensors * Q3M_ENTRY_SIZE;
    if (toff < data_start || toff > h->total_bytes || tbytes > h->total_bytes - toff) return Q3M_E_BOUNDS;

    out->layer = layer;
    out->role = role;
    out->rows = rows;
    out->cols = cols;
    out->shift = shift;
    out->offset = toff;
    out->bytes = tbytes;
    return Q3M_OK;
}

/* Stream one validated tensor payload into caller's bounded `buf` (cap bytes). */
static inline int q3m_stream_tensor(const struct q3m_tensor_desc *d,
                                    q3m_read_fn rd,
                                    void *ctx,
                                    uint8_t *buf,
                                    size_t cap)
{
    if (!d || !rd || !buf) return Q3M_E_ARG;
    if (d->bytes == 0 || d->bytes > (uint64_t)cap) return Q3M_E_BOUNDS;
    return q3m_read_exact(rd, ctx, d->offset, buf, d->bytes);
}

#endif /* AURORA_QWEN3_MODEL_H */
