#include <qwen3_qk.h>
#include <stdio.h>
#include <stdlib.h>

struct example {
    int position, is_key;
    int16_t input[4], expected[4];
};

/* Q12-rounded float64 per-head results from qwen3_attention_fixture.py. */
static const struct example cases[] = {
    {0, 0, {-4,-2,2,-5}, {-4681,-1170,2926,-4389}},
    {0, 0, {-1,1,5,-2}, {-1471,736,9196,-2207}},
    {0, 0, {2,4,-5,1}, {2416,2416,-7549,906}},
    {0, 0, {5,-6,-2,4}, {4551,-2731,-2276,2731}},
    {0, 1, {1,3,-6,0}, {906,5435,-7247,0}},
    {0, 1, {4,6,-3,3}, {2937,8812,-2937,1469}},
    {1, 0, {-2,1,6,0}, {-9457,640,3031,1}},
    {1, 0, {1,4,-4,3}, {6001,2525,-2351,2847}},
    {1, 0, {4,-6,-1,6}, {2790,-2609,2336,3905}},
    {1, 0, {-6,-3,2,-4}, {-5432,-1521,-3758,-3050}},
    {1, 1, {3,6,-2,5}, {2760,8568,774,2389}},
    {1, 1, {6,-4,1,-5}, {1475,-5563,4013,-2324}},
    {2, 0, {0,4,-3,5}, {3950,2308,1808,4349}},
    {2, 0, {3,-6,0,-5}, {-1222,-2930,2671,-3678}},
    {2, 0, {6,-3,3,-2}, {-6354,-1610,4190,-1617}},
    {2, 0, {-4,0,6,1}, {-5801,-2,-7605,844}},
    {2, 1, {5,-4,2,-3}, {-3767,-6685,2873,-1686}},
    {2, 1, {-5,-1,5,0}, {-3425,-1721,-6298,-3}},
};

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed at line %d\n", __LINE__); return 1; } } while (0)

int main(void)
{
    const int16_t gain[][4] = {{4096,2048,5120,3072}, {3072,6144,4096,2048}};
    const int16_t cosine[][2] = {{32767,32767}, {17705,32767}, {-13636,32767}};
    const int16_t sine[][2] = {{0,0}, {27573,33}, {29796,66}};
    int max_error = 0;
    for (size_t n = 0; n < sizeof(cases) / sizeof(*cases); ++n) {
        const struct example *c = &cases[n];
        int16_t head[4];
        for (int i = 0; i < 4; ++i) head[i] = c->input[i] * 256;
        CHECK(q3_qk_prepare(head, gain[c->is_key], cosine[c->position],
                            sine[c->position], 4, head) == Q3_OK);
        for (int i = 0; i < 4; ++i) {
            int error = abs(head[i] - c->expected[i]);
            if (error > max_error) max_error = error;
        }
    }
    CHECK(max_error <= 2);
    int16_t full[128] = {0}, scale[128], cos0[64], sin0[64] = {0};
    for (int i = 0; i < 128; ++i) scale[i] = 32767;
    for (int i = 0; i < 64; ++i) cos0[i] = 32767;
    CHECK(q3_qk_prepare(full, scale, cos0, sin0, 128, full) == Q3_OK);
    for (int i = 0; i < 128; ++i) CHECK(full[i] == 0);
    full[1] = INT16_MIN;
    full[64] = INT16_MAX;
    CHECK(q3_qk_prepare(full, scale, cos0, sin0, 128, full) == Q3_OK);
    CHECK(full[1] == INT16_MIN && full[64] == INT16_MAX);
    CHECK(q3_qk_prepare(full, scale, cos0, sin0, 127, full) == Q3_E_DIMS);
    CHECK(q3_qk_prepare(full, scale, cos0, sin0, 130, full) == Q3_E_DIMS);
    CHECK(q3_qk_prepare(NULL, scale, cos0, sin0, 4, full) == Q3_E_ARG);
    printf("qwen3_qk: 72 fixture channels, maximum error %d Q12 units\n", max_error);
    return 0;
}
