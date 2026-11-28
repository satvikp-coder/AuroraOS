#include <qwen3_small_layer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
    int8_t data[256];
    size_t used;
};

static int64_t read_weights(void *ctx, uint64_t off, uint8_t *buf, uint64_t len)
{
    struct fixture *f = ctx;
    if (off >= f->used || len > f->used - off) return -1;
    if (len > 3) len = 3; /* weights can cross read boundaries */
    memcpy(buf, f->data + off, (size_t)len);
    return (int64_t)len;
}

static void matrix(struct fixture *f, struct q3m_tensor_desc *d, int rows, int cols,
                   int role, int layer)
{
    *d = (struct q3m_tensor_desc){.rows = rows, .cols = cols, .shift = 4,
                                  .offset = f->used, .bytes = rows * cols};
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            f->data[f->used++] = (r*7 + c*11 + r*c*3 + role*5 + layer*3) % 19 - 9;
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed at line %d\n", __LINE__); return 1; } } while (0)

int main(void)
{
    const int16_t gains[][4] = {{4096,3584,4608,3840}, {3840,4352,4096,3584},
                                {4096,3072,5120,3584}, {3584,4608,4096,3840}};
    const int16_t cosine[] = {32767,32767,17705,32767,-13636,32767};
    const int16_t sine[] = {0,0,27573,33,29796,66};
    const struct q3_gqa_shape shape = {2,1,4,3};
    struct q3l_work work;
    struct q3l_weights m = {.input_gain=gains[0], .post_gain=gains[1],
                            .q_gain=gains[2], .k_gain=gains[3]};
    struct fixture f = {0};
    int8_t span[3][12] = {{12,-20,18,27, -17,22,31,-9, 9,25,-13,15}};
    /* Float64 PyTorch RMSNorm, split-half RoPE, causal SDPA, SiLU and residuals.
     * Both layers consumed the preceding float result in the independent oracle.
     * Goldens are rounded to the nearest Q5 unit only after each full layer. */
    const int expected[][12] = {{27,-44,31,1, 4,-8,31,-11, 10,10,-1,17},
                                {19,-30,45,-8, 1,-20,39,-32, -3,-5,33,-15}};
    for (int layer = 0; layer < 2; ++layer) {
        f.used = 0;
        matrix(&f,&m.q,8,4,0,layer);
        matrix(&f,&m.k,4,4,1,layer);
        matrix(&f,&m.v,4,4,2,layer);
        matrix(&f,&m.o,4,8,3,layer);
        matrix(&f,&m.gate,6,4,4,layer);
        matrix(&f,&m.up,6,4,5,layer);
        matrix(&f,&m.down,4,6,6,layer);
        CHECK(q3l_forward(&shape,4,6,&m,read_weights,&f,cosine,sine,6,
                          span[layer],span[layer+1],12,&work) == 0);
        int error = 0;
        for (int i = 0; i < 12; ++i) {
            int diff = abs(expected[layer][i] - span[layer+1][i]);
            if (diff > error) error = diff;
        }
        printf("layer %d max error %d Q5 units\n", layer+1, error);
        CHECK(error <= (layer == 0 ? 2 : 6));
        /* Running only the first position must reproduce the full-span prefix. */
        struct q3_gqa_shape prefix = shape;
        prefix.tokens = 1;
        int8_t first[4];
        CHECK(q3l_forward(&prefix,4,6,&m,read_weights,&f,cosine,sine,2,
                          span[layer],first,4,&work) == 0);
        CHECK(memcmp(first, span[layer+1], 4) == 0);
    }
    CHECK(q3l_forward(&shape,4,6,&m,read_weights,&f,cosine,sine,5,
                      span[0],span[1],12,&work) == Q3M_E_BOUNDS);
    m.k.rows = 3;
    CHECK(q3l_forward(&shape,4,6,&m,read_weights,&f,cosine,sine,6,
                      span[0],span[1],12,&work) == Q3M_E_DIMS);
    m.k.rows = 4;
    f.used = 7;
    CHECK(q3l_forward(&shape,4,6,&m,read_weights,&f,cosine,sine,6,
                      span[0],span[1],12,&work) == Q3M_E_READ);
    return 0;
}
