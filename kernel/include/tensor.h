#ifndef AURORA_TENSOR_H
#define AURORA_TENSOR_H

/*
 * kernel/include/tensor.h — bounded fixed-size int8 tensor primitive (AI-03).
 *
 * A tok_tensor owns a fixed inline buffer sized by TOK_TENSOR_CAP (compile-time, default
 * 4096 elements) and a `len` tracking the number of valid elements. No dynamic allocation,
 * no libc, no global state. All access is bounds-checked against `len` and TOK_TENSOR_CAP.
 *
 * EL0 / unprivileged safe: pure functions, suitable for host tests and for EL0 programs.
 */

#include <stdint.h>
#include <stddef.h>

#ifndef TOK_TENSOR_CAP
#define TOK_TENSOR_CAP 4096u   /* max elements per tensor; enlarge only with a memory review */
#endif

struct tok_tensor {
    int8_t data[TOK_TENSOR_CAP];
    size_t len;                /* number of valid elements in data[0..len) */
};

/* Zero a tensor: len = 0. Contents beyond len are left untouched. */
void tensor_init(struct tok_tensor *t);

/* Number of valid elements. */
size_t tensor_len(const struct tok_tensor *t);

/* Read element i into *out. Returns 0 on success, -1 if i >= len. */
int tensor_get(const struct tok_tensor *t, size_t i, int8_t *out);

/* Write element i = v. Returns 0 on success, -1 if i >= len. */
int tensor_set(struct tok_tensor *t, size_t i, int8_t v);

/* Append v at the end (index len). Returns 0 on success, -1 if len == TOK_TENSOR_CAP. */
int tensor_push(struct tok_tensor *t, int8_t v);

#endif /* AURORA_TENSOR_H */
