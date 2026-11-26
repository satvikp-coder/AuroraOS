/* tests/kernel-host/ai_pipeline_test.c - host integration test for the EL0 AI pipeline:
 * tokenizer -> embedding lookup -> bounded tensor -> quant_matvec -> quant_relu -> argmax.
 * Built with -Wall -Wextra -Werror -fsanitize=address,undefined via run.sh.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <tokenizer.h>
#include <tensor.h>
#include <quant_ops.h>

static int passed, failed;
#define CHECK(name, cond) do { \
    if (cond) { passed++; } \
    else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

#define EMBED_VOCAB_SIZE 16u
#define EMBED_DIM        4u
#define OUT_DIM          4u

/* Lookup a single token's embedding into a bounded tok_tensor.
 * Returns 0 on success, -1 if token_id is out of bounds or tensor_push fails. */
static int embed_lookup_to_tensor(const int8_t table[EMBED_VOCAB_SIZE][EMBED_DIM],
                                  uint32_t token_id,
                                  struct tok_tensor *out)
{
    if (!out || token_id >= EMBED_VOCAB_SIZE) return -1;
    tensor_init(out);
    for (size_t i = 0; i < EMBED_DIM; i++) {
        if (tensor_push(out, table[token_id][i]) != 0) return -1;
    }
    return 0;
}

/* Deterministic argmax over int8_t array of length n (ties break to lowest index). */
static size_t argmax_i8(const int8_t *v, size_t n)
{
    size_t best_idx = 0;
    int8_t best_val = v[0];
    for (size_t i = 1; i < n; i++) {
        if (v[i] > best_val) {
            best_val = v[i];
            best_idx = i;
        }
    }
    return best_idx;
}

int main(void)
{
    /* 1. Synthetic BPE vocabulary and merge table */
    static const struct tok_entry vocab[] = {
        { "h",     4 },
        { "e",     5 },
        { "l",     6 },
        { "o",     7 },
        { " ",     8 },
        { "he",    9 },
        { "ll",   10 },
        { "llo",  11 },
        { "hello",12 },
    };
    static const struct tok_merge merges[] = {
        { 4,  5,  9, 0 }, /* "h" + "e" -> "he" */
        { 6,  6, 10, 1 }, /* "l" + "l" -> "ll" */
        { 10, 7, 11, 2 }, /* "ll" + "o" -> "llo" */
        { 9, 11, 12, 3 }, /* "he" + "llo" -> "hello" */
    };

    struct tokenizer tok;
    tokenizer_init(&tok, vocab, sizeof(vocab) / sizeof(vocab[0]),
                   merges, sizeof(merges) / sizeof(merges[0]));
    CHECK("tokenizer valid", tok.valid == 1);

    /* 2. Deterministic embedding table (16 x 4) */
    static const int8_t embed_table[EMBED_VOCAB_SIZE][EMBED_DIM] = {
        [TOK_ID_BOS] = {  1,  0,  0,  0 },
        [TOK_ID_EOS] = {  0,  1,  0,  0 },
        [TOK_ID_PAD] = {  0,  0,  0,  0 },
        [TOK_ID_UNK] = { -1, -1, -1, -1 },
        [12]         = { 10, -5, 20,  4 }, /* "hello" */
    };

    /* 3. Deterministic weight matrix (4 x 4, row-major) */
    static const int8_t weights[OUT_DIM * EMBED_DIM] = {
        /* row 0: 10*1 + (-5)*0 + 20*0 + 4*0 = 10 */
         1,  0,  0,  0,
        /* row 1: 10*(-2) + (-5)*1 + 20*(-1) + 4*0 = -45 (clamped to 0 by ReLU) */
        -2,  1, -1,  0,
        /* row 2: 10*2 + (-5)*(-1) + 20*3 + 4*1 = 20 + 5 + 60 + 4 = 89 */
         2, -1,  3,  1,
        /* row 3: 10*0 + (-5)*2 + 20*1 + 4*2 = 0 - 10 + 20 + 8 = 18 */
         0,  2,  1,  2,
    };

    /* Run full pipeline twice to verify determinism */
    uint32_t ids_a[8], ids_b[8];
    int n_a = tokenizer_encode(&tok, "hello", 5, ids_a, 8, 1, 1);
    int n_b = tokenizer_encode(&tok, "hello", 5, ids_b, 8, 1, 1);
    CHECK("encode count == 3", n_a == 3 && n_b == 3);
    CHECK("encode deterministic", memcmp(ids_a, ids_b, 3 * sizeof(uint32_t)) == 0);
    CHECK("encoded [BOS, hello, EOS]",
          ids_a[0] == TOK_ID_BOS && ids_a[1] == 12 && ids_a[2] == TOK_ID_EOS);

    /* Embedding lookup for token 12 ("hello") -> bounded tensor */
    struct tok_tensor emb;
    CHECK("embed lookup ok", embed_lookup_to_tensor(embed_table, ids_a[1], &emb) == 0);
    CHECK("tensor len == EMBED_DIM", tensor_len(&emb) == EMBED_DIM);

    int8_t elem = 0;
    CHECK("tensor_get(0) == 10", tensor_get(&emb, 0, &elem) == 0 && elem == 10);
    CHECK("tensor_get(1) == -5", tensor_get(&emb, 1, &elem) == 0 && elem == -5);
    CHECK("tensor_get(2) == 20", tensor_get(&emb, 2, &elem) == 0 && elem == 20);
    CHECK("tensor_get(3) == 4",  tensor_get(&emb, 3, &elem) == 0 && elem == 4);

    /* quant_matvec (mult=1, shift=0, zp_out=0 -> exact int8 clamped dot) */
    int8_t logits_a[OUT_DIM], logits_b[OUT_DIM];
    CHECK("quant_matvec a", quant_matvec(weights, OUT_DIM, EMBED_DIM,
                                         emb.data, 1, 0, 0, logits_a) == 0);
    CHECK("quant_matvec b", quant_matvec(weights, OUT_DIM, EMBED_DIM,
                                         emb.data, 1, 0, 0, logits_b) == 0);
    CHECK("matvec deterministic", memcmp(logits_a, logits_b, OUT_DIM) == 0);
    CHECK("logits exact",
          logits_a[0] == 10 && logits_a[1] == -45 && logits_a[2] == 89 && logits_a[3] == 18);

    /* quant_relu */
    int8_t relu_out[OUT_DIM];
    CHECK("quant_relu ok", quant_relu(logits_a, OUT_DIM, relu_out) == 0);
    CHECK("relu exact",
          relu_out[0] == 10 && relu_out[1] == 0 && relu_out[2] == 89 && relu_out[3] == 18);

    /* argmax */
    size_t best = argmax_i8(relu_out, OUT_DIM);
    CHECK("argmax == 2", best == 2);

    /* 4. Bounds & error-handling checks */
    uint32_t small_out[2];
    CHECK("encode overflow rejected",
          tokenizer_encode(&tok, "hello", 5, small_out, 2, 1, 1) == TOK_E_TOO_LONG);

    const char bad_utf8[2] = { (char)0xFF, (char)0xFE };
    CHECK("bad utf8 rejected",
          tokenizer_encode(&tok, bad_utf8, 2, ids_a, 8, 0, 0) == TOK_E_BAD_UTF8);

    /* Fallback byte token ID (TOK_BYTE_ID_BASE + 'z') must be rejected by embed_lookup */
    uint32_t oov_ids[4];
    int oov_n = tokenizer_encode(&tok, "z", 1, oov_ids, 4, 0, 0);
    CHECK("oov byte token encoded", oov_n == 1 && oov_ids[0] == TOK_BYTE_ID_BASE + 'z');
    CHECK("oov embed lookup rejected",
          embed_lookup_to_tensor(embed_table, oov_ids[0], &emb) == -1);

    /* Tensor OOB */
    CHECK("tensor_get OOB", tensor_get(&emb, EMBED_DIM, &elem) == -1);
    CHECK("tensor_set OOB", tensor_set(&emb, EMBED_DIM, 1) == -1);

    /* quant_ops invalid args */
    CHECK("quant_matvec zero rows",
          quant_matvec(weights, 0, EMBED_DIM, emb.data, 1, 0, 0, logits_a) == -1);
    CHECK("quant_matvec bad mult",
          quant_matvec(weights, OUT_DIM, EMBED_DIM, emb.data, 0, 0, 0, logits_a) == -1);
    CHECK("quant_matvec bad shift",
          quant_matvec(weights, OUT_DIM, EMBED_DIM, emb.data, 1, 32, 0, logits_a) == -1);
    CHECK("quant_relu null", quant_relu(NULL, OUT_DIM, relu_out) == -1);

    printf("ai_pipeline_test: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
