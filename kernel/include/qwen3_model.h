/*
 * kernel/include/qwen3_model.h - Qwen3 quantized model metadata & weight-streaming loader (AI-06).
 *
 * Pure C, freestanding, no allocation, no libc, no floating point, no globals.
 * Parses an explicit little-endian V1 or V2 model header + tensor directory via a
 * SYS_MODEL-compatible read callback (chunks <= Q3M_CHUNK = 4096 bytes) and streams
 * individual tensor payloads into caller-supplied bounded buffers.
 */
#ifndef AURORA_QWEN3_MODEL_H
#define AURORA_QWEN3_MODEL_H

#include <stddef.h>
#include <stdint.h>

#define Q3M_HDR_SIZE       48u
#define Q3M_V2_HDR_SIZE    56u
#define Q3M_V2_LAYER_ROLES 11u
#define Q3M_V2_MAX_QUERY_WIDTH 4096u
#define Q3M_GLOBAL_LAYER   UINT16_MAX
#define Q3M_TIED_EMBED     1u
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
    Q3M_T_LM_HEAD = 10,
    Q3M_T_INPUT_NORM = Q3M_T_RMSNORM,
    Q3M_T_POST_ATTN_NORM = 11,
    Q3M_T_Q_NORM = 12,
    Q3M_T_K_NORM = 13,
    Q3M_T_FINAL_NORM = 14
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
    uint32_t num_kv_heads;
    uint32_t flags;
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

static inline uint32_t q3m_header_size(uint32_t version)
{
    return version == 2u ? Q3M_V2_HDR_SIZE : Q3M_HDR_SIZE;
}

/* V2 appends the KV head count and flags to the common 48-byte prefix. */
static inline int q3m_parse_header(const uint8_t *buf, size_t len, uint64_t obj_size, struct q3m_header *out)
{
    if (!buf || !out) return Q3M_E_ARG;
    if (len < Q3M_HDR_SIZE || obj_size < Q3M_HDR_SIZE) return Q3M_E_TRUNC;
    if (buf[0] != 'A' || buf[1] != 'U' || buf[2] != 'R' || buf[3] != 'Q' ||
        buf[4] != '3' || buf[5] != 'V' || (buf[6] != '1' && buf[6] != '2') || buf[7] != 0) return Q3M_E_MAGIC;

    uint32_t ver = q3m_le32(buf + 8);
    if (ver != (uint32_t)(buf[6] - '0')) return Q3M_E_VERSION;
    uint32_t header_size = q3m_header_size(ver);
    if (len < header_size || obj_size < header_size) return Q3M_E_TRUNC;

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
    uint64_t query_width = (uint64_t)heads * hdim;
    uint32_t kv_heads = heads, flags = 0;
    if (ver == 1u) {
        if (query_width > hidden) return Q3M_E_DIMS;
        if (!ntens || ntens > Q3M_MAX_TENSORS) return Q3M_E_FORMAT;
    } else {
        kv_heads = q3m_le32(buf + 48);
        flags = q3m_le32(buf + 52);
        if (!kv_heads || heads % kv_heads || hdim % 2u ||
            query_width > Q3M_V2_MAX_QUERY_WIDTH) return Q3M_E_DIMS;
        if (flags & ~Q3M_TIED_EMBED) return Q3M_E_FORMAT;
        uint32_t globals = (flags & Q3M_TIED_EMBED) ? 2u : 3u;
        if (ntens != layers * Q3M_V2_LAYER_ROLES + globals) return Q3M_E_FORMAT;
    }
    if (total != obj_size) return Q3M_E_BOUNDS;

    uint64_t dir_bytes = (uint64_t)ntens * Q3M_ENTRY_SIZE;
    if (dir_bytes > total - header_size) return Q3M_E_BOUNDS;

    out->version = ver;
    out->vocab_size = vocab;
    out->hidden_dim = hidden;
    out->intermediate_dim = inter;
    out->num_layers = layers;
    out->num_heads = heads;
    out->head_dim = hdim;
    out->num_tensors = ntens;
    out->total_bytes = total;
    out->num_kv_heads = kv_heads;
    out->flags = flags;
    return Q3M_OK;
}

/* V2 directory order defines each role exactly once. Vectors are 1 x width. */
static inline int q3m_v2_check_shape(const struct q3m_header *h, uint32_t index,
                                     uint16_t layer, uint16_t role, uint32_t rows, uint32_t cols)
{
    static const uint16_t roles[Q3M_V2_LAYER_ROLES] = {
        Q3M_T_INPUT_NORM, Q3M_T_WQ, Q3M_T_WK, Q3M_T_WV, Q3M_T_Q_NORM,
        Q3M_T_K_NORM, Q3M_T_WO, Q3M_T_POST_ATTN_NORM, Q3M_T_WGATE, Q3M_T_WUP, Q3M_T_WDOWN
    };
    uint32_t globals = (h->flags & Q3M_TIED_EMBED) ? 2u : 3u;
    uint16_t expected_role, expected_layer = Q3M_GLOBAL_LAYER;
    if (index < globals) {
        expected_role = index == 0 ? Q3M_T_EMBED : index == 1 ? Q3M_T_FINAL_NORM : Q3M_T_LM_HEAD;
    } else {
        uint32_t slot = index - globals;
        expected_role = roles[slot % Q3M_V2_LAYER_ROLES];
        expected_layer = (uint16_t)(slot / Q3M_V2_LAYER_ROLES);
    }
    if (role != expected_role || layer != expected_layer) return Q3M_E_FORMAT;
    uint32_t r = 1u, c = h->hidden_dim;
    switch (role) {
    case Q3M_T_EMBED: case Q3M_T_LM_HEAD: r = h->vocab_size; break;
    case Q3M_T_WQ: r = h->num_heads * h->head_dim; break;
    case Q3M_T_WK: case Q3M_T_WV: r = h->num_kv_heads * h->head_dim; break;
    case Q3M_T_WO: r = h->hidden_dim; c = h->num_heads * h->head_dim; break;
    case Q3M_T_Q_NORM: case Q3M_T_K_NORM: c = h->head_dim; break;
    case Q3M_T_WGATE: case Q3M_T_WUP: r = h->intermediate_dim; break;
    case Q3M_T_WDOWN: r = h->hidden_dim; c = h->intermediate_dim; break;
    }
    return rows == r && cols == c ? Q3M_OK : Q3M_E_DIMS;
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
    uint64_t off = (uint64_t)q3m_header_size(h->version) + (uint64_t)index * Q3M_ENTRY_SIZE;
    int rc = q3m_read_exact(rd, ctx, off, raw, Q3M_ENTRY_SIZE);
    if (rc != Q3M_OK) return rc;

    uint16_t layer = q3m_le16(raw + 0);
    uint16_t role  = q3m_le16(raw + 2);
    uint32_t rows  = q3m_le32(raw + 4);
    uint32_t cols  = q3m_le32(raw + 8);
    uint32_t shift = q3m_le32(raw + 12);
    uint64_t toff  = q3m_le64(raw + 16);
    uint64_t tbytes = q3m_le64(raw + 24);

    if (h->version == 2u) {
        rc = q3m_v2_check_shape(h, index, layer, role, rows, cols);
        if (rc != Q3M_OK) return rc;
    } else if (layer >= h->num_layers || role < Q3M_T_EMBED || role > Q3M_T_LM_HEAD) {
        return Q3M_E_FORMAT;
    }
    if (rows == 0 || cols == 0 || shift > 31u) return Q3M_E_DIMS;

    uint64_t expected_elems = (uint64_t)rows * (uint64_t)cols;
    if (tbytes != expected_elems) return Q3M_E_BOUNDS;

    uint64_t data_start = (uint64_t)q3m_header_size(h->version) + (uint64_t)h->num_tensors * Q3M_ENTRY_SIZE;
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

/* Preflight every V2 descriptor and require packed payloads in directory order. */
static inline int q3m_validate_v2_directory(const struct q3m_header *h, q3m_read_fn rd, void *ctx)
{
    if (!h || !rd) return Q3M_E_ARG;
    if (h->version != 2u) return Q3M_E_VERSION;
    uint64_t cursor = Q3M_V2_HDR_SIZE + (uint64_t)h->num_tensors * Q3M_ENTRY_SIZE;
    for (uint32_t i = 0; i < h->num_tensors; ++i) {
        struct q3m_tensor_desc d;
        int rc = q3m_read_tensor_desc(h, i, rd, ctx, &d);
        if (rc != Q3M_OK) return rc;
        if (d.offset != cursor) return Q3M_E_BOUNDS;
        cursor += d.bytes;
    }
    return cursor == h->total_bytes ? Q3M_OK : Q3M_E_BOUNDS;
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
