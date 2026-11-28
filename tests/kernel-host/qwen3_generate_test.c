/* tests/kernel-host/qwen3_generate_test.c - host tests for kernel/include/qwen3_generate.h */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <qwen3_generate.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

struct mem_blob {
    uint8_t data[4096];
    size_t used;
};

static int64_t blob_rd(void *ctx, uint64_t off, uint8_t *buf, uint64_t len)
{
    struct mem_blob *b = (struct mem_blob *)ctx;
    if (!b || len == 0 || len > Q3M_CHUNK || off > b->used || len > b->used - off) return -1;
    memcpy(buf, b->data + off, (size_t)len);
    return (int64_t)len;
}

static struct q3m_tensor_desc add_tensor(struct mem_blob *b, uint16_t layer, uint16_t role,
                                         uint32_t rows, uint32_t cols, uint32_t shift, int8_t fill)
{
    struct q3m_tensor_desc d;
    memset(&d, 0, sizeof(d));
    d.layer = layer; d.role = role; d.rows = rows; d.cols = cols; d.shift = shift;
    d.offset = b->used;
    d.bytes = (uint64_t)rows * cols;
    memset(b->data + b->used, (uint8_t)fill, (size_t)d.bytes);
    b->used += (size_t)d.bytes;
    return d;
}

static int filter_allow_all(void *ctx, uint32_t tok)
{
    (void)ctx;
    (void)tok;
    return 1;
}

static int filter_mask_tok1(void *ctx, uint32_t tok)
{
    (void)ctx;
    return tok != 1u;
}

int main(void)
{
    static struct mem_blob blob;
    static struct q3g_work work;
    memset(&blob, 0, sizeof(blob));

    static const int16_t gain4[4] = { 256, 256, 256, 256 };
    static const int16_t head_gain[2] = { 256, 256 };
    static int16_t cosine[Q3L_TOKENS], sine[Q3L_TOKENS];
    for (uint32_t i = 0; i < Q3L_TOKENS; ++i) {
        cosine[i] = 32767;
        sine[i] = 0;
    }

    struct q3g_model m;
    memset(&m, 0, sizeof(m));
    m.hidden = 4;
    m.inter = 4;
    m.num_layers = 2;
    m.attn.tokens = 1;
    m.attn.query_heads = 2;
    m.attn.kv_heads = 1;
    m.attn.width = 2;
    m.final_norm_gain = gain4;

    /* 4 vocab tokens x 4 hidden */
    m.embed = add_tensor(&blob, 0, Q3M_T_EMBED, 4, 4, 0, 1);
    /* Make token 0 embedding [2, 2, 2, 2], token 1 [4, 4, 4, 4], token 2 [1, 1, 1, 1], token 3 (EOS) [8, 8, 8, 8] */
    memset(blob.data + m.embed.offset + 0, 2, 4);
    memset(blob.data + m.embed.offset + 4, 4, 4);
    memset(blob.data + m.embed.offset + 8, 1, 4);
    memset(blob.data + m.embed.offset + 12, 8, 4);

    /* LM head: 4 x 4 where token 1 has highest positive weights, token 3 has negative weights */
    m.lm_head = add_tensor(&blob, 0, Q3M_T_LM_HEAD, 4, 4, 0, 0);
    memset(blob.data + m.lm_head.offset + 0, 1, 4);
    memset(blob.data + m.lm_head.offset + 4, 5, 4);
    memset(blob.data + m.lm_head.offset + 8, 2, 4);
    memset(blob.data + m.lm_head.offset + 12, (uint8_t)-3, 4);

    for (uint32_t l = 0; l < 2; ++l) {
        m.layers[l].q    = add_tensor(&blob, (uint16_t)l, Q3M_T_WQ, 4, 4, 4, 2);
        m.layers[l].k    = add_tensor(&blob, (uint16_t)l, Q3M_T_WK, 2, 4, 4, 2);
        m.layers[l].v    = add_tensor(&blob, (uint16_t)l, Q3M_T_WV, 2, 4, 4, 2);
        m.layers[l].o    = add_tensor(&blob, (uint16_t)l, Q3M_T_WO, 4, 4, 4, 2);
        m.layers[l].gate = add_tensor(&blob, (uint16_t)l, Q3M_T_WGATE, 4, 4, 4, 2);
        m.layers[l].up   = add_tensor(&blob, (uint16_t)l, Q3M_T_WUP, 4, 4, 4, 2);
        m.layers[l].down = add_tensor(&blob, (uint16_t)l, Q3M_T_WDOWN, 4, 4, 4, 2);
        m.layers[l].input_gain = gain4;
        m.layers[l].post_gain = gain4;
        m.layers[l].q_gain = head_gain;
        m.layers[l].k_gain = head_gain;
    }

    uint32_t tokens[Q3L_TOKENS] = { 0 };
    uint32_t total = 0;

    /* Bad args / bounds */
    CHECK("null model rejected", q3g_generate(0, blob_rd, &blob, cosine, sine, Q3L_TOKENS, tokens, 1, 4, 2, 3, 0, 0, &work, &total) == Q3M_E_ARG);
    CHECK("zero prompt rejected", q3g_generate(&m, blob_rd, &blob, cosine, sine, Q3L_TOKENS, tokens, 0, 4, 2, 3, 0, 0, &work, &total) == Q3M_E_BOUNDS);
    CHECK("cap > Q3L_TOKENS rejected", q3g_generate(&m, blob_rd, &blob, cosine, sine, Q3L_TOKENS, tokens, 1, Q3L_TOKENS + 1, 2, 3, 0, 0, &work, &total) == Q3M_E_BOUNDS);

    /* Generate 2 tokens with 2-layer decoder */
    tokens[0] = 0;
    CHECK("2-layer autoregressive generation succeeds",
          q3g_generate(&m, blob_rd, &blob, cosine, sine, Q3L_TOKENS, tokens, 1, 6, 2, 3, filter_allow_all, 0, &work, &total) == Q3M_OK);
    CHECK("generated 2 new tokens -> total 3", total == 3);
    CHECK("selected token 1 (highest positive LM head row)", tokens[1] == 1 && tokens[2] == 1);

    /* Filter masks token 1 -> picks token 2 */
    tokens[0] = 0;
    CHECK("filtered generation succeeds",
          q3g_generate(&m, blob_rd, &blob, cosine, sine, Q3L_TOKENS, tokens, 1, 6, 2, 3, filter_mask_tok1, 0, &work, &total) == Q3M_OK);
    CHECK("filtered generation selects token 2", tokens[1] == 2 && tokens[2] == 2);

    /* EOS early stop: make token 2 be EOS */
    tokens[0] = 0;
    CHECK("EOS stops after 1 new token",
          q3g_generate(&m, blob_rd, &blob, cosine, sine, Q3L_TOKENS, tokens, 1, 6, 4, 2, filter_mask_tok1, 0, &work, &total) == Q3M_OK &&
          total == 2 && tokens[1] == 2);

    printf("qwen3_generate: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
