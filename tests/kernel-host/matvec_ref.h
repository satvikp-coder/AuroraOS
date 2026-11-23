/* tests/kernel-host/matvec_ref.h - an INDEPENDENT reference for tests/programs/matvec_core.h, used by the host test (matvec_test.c) and by the test
 * model generator (tests/tools/mktensor.c). Written differently on purpose: whole arrays instead of a byte stream, int64 accumulation with no limits,
 * rounding through C division instead of shifts, and a serializer that writes the format from its description. Host only (libc). */
#ifndef AURORA_MATVEC_REF_H
#define AURORA_MATVEC_REF_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* floor((acc * mult + 2^(shift-1)) / 2^shift), or acc * mult for shift 0, clamped to int8 */
static inline int ref_requant(int64_t acc, int64_t mult, unsigned shift)
{
    int64_t t = acc * mult, q;
    if (shift == 0) q = t;
    else {
        int64_t d = (int64_t)1 << shift;
        t += d / 2;
        q = t / d;                          /* C division truncates toward zero ... */
        if (t % d != 0 && t < 0) q -= 1;    /* ... so step down for negative non-exact quotients (floor) */
    }
    return q > 127 ? 127 : q < -128 ? -128 : (int)q;
}

/* w is rows x cols row-major. acc_out may be NULL. */
static inline void ref_matvec(const int8_t *w, const int8_t *x, const int64_t *mult, unsigned rows, unsigned cols, unsigned shift, int64_t *acc_out, int *q_out)
{
    for (unsigned r = 0; r < rows; r++) {
        int64_t a = 0;
        for (unsigned c = 0; c < cols; c++) a += (int64_t)w[(size_t)r * cols + c] * (int64_t)x[c];
        if (acc_out) acc_out[r] = a;
        q_out[r] = ref_requant(a, mult[r], shift);
    }
}

static inline void ref_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* Serialize an AURQMV1 object (header fields given raw so callers can write hostile headers). Returns a malloc'd buffer of *len bytes. */
static inline uint8_t *ref_serialize(uint32_t hrows, uint32_t hcols, uint32_t shift, uint32_t version, uint32_t res0, uint32_t res1,
                                     const int8_t *x, const int64_t *mult, const int8_t *w, unsigned rows, unsigned cols, size_t *len)
{
    size_t n = 32 + (size_t)cols + (size_t)rows * (4 + (size_t)cols);
    uint8_t *b = malloc(n ? n : 1);
    if (!b) return NULL;
    memcpy(b, "AURQMV1", 8);                /* includes the terminating NUL */
    ref_put32(b + 8, version); ref_put32(b + 12, hrows); ref_put32(b + 16, hcols); ref_put32(b + 20, shift); ref_put32(b + 24, res0); ref_put32(b + 28, res1);
    size_t o = 32;
    for (unsigned c = 0; c < cols; c++) b[o++] = (uint8_t)x[c];
    for (unsigned r = 0; r < rows; r++) {
        ref_put32(b + o, (uint32_t)mult[r]); o += 4;
        for (unsigned c = 0; c < cols; c++) b[o++] = (uint8_t)w[(size_t)r * cols + c];
    }
    *len = n;
    return b;
}
#endif
