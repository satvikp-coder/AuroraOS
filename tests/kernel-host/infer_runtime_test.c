/* tests/kernel-host/infer_runtime_test.c - host tests for kernel/include/infer_runtime.h */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <infer_runtime.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

int main(void)
{
    static const struct tok_entry vocab[] = {
        { "ab", TOK_ID_FIRST + 0 },
        { "cd", TOK_ID_FIRST + 1 },
    };
    static const struct tok_merge merges[] = {
        { TOK_BYTE_ID_BASE + 'a', TOK_BYTE_ID_BASE + 'b', TOK_ID_FIRST + 0, 0 },
        { TOK_BYTE_ID_BASE + 'c', TOK_BYTE_ID_BASE + 'd', TOK_ID_FIRST + 1, 1 },
    };

    struct tokenizer tok;
    tokenizer_init(&tok, vocab, 2, merges, 2);

    /* 6 vocab entries x 4 embed_dim */
    static const int8_t embed[6 * 4] = {
        0, 0, 0, 0,
        1, 1, 1, 1,
        2, 2, 2, 2,
        3, 3, 3, 3,
        10, 20, -5, 4,   /* token 4 ("ab") */
        -4, 8, 12, 6,    /* token 5 ("cd") */
    };
    /* w1: 3 hidden x 4 embed_dim */
    static const int8_t w1[3 * 4] = {
        2, 1, 0, 1,
        -1, -1, 1, 0,
        1, 2, 1, 1
    };
    /* w2: 2 classes x 3 hidden */
    static const int8_t w2[2 * 3] = {
        1, 0, 2,
        -1, 1, 0
    };

    struct ir_mlp_model model = {
        .vocab_size = 6,
        .embed_dim = 4,
        .hidden_dim = 3,
        .num_classes = 2,
        .embed_table = embed,
        .w1 = w1,
        .w1_mult = 1,
        .w1_shift = 2,
        .w1_zp_out = 0,
        .w2 = w2,
        .w2_mult = 1,
        .w2_shift = 1,
        .w2_zp_out = 0
    };

    /* Model validation */
    CHECK("valid model passes", ir_validate_model(&model) == IR_OK);
    CHECK("null model rejected", ir_validate_model(0) == IR_E_ARG);
    struct ir_mlp_model bad = model;
    bad.embed_table = 0; CHECK("null embed rejected", ir_validate_model(&bad) == IR_E_ARG);
    bad = model; bad.vocab_size = 0; CHECK("zero vocab rejected", ir_validate_model(&bad) == IR_E_DIMS);
    bad = model; bad.embed_dim = IR_MAX_DIM + 1; CHECK("oversized embed_dim rejected", ir_validate_model(&bad) == IR_E_DIMS);
    bad = model; bad.hidden_dim = IR_MAX_HIDDEN + 1; CHECK("oversized hidden_dim rejected", ir_validate_model(&bad) == IR_E_DIMS);
    bad = model; bad.num_classes = IR_MAX_CLASSES + 1; CHECK("oversized num_classes rejected", ir_validate_model(&bad) == IR_E_DIMS);
    bad = model; bad.w1_mult = 0; CHECK("zero w1_mult rejected", ir_validate_model(&bad) == IR_E_SCALE);
    bad = model; bad.w2_shift = 32; CHECK("shift 32 rejected", ir_validate_model(&bad) == IR_E_SCALE);

    /* End-to-end execution */
    struct ir_result res;
    CHECK("run_text abcd succeeds", ir_run_text(&tok, &model, "abcd", 4, &res) == IR_OK);
    CHECK("token count is 2 (ab, cd)", res.token_count == 2 && res.tokens[0] == 4 && res.tokens[1] == 5);
    /* Hand-verified:
     * emb = avg([10, 20, -5, 4], [-4, 8, 12, 6]) = [3, 14, 3, 5]
     * h1_acc = [2*3+1*14+0*3+1*5=25, -3-14+3=-14, 3+28+3+5=39]
     * h1_q (mult=1, shift=2) = [6, -3, 10]
     * h1_relu = [6, 0, 10]
     * logits_acc = [1*6+0+2*10=26, -6+0+0=-6]
     * logits_q (mult=1, shift=1) = [13, -3]
     * argmax = 0
     */
    CHECK("logits match hand-computed [13, -3]", res.logits[0] == 13 && res.logits[1] == -3);
    CHECK("argmax is 0", res.argmax == 0);
    CHECK("output validation passes", ir_validate_output(&res, 2) == IR_OK);

    /* Output validation negative tests */
    struct ir_result bad_res = res;
    bad_res.argmax = 2; CHECK("out-of-range argmax rejected", ir_validate_output(&bad_res, 2) == IR_E_OUTPUT);
    bad_res = res; bad_res.argmax = 1; CHECK("non-maximum argmax rejected", ir_validate_output(&bad_res, 2) == IR_E_OUTPUT);
    bad_res = res; bad_res.status = IR_E_EXEC; CHECK("non-OK status rejected", ir_validate_output(&bad_res, 2) == IR_E_OUTPUT);
    bad_res = res; bad_res.num_classes = 3; CHECK("class count mismatch rejected", ir_validate_output(&bad_res, 2) == IR_E_OUTPUT);

    /* Invalid inputs to run_text */
    CHECK("empty text rejected", ir_run_text(&tok, &model, "", 0, &res) == IR_E_ARG);
    CHECK("oversized text rejected", ir_run_text(&tok, &model, "a", TOK_MAX_INPUT + 1, &res) == IR_E_ARG);
    CHECK("null tokenizer rejected", ir_run_text(0, &model, "ab", 2, &res) == IR_E_ARG);

    /* Tensor dim mismatch */
    struct tok_tensor wrong_t;
    tensor_init(&wrong_t);
    tensor_push(&wrong_t, 1);
    CHECK("mismatched tensor dim rejected", ir_forward_mlp(&model, &wrong_t, &res) == IR_E_DIMS);

    printf("infer_runtime: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
