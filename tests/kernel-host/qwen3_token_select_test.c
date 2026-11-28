#include <qwen3_token_select.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "failure at line %d\n", __LINE__); return 1; } } while (0)

struct source {
    int8_t weights[8];
    uint64_t fail_from;
    unsigned reads;
};

static int64_t read_rows(void *ctx, uint64_t offset, uint8_t *data, uint64_t count)
{
    struct source *src = ctx;
    src->reads++;
    if (offset >= src->fail_from || offset > sizeof src->weights ||
        count > sizeof src->weights - offset)
        return -1;
    memcpy(data, src->weights + offset, (size_t)count);
    return (int64_t)count;
}

static int filter_id(void *ctx, uint32_t id)
{
    const int *mask = ctx;
    if (*mask < 0) return *mask;
    return (*mask & (1 << id)) != 0;
}

int main(void)
{
    struct q3m_tensor_desc tensor = { .role = Q3M_T_LM_HEAD, .rows = 4, .cols = 2, .bytes = 8 };
    struct source src = { .weights = { 1, 1, 2, 2, 2, 2, -1, -1 }, .fail_from = UINT64_MAX };
    const int8_t input[] = { 100, 100 };
    int8_t scratch[4];
    uint32_t token = 99;
    CHECK(q3m_select_token(&tensor, read_rows, &src, input, 2, scratch, 4, 0, 0, &token) == Q3M_OK);
    CHECK(token == 1 && src.reads == 2);
    int mask = 1 << 3;
    tensor.role = Q3M_T_EMBED;
    CHECK(q3m_select_token(&tensor, read_rows, &src, input, 2, scratch, 4, filter_id, &mask, &token) == Q3M_OK);
    CHECK(token == 3);
    CHECK(q3m_read_embedding(&tensor, read_rows, &src, 3, scratch, 4) == Q3M_OK);
    CHECK(scratch[0] == -1 && scratch[1] == -1);
    CHECK(q3m_read_embedding(&tensor, read_rows, &src, 4, scratch, 4) == Q3M_E_NOENT);
    CHECK(q3m_read_embedding(&tensor, read_rows, &src, 3, scratch, 1) == Q3M_E_BOUNDS);
    mask = 0;
    CHECK(q3m_select_token(&tensor, read_rows, &src, input, 2, scratch, 4, filter_id, &mask, &token) == Q3M_E_NOENT);
    CHECK(token == 3);
    mask = -100;
    CHECK(q3m_select_token(&tensor, read_rows, &src, input, 2, scratch, 4, filter_id, &mask, &token) == -100);
    CHECK(token == 3);
    src.fail_from = 4;
    CHECK(q3m_select_token(&tensor, read_rows, &src, input, 2, scratch, 4, 0, 0, &token) == Q3M_E_READ);
    CHECK(token == 3);
    CHECK(q3m_select_token(&tensor, read_rows, &src, input, 2, scratch, 1, 0, 0, &token) == Q3M_E_BOUNDS);
    tensor.offset = UINT64_MAX - 1;
    CHECK(q3m_select_token(&tensor, read_rows, &src, input, 2, scratch, 4, 0, 0, &token) == Q3M_E_BOUNDS);
    puts("qwen3_token_select: 16 passed, 0 failed");
    return 0;
}
