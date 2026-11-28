/*
 * tests/programs/inferclient.c - EL0 int8 matrix-vector program for tests/qemu/infer-tests.sh, launched by the AURORA_INFER_TEST kernel hook
 * (kernel/infertest.c), which chooses the identity (PRINCIPAL_INFER, or an ordinary application identity) and passes the mode in x0:
 *   mode k (1..16)  read model-volume object k-1 through SYS_MODEL (11), validate it as an AURQMV1 tensor, compute the quantized matvec
 *                   (tests/programs/matvec_core.h, the code the host test exercises) streaming the payload in reads of at most 4096 bytes,
 *                   and print every row as "ic: y <row> <acc> <q>" ONLY after the whole object validated.
 * Mode 1 also runs a synthetic dense/ReLU/dense graph on the first three output values.
 * Exit codes: 0 computed and printed; 3 the object was rejected ("ic: reject <reason>"); 4 the kernel refused SYS_MODEL ("ic: refused <code>":
 * -7 EPERM without PRINCIPAL_INFER, -4 ENOSYS without a valid volume); 5 no such object. Every path exits; nothing here faults on hostile data.
 * Memory: static buffers only (one 4096-byte read buffer + the run state, about 13 KiB); no SYS_MEM heap, no floating point.
 * The result is untrusted data: it is printed for comparison and nothing acts on it. Output goes through SYS_WRITE ("[user pN] " prefix by the kernel).
 */
#include "common.h"
#include "matvec_core.h"
#include <quant_ops.h>
#include <tensor.h>
#include <tokenizer.h>
#include <infer_runtime.h>
#include <qwen3_ops.h>
#include <qwen3_projection.h>
#include <qwen3_gqa.h>
#include <qwen3_token_select.h>
#include <qwen3_generate.h>
#define SYS_MODEL 11
#define INFO 1
#define READ 2
#define COUNT 0xFFFFFFFFL
typedef unsigned char u8;

_Static_assert(MV_CHUNK <= 4096, "a SYS_MODEL read is at most 4096 bytes");

struct obj { u64 size; u32 kind, reserved; char name[32]; u8 digest[32]; };
static i64 mc(i64 op, i64 a, i64 b, i64 c, i64 d) { return sc(SYS_MODEL, op, a, b, c, d); }

struct q3g_mem_blob { uint8_t data[1024]; size_t used; };
static union {
    struct {
        struct mv_run run;
        u8 buf[MV_CHUNK];
    } mv;
    struct {
        struct q3g_work w;
        struct q3g_model m;
        struct q3g_mem_blob blob;
    } q3g;
} U;
#define R U.mv.run
#define buf U.mv.buf
static struct obj o;

/* one line, one SYS_WRITE */
static char line[160]; static u64 ln;
static void put(const char *s) { while (*s && ln < sizeof line - 1) line[ln++] = *s++; }
static void putd(i64 v)
{
    char d[24]; int k = 0; u64 u;
    if (v < 0) { put("-"); u = (u64)0 - (u64)v; } else u = (u64)v;
    if (!u) d[k++] = '0';
    while (u) { d[k++] = (char)('0' + u % 10); u /= 10; }
    while (k && ln < sizeof line - 1) line[ln++] = d[--k];
}
static void flush(void) { line[ln] = 0; say(line); ln = 0; }

static int64_t rd(void *ctx, uint64_t off, uint8_t *b, uint64_t len) { return mc(READ, (i64)*(u64 *)ctx, (i64)off, (i64)b, (i64)len); }

/* Execute two fixed synthetic dense layers on three verified tensor outputs. */
static int tiny_graph(void)
{
    static const int8_t first[6] = { 1, -2, 3, -3, 2, -1 };
    static const int8_t second[4] = { 2, -3, -1, 2 };
    int8_t input[3], hidden[2], active[2], result[2];

    if (R.h.rows < 3) return -1;
    for (unsigned i = 0; i < 3; i++) input[i] = (int8_t)R.out_q[i];
    if (quant_matvec(first, 2, 3, input, 1, 1, 0, hidden) ||
        quant_relu(hidden, 2, active) ||
        quant_matvec(second, 2, 2, active, 1, 2, 0, result)) return -1;

    for (unsigned i = 0; i < 2; i++) {
        put("ic: graph "); putd(i); put(" "); putd(result[i]); flush();
    }
    return 0;
}

/* Execute the full synthetic AI pipeline:
 * tokenizer -> embedding lookup -> bounded tensor -> quant_matvec -> quant_relu -> argmax. */
static int tiny_pipeline(void)
{
    static const struct tok_entry vocab[] = {
        { "ab", 4 },
        { "cd", 5 },
        { "abcd", 6 },
    };
    static const struct tok_merge merges[] = {
        { TOK_BYTE_ID_BASE + 'a', TOK_BYTE_ID_BASE + 'b', 4, 0 },
        { TOK_BYTE_ID_BASE + 'c', TOK_BYTE_ID_BASE + 'd', 5, 1 },
        { 4, 5, 6, 2 },
    };
    static const int8_t embed[4][3] = {
        {  2, -1,  1 }, /* 0: TOK_ID_BOS */
        {  4,  3, -2 }, /* 1: merged token 6 ("abcd") */
        { -1,  2,  3 }, /* 2: fallback byte '!' */
        {  1,  0, -1 }, /* 3: TOK_ID_EOS */
    };
    static const int8_t w1[9] = {
         1,  2, -1,
        -2,  1,  2,
         2, -1,  1,
    };
    static const int8_t w2[9] = {
         1, -1,  2,
         2,  1, -1,
        -1,  2,  1,
    };

    static struct tokenizer tok;
    static struct tok_tensor emb;
    uint32_t tokens[8];
    int8_t pooled[3] = { 0, 0, 0 };
    int8_t hidden[3], active[3], logits[3];

    tokenizer_init(&tok, vocab, 3, merges, 3);
    int ntok = tokenizer_encode(&tok, "abcd!", 5, tokens, 8, 1, 1);
    if (ntok != 4) return -1;

    tensor_init(&emb);
    for (int i = 0; i < ntok; i++) {
        unsigned row = 2; /* fallback byte */
        if (tokens[i] == TOK_ID_BOS) row = 0;
        else if (tokens[i] == 6) row = 1;
        else if (tokens[i] == TOK_ID_EOS) row = 3;
        for (unsigned j = 0; j < 3; j++) {
            if (tensor_push(&emb, embed[row][j]) != 0) return -1;
        }
    }
    if (tensor_len(&emb) != 12) return -1;

    for (int i = 0; i < ntok; i++) {
        for (unsigned j = 0; j < 3; j++) {
            int8_t v = 0;
            if (tensor_get(&emb, (size_t)(i * 3 + (int)j), &v) != 0) return -1;
            pooled[j] = (int8_t)(pooled[j] + v);
        }
    }

    if (quant_matvec(w1, 3, 3, pooled, 1, 1, 0, hidden) ||
        quant_relu(hidden, 3, active) ||
        quant_matvec(w2, 3, 3, active, 1, 2, 0, logits)) return -1;

    unsigned best = 0;
    for (unsigned i = 1; i < 3; i++) {
        if (logits[i] > logits[best]) best = i;
    }

    put("ic: pipe tokens "); putd(ntok);
    for (int i = 0; i < ntok; i++) { put(" "); putd((i64)tokens[i]); }
    flush();
    put("ic: pipe logits "); putd(logits[0]); put(" "); putd(logits[1]); put(" "); putd(logits[2]);
    put(" argmax "); putd((i64)best); flush();
    return 0;
}

/* Execute the reusable infer_runtime.h pipeline in EL0. */
static int runtime_el0_check(void)
{
    static const struct tok_entry vocab[] = {
        { "ab", TOK_ID_FIRST + 0 },
        { "cd", TOK_ID_FIRST + 1 },
    };
    static const struct tok_merge merges[] = {
        { TOK_BYTE_ID_BASE + 'a', TOK_BYTE_ID_BASE + 'b', TOK_ID_FIRST + 0, 0 },
        { TOK_BYTE_ID_BASE + 'c', TOK_BYTE_ID_BASE + 'd', TOK_ID_FIRST + 1, 1 },
    };
    static const int8_t embed[6 * 4] = {
        0, 0, 0, 0,
        1, 1, 1, 1,
        2, 2, 2, 2,
        3, 3, 3, 3,
        10, 20, -5, 4,
        -4, 8, 12, 6,
    };
    static const int8_t w1[3 * 4] = {
        2, 1, 0, 1,
        -1, -1, 1, 0,
        1, 2, 1, 1
    };
    static const int8_t w2[2 * 3] = {
        1, 0, 2,
        -1, 1, 0
    };
    static struct tokenizer tok;
    static struct ir_result res;
    struct ir_mlp_model model;
    model.vocab_size = 6;
    model.embed_dim = 4;
    model.hidden_dim = 3;
    model.num_classes = 2;
    model.embed_table = embed;
    model.w1 = w1;
    model.w1_mult = 1;
    model.w1_shift = 2;
    model.w1_zp_out = 0;
    model.w2 = w2;
    model.w2_mult = 1;
    model.w2_shift = 1;
    model.w2_zp_out = 0;

    tokenizer_init(&tok, vocab, 2, merges, 2);
    if (ir_run_text(&tok, &model, "abcd", 4, &res) != IR_OK) return -1;

    put("ic: runtime logits "); putd(res.logits[0]); put(" "); putd(res.logits[1]);
    put(" argmax "); putd((i64)res.argmax); flush();
    return 0;
}

/* Execute a quantized Qwen3-style decoder block (RMSNorm -> RoPE -> KV cache + causal attention -> SwiGLU) in EL0. */
static int qwen3_layer_el0(void)
{
    static struct qwen3_kv_cache kv;
    static const int8_t x[4]     = { 10, -10, 10, -10 };
    static const int8_t gamma[4] = { 64, 64, 64, 64 };
    static const int8_t k0[4]    = { 10, 0, 0, 0 };
    static const int8_t v0[4]    = { 100, 0, 0, 0 };
    static const int8_t k1[4]    = { 0, 10, 0, 0 };
    static const int8_t v1[4]    = { 0, 100, 0, 0 };
    static const int8_t up[4]    = { 32, 32, 32, 32 };

    int8_t norm[4], q_rot[4], attn[4], mlp[4];
    if (qwen3_rmsnorm(x, gamma, 4, 0, norm) != Q3_OK) return -1;

    /* RoPE on pair (0,1) with identity cos=32767, sin=0 and pair (2,3) with 90-deg cos=0, sin=32767 */
    qwen3_rope_pair(norm[0], norm[1], 32767, 0, &q_rot[0], &q_rot[1]);
    qwen3_rope_pair(norm[2], norm[3], 0, 32767, &q_rot[2], &q_rot[3]);

    if (qwen3_kv_init(&kv, 4) != Q3_OK) return -1;
    if (qwen3_kv_push(&kv, k0, v0) != Q3_OK) return -1;
    if (qwen3_kv_push(&kv, k1, v1) != Q3_OK) return -1;
    if (qwen3_attn_head(&kv, q_rot, attn) != Q3_OK) return -1;
    if (qwen3_swiglu(attn, up, 4, 5, mlp) != Q3_OK) return -1;

    put("ic: qwen3 norm "); putd(norm[0]); put(" "); putd(norm[1]);
    put(" attn "); putd(attn[0]); put(" "); putd(attn[1]);
    put(" mlp "); putd(mlp[0]); put(" "); putd(mlp[1]); flush();
    return 0;
}

/* Exercise Q/K preparation and a causal query with shared KV heads. */
static int qwen3_gqa_el0(void)
{
    const struct q3_gqa_shape shape = { 4, 2, 2, 3 };
    const int16_t zeros[12] = { 0 };
    const int16_t gains[] = { 4096, 4096 };
    const int16_t cosine = 32767, sine = 0;
    const int16_t values[] = { 4, -8, 12, 28, 8, -4, 20, 36, 120, 120, -120, -120 };
    int16_t query[8], out[8];
    for (size_t head = 0; head < 4; ++head)
        if (q3_qk_prepare(zeros, gains, &cosine, &sine, 2, query + head * 2) != Q3_OK)
            return -1;
    if (q3_gqa_context(&shape, 1, query, 8, zeros, values, 12, out, 8) != Q3_OK)
        return -1;
    const int16_t expected[] = { 6, -6, 6, -6, 16, 32, 16, 32 };
    for (size_t i = 0; i < 8; ++i)
        if (out[i] != expected[i]) return -1;
    say("ic: qwen3_gqa four query heads share two causal KV heads");
    return 0;
}

static int64_t q3g_blob_rd(void *ctx, uint64_t off, uint8_t *dst, uint64_t len)
{
    struct q3g_mem_blob *b = (struct q3g_mem_blob *)ctx;
    if (!b || len == 0 || len > Q3M_CHUNK || off > b->used || len > b->used - off) return -1;
    for (uint64_t i = 0; i < len; ++i) dst[i] = b->data[off + i];
    return (int64_t)len;
}

static struct q3m_tensor_desc q3g_add_tensor(struct q3g_mem_blob *b, uint16_t layer, uint16_t role,
                                             uint32_t rows, uint32_t cols, uint32_t shift, int8_t fill)
{
    struct q3m_tensor_desc d;
    d.layer = layer; d.role = role; d.rows = rows; d.cols = cols; d.shift = shift;
    d.offset = b->used;
    d.bytes = (uint64_t)rows * cols;
    for (uint64_t i = 0; i < d.bytes; ++i) b->data[b->used + i] = (uint8_t)fill;
    b->used += (size_t)d.bytes;
    return d;
}

static int qwen3_generate_el0(void)
{
    static const int16_t gain4[4] = { 256, 256, 256, 256 };
    static const int16_t head_gain[2] = { 256, 256 };
    int16_t cosine[Q3L_TOKENS], sine[Q3L_TOKENS];
    for (uint32_t i = 0; i < Q3L_TOKENS; ++i) { cosine[i] = 32767; sine[i] = 0; }
    U.q3g.blob.used = 0;

    uint8_t *mp = (uint8_t *)&U.q3g.m;
    for (size_t i = 0; i < sizeof(U.q3g.m); ++i) mp[i] = 0;
    U.q3g.m.hidden = 4; U.q3g.m.inter = 4; U.q3g.m.num_layers = 2;
    U.q3g.m.attn.tokens = 1; U.q3g.m.attn.query_heads = 2; U.q3g.m.attn.kv_heads = 1; U.q3g.m.attn.width = 2;
    U.q3g.m.final_norm_gain = gain4;

    U.q3g.m.embed = q3g_add_tensor(&U.q3g.blob, 0, Q3M_T_EMBED, 4, 4, 0, 1);
    for (size_t i = 0; i < 4; ++i) {
        U.q3g.blob.data[U.q3g.m.embed.offset + i] = 2;
        U.q3g.blob.data[U.q3g.m.embed.offset + 4 + i] = 4;
        U.q3g.blob.data[U.q3g.m.embed.offset + 8 + i] = 1;
        U.q3g.blob.data[U.q3g.m.embed.offset + 12 + i] = 8;
    }

    U.q3g.m.lm_head = q3g_add_tensor(&U.q3g.blob, 0, Q3M_T_LM_HEAD, 4, 4, 0, 0);
    for (size_t i = 0; i < 4; ++i) {
        U.q3g.blob.data[U.q3g.m.lm_head.offset + i] = 1;
        U.q3g.blob.data[U.q3g.m.lm_head.offset + 4 + i] = 5;
        U.q3g.blob.data[U.q3g.m.lm_head.offset + 8 + i] = 2;
        U.q3g.blob.data[U.q3g.m.lm_head.offset + 12 + i] = (uint8_t)-3;
    }

    for (uint32_t l = 0; l < 2; ++l) {
        struct q3m_tensor_desc *const dst[7] = {
            &U.q3g.m.layers[l].q, &U.q3g.m.layers[l].k, &U.q3g.m.layers[l].v, &U.q3g.m.layers[l].o,
            &U.q3g.m.layers[l].gate, &U.q3g.m.layers[l].up, &U.q3g.m.layers[l].down
        };
        static const uint16_t roles[7] = {
            Q3M_T_WQ, Q3M_T_WK, Q3M_T_WV, Q3M_T_WO, Q3M_T_WGATE, Q3M_T_WUP, Q3M_T_WDOWN
        };
        static const uint8_t rows[7] = { 4, 2, 2, 4, 4, 4, 4 };
        for (int j = 0; j < 7; ++j)
            *dst[j] = q3g_add_tensor(&U.q3g.blob, (uint16_t)l, roles[j], rows[j], 4, 4, 2);
        U.q3g.m.layers[l].input_gain = gain4;
        U.q3g.m.layers[l].post_gain = gain4;
        U.q3g.m.layers[l].q_gain = head_gain;
        U.q3g.m.layers[l].k_gain = head_gain;
    }

    uint32_t tokens[Q3L_TOKENS] = { 0 };
    uint32_t total = 0;
    if (q3g_generate(&U.q3g.m, q3g_blob_rd, &U.q3g.blob, cosine, sine, Q3L_TOKENS,
                     tokens, 1, 6, 2, 3, 0, 0, &U.q3g.w, &total) != Q3M_OK)
        return -1;
    if (total != 3 || tokens[1] != 1 || tokens[2] != 1)
        return -1;
    put("ic: qwen3_generate total "); putd(total);
    put(" tokens "); putd(tokens[0]); put(" "); putd(tokens[1]); put(" "); putd(tokens[2]); flush();
    return 0;
}

/* Chain two synthetic projections fetched from the verified model volume. */
static int qwen3_stream_el0(u64 object, u64 size)
{
    uint8_t header[Q3M_HDR_SIZE];
    struct q3m_header model;
    struct q3m_tensor_desc first, second, logits, embedding;
    if (q3m_read_exact(rd, &object, 0, header, sizeof(header)) != Q3M_OK ||
        q3m_parse_header(header, sizeof(header), size, &model) != Q3M_OK ||
        model.num_layers != 2 || model.num_tensors != 4 ||
        q3m_read_tensor_desc(&model, 0, rd, &object, &first) != Q3M_OK ||
        q3m_read_tensor_desc(&model, 1, rd, &object, &second) != Q3M_OK ||
        q3m_read_tensor_desc(&model, 2, rd, &object, &logits) != Q3M_OK ||
        q3m_read_tensor_desc(&model, 3, rd, &object, &embedding) != Q3M_OK ||
        embedding.rows != model.vocab_size || embedding.cols != model.hidden_dim ||
        logits.role != Q3M_T_LM_HEAD || logits.rows != model.vocab_size ||
        first.layer != 0 || first.role != Q3M_T_WQ || first.rows != 4 ||
        second.layer != 1 || second.role != Q3M_T_WDOWN || second.rows != 2)
        return -1;

    int8_t input[4];
    if (q3m_read_embedding(&embedding, rd, &object, 2, input, sizeof(input)) != Q3M_OK)
        return -1;
    const int8_t expected[4] = { 4, 2, 0, -2 };
    for (size_t i = 0; i < sizeof(input); ++i)
        if (input[i] != expected[i]) return -1;
    say("ic: qwen3 model embedding row 2 matched");
    int8_t hidden[4], output[2], scratch[4];
    if (q3m_project_i8(&first, rd, &object, input, sizeof(input), hidden,
                       sizeof(hidden), scratch, sizeof(scratch)) != Q3M_OK ||
        q3m_project_i8(&second, rd, &object, hidden, sizeof(hidden), output,
                       sizeof(output), scratch, sizeof(scratch)) != Q3M_OK)
        return -1;

    put("ic: qwen3_stream layers "); putd(model.num_layers);
    put(" y "); putd(output[0]); put(" "); putd(output[1]); flush();
    uint32_t token;
    if (q3m_select_token(&logits, rd, &object, hidden, sizeof(hidden), scratch,
                         sizeof(scratch), 0, 0, &token) != Q3M_OK)
        return -1;
    put("ic: qwen3 greedy token "); putd(token); flush();
    return 0;
}

void __attribute__((section(".text._start"))) _start(u64 mode)
{
    sayn("ic: mode ", mode);
    i64 cnt = mc(INFO, COUNT, 0, 0, 0);
    if (cnt < 0) { put("ic: refused "); putd(cnt); flush(); quit(4); }
    u64 k = mode - 1;
    if (mode == 0 || k >= (u64)cnt) { say("ic: no such object"); quit(5); }
    i64 r = mc(INFO, (i64)k, (i64)&o, 0, 0);
    if (r < 0) { put("ic: refused "); putd(r); flush(); quit(4); }
    o.name[31] = 0;
    put("ic: object "); putd((i64)k); put(" "); put(o.name); put(" size "); putd((i64)o.size); put(" kind "); putd((i64)o.kind); flush();

    if (mode == 12) {
        if (qwen3_stream_el0(k, o.size)) { say("ic: qwen3_stream rejected"); quit(3); }
        quit(0);
    }

    int e = mv_check_object(o.kind, o.size);
    if (e == MV_OK) e = mv_run_object(&R, o.size, rd, &k, buf);
    if (e != MV_OK) { put("ic: reject "); put(mv_strerror(e)); flush(); quit(3); }

    for (u32 i = 0; i < R.h.rows; i++) { put("ic: y "); putd(i); put(" "); putd(R.out_acc[i]); put(" "); putd(R.out_q[i]); flush(); }
    put("ic: done "); putd(R.h.rows); put(" rows "); putd(R.h.cols); put(" cols"); flush();
    if (mode == 1 && tiny_graph()) { say("ic: graph rejected"); quit(3); }
    if (mode == 1 && tiny_pipeline()) { say("ic: pipe rejected"); quit(3); }
    if (mode == 1 && runtime_el0_check()) { say("ic: runtime rejected"); quit(3); }
    if (mode == 1 && qwen3_layer_el0()) { say("ic: qwen3 rejected"); quit(3); }
    if (mode == 1 && qwen3_gqa_el0()) { say("ic: qwen3_gqa rejected"); quit(3); }
    if (mode == 1 && qwen3_generate_el0()) { say("ic: qwen3_generate rejected"); quit(3); }
    quit(0);
}
