/*
 * kernel/include/qwen3_full.h - Full-scale streamed Qwen3-0.6B inference runtime.
 *
 * Pure C11, no heap allocation, caller-supplied bounded arena (`struct q3f_arena`).
 * Streams every int8 weight tensor row-by-row from an AURQ3V2 image via `q3m_read_fn`
 * in <= Q3M_CHUNK (4096-byte) reads, supporting:
 *   - Qwen3-0.6B geometry: hidden=1024, inter=3072, layers=28, heads=16, kv_heads=8, head_dim=128, vocab=151936
 *   - Tied or untied LM head (Q3M_TIED_EMBED)
 *   - Per-tensor signed power-of-2 scale `shift` in [-15, 31] (`q * 2^(-shift)`)
 *   - RMSNorm (input_layernorm, post_attention_layernorm, q_norm, k_norm, final_norm)
 *   - RoPE (theta = 1,000,000) + GQA (num_heads / num_kv_heads) + KV-cache
 *   - SwiGLU MLP (silu(gate) * up -> down)
 *   - Streamed greedy argmax over full vocabulary
 */
#ifndef AURORA_QWEN3_FULL_H
#define AURORA_QWEN3_FULL_H

#include <qwen3_model.h>
#ifndef Q3F_SINGLE_POSITION
#include <math.h>
#endif
#include <stddef.h>
#include <stdint.h>

#ifdef Q3F_SINGLE_POSITION
#define Q3F_MAX_CTX    1u
#define Q3F_HIDDEN     1024u
#define Q3F_INTER      3072u
#else
#define Q3F_MAX_CTX    32u
#define Q3F_HIDDEN     Q3M_MAX_HIDDEN
#define Q3F_INTER      Q3M_MAX_INTER
#endif
#define Q3F_MAX_Q_DIM  2048u  /* 16 * 128 */
#define Q3F_MAX_KV_DIM 1024u  /* 8 * 128 */

struct q3f_layer_descs {
    struct q3m_tensor_desc in_norm;
    struct q3m_tensor_desc q_proj;
    struct q3m_tensor_desc k_proj;
    struct q3m_tensor_desc v_proj;
    struct q3m_tensor_desc q_norm;
    struct q3m_tensor_desc k_norm;
    struct q3m_tensor_desc o_proj;
    struct q3m_tensor_desc post_norm;
    struct q3m_tensor_desc gate_proj;
    struct q3m_tensor_desc up_proj;
    struct q3m_tensor_desc down_proj;
};

struct q3f_arena {
    struct q3m_header hdr;
    struct q3m_tensor_desc embed;
    struct q3m_tensor_desc final_norm;
    struct q3m_tensor_desc lm_head;
    struct q3f_layer_descs layers[Q3M_MAX_LAYERS];

    float x[Q3F_HIDDEN];
    float xb[Q3M_MAX_HIDDEN];
    float xb2[Q3F_HIDDEN];
#ifndef Q3F_SINGLE_POSITION
    float q[Q3F_MAX_Q_DIM];
    float k[Q3F_MAX_KV_DIM];
#endif
    float v[Q3F_MAX_KV_DIM];
#ifndef Q3F_SINGLE_POSITION
    float att[Q3F_MAX_CTX];
#endif
    float hb[Q3F_INTER];
    float hb2[Q3F_INTER];
    float norm_w[Q3F_HIDDEN];
#ifndef Q3F_SINGLE_POSITION
    float q_norm_w[Q3M_MAX_HEAD_DIM];
    float k_norm_w[Q3M_MAX_HEAD_DIM];

    /* KV cache: [layer][pos][kv_dim] */
    float k_cache[Q3M_MAX_LAYERS][Q3F_MAX_CTX][Q3F_MAX_KV_DIM];
    float v_cache[Q3M_MAX_LAYERS][Q3F_MAX_CTX][Q3F_MAX_KV_DIM];

#endif

    float last_logit;
    uint8_t read_buf[Q3M_CHUNK];
};

static inline float q3f_scale_for_shift(uint32_t raw_shift)
{
    int32_t s = (int32_t)raw_shift;
#ifdef Q3F_SINGLE_POSITION
    return s < 0 ? (float)(UINT32_C(1) << (unsigned)-s) :
                   1.0f / (float)(UINT32_C(1) << (unsigned)s);
#else
    return ldexpf(1.0f, -s);
#endif
}

static inline int q3f_load_index(struct q3f_arena *a, q3m_read_fn rd, void *ctx, uint64_t total_bytes)
{
    if (!a || !rd) return Q3M_E_ARG;
    uint8_t hbuf[Q3M_V2_HDR_SIZE];
    int rc = q3m_read_exact(rd, ctx, 0, hbuf, Q3M_V2_HDR_SIZE);
    if (rc != Q3M_OK) return rc;
    rc = q3m_parse_header(hbuf, Q3M_V2_HDR_SIZE, total_bytes, &a->hdr);
    if (rc != Q3M_OK) return rc;
    if (a->hdr.version != 2u) return Q3M_E_VERSION;
    if (a->hdr.hidden_dim > Q3F_HIDDEN || a->hdr.intermediate_dim > Q3F_INTER)
        return Q3M_E_DIMS;
    if (a->hdr.num_heads * a->hdr.head_dim > Q3F_MAX_Q_DIM ||
        a->hdr.num_kv_heads * a->hdr.head_dim > Q3F_MAX_KV_DIM ||
        a->hdr.num_kv_heads == 0 ||
        (a->hdr.num_heads % a->hdr.num_kv_heads) != 0)
        return Q3M_E_DIMS;

    rc = q3m_validate_v2_directory(&a->hdr, rd, ctx);
    if (rc != Q3M_OK) return rc;

    uint8_t *lp = (uint8_t *)a->layers;
    for (size_t i = 0; i < sizeof(a->layers); ++i) lp[i] = 0;

    int have_embed = 0, have_fnorm = 0, have_lm = 0;
    for (uint32_t i = 0; i < a->hdr.num_tensors; ++i) {
        struct q3m_tensor_desc d;
        rc = q3m_read_tensor_desc(&a->hdr, i, rd, ctx, &d);
        if (rc != Q3M_OK) return rc;
        if (d.layer == Q3M_GLOBAL_LAYER) {
            if (d.role == Q3M_T_EMBED) { a->embed = d; have_embed = 1; }
            else if (d.role == Q3M_T_FINAL_NORM) { a->final_norm = d; have_fnorm = 1; }
            else if (d.role == Q3M_T_LM_HEAD) { a->lm_head = d; have_lm = 1; }
        } else if (d.layer < a->hdr.num_layers) {
            struct q3f_layer_descs *L = &a->layers[d.layer];
            switch (d.role) {
            case Q3M_T_INPUT_NORM:     L->in_norm = d; break;
            case Q3M_T_WQ:             L->q_proj = d; break;
            case Q3M_T_WK:             L->k_proj = d; break;
            case Q3M_T_WV:             L->v_proj = d; break;
            case Q3M_T_Q_NORM:         L->q_norm = d; break;
            case Q3M_T_K_NORM:         L->k_norm = d; break;
            case Q3M_T_WO:             L->o_proj = d; break;
            case Q3M_T_POST_ATTN_NORM: L->post_norm = d; break;
            case Q3M_T_WGATE:          L->gate_proj = d; break;
            case Q3M_T_WUP:            L->up_proj = d; break;
            case Q3M_T_WDOWN:          L->down_proj = d; break;
            default: return Q3M_E_FORMAT;
            }
        }
    }
    if (!have_embed || !have_fnorm) return Q3M_E_FORMAT;
    if (a->hdr.flags & Q3M_TIED_EMBED) {
        a->lm_head = a->embed;
        a->lm_head.role = Q3M_T_LM_HEAD;
    } else if (!have_lm) {
        return Q3M_E_FORMAT;
    }
    return Q3M_OK;
}

static inline int q3f_read_vec(const struct q3m_tensor_desc *d, q3m_read_fn rd, void *ctx,
                               uint8_t *scratch, float *out, uint32_t len)
{
    if (d->rows != 1u || d->cols != len || len > Q3M_CHUNK) return Q3M_E_DIMS;
    int rc = q3m_read_exact(rd, ctx, d->offset, scratch, len);
    if (rc != Q3M_OK) return rc;
    float s = q3f_scale_for_shift(d->shift);
    for (uint32_t i = 0; i < len; ++i)
        out[i] = (float)(int8_t)scratch[i] * s;
    return Q3M_OK;
}

static inline void q3f_rmsnorm(float *out, const float *x, const float *w, uint32_t n)
{
    float ss = 0.0f;
    for (uint32_t i = 0; i < n; ++i) ss += x[i] * x[i];
    ss = 1.0f / __builtin_sqrtf(ss / (float)n + 1e-6f);
    for (uint32_t i = 0; i < n; ++i) out[i] = x[i] * ss * w[i];
}

static inline int q3f_matvec(const struct q3m_tensor_desc *d, q3m_read_fn rd, void *ctx,
                             uint8_t *scratch, const float *x, float *out)
{
    uint32_t rows = d->rows, cols = d->cols;
    if (!rows || !cols || cols > Q3M_CHUNK) return Q3M_E_DIMS;
    float s = q3f_scale_for_shift(d->shift);
    uint32_t batch = Q3M_CHUNK / cols;
    if (batch == 0) batch = 1;
    for (uint32_t r = 0; r < rows;) {
        uint32_t cnt = rows - r;
        if (cnt > batch) cnt = batch;
        int rc = q3m_read_exact(rd, ctx, d->offset + (uint64_t)r * cols, scratch, (uint64_t)cnt * cols);
        if (rc != Q3M_OK) return rc;
        for (uint32_t b = 0; b < cnt; ++b) {
            const int8_t *wrow = (const int8_t *)(scratch + (size_t)b * cols);
            float sum = 0.0f;
            for (uint32_t c = 0; c < cols; ++c)
                sum += (float)wrow[c] * x[c];
            out[r + b] = sum * s;
        }
        r += cnt;
    }
    return Q3M_OK;
}

#ifndef Q3F_SINGLE_POSITION
static inline void q3f_rope_head(float *vec, uint32_t head_dim, uint32_t pos, float theta)
{
    uint32_t half = head_dim / 2u;
    for (uint32_t i = 0; i < half; ++i) {
        float freq = 1.0f / powf(theta, (float)(2u * i) / (float)head_dim);
        float angle = (float)pos * freq;
        float c = cosf(angle), s = sinf(angle);
        float v0 = vec[i], v1 = vec[i + half];
        vec[i]        = v0 * c - v1 * s;
        vec[i + half] = v0 * s + v1 * c;
    }
}

#endif

static inline float q3f_silu(float x)
{
#ifndef Q3F_SINGLE_POSITION
    return x / (1.0f + expf(-x));
#else
    if (x != x || x >= 20.0f) return x;
    if (x <= -20.0f) return 0.0f;
    float abs_x = x < 0.0f ? -x : x;
    const float ln2 = 0.6931471805599453f;
    unsigned power = (unsigned)(abs_x / ln2);
    float remainder = abs_x - (float)power * ln2;
    float exp_r = 1.0f, term = 1.0f;
    for (unsigned n = 1; n <= 8; ++n) {
        term *= remainder / (float)n;
        exp_r += term;
    }
    float e = exp_r * (float)(UINT32_C(1) << power);
    return x < 0.0f ? x / (1.0f + e) : (x * e) / (1.0f + e);
#endif
}

static inline int q3f_forward_token(struct q3f_arena *a, q3m_read_fn rd, void *ctx,
                                    uint32_t token, uint32_t pos, uint32_t *out_next_token)
{
    if (!a || !rd || pos >= Q3F_MAX_CTX) return Q3M_E_BOUNDS;
    uint32_t H = a->hdr.hidden_dim;
    uint32_t I = a->hdr.intermediate_dim;
    uint32_t NH = a->hdr.num_heads;
    uint32_t NKV = a->hdr.num_kv_heads;
    uint32_t HD = a->hdr.head_dim;
    uint32_t KV_MUL = NH / NKV;

    /* 1. Embedding lookup */
    if (token >= a->embed.rows) return Q3M_E_NOENT;
    int rc = q3m_read_exact(rd, ctx, a->embed.offset + (uint64_t)token * H, a->read_buf, H);
    if (rc != Q3M_OK) return rc;
    float es = q3f_scale_for_shift(a->embed.shift);
    for (uint32_t i = 0; i < H; ++i) a->x[i] = (float)(int8_t)a->read_buf[i] * es;

    /* 2. Transformer layers */
    for (uint32_t l = 0; l < a->hdr.num_layers; ++l) {
        const struct q3f_layer_descs *L = &a->layers[l];
        if ((rc = q3f_read_vec(&L->in_norm, rd, ctx, a->read_buf, a->norm_w, H)) != Q3M_OK) return rc;
        q3f_rmsnorm(a->xb, a->x, a->norm_w, H);

#ifdef Q3F_SINGLE_POSITION
        /* At position zero each causal head has exactly one value.
         * Its softmax weight is one, independent of Q/K or their rotation. */
        if ((rc = q3f_matvec(&L->v_proj, rd, ctx, a->read_buf, a->xb, a->v)) != Q3M_OK) return rc;
        for (uint32_t head = 0; head < NH; ++head)
            for (uint32_t d = 0; d < HD; ++d)
                a->xb[head * HD + d] = a->v[(head / KV_MUL) * HD + d];
#else
        if ((rc = q3f_matvec(&L->q_proj, rd, ctx, a->read_buf, a->xb, a->q)) != Q3M_OK) return rc;
        if ((rc = q3f_matvec(&L->k_proj, rd, ctx, a->read_buf, a->xb, a->k)) != Q3M_OK) return rc;
        if ((rc = q3f_matvec(&L->v_proj, rd, ctx, a->read_buf, a->xb, a->v)) != Q3M_OK) return rc;

        if ((rc = q3f_read_vec(&L->q_norm, rd, ctx, a->read_buf, a->q_norm_w, HD)) != Q3M_OK) return rc;
        if ((rc = q3f_read_vec(&L->k_norm, rd, ctx, a->read_buf, a->k_norm_w, HD)) != Q3M_OK) return rc;

        for (uint32_t h = 0; h < NH; ++h) {
            q3f_rmsnorm(a->q + h * HD, a->q + h * HD, a->q_norm_w, HD);
            q3f_rope_head(a->q + h * HD, HD, pos, 1000000.0f);
        }
        for (uint32_t h = 0; h < NKV; ++h) {
            q3f_rmsnorm(a->k + h * HD, a->k + h * HD, a->k_norm_w, HD);
            q3f_rope_head(a->k + h * HD, HD, pos, 1000000.0f);
        }

        uint32_t kv_dim = NKV * HD;
        for (uint32_t i = 0; i < kv_dim; ++i) {
            a->k_cache[l][pos][i] = a->k[i];
            a->v_cache[l][pos][i] = a->v[i];
        }

        float inv_sqrt_hd = 1.0f / sqrtf((float)HD);
        for (uint32_t h = 0; h < NH; ++h) {
            const float *qh = a->q + h * HD;
            uint32_t kvh = h / KV_MUL;
            float max_att = -1e30f;
            for (uint32_t t = 0; t <= pos; ++t) {
                const float *kh = a->k_cache[l][t] + kvh * HD;
                float score = 0.0f;
                for (uint32_t d = 0; d < HD; ++d) score += qh[d] * kh[d];
                score *= inv_sqrt_hd;
                a->att[t] = score;
                if (score > max_att) max_att = score;
            }
            float sum_exp = 0.0f;
            for (uint32_t t = 0; t <= pos; ++t) {
                a->att[t] = expf(a->att[t] - max_att);
                sum_exp += a->att[t];
            }
            float inv_sum = 1.0f / sum_exp;
            for (uint32_t t = 0; t <= pos; ++t) a->att[t] *= inv_sum;

            float *out_h = a->xb + h * HD;
            for (uint32_t d = 0; d < HD; ++d) out_h[d] = 0.0f;
            for (uint32_t t = 0; t <= pos; ++t) {
                const float *vh = a->v_cache[l][t] + kvh * HD;
                float p = a->att[t];
                for (uint32_t d = 0; d < HD; ++d) out_h[d] += p * vh[d];
            }
        }

#endif

        if ((rc = q3f_matvec(&L->o_proj, rd, ctx, a->read_buf, a->xb, a->xb2)) != Q3M_OK) return rc;
        for (uint32_t i = 0; i < H; ++i) a->x[i] += a->xb2[i];

        if ((rc = q3f_read_vec(&L->post_norm, rd, ctx, a->read_buf, a->norm_w, H)) != Q3M_OK) return rc;
        q3f_rmsnorm(a->xb, a->x, a->norm_w, H);

        if ((rc = q3f_matvec(&L->gate_proj, rd, ctx, a->read_buf, a->xb, a->hb)) != Q3M_OK) return rc;
        if ((rc = q3f_matvec(&L->up_proj, rd, ctx, a->read_buf, a->xb, a->hb2)) != Q3M_OK) return rc;
        for (uint32_t i = 0; i < I; ++i) {
            float g = a->hb[i];
            a->hb[i] = q3f_silu(g) * a->hb2[i];
        }
        if ((rc = q3f_matvec(&L->down_proj, rd, ctx, a->read_buf, a->hb, a->xb)) != Q3M_OK) return rc;
        for (uint32_t i = 0; i < H; ++i) a->x[i] += a->xb[i];
    }

    if (!out_next_token) return Q3M_OK;

    /* 3. Final RMSNorm + streamed greedy argmax over LM head */
    if ((rc = q3f_read_vec(&a->final_norm, rd, ctx, a->read_buf, a->norm_w, H)) != Q3M_OK) return rc;
    q3f_rmsnorm(a->x, a->x, a->norm_w, H);

    uint32_t V = a->lm_head.rows;
    uint32_t batch = Q3M_CHUNK / H;
    if (batch == 0) batch = 1;
    uint32_t best_id = 0;
    float best_logit = -1e30f;
    for (uint32_t r = 0; r < V;) {
        uint32_t cnt = V - r;
        if (cnt > batch) cnt = batch;
        rc = q3m_read_exact(rd, ctx, a->lm_head.offset + (uint64_t)r * H, a->read_buf, (uint64_t)cnt * H);
        if (rc != Q3M_OK) return rc;
        for (uint32_t b = 0; b < cnt; ++b) {
            const int8_t *wrow = (const int8_t *)(a->read_buf + (size_t)b * H);
            float sum = 0.0f;
            for (uint32_t c = 0; c < H; ++c) sum += (float)wrow[c] * a->x[c];
            if (!__builtin_isfinite(sum)) return Q3M_E_FORMAT;
            if (sum > best_logit) {
                best_logit = sum;
                best_id = r + b;
            }
        }
        r += cnt;
    }
    a->last_logit = best_logit * q3f_scale_for_shift(a->lm_head.shift);
    *out_next_token = best_id;
    return Q3M_OK;
}

#endif /* AURORA_QWEN3_FULL_H */
