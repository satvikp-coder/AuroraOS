/*
 * kernel/proc/vec_ops.c — int8 vector operations (AI-05).
 *
 * Element-wise int8 addition with saturation. Pure C: no libc, no dynamic
 * allocation, no global state. See kernel/include/vec_ops.h.
 */
#include <vec_ops.h>

int vec_add_sat(const int8_t *a, const int8_t *b, size_t n, int8_t *out)
{
    if (!a || !b || !out) return -1;
    for (size_t i = 0; i < n; i++) {
        int32_t v = (int32_t)a[i] + (int32_t)b[i];
        if (v < -128) v = -128;
        if (v > 127)  v = 127;
        out[i] = (int8_t)v;
    }
    return 0;
}

int vec_add_sat_alias(const int8_t *a, const int8_t *b, size_t n, int8_t *out)
{
    /* Read both inputs fully before writing, so out may safely alias a or b. */
    if (!a || !b || !out) return -1;
    int8_t tmp[16]; /* small staging; process in chunks to avoid large stack use */
    for (size_t i = 0; i < n; i += 16) {
        size_t chunk = n - i; if (chunk > 16) chunk = 16;
        for (size_t k = 0; k < chunk; k++) {
            int32_t v = (int32_t)a[i + k] + (int32_t)b[i + k];
            if (v < -128) v = -128;
            if (v > 127)  v = 127;
            tmp[k] = (int8_t)v;
        }
        for (size_t k = 0; k < chunk; k++) out[i + k] = tmp[k];
    }
    return 0;
}
