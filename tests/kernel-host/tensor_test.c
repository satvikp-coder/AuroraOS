/* tests/kernel-host/tensor_test.c - host tests for the bounded int8 tensor (AI-03).
 * Boundary values and edge cases. Built with ASan/UBSan via run.sh.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <tensor.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

int main(void)
{
    struct tok_tensor t;

    /* init -> len 0 */
    t.len = 999; /* dirty state */
    tensor_init(&t);
    CHECK("init len0", tensor_len(&t) == 0);

    /* get/set on empty tensor -> out of range */
    int8_t v = 0;
    CHECK("get empty", tensor_get(&t, 0, &v) == -1);
    CHECK("set empty", tensor_set(&t, 0, 7) == -1);

    /* push fills the tensor */
    CHECK("push a", tensor_push(&t, 10) == 0);
    CHECK("push b", tensor_push(&t, -20) == 0);
    CHECK("push c", tensor_push(&t, 127) == 0);
    CHECK("len after 3 pushes", tensor_len(&t) == 3);

    /* get within bounds */
    CHECK("get 0", tensor_get(&t, 0, &v) == 0 && v == 10);
    CHECK("get 1", tensor_get(&t, 1, &v) == 0 && v == -20);
    CHECK("get 2", tensor_get(&t, 2, &v) == 0 && v == 127);

    /* get past len -> fail */
    CHECK("get past len", tensor_get(&t, 3, &v) == -1);
    CHECK("get far past", tensor_get(&t, 1000, &v) == -1);

    /* set within bounds */
    CHECK("set 1", tensor_set(&t, 1, 100) == 0);
    CHECK("set 1 got", tensor_get(&t, 1, &v) == 0 && v == 100);

    /* set past len -> fail, no modification */
    CHECK("set past len", tensor_set(&t, 5, 1) == -1);
    CHECK("set past len unchanged", tensor_len(&t) == 3 && v == 100); /* v still last get */

    /* get/set NULL handling */
    CHECK("get NULL tensor", tensor_get(NULL, 0, &v) == -1);
    CHECK("get NULL out", tensor_get(&t, 0, NULL) == -1);
    CHECK("set NULL tensor", tensor_set(NULL, 0, 1) == -1);

    /* push to capacity, then one more fails */
    struct tok_tensor big;
    tensor_init(&big);
    size_t pushed = 0;
    while (tensor_push(&big, (int8_t)1) == 0) pushed++;
    CHECK("push to cap", big.len == TOK_TENSOR_CAP && pushed == TOK_TENSOR_CAP);
    CHECK("push beyond cap fails", tensor_push(&big, 1) == -1);
    CHECK("len stays at cap", big.len == TOK_TENSOR_CAP);

    /* tensor_init(NULL) is a no-op (no crash) */
    tensor_init(NULL);
    CHECK("init NULL no crash", 1);

    printf("tensor_test: passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
