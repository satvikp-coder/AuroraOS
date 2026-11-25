/*
 * kernel/proc/tensor.c — bounded fixed-size int8 tensor implementation (AI-03).
 * Pure C: no libc, no dynamic allocation, no global state. See kernel/include/tensor.h.
 */
#include <tensor.h>

void tensor_init(struct tok_tensor *t)
{
    if (!t) return;
    t->len = 0;
}

size_t tensor_len(const struct tok_tensor *t)
{
    if (!t) return 0;
    return t->len;
}

int tensor_get(const struct tok_tensor *t, size_t i, int8_t *out)
{
    if (!t || !out) return -1;
    if (i >= t->len) return -1;
    *out = t->data[i];
    return 0;
}

int tensor_set(struct tok_tensor *t, size_t i, int8_t v)
{
    if (!t) return -1;
    if (i >= t->len) return -1;
    t->data[i] = v;
    return 0;
}

int tensor_push(struct tok_tensor *t, int8_t v)
{
    if (!t) return -1;
    if (t->len >= TOK_TENSOR_CAP) return -1;
    t->data[t->len++] = v;
    return 0;
}
