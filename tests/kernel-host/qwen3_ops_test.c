/* tests/kernel-host/qwen3_ops_test.c - host tests for kernel/include/qwen3_ops.h */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <qwen3_ops.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

int main(void)
{
    /* isqrt64 */
    CHECK("isqrt(0) == 0", q3_isqrt64(0) == 0);
    CHECK("isqrt(1) == 1", q3_isqrt64(1) == 1);
    CHECK("isqrt(16) == 4", q3_isqrt64(16) == 4);
    CHECK("isqrt(10000) == 100", q3_isqrt64(10000) == 100);

    /* RMSNorm */
    int8_t x[4] = { 10, -10, 10, -10 };
    int8_t g[4] = { 64, 64, 64, 64 };
    int8_t out[4];
    CHECK("rmsnorm null rejected", qwen3_rmsnorm(0, g, 4, 1, out) == Q3_E_ARG);
    CHECK("rmsnorm zero dim rejected", qwen3_rmsnorm(x, g, 0, 1, out) == Q3_E_DIMS);
    CHECK("rmsnorm oversized dim rejected", qwen3_rmsnorm(x, g, Q3_MAX_DIM + 1, 1, out) == Q3_E_DIMS);
    CHECK("rmsnorm valid succeeds", qwen3_rmsnorm(x, g, 4, 0, out) == Q3_OK);
    CHECK("rmsnorm symmetric output [64,-64,64,-64]", out[0] == 64 && out[1] == -64 && out[2] == 64 && out[3] == -64);

    int8_t zeros[4] = { 0, 0, 0, 0 };
    CHECK("rmsnorm zero input safe with eps=0", qwen3_rmsnorm(zeros, g, 4, 0, out) == Q3_OK && out[0] == 0);

    /* SiLU & SwiGLU */
    CHECK("silu(0) == 0", qwen3_silu_i8(0) == 0);
    CHECK("silu(-128) == 0", qwen3_silu_i8(-128) == 0);
    CHECK("silu(96) == 96", qwen3_silu_i8(96) == 96);
    int8_t gate[4] = { 96, -128, 0, 64 };
    int8_t up[4]   = { 32,   50, 10, 16 };
    CHECK("swiglu null rejected", qwen3_swiglu(0, up, 4, 4, out) == Q3_E_ARG);
    CHECK("swiglu bad shift rejected", qwen3_swiglu(gate, up, 4, 16, out) == Q3_E_DIMS);
    CHECK("swiglu succeeds", qwen3_swiglu(gate, up, 4, 5, out) == Q3_OK);
    CHECK("swiglu gate=-128 -> 0", out[1] == 0);
    CHECK("swiglu gate=0 -> 0", out[2] == 0);
    CHECK("swiglu gate=96, up=32, shift=5 -> 96", out[0] == 96);

    /* RoPE pair */
    int8_t r0 = 0, r1 = 0;
    qwen3_rope_pair(40, -20, 32767, 0, &r0, &r1); /* identity cos=1, sin=0 */
    CHECK("rope identity near [40, -20]", r0 >= 39 && r0 <= 40 && r1 >= -20 && r1 <= -19);
    qwen3_rope_pair(40, 0, 0, 32767, &r0, &r1); /* 90 deg: cos=0, sin=1 -> (0, 40) */
    CHECK("rope 90 deg [0, ~40]", r0 == 0 && r1 >= 39 && r1 <= 40);

    /* KV cache & causal attention */
    struct qwen3_kv_cache kv;
    CHECK("kv_init bad dim rejected", qwen3_kv_init(&kv, 0) == Q3_E_DIMS && qwen3_kv_init(&kv, Q3_MAX_HEAD_DIM + 1) == Q3_E_DIMS);
    CHECK("kv_init valid", qwen3_kv_init(&kv, 4) == Q3_OK);
    CHECK("attn on empty cache rejected", qwen3_attn_head(&kv, x, out) == Q3_E_DIMS);

    int8_t k0[4] = { 10, 0, 0, 0 }, v0[4] = { 100, 0, 0, 0 };
    int8_t k1[4] = { 0, 10, 0, 0 }, v1[4] = { 0, 100, 0, 0 };
    int8_t q[4]  = { 10, 0, 0, 0 };
    CHECK("kv_push 0", qwen3_kv_push(&kv, k0, v0) == Q3_OK);
    CHECK("kv_push 1", qwen3_kv_push(&kv, k1, v1) == Q3_OK && kv.seq_len == 2);
    CHECK("attn attends more to k0", qwen3_attn_head(&kv, q, out) == Q3_OK && out[0] > out[1]);

    for (uint32_t t = 2; t < Q3_MAX_SEQ; t++) {
        CHECK("kv_push fill", qwen3_kv_push(&kv, k0, v0) == Q3_OK);
    }
    CHECK("kv_push full rejected", qwen3_kv_push(&kv, k0, v0) == Q3_E_FULL);

    printf("qwen3_ops: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
