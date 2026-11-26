/* tests/kernel-host/quant_ops_test.c - host tests for the quantized primitives (AI-04).
 * Boundary values, edge cases and overflow cases. Built with ASan/UBSan via run.sh.
 * Pure host test: stdio/stdlib/string are fine here (host only, not the primitive code).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <quant_ops.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

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

    static int8_t big[131072];
    memset(big, -128, sizeof big);
    CHECK("largest worst-case dot product stays in int32", quant_dot(big, big, 131071, &acc) == 0 && acc == INT32_MAX - 16383);
    CHECK("one more worst-case product is refused", quant_dot(big, big, 131072, &acc) == -1);

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
    CHECK("matvec rejects overflowing matrix shape", quant_matvec(mat, SIZE_MAX, 2, x, 1, 0, 0, y) == -1);
    CHECK("large positive and negative scaled results saturate", quant_matvec(mat, 2, 2, x, INT32_MAX, 0, 127, y) == 0
          && y[0] == 127 && y[1] == -128);
    CHECK("negative rounding uses floor after the half offset", quant_matvec(mat, 2, 2, x, 1, 2, 0, y) == 0
          && y[0] == 3 && y[1] == -2);
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

int main(void)
{
    test_quant_dot();
    test_quant_matvec();
    test_quant_relu();
    printf("quant_ops_test: passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
