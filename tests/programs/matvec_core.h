/* tests/programs/matvec_core.h - an int8 quantized matrix-vector product over one model-volume tensor object, as a pure, bounded, integer-only
 * state machine (no libc, no allocation, no floating point). Used by the EL0 program (tests/programs/inferclient.c) and, byte for byte, by the host
 * test (tests/kernel-host/matvec_test.c). This is ONE primitive, not a model: no layers, no activations, no tokenizer, no forward pass.
 *
 * Tensor object ("AURQMV1", model-volume kind MV_KIND = 1), all integers little-endian:
 *   0  magic "AURQMV1\0" | 8 u32 version = 1 | 12 u32 rows | 16 u32 cols | 20 u32 shift | 24 u32 reserved = 0 | 28 u32 reserved = 0      (32 bytes)
 *   32 x[cols]           int8 activations
 *   then `rows` records: i32 mult (1 .. 2^31-1) | int8 w[cols]                                                                           (4 + cols bytes each)
 * The object size must be EXACTLY 32 + cols + rows * (4 + cols): a header that claims more (or fewer) bytes than the object holds is rejected
 * before a single payload byte is used.
 *
 * Arithmetic (symmetric quantization, no zero points):
 *   acc[r] = sum_c w[r][c] * x[c]                              int32; the dimension limits make overflow impossible (see the static assert)
 *   q[r]   = clamp(floor((acc[r] * mult[r] + 2^(shift-1)) / 2^shift), -128, 127)      for shift > 0 (round half up); acc * mult when shift == 0
 *            the product is formed in int64: |acc| <= 2^26 and mult < 2^31, so |acc * mult| + 2^30 < 2^58: no 64-bit overflow either.
 *
 * Streaming: the payload is fed in chunks of any size (mv_feed); nothing assumes a row fits in one read. mv_run_object() reads the object through
 * a caller-supplied read function in chunks of at most MV_CHUNK (= MODEL_READ_MAX, 4096) bytes and requires every read to return exactly what was
 * asked; anything else (short, long, zero, negative) stops the run. Results are only valid when the run returns MV_OK.
 * Results are UNTRUSTED DATA: the object is integrity-checked by the kernel (unkeyed SHA-256), not authenticated.
 */
#ifndef AURORA_MATVEC_CORE_H
#define AURORA_MATVEC_CORE_H
#include <stdint.h>

#define MV_KIND       1u               /* the model-volume table "kind" of an AURQMV1 tensor object */
#define MV_VERSION    1u
#define MV_HDR_SIZE   32u
#define MV_MAX_ROWS   1024u
#define MV_MAX_COLS   4096u
#define MV_MAX_SHIFT  31u
#define MV_CHUNK      4096u            /* largest read the run asks for (SYS_MODEL's MODEL_READ_MAX) */

/* the worst |acc| is cols * 128 * 128 (every product (-128) * (-128)); it must fit in int32 */
_Static_assert((int64_t)MV_MAX_COLS * 128 * 128 <= (int64_t)INT32_MAX, "MV_MAX_COLS allows an int32 accumulator overflow");

enum { MV_OK = 0, MV_ESHORT = -1, MV_EMAGIC = -2, MV_EVERSION = -3, MV_EFORMAT = -4, MV_EDIMS = -5, MV_ESHIFT = -6, MV_ESIZE = -7,
       MV_EKIND = -8, MV_ESCALE = -9, MV_ETRUNC = -10, MV_EOVER = -11, MV_EREAD = -12 };

static inline const char *mv_strerror(int e)
{
    switch (e) {
    case MV_OK: return "ok";
    case MV_ESHORT: return "short";              /* object smaller than the 32-byte header */
    case MV_EMAGIC: return "magic";
    case MV_EVERSION: return "version";
    case MV_EFORMAT: return "format";            /* a reserved header field is not zero */
    case MV_EDIMS: return "dims";                /* rows or cols zero or above the limit */
    case MV_ESHIFT: return "shift";
    case MV_ESIZE: return "size";                /* the header's dimensions do not describe exactly this object */
    case MV_EKIND: return "kind";
    case MV_ESCALE: return "scale";              /* a row multiplier is not in 1 .. 2^31-1 */
    case MV_ETRUNC: return "trunc";              /* a read returned other than the bytes asked for, or the stream ended early */
    case MV_EOVER: return "over";                /* bytes fed after the last row */
    case MV_EREAD: return "read";                /* the read function returned an error */
    default: return "unknown";
    }
}

struct mv_hdr { uint32_t rows, cols, shift; };

struct mv_run {
    struct mv_hdr h;
    uint32_t phase;                    /* 0 activations, 1 multiplier bytes, 2 weights, 3 done */
    uint32_t row, col, mbytes, mraw, mult;
    int32_t acc;
    int err;
    int8_t x[MV_MAX_COLS];
    int32_t out_acc[MV_MAX_ROWS];
    int8_t out_q[MV_MAX_ROWS];
};

static inline uint32_t mv_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline int32_t mv_s8(uint8_t b) { return (int32_t)b - (b >= 128u ? 256 : 0); }      /* two's-complement byte without an implementation-defined cast */

/* Bytes an object with these dimensions must have; 0 for dimensions outside the limits (never computed for them, so it cannot wrap). */
static inline uint64_t mv_object_size(uint32_t rows, uint32_t cols)
{
    if (rows == 0 || cols == 0 || rows > MV_MAX_ROWS || cols > MV_MAX_COLS) return 0;
    return (uint64_t)MV_HDR_SIZE + cols + (uint64_t)rows * (4u + (uint64_t)cols);
}

/* The model-volume table entry (kind, size) before anything is read. */
static inline int mv_check_object(uint32_t kind, uint64_t size)
{
    if (kind != MV_KIND) return MV_EKIND;
    if (size < MV_HDR_SIZE) return MV_ESHORT;
    return MV_OK;
}

/* Validate the 32 header bytes in `b` (blen available) against the object's size. */
static inline int mv_parse_header(const uint8_t *b, uint64_t blen, uint64_t object_size, struct mv_hdr *h)
{
    static const uint8_t magic[8] = { 'A', 'U', 'R', 'Q', 'M', 'V', '1', 0 };
    if (blen < MV_HDR_SIZE || object_size < MV_HDR_SIZE) return MV_ESHORT;
    for (int i = 0; i < 8; i++) if (b[i] != magic[i]) return MV_EMAGIC;
    if (mv_le32(b + 8) != MV_VERSION) return MV_EVERSION;
    if (mv_le32(b + 24) != 0 || mv_le32(b + 28) != 0) return MV_EFORMAT;
    uint32_t rows = mv_le32(b + 12), cols = mv_le32(b + 16), shift = mv_le32(b + 20);
    if (rows == 0 || cols == 0 || rows > MV_MAX_ROWS || cols > MV_MAX_COLS) return MV_EDIMS;
    if (shift > MV_MAX_SHIFT) return MV_ESHIFT;
    if (mv_object_size(rows, cols) != object_size) return MV_ESIZE;
    h->rows = rows; h->cols = cols; h->shift = shift;
    return MV_OK;
}

/* floor(v / 2^s) for 0 < s <= 32, without a right shift of a negative value (|v| < 2^62 here). */
static inline int64_t mv_floor_shift(int64_t v, uint32_t s)
{
    if (v >= 0) return v >> s;
    uint64_t m = (uint64_t)(-v);
    return -(int64_t)((m + (((uint64_t)1 << s) - 1)) >> s);
}

static inline int8_t mv_requant(int32_t acc, uint32_t mult, uint32_t shift)
{
    int64_t p = (int64_t)acc * (int64_t)mult;
    if (shift > 0) p = mv_floor_shift(p + ((int64_t)1 << (shift - 1)), shift);
    if (p > 127) p = 127;
    if (p < -128) p = -128;
    return (int8_t)p;
}

static inline void mv_begin(struct mv_run *r, const struct mv_hdr *h)
{
    r->h = *h; r->phase = 0; r->row = r->col = r->mbytes = r->mraw = r->mult = 0; r->acc = 0; r->err = MV_OK;
}

/* Feed the next n payload bytes (everything after the header). Errors are sticky. */
static inline int mv_feed(struct mv_run *r, const uint8_t *p, uint64_t n)
{
    for (uint64_t i = 0; i < n && r->err == MV_OK; i++) {
        uint8_t b = p[i];
        switch (r->phase) {
        case 0:
            r->x[r->col] = (int8_t)mv_s8(b);
            if (++r->col == r->h.cols) { r->col = 0; r->phase = 1; }
            break;
        case 1:
            r->mraw |= (uint32_t)b << (8 * r->mbytes);
            if (++r->mbytes == 4) {
                if (r->mraw == 0 || r->mraw > 0x7FFFFFFFu) { r->err = MV_ESCALE; break; }
                r->mult = r->mraw; r->acc = 0; r->phase = 2;
            }
            break;
        case 2:
            r->acc += mv_s8(b) * (int32_t)r->x[r->col];
            if (++r->col == r->h.cols) {
                r->out_acc[r->row] = r->acc;
                r->out_q[r->row] = mv_requant(r->acc, r->mult, r->h.shift);
                r->col = 0; r->mbytes = 0; r->mraw = 0;
                r->phase = ++r->row == r->h.rows ? 3 : 1;
            }
            break;
        default:
            r->err = MV_EOVER;
        }
    }
    return r->err;
}

static inline int mv_finish(const struct mv_run *r)
{
    if (r->err != MV_OK) return r->err;
    return r->phase == 3 ? MV_OK : MV_ETRUNC;
}

/* Read function: copy up to len bytes at offset off of the object into buf; return the count or a negative error. */
typedef int64_t (*mv_read_fn)(void *ctx, uint64_t off, uint8_t *buf, uint64_t len);

/* Validate and compute one whole object of `size` bytes. `buf` must hold MV_CHUNK bytes. */
static inline int mv_run_object(struct mv_run *r, uint64_t size, mv_read_fn rd, void *ctx, uint8_t *buf)
{
    struct mv_hdr h;
    if (size < MV_HDR_SIZE) return MV_ESHORT;
    int64_t n = rd(ctx, 0, buf, MV_HDR_SIZE);
    if (n < 0) return MV_EREAD;
    if (n != (int64_t)MV_HDR_SIZE) return MV_ETRUNC;
    int e = mv_parse_header(buf, MV_HDR_SIZE, size, &h);
    if (e != MV_OK) return e;
    mv_begin(r, &h);
    for (uint64_t off = MV_HDR_SIZE; off < size; ) {
        uint64_t want = size - off < MV_CHUNK ? size - off : MV_CHUNK;
        n = rd(ctx, off, buf, want);
        if (n < 0) return MV_EREAD;
        if (n != (int64_t)want) return MV_ETRUNC;
        e = mv_feed(r, buf, want);
        if (e != MV_OK) return e;
        off += want;
    }
    return mv_finish(r);
}
#endif
