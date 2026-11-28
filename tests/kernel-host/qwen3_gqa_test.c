#include <qwen3_gqa.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "failed at line %d\n", __LINE__); return 1; } } while (0)

static int16_t projected(int token, int head, int channel, int seed)
{
    return (int16_t)(((token + 1) * (channel + 2) + head * 3 +
                      channel * channel + seed) % 13 - 6);
}

int main(void)
{
    /* Rounded Q12 output from the independent Qwen3 attention fixture. */
    const int16_t expected[3][16] = {
        {-8192, 0, 16384, -12288, -8192, 0, 16384, -12288, 4096, 12288, -24576, 0, 4096, 12288, -24576, 0},
        {-3563, 6943, -4446, -716, -3671, 6781, -3959, -986, 10146, 21364, -12475, 15126, 9108, 19806, -14553, 12529},
        {-1165, 10540, -11204, 1247, 5978, 21255, -3784, -20460, 14202, 10720, -4364, 8538, 18013, -11200, 3258, -9571}
    };
    const int16_t q_gain[] = {4096, 2048, 5120, 3072};
    const int16_t k_gain[] = {3072, 6144, 4096, 2048};
    const int16_t cosine[][2] = {{32767, 32767}, {17705, 32767}, {-13636, 32767}};
    const int16_t sine[][2] = {{0, 0}, {27573, 33}, {29796, 66}};
    const struct q3_gqa_shape shape = {4, 2, 4, 3};
    int16_t keys[24], values[24], query[16], out[16];
    int max_error = 0;
    for (int token = 0; token < 3; ++token) {
        for (int head = 0; head < 2; ++head) {
            int16_t *key = keys + (token * 2 + head) * 4;
            for (int i = 0; i < 4; ++i) {
                key[i] = projected(token, head, i, 5) * 256;
                values[(token * 2 + head) * 4 + i] = projected(token, head, i, 2) * 4096;
            }
            CHECK(q3_qk_prepare(key, k_gain, cosine[token], sine[token], 4, key) == Q3_OK);
        }
        for (int head = 0; head < 4; ++head) {
            int16_t *q = query + head * 4;
            for (int i = 0; i < 4; ++i) q[i] = projected(token, head, i, 0) * 256;
            CHECK(q3_qk_prepare(q, q_gain, cosine[token], sine[token], 4, q) == Q3_OK);
        }
        CHECK(q3_gqa_context(&shape, (uint32_t)token, query, 16, keys, values, 24, out, 16) == Q3_OK);
        for (int i = 0; i < 16; ++i) {
            int error = abs(out[i] - expected[token][i]);
            if (error > max_error) max_error = error;
        }
    }
    CHECK(max_error <= 8);

    /* Reject invalid geometry, output capacity and explicit cache bounds. */
    struct q3_gqa_shape invalid = shape;
    invalid.kv_heads = 3;
    CHECK(q3_gqa_context(&invalid, 0, query, 16, keys, values, 24, out, 16) == Q3_E_DIMS);
    invalid = shape;
    invalid.tokens = Q3_GQA_MAX_TOKENS + 1;
    CHECK(q3_gqa_context(&invalid, 0, query, 16, keys, values, 24, out, 16) == Q3_E_DIMS);
    CHECK(q3_gqa_context(&shape, 3, query, 16, keys, values, 24, out, 16) == Q3_E_DIMS);
    CHECK(q3_gqa_context(&shape, 0, query, 15, keys, values, 24, out, 16) == Q3_E_DIMS);
    CHECK(q3_gqa_context(&shape, 0, query, 16, keys, values, 23, out, 16) == Q3_E_DIMS);
    CHECK(q3_gqa_context(&shape, 0, query, 16, keys, values, 24, out, 15) == Q3_E_DIMS);

    /* Maximum exposed cache length and width with full-scale signed values. */
    static int16_t wide_keys[Q3_GQA_MAX_TOKENS * Q3_QK_MAX_WIDTH];
    static int16_t wide_values[Q3_GQA_MAX_TOKENS * Q3_QK_MAX_WIDTH];
    int16_t wide_query[Q3_QK_MAX_WIDTH], wide_out[Q3_QK_MAX_WIDTH];
    const struct q3_gqa_shape wide = {1, 1, Q3_QK_MAX_WIDTH, Q3_GQA_MAX_TOKENS};
    const size_t wide_len = sizeof wide_keys / sizeof wide_keys[0];
    for (size_t i = 0; i < wide_len; ++i) {
        wide_keys[i] = i < Q3_QK_MAX_WIDTH ? INT16_MAX : INT16_MIN;
        wide_values[i] = i < Q3_QK_MAX_WIDTH ? INT16_MAX : INT16_MIN;
    }
    for (size_t i = 0; i < Q3_QK_MAX_WIDTH; ++i) wide_query[i] = INT16_MIN;
    CHECK(q3_gqa_context(&wide, wide.tokens - 1, wide_query, wide.width,
                         wide_keys, wide_values, wide_len, wide_out, wide.width) == Q3_OK);
    for (size_t i = 0; i < wide.width; ++i) CHECK(wide_out[i] == INT16_MIN);
    printf("qwen3_gqa: 48 reference channels passed, max error %d/4096; bounds and wide cache passed\n", max_error);
    return 0;
}
