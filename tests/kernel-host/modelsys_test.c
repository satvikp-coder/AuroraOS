/* tests/kernel-host/modelsys_test.c - the SYS_MODEL range rule (model_read_clamp in kernel/include/modelsys.h) against hostile values: 2^64-1 everywhere,
 * offset == size, length 0 / 4096 / 4097, empty objects, and a sweep that compares it with an independent 128-bit reference. Host, ASan/UBSan. */
#include <stdio.h>
#include <stdint.h>
#include <modelsys.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)
#define M64 0xFFFFFFFFFFFFFFFFull

/* reference: the same rule written with unsigned __int128 so no comparison here can wrap */
static int64_t ref(uint64_t size, uint64_t off, uint64_t len)
{
    if (len == 0) return MODEL_CLAMP_EINVAL;
    if (len > MODEL_READ_MAX) return MODEL_CLAMP_E2BIG;
    if ((unsigned __int128)off > (unsigned __int128)size) return MODEL_CLAMP_EINVAL;
    unsigned __int128 end = (unsigned __int128)off + len;
    if (end > size) end = size;
    return (int64_t)(end - off);
}

int main(void)
{
    printf("SYS_MODEL range-clamp tests\n");
    CHECK("the ABI record is 80 bytes and MODEL_READ_MAX is 4096", sizeof(struct user_model_obj) == 80 && MODEL_READ_MAX == 4096u && SYS_MODEL == 11);
    CHECK("error codes are AE_EINVAL (-1) and AE_E2BIG (-3)", MODEL_CLAMP_EINVAL == -1 && MODEL_CLAMP_E2BIG == -3);
    CHECK("plain read inside the object", model_read_clamp(10000, 0, 4096) == 4096 && model_read_clamp(10000, 4096, 4096) == 4096);
    CHECK("the tail is clamped (10000 - 8192 = 1808)", model_read_clamp(10000, 8192, 4096) == 1808);
    CHECK("offset == size is EOF: 0, for any valid length", model_read_clamp(10000, 10000, 1) == 0 && model_read_clamp(10000, 10000, 4096) == 0 && model_read_clamp(512, 512, 4096) == 0);
    CHECK("offset == size + 1 is EINVAL", model_read_clamp(10000, 10001, 1) == MODEL_CLAMP_EINVAL);
    CHECK("length 0 is EINVAL, even at offset 0 and at EOF", model_read_clamp(10000, 0, 0) == MODEL_CLAMP_EINVAL && model_read_clamp(10000, 10000, 0) == MODEL_CLAMP_EINVAL);
    CHECK("length 4096 is allowed, 4097 is E2BIG", model_read_clamp(1u << 20, 0, 4096) == 4096 && model_read_clamp(1u << 20, 0, 4097) == MODEL_CLAMP_E2BIG);
    CHECK("length 2^64-1 is E2BIG (never truncated to a small number)", model_read_clamp(M64, 0, M64) == MODEL_CLAMP_E2BIG && model_read_clamp(10, 0, M64) == MODEL_CLAMP_E2BIG);
    CHECK("offset 2^64-1 on a normal object is EINVAL", model_read_clamp(10000, M64, 1) == MODEL_CLAMP_EINVAL && model_read_clamp(10000, M64, 4096) == MODEL_CLAMP_EINVAL);
    CHECK("offset 2^64-1 + length 4096 cannot wrap into the object (size 2^64-1: offset == size -> 0)", model_read_clamp(M64, M64, 4096) == 0);
    CHECK("offsets 2^64-4097 / 2^64-4096 / 2^64-4095 / 2^64-2 on a 2^64-1 object -> 4096 / 4096 / 4095 / 1, no wrap", model_read_clamp(M64, M64 - 4097, 4096) == 4096 && model_read_clamp(M64, M64 - 4096, 4096) == 4096
          && model_read_clamp(M64, M64 - 4095, 4096) == 4095 && model_read_clamp(M64, M64 - 1, 4096) == 1);
    CHECK("offset 2^63 on a small object is EINVAL", model_read_clamp(512, 1ull << 63, 1) == MODEL_CLAMP_EINVAL);
    CHECK("an empty object: only EOF (offset 0) or EINVAL", model_read_clamp(0, 0, 1) == 0 && model_read_clamp(0, 1, 1) == MODEL_CLAMP_EINVAL && model_read_clamp(0, 0, 0) == MODEL_CLAMP_EINVAL);
    CHECK("the result never exceeds the length or the bytes left", model_read_clamp(700, 699, 4096) == 1 && model_read_clamp(700, 0, 1) == 1 && model_read_clamp(700, 0, 700) == 700);
    {   /* sweep: interesting values for each argument, all combinations, against the 128-bit reference; and every result in [0, len] lies inside the object */
        static const uint64_t V[] = { 0, 1, 2, 511, 512, 513, 4095, 4096, 4097, 8191, 8192, 10000, 0xFFFFFFFFull, 0x100000000ull, 1ull << 40, 1ull << 62,
                                      (1ull << 63) - 1, 1ull << 63, (1ull << 63) + 1, M64 - 4097, M64 - 4096, M64 - 4095, M64 - 1, M64 };
        const int n = (int)(sizeof V / sizeof V[0]);
        long bad = 0, total = 0;
        for (int a = 0; a < n; a++) for (int b = 0; b < n; b++) for (int c = 0; c < n; c++) {
            int64_t got = model_read_clamp(V[a], V[b], V[c]), want = ref(V[a], V[b], V[c]);
            total++;
            if (got != want) bad++;
            if (got >= 0 && ((uint64_t)got > V[c] || (unsigned __int128)V[b] + (uint64_t)got > V[a])) bad++;
        }
        printf("  sweep: %ld combinations\n", total);
        CHECK("sweep: 24^3 hostile combinations agree with the 128-bit reference and stay inside the object", bad == 0 && total == 13824);
    }
    {   /* 2,000,000 pseudo-random triples, half of them near the object end */
        uint64_t s = 88172645463325252ull; long bad = 0;
        for (int i = 0; i < 2000000; i++) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17; uint64_t size = s;
            s ^= s << 13; s ^= s >> 7; s ^= s << 17; uint64_t off = (i & 1) ? size - (s % 8192) : s;
            s ^= s << 13; s ^= s >> 7; s ^= s << 17; uint64_t len = (i & 2) ? s % 4200 : s;
            if (model_read_clamp(size, off, len) != ref(size, off, len)) bad++;
        }
        CHECK("2,000,000 random triples agree with the reference", bad == 0);
    }
    printf("SYS_MODEL range clamp: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
