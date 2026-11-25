/* tests/kernel-host/quant_ops_test.c - host tests for the quantized primitives (AI-04).
 * Boundary values, edge cases and overflow cases. Built with ASan/UBSan via run.sh.
 * Pure host test: stdio/stdlib/string are fine here (host only, not the primitive code).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <quant_ops.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static int approx_eq(float a, float b, float tol)
{
    float d = a - b; if (d < 0) d = -d; return d <= tol;
}

/* ---------- quant_dot ---------- */
static void test_quant_dot(void)
{
    int32_t acc;
    int8_t a[] = { 1, 2, 3 }; int8_t b[] = { 4, 5, 6 };
    CHECK("dot basic", quant_dot(a, b, 3, &acc) == 0 && acc == 32);   /* 4+10+18 */

    int8_t neg[] = { -5, -6 }; int8_t pos[] = { 7, 8 };
    CHECK("dot mixed signs", quant_dot(neg, pos, 2, &acc) == 0 && acc == -83); /* -35-48 */

    /* all 127: each product 16129 */
    static int8_t all127[4]; for (size_t i = 0; i < 4; i++) all127[i] = 127;
    CHECK("dot all127 n4", quant_dot(all127, all127, 4, &acc) == 0 && acc == (int32_t)(4 * 16129));

    /* all -128: each product 16384 */
    static int8_t allm128[4]; for (size_t i = 0; i < 4; i++) allm128[i] = -128;
    CHECK("dot all-128 n4", quant_dot(allm128, allm128, 4, &acc) == 0 && acc == (int32_t)(4 * 16384));

    /* overflow guard: n > 131072 rejected (131072*16384 = 2^27 fits; beyond is refused) */
    static int8_t big[131072]; for (size_t i = 0; i < 131072; i++) big[i] = 127;
    CHECK("dot n==131072 ok", quant_dot(big, big, 131072, &acc) == 0);
    CHECK("dot n==131073 rejected", quant_dot(big, big, 131073, &acc) == -1);

    CHECK("dot n==0 rejected", quant_dot(a, b, 0, &acc) == -1);
    CHECK("dot NULL a", quant_dot(NULL, b, 3, &acc) == -1);
    CHECK("dot NULL b", quant_dot(a, NULL, 3, &acc) == -1);
    CHECK("dot NULL out", quant_dot(a, b, 3, NULL) == -1);
}

/* ---------- quant_matvec ---------- */
static void test_quant_matvec(void)
{
    int8_t y[4];
    /* 2x2 identity scaled: row0=[1,0], row1=[0,1], x=[10,-10], mult=1<<10, shift=10 => y=x */
    int8_t mat[4] = { 1, 0, 0, 1 };
    int8_t x[2] = { 10, -10 };
    CHECK("matvec identity", quant_matvec(mat, 2, 2, x, 1 << 10, 10, 0, y) == 0
        && y[0] == 10 && y[1] == -10);

    /* saturation: large positive accumulates clamp to 127 */
    int8_t sat[4] = { 127, 127, 127, 127 };
    int8_t xs[2] = { 127, 127 };
    CHECK("matvec saturate hi", quant_matvec(sat, 2, 2, xs, 1 << 10, 10, 0, y) == 0
        && y[0] == 127 && y[1] == 127);

    int8_t sn[4] = { -128, -128, -128, -128 };
    CHECK("matvec saturate lo", quant_matvec(sn, 2, 2, xs, 1 << 10, 10, 0, y) == 0
        && y[0] == -128 && y[1] == -128);

    /* shift == 0 (no scaling) */
    CHECK("matvec shift0", quant_matvec(mat, 2, 2, x, 1, 0, 0, y) == 0
        && y[0] == 10 && y[1] == -10);

    /* output zero point */
    CHECK("matvec zp", quant_matvec(mat, 2, 2, x, 1 << 10, 10, 10, y) == 0
        && y[0] == 20 && y[1] == 0);

    /* rejection: NULL */
    CHECK("matvec NULL mat", quant_matvec(NULL, 2, 2, x, 1, 0, 0, y) == -1);
    CHECK("matvec NULL x", quant_matvec(mat, 2, 2, NULL, 1, 0, 0, y) == -1);
    CHECK("matvec NULL y", quant_matvec(mat, 2, 2, x, 1, 0, 0, NULL) == -1);
    /* rejection: zero dims */
    CHECK("matvec rows0", quant_matvec(mat, 0, 2, x, 1, 0, 0, y) == -1);
    CHECK("matvec cols0", quant_matvec(mat, 2, 0, x, 1, 0, 0, y) == -1);
    /* rejection: mult<=0, shift>31 */
    CHECK("matvec mult0", quant_matvec(mat, 2, 2, x, 0, 0, 0, y) == -1);
    CHECK("matvec shift32", quant_matvec(mat, 2, 2, x, 1, 32, 0, y) == -1);

    /* overflow guard: cols too large */
    CHECK("matvec cols huge", quant_matvec(mat, 1, (size_t)1 << 30, x, 1, 0, 0, y) == -1);
}

/* ---------- quant_relu ---------- */
static void test_quant_relu(void)
{
    int8_t in[] = { -128, -5, 0, 3, 127 };
    int8_t out[5];
    CHECK("relu", quant_relu(in, 5, out) == 0
        && out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 3 && out[4] == 127);
    CHECK("relu NULL x", quant_relu(NULL, 5, out) == -1);
    CHECK("relu NULL out", quant_relu(in, 5, NULL) == -1);
}

/* ---------- quantize / dequantize ---------- */
static void test_quantize_scalar(void)
{
    int8_t q; float f;
    CHECK("q 0.5", quant_quantize(0.5f, 0.01f, 0, &q) == 0 && q == 50);
    CHECK("q negative", quant_quantize(-0.5f, 0.01f, 0, &q) == 0 && q == -50);
    CHECK("q clamp hi", quant_quantize(2.0f, 0.01f, 0, &q) == 0 && q == 127);
    CHECK("q clamp lo", quant_quantize(-2.0f, 0.01f, 0, &q) == 0 && q == -128);
    CHECK("q zero point", quant_quantize(0.5f, 0.01f, 50, &q) == 0 && q == -128); /* (0.5-50)/0.01 = -4950 -> clamp lo */
    CHECK("q scale<=0", quant_quantize(0.5f, 0.0f, 0, &q) == -1);
    CHECK("q scale neg", quant_quantize(0.5f, -0.01f, 0, &q) == -1);
    CHECK("q NULL out", quant_quantize(0.5f, 0.01f, 0, NULL) == -1);

    CHECK("dq basic", quant_dequantize(50, 0.01f, 0, &f) == 0 && approx_eq(f, 0.5f, 0.001f));
    CHECK("dq zp", quant_dequantize(50, 0.01f, 50, &f) == 0 && approx_eq(f, 0.0f, 0.001f));
    CHECK("dq NULL out", quant_dequantize(50, 0.01f, 0, NULL) == -1);
    CHECK("dq scale<=0", quant_dequantize(50, 0.0f, 0, &f) == -1);
}

static void test_quantize_vec(void)
{
    float f[3] = { 0.5f, -0.25f, 2.0f };
    int8_t q[3]; float out[3];
    CHECK("qvec", quant_quantize_vec(f, 3, 0.01f, 0, q) == 0
        && q[0] == 50 && q[1] == -25 && q[2] == 127);
    CHECK("qvec NULL", quant_quantize_vec(NULL, 3, 0.01f, 0, q) == -1);
    CHECK("qvec NULL out", quant_quantize_vec(f, 3, 0.01f, 0, NULL) == -1);
    CHECK("qvec scale<=0", quant_quantize_vec(f, 3, 0.0f, 0, q) == -1);

    CHECK("dqvec", quant_dequantize_vec(q, 3, 0.01f, 0, out) == 0
        && approx_eq(out[0], 0.5f, 0.001f)
        && approx_eq(out[1], -0.25f, 0.001f)
        && approx_eq(out[2], 1.27f, 0.001f));
    CHECK("dqvec NULL", quant_dequantize_vec(NULL, 3, 0.01f, 0, out) == -1);
    CHECK("dqvec NULL out", quant_dequantize_vec(q, 3, 0.01f, 0, NULL) == -1);
    CHECK("dqvec scale<=0", quant_dequantize_vec(q, 3, 0.0f, 0, out) == -1);
}

int main(void)
{
    test_quant_dot();
    test_quant_matvec();
    test_quant_relu();
    test_quantize_scalar();
    test_quantize_vec();
    printf("quant_ops_test: passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
