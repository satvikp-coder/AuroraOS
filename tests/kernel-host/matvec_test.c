/* tests/kernel-host/matvec_test.c - the int8 matrix-vector primitive (tests/programs/matvec_core.h, the same header the EL0 program compiles)
 * against hand-computed vectors and an independent reference (matvec_ref.h): rounding, saturation, the int32 accumulator at the maximum dimensions,
 * zero / oversized / lying dimensions, size mismatches, short objects, truncated / failing / over-long reads, arbitrary chunking.
 * Host, ASan/UBSan (UBSan would also report any signed overflow inside the core). Mutation check: tests/kernel-host/mutate_matvec.py. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../programs/matvec_core.h"
#include "matvec_ref.h"

static int passed, failed;
#define CHECK(name, cond) do { if (cond) passed++; else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static struct mv_run R;
static uint8_t chunk[MV_CHUNK];

/* a fake device: the object bytes, plus injected misbehaviour */
struct dev {
    const uint8_t *b; uint64_t have;       /* bytes the "device" really has (may be fewer than the object size given to the run) */
    int64_t fail_at, short_at, zero_at, long_at;   /* offsets at/after which a read errors / is one short / returns 0 / claims one more (-1 = never) */
    int calls; uint64_t biggest;
};
static void dev_init(struct dev *d, const uint8_t *b, uint64_t have) { memset(d, 0, sizeof *d); d->b = b; d->have = have; d->fail_at = d->short_at = d->zero_at = d->long_at = -1; }
static int64_t dev_read(void *ctx, uint64_t off, uint8_t *buf, uint64_t len)
{
    struct dev *d = ctx;
    d->calls++; if (len > d->biggest) d->biggest = len;
    if (len == 0 || len > MV_CHUNK) return -3;
    if (d->fail_at >= 0 && off >= (uint64_t)d->fail_at) return -5;
    if (d->zero_at >= 0 && off >= (uint64_t)d->zero_at) return 0;
    if (off > d->have) return -1;
    uint64_t n = d->have - off < len ? d->have - off : len;
    memcpy(buf, d->b + off, n);
    if (d->short_at >= 0 && off >= (uint64_t)d->short_at && n > 0) return (int64_t)n - 1;
    if (d->long_at >= 0 && off >= (uint64_t)d->long_at) return (int64_t)n + 1;
    return (int64_t)n;
}

static uint64_t seed = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(seed >> 33); }

/* a model with explicit arrays */
struct model { unsigned rows, cols, shift; int8_t *x, *w; int64_t *mult; uint8_t *obj; size_t len; };
static void model_free(struct model *m) { free(m->x); free(m->w); free(m->mult); free(m->obj); memset(m, 0, sizeof *m); }
static void model_alloc(struct model *m, unsigned rows, unsigned cols, unsigned shift)
{
    m->rows = rows; m->cols = cols; m->shift = shift;
    m->x = calloc(cols ? cols : 1, 1); m->w = calloc((size_t)rows * cols + 1, 1); m->mult = calloc(rows ? rows : 1, sizeof *m->mult);
    if (!m->x || !m->w || !m->mult) { printf("out of memory\n"); exit(2); }
}
static void model_pack(struct model *m) { free(m->obj); m->obj = ref_serialize(m->rows, m->cols, m->shift, 1, 0, 0, m->x, m->mult, m->w, m->rows, m->cols, &m->len); }
static void model_random(struct model *m, unsigned rows, unsigned cols, unsigned shift)
{
    model_alloc(m, rows, cols, shift);
    for (unsigned c = 0; c < cols; c++) m->x[c] = (int8_t)((int)(rnd() & 0xFF) - 128);
    for (size_t i = 0; i < (size_t)rows * cols; i++) m->w[i] = (int8_t)((int)(rnd() & 0xFF) - 128);
    for (unsigned r = 0; r < rows; r++) {
        uint32_t k = rnd() % 4;
        m->mult[r] = k == 0 ? 1 : k == 1 ? 0x7FFFFFFF : k == 2 ? 1 + (rnd() % 65536) : 1 + (rnd() & 0x7FFFFFFE);
    }
    model_pack(m);
}
/* run the core over the object (size = m->len) through a clean device */
static int run_clean(const struct model *m) { struct dev d; dev_init(&d, m->obj, m->len); return mv_run_object(&R, m->len, dev_read, &d, chunk); }
/* the core's results equal the reference */
static int matches_ref(const struct model *m)
{
    int64_t *acc = malloc(sizeof *acc * (m->rows ? m->rows : 1)); int *q = malloc(sizeof *q * (m->rows ? m->rows : 1)); int ok = 1;
    ref_matvec(m->w, m->x, m->mult, m->rows, m->cols, m->shift, acc, q);
    for (unsigned r = 0; r < m->rows; r++) if ((int64_t)R.out_acc[r] != acc[r] || (int)R.out_q[r] != q[r]) ok = 0;
    free(acc); free(q);
    return ok;
}

static void test_requant(void)
{
    CHECK("requant: shift 0 is acc * mult", mv_requant(6, 7, 0) == 42);
    CHECK("requant: saturates high at 127", mv_requant(1000, 1, 0) == 127 && mv_requant(128, 1, 0) == 127 && mv_requant(127, 1, 0) == 127);
    CHECK("requant: saturates low at -128", mv_requant(-1000, 1, 0) == -128 && mv_requant(-129, 1, 0) == -128 && mv_requant(-128, 1, 0) == -128);
    CHECK("requant: 2.5 rounds half up to 3", mv_requant(5, 1, 1) == 3);
    CHECK("requant: -2.5 rounds half up to -2", mv_requant(-5, 1, 1) == -2);
    CHECK("requant: -3 exact stays -3", mv_requant(-6, 1, 1) == -3);
    CHECK("requant: -2.75 rounds to -3", mv_requant(-11, 1, 2) == -3);
    CHECK("requant: 64.5 rounds half up to 65", mv_requant(129, 1, 1) == 65);
    CHECK("requant: the largest product (2^26 * (2^31-1)) at shift 31 saturates to 127, no 64-bit overflow", mv_requant(67108864, 0x7FFFFFFF, 31) == 127);
    CHECK("requant: the most negative product at shift 31 saturates to -128", mv_requant(-67108864, 0x7FFFFFFF, 31) == -128);
    CHECK("requant: 2^26 with mult 1 at shift 20 is exactly 64", mv_requant(67108864, 1, 20) == 64);
    CHECK("requant: 1 at shift 31 is 0 and -1 at shift 31 is 0", mv_requant(1, 1, 31) == 0 && mv_requant(-1, 1, 31) == 0);
    /* sweep against the reference */
    int bad = 0;
    for (int i = 0; i < 200000; i++) {
        int32_t acc = (int32_t)(rnd() % 134217729u) - 67108864;         /* [-2^26, 2^26] */
        uint32_t mult = rnd() % 4 == 0 ? 1 + rnd() % 300 : 1 + (rnd() & 0x7FFFFFFE);
        uint32_t sh = rnd() % 32;
        if ((int)mv_requant(acc, mult, sh) != ref_requant(acc, mult, sh)) bad++;
    }
    CHECK("requant: 200000 random (acc, mult, shift) agree with the division-based reference", bad == 0);
}

static void test_hand(void)
{
    /* x = [1, -2, 3], shift 2
     * row 0: mult 4,    w [4, 5, -6]     acc 4 - 10 - 18 = -24;   (-96 + 2) / 4 = -23.5 -> floor -24
     * row 1: mult 3,    w [-128, 127, 0] acc -128 - 254 = -382;   (-1146 + 2) / 4 = -286 -> clamp -128
     * row 2: mult 5,    w [1, 1, 1]      acc 2;                   (10 + 2) / 4 = 3
     * row 3: mult 5,    w [-1, -1, -1]   acc -2;                  (-10 + 2) / 4 = -2
     * row 4: mult 1000, w [127, 0, 0]    acc 127;                 (127000 + 2) / 4 = 31750 -> clamp 127 */
    struct model m; model_alloc(&m, 5, 3, 2);
    const int8_t x[3] = { 1, -2, 3 }, w[15] = { 4, 5, -6, -128, 127, 0, 1, 1, 1, -1, -1, -1, 127, 0, 0 };
    const int64_t mult[5] = { 4, 3, 5, 5, 1000 };
    const int32_t want_acc[5] = { -24, -382, 2, -2, 127 }; const int want_q[5] = { -24, -128, 3, -2, 127 };
    memcpy(m.x, x, 3); memcpy(m.w, w, 15); memcpy(m.mult, mult, sizeof mult); model_pack(&m);
    CHECK("hand: the object is 32 + 3 + 5 * 7 = 70 bytes", m.len == 70 && mv_object_size(5, 3) == 70);
    CHECK("hand: runs", run_clean(&m) == MV_OK);
    int ok = 1; for (int r = 0; r < 5; r++) if (R.out_acc[r] != want_acc[r] || R.out_q[r] != want_q[r]) ok = 0;
    CHECK("hand: every accumulator and output equals the hand-computed value", ok);
    CHECK("hand: and the reference agrees", matches_ref(&m));
    model_free(&m);

    model_alloc(&m, 1, 1, 0); m.x[0] = 2; m.w[0] = 3; m.mult[0] = 7; model_pack(&m);
    CHECK("hand: 1x1 shift 0: 3 * 2 * 7 = 42", run_clean(&m) == MV_OK && R.out_acc[0] == 6 && R.out_q[0] == 42);
    model_free(&m);
}

static void test_limits(void)
{
    CHECK("limits: the worst accumulator at MV_MAX_COLS fits int32", (int64_t)MV_MAX_COLS * 16384 <= INT32_MAX);
    CHECK("limits: (for scale) 131072 columns of (-128)^2 would NOT fit int32: the limit is what prevents overflow", 131072LL * 16384 > INT32_MAX);
    struct model m;
    /* maximum dimensions, every product (-128) * (-128): acc = 4096 * 16384 = 67108864 exactly, in every row */
    model_alloc(&m, MV_MAX_ROWS, MV_MAX_COLS, 31);
    memset(m.x, 0x80, MV_MAX_COLS); memset(m.w, 0x80, (size_t)MV_MAX_ROWS * MV_MAX_COLS);
    for (unsigned r = 0; r < MV_MAX_ROWS; r++) m.mult[r] = 0x7FFFFFFF;
    model_pack(&m);
    struct dev d; dev_init(&d, m.obj, m.len);
    int rc = mv_run_object(&R, m.len, dev_read, &d, chunk);
    int all = 1; for (unsigned r = 0; r < MV_MAX_ROWS; r++) if (R.out_acc[r] != 67108864 || R.out_q[r] != 127) all = 0;
    CHECK("limits: 1024 x 4096 of (-128)*(-128) runs; every acc is exactly 67108864 and saturates to 127", rc == MV_OK && all);
    CHECK("limits: and the reference agrees", matches_ref(&m));
    CHECK("limits: no read asked for more than 4096 bytes", d.biggest == MV_CHUNK);
    /* the most negative accumulator: x = 127, w = -128 */
    memset(m.x, 0x7F, MV_MAX_COLS); model_pack(&m);
    rc = run_clean(&m); all = 1; for (unsigned r = 0; r < MV_MAX_ROWS; r++) if (R.out_acc[r] != -66584576 || R.out_q[r] != -128) all = 0;
    CHECK("limits: x = 127, w = -128: every acc is exactly -66584576 and saturates to -128", rc == MV_OK && all);
    model_free(&m);

    /* one past each limit is refused even when the object size matches the claimed dimensions */
    model_random(&m, 1, MV_MAX_COLS + 1, 0);
    CHECK("limits: cols 4097 (object size consistent) -> dims", run_clean(&m) == MV_EDIMS);
    model_free(&m);
    model_random(&m, MV_MAX_ROWS + 1, 1, 0);
    CHECK("limits: rows 1025 (object size consistent) -> dims", run_clean(&m) == MV_EDIMS);
    model_free(&m);
    model_random(&m, MV_MAX_ROWS, 1, 0);
    CHECK("limits: rows 1024 exactly is accepted", run_clean(&m) == MV_OK && matches_ref(&m));
    model_free(&m);
    model_random(&m, 1, MV_MAX_COLS, 0);
    CHECK("limits: cols 4096 exactly is accepted", run_clean(&m) == MV_OK && matches_ref(&m));
    model_free(&m);
    model_random(&m, 2, 3, 0);
    ref_put32(m.obj + 16, 131072);
    CHECK("limits: cols 131072 (the int32 overflow point) -> dims", run_clean(&m) == MV_EDIMS);
    ref_put32(m.obj + 12, 0xFFFFFFFFu); ref_put32(m.obj + 16, 0xFFFFFFFFu);
    CHECK("limits: rows = cols = 2^32-1 -> dims (no size arithmetic wraps)", run_clean(&m) == MV_EDIMS && mv_object_size(0xFFFFFFFFu, 0xFFFFFFFFu) == 0);
    model_free(&m);
}

static void test_hostile(void)
{
    struct model m; struct dev d;
    model_alloc(&m, 0, 3, 0); model_pack(&m);
    CHECK("zero dims: rows 0 -> dims", run_clean(&m) == MV_EDIMS);
    model_free(&m);
    model_alloc(&m, 3, 0, 0); model_pack(&m);
    CHECK("zero dims: cols 0 -> dims", run_clean(&m) == MV_EDIMS);
    model_free(&m);
    CHECK("zero dims: mv_object_size(0, n) and (n, 0) are 0", mv_object_size(0, 5) == 0 && mv_object_size(5, 0) == 0);

    model_random(&m, 37, 300, 24);
    CHECK("baseline: 37 x 300 runs and matches the reference", run_clean(&m) == MV_OK && matches_ref(&m));
    uint8_t *big = malloc(m.len + 16); memcpy(big, m.obj, m.len); memset(big + m.len, 0, 16);
    dev_init(&d, big, m.len + 1);
    CHECK("size: one trailing byte after the last row -> size", mv_run_object(&R, m.len + 1, dev_read, &d, chunk) == MV_ESIZE && d.calls == 1);
    dev_init(&d, m.obj, m.len - 1);
    CHECK("size: one byte missing -> size (detected from the header, before any payload read)", mv_run_object(&R, m.len - 1, dev_read, &d, chunk) == MV_ESIZE && d.calls == 1);
    ref_put32(big + 12, 38);
    dev_init(&d, big, m.len);
    CHECK("lying header: rows 38 over an object holding 37 -> size", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_ESIZE && d.calls == 1);
    ref_put32(big + 12, 37); ref_put32(big + 16, 301);
    CHECK("lying header: cols 301 over an object holding 300 -> size", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_ESIZE);
    ref_put32(big + 16, 300);

    for (uint64_t s = 0; s < MV_HDR_SIZE; s++) {
        dev_init(&d, m.obj, s);
        if (mv_run_object(&R, s, dev_read, &d, chunk) != MV_ESHORT || d.calls != 0) { CHECK("short object: sizes 0..31 -> short, nothing read", 0); break; }
        if (s == MV_HDR_SIZE - 1) CHECK("short object: sizes 0..31 -> short, nothing read", 1);
    }
    CHECK("short object: the table check refuses size 31 and 0", mv_check_object(MV_KIND, 31) == MV_ESHORT && mv_check_object(MV_KIND, 0) == MV_ESHORT);
    CHECK("kind: 0 and 2 are refused, 1 with 32 bytes passes the table check", mv_check_object(0, 1000) == MV_EKIND && mv_check_object(2, 1000) == MV_EKIND && mv_check_object(MV_KIND, 32) == MV_OK);
    CHECK("short object: a 32-byte object has no payload -> size", (dev_init(&d, m.obj, 32), mv_run_object(&R, 32, dev_read, &d, chunk)) == MV_ESIZE);

    /* the device misbehaves (or holds less than the verified size says) */
    dev_init(&d, m.obj, m.len); d.short_at = 32 + 4096;
    CHECK("truncated read: the second payload chunk comes back one byte short -> trunc", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_ETRUNC);
    dev_init(&d, m.obj, m.len); d.short_at = 0;
    CHECK("truncated read: the header read comes back short -> trunc", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_ETRUNC && d.calls == 1);
    dev_init(&d, m.obj, m.len); d.zero_at = 100;
    CHECK("truncated read: a read returns 0 (EOF) early -> trunc", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_ETRUNC);
    dev_init(&d, m.obj, m.len - 10);
    CHECK("truncated read: the device holds 10 bytes fewer than the object size -> trunc", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_ETRUNC);
    dev_init(&d, m.obj, m.len); d.long_at = 32;
    CHECK("over-long read: a read claims one byte more than asked -> trunc", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_ETRUNC);
    dev_init(&d, m.obj, m.len); d.fail_at = 32 + 4096;
    CHECK("failing read: an error mid-object -> read", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_EREAD);
    dev_init(&d, m.obj, m.len); d.fail_at = 0;
    CHECK("failing read: an error on the header -> read", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_EREAD);
    dev_init(&d, m.obj, m.len);
    CHECK("chunks: 11580 bytes are read as header + 4096 + 4096 + 3356 (4 reads, none above 4096)", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_OK && d.calls == 4 && d.biggest == 4096 && m.len == 11580);

    memcpy(big, m.obj, m.len); big[0] = 'B'; dev_init(&d, big, m.len);
    CHECK("header: bad magic -> magic", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_EMAGIC);
    memcpy(big, m.obj, m.len); big[7] = '!'; dev_init(&d, big, m.len);
    CHECK("header: magic without its NUL -> magic", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_EMAGIC);
    memcpy(big, m.obj, m.len); ref_put32(big + 8, 2); dev_init(&d, big, m.len);
    CHECK("header: version 2 -> version", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_EVERSION);
    memcpy(big, m.obj, m.len); big[24] = 1; dev_init(&d, big, m.len);
    CHECK("header: reserved word 0 non-zero -> format", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_EFORMAT);
    memcpy(big, m.obj, m.len); big[31] = 0x80; dev_init(&d, big, m.len);
    CHECK("header: reserved word 1 non-zero -> format", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_EFORMAT);
    memcpy(big, m.obj, m.len); ref_put32(big + 20, 32); dev_init(&d, big, m.len);
    CHECK("header: shift 32 -> shift", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_ESHIFT);
    memcpy(big, m.obj, m.len); ref_put32(big + 20, 31); dev_init(&d, big, m.len);
    CHECK("header: shift 31 is accepted", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_OK);
    size_t rec = 32 + 300 + 20 * 304;                     /* row 20's multiplier */
    memcpy(big, m.obj, m.len); ref_put32(big + rec, 0); dev_init(&d, big, m.len);
    CHECK("scale: a zero multiplier in row 20 -> scale", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_ESCALE);
    memcpy(big, m.obj, m.len); ref_put32(big + rec, 0x80000000u); dev_init(&d, big, m.len);
    CHECK("scale: a negative (2^31) multiplier -> scale", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_ESCALE);
    memcpy(big, m.obj, m.len); ref_put32(big + rec, 0xFFFFFFFFu); dev_init(&d, big, m.len);
    CHECK("scale: multiplier -1 -> scale", mv_run_object(&R, m.len, dev_read, &d, chunk) == MV_ESCALE);
    free(big);

    /* the stream machine on its own */
    struct mv_hdr h; CHECK("stream: header parses", mv_parse_header(m.obj, m.len, m.len, &h) == MV_OK && h.rows == 37 && h.cols == 300 && h.shift == 24);
    CHECK("stream: a header buffer of 31 bytes -> short", mv_parse_header(m.obj, 31, m.len, &h) == MV_ESHORT);
    mv_begin(&R, &h);
    CHECK("stream: finishing before any byte -> trunc", mv_finish(&R) == MV_ETRUNC);
    mv_feed(&R, m.obj + 32, m.len - 33);
    CHECK("stream: one byte short of the end -> trunc", mv_finish(&R) == MV_ETRUNC);
    mv_feed(&R, m.obj + m.len - 1, 1);
    CHECK("stream: the last byte completes it", mv_finish(&R) == MV_OK && matches_ref(&m));
    uint8_t extra = 0;
    CHECK("stream: a byte after the last row -> over, and the error sticks", mv_feed(&R, &extra, 1) == MV_EOVER && mv_finish(&R) == MV_EOVER && mv_feed(&R, &extra, 0) == MV_EOVER);
    model_free(&m);
}

static void test_random(void)
{
    int bad = 0, runs = 0;
    for (int it = 0; it < 300; it++) {
        struct model m; model_random(&m, 1 + rnd() % 40, 1 + rnd() % 700, rnd() % 32);
        struct mv_hdr h;
        if (mv_parse_header(m.obj, m.len, m.len, &h) != MV_OK) { bad++; model_free(&m); continue; }
        mv_begin(&R, &h);
        size_t off = 32;                                        /* feed in random pieces, 1 .. 5000 bytes */
        while (off < m.len) { size_t n = 1 + rnd() % 5000; if (n > m.len - off) n = m.len - off; mv_feed(&R, m.obj + off, n); off += n; }
        if (mv_finish(&R) != MV_OK || !matches_ref(&m)) bad++;
        if (run_clean(&m) != MV_OK || !matches_ref(&m)) bad++;
        runs++; model_free(&m);
    }
    CHECK("random: 300 models (1..40 x 1..700, every shift, extreme multipliers), random chunking and whole-object runs, all equal the reference", bad == 0 && runs == 300);
}

int main(void)
{
    printf("int8 matvec tests\n");
    test_requant();
    test_hand();
    test_limits();
    test_hostile();
    test_random();
    printf("matvec: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
