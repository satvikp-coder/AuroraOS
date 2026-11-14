/*
 * kernel/gfx/gfx_shape.c - rounded rectangles and borders (stage G3). Pure, integer-only, no libm, no floating point.
 *
 * COVERAGE RULE (exact, no anti-aliasing - Tier 0 is opaque):
 *   A pixel (x, y) belongs to a rounded rectangle iff its CENTRE (x + 1/2, y + 1/2) lies inside the rectangle and, when the
 *   centre falls in a corner quadrant, no farther than the radius r from that corner's circle centre. The corner circle centres
 *   are at (rx + r, ry + r), (rx + w - r, ry + r), (rx + r, ry + h - r), (rx + w - r, ry + h - r) in pixel-EDGE coordinates.
 *   In doubled coordinates (to stay in integers): dx = 2x + 1 - 2*cx, dy = 2y + 1 - 2*cy, and the pixel is inside iff
 *   dx*dx + dy*dy <= (2r)^2.  Per row this gives an exact span: |dx| <= isqrt((2r)^2 - dy*dy).
 *   - The radius is clamped to [0, min(w, h) / 2]; radius 0 is a plain rectangle; radius min(w,h)/2 on a square is a circle,
 *     on a w > h rectangle a pill.
 *   - A BORDER of width b is the outer shape minus the inner shape, where the inner shape is the rectangle inset by b on every
 *     side with radius max(r - b, 0). The two share the same corner centres (concentric), so the border has even thickness
 *     and the border plus the inner fill equals the outer fill exactly, with no overlap and no gap.
 *   - Everything is clipped to the surface by clipping each row span; nothing outside the surface is touched, and rectangles
 *     that start far off-screen cost only the visible rows.
 * Arithmetic is int64; the largest intermediate is (2r)^2 with r <= INT32_MAX / 2, below 2^62.
 */
#include <gfx.h>

static uint64_t isqrt64(uint64_t n)                 /* floor(sqrt(n)), exact */
{
    uint64_t r = 0, bit = 1ULL << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r;
}
static int64_t floor_div2(int64_t a) { return a >= 0 ? a / 2 : -((-a + 1) / 2); }
static int64_t ceil_div2(int64_t a)  { return -floor_div2(-a); }

int32_t gfx_round_clamp_radius(int32_t w, int32_t h, int32_t r)
{
    if (w <= 0 || h <= 0 || r <= 0) return 0;
    int32_t m = (w < h ? w : h) / 2;
    return r < m ? r : m;
}

/* Inclusive pixel span of the shape on `row`. Returns 0 when the row misses the shape. */
static int row_span(int64_t rx, int64_t ry, int64_t w, int64_t h, int64_t r, int64_t row, int64_t *lo, int64_t *hi)
{
    if (w <= 0 || h <= 0 || row < ry || row >= ry + h) return 0;
    int64_t cy;
    if (r > 0 && row < ry + r) cy = ry + r;
    else if (r > 0 && row >= ry + h - r) cy = ry + h - r;
    else { *lo = rx; *hi = rx + w - 1; return 1; }    /* straight part of the sides */
    int64_t dy = 2 * row + 1 - 2 * cy;
    uint64_t rem = (uint64_t)(4 * r * r - dy * dy);   /* > 0: |dy| <= 2r - 1 for every row in the corner band */
    int64_t hw = (int64_t)isqrt64(rem);
    int64_t cxl = rx + r, cxr = rx + w - r;
    int64_t a = ceil_div2(2 * cxl - hw - 1), b = floor_div2(2 * cxr + hw - 1);
    if (a < rx) a = rx;
    if (b > rx + w - 1) b = rx + w - 1;
    if (b < a) return 0;
    *lo = a; *hi = b;
    return 1;
}

static void put_span(const struct gfx_surface *s, int64_t y, int64_t x0, int64_t x1, uint32_t color)
{
    if (x0 < 0) x0 = 0;
    if (x1 > (int64_t)s->width - 1) x1 = (int64_t)s->width - 1;
    if (x1 < x0 || y < 0 || y >= (int64_t)s->height) return;
    uint32_t *p = (uint32_t *)(void *)(s->pixels + (uint64_t)y * s->stride_bytes + (uint64_t)x0 * GFX_BPP_BYTES);
    for (int64_t x = x0; x <= x1; x++) *p++ = color;
}

static void rows_range(const struct gfx_surface *s, int64_t ry, int64_t h, int64_t *y0, int64_t *y1)
{
    *y0 = ry < 0 ? 0 : ry;
    *y1 = ry + h > (int64_t)s->height ? (int64_t)s->height : ry + h;
}

int gfx_fill_round_rect(const struct gfx_surface *s, struct gfx_rect q, int32_t radius, uint32_t color)
{
    int rc = gfx_surface_check(s);
    if (rc) return rc;
    if (q.w <= 0 || q.h <= 0) return GFX_OK;
    int64_t r = gfx_round_clamp_radius(q.w, q.h, radius), y0, y1;
    rows_range(s, q.y, q.h, &y0, &y1);
    for (int64_t y = y0; y < y1; y++) {
        int64_t lo, hi;
        if (row_span(q.x, q.y, q.w, q.h, r, y, &lo, &hi)) put_span(s, y, lo, hi, color);
    }
    return GFX_OK;
}

int gfx_stroke_round_rect(const struct gfx_surface *s, struct gfx_rect q, int32_t radius, int32_t border, uint32_t color)
{
    int rc = gfx_surface_check(s);
    if (rc) return rc;
    if (q.w <= 0 || q.h <= 0 || border <= 0) return GFX_OK;
    int64_t r = gfx_round_clamp_radius(q.w, q.h, radius), b = border;
    int64_t iw = (int64_t)q.w - 2 * b, ih = (int64_t)q.h - 2 * b;
    int64_t ir = r > b ? r - b : 0;
    int64_t y0, y1;
    rows_range(s, q.y, q.h, &y0, &y1);
    for (int64_t y = y0; y < y1; y++) {
        int64_t ol, oh, il, ih2;
        if (!row_span(q.x, q.y, q.w, q.h, r, y, &ol, &oh)) continue;
        if (iw > 0 && ih > 0 && row_span((int64_t)q.x + b, (int64_t)q.y + b, iw, ih, ir, y, &il, &ih2)) {
            put_span(s, y, ol, il - 1, color);
            put_span(s, y, ih2 + 1, oh, color);
        } else {
            put_span(s, y, ol, oh, color);
        }
    }
    return GFX_OK;
}

/*
 * The G3 diagnostic image (1024x768). Drawn by the kernel only in a test build (AURORA_GFX_SHAPES=1) so that the G2 screenshot
 * expectations stay valid for the normal kernel. tests/qemu/gfx_check.py re-renders this same layout with its own independent
 * integer implementation of the coverage rule and compares the screenshot byte for byte.
 */
int gfx_draw_shapes_pattern(const struct gfx_surface *s)
{
    int rc = gfx_surface_check(s);
    if (rc) return rc;
    if (s->width < 1024u || s->height < 768u) return GFX_ESIZE;
    if ((rc = gfx_clear(s, gfx_rgb(16, 24, 40)))) return rc;
    if ((rc = gfx_fill_round_rect(s, (struct gfx_rect){ 64, 64, 400, 240 }, 32, gfx_rgb(240, 240, 240)))) return rc;      /* A */
    if ((rc = gfx_stroke_round_rect(s, (struct gfx_rect){ 512, 64, 448, 240 }, 48, 8, gfx_rgb(255, 128, 0)))) return rc;  /* B: border only */
    if ((rc = gfx_fill_round_rect(s, (struct gfx_rect){ 64, 352, 400, 96 }, 48, gfx_rgb(0, 160, 255)))) return rc;       /* C: pill */
    if ((rc = gfx_fill_round_rect(s, (struct gfx_rect){ 512, 352, 192, 192 }, 96, gfx_rgb(0, 200, 80)))) return rc;      /* D: circle */
    if ((rc = gfx_fill_round_rect(s, (struct gfx_rect){ 736, 352, 224, 192 }, 24, gfx_rgb(60, 60, 60)))) return rc;      /* E: fill + border */
    if ((rc = gfx_stroke_round_rect(s, (struct gfx_rect){ 736, 352, 224, 192 }, 24, 4, gfx_rgb(255, 255, 255)))) return rc;
    if ((rc = gfx_fill_round_rect(s, (struct gfx_rect){ -60, -60, 100, 80 }, 30, gfx_rgb(255, 0, 0)))) return rc;      /* F: clipped, top-left */
    if ((rc = gfx_fill_round_rect(s, (struct gfx_rect){ 944, 688, 160, 120 }, 40, gfx_rgb(255, 0, 255)))) return rc;     /* G: clipped, bottom-right */
    if ((rc = gfx_fill_round_rect(s, (struct gfx_rect){ 64, 496, 120, 64 }, 1000, gfx_rgb(255, 255, 0)))) return rc;     /* H: radius clamped to a pill */
    if ((rc = gfx_fill_round_rect(s, (struct gfx_rect){ 240, 496, 1, 1 }, 5, gfx_rgb(255, 255, 255)))) return rc;        /* tiny shapes */
    if ((rc = gfx_fill_round_rect(s, (struct gfx_rect){ 256, 496, 2, 2 }, 1, gfx_rgb(255, 255, 255)))) return rc;
    if ((rc = gfx_fill_round_rect(s, (struct gfx_rect){ 272, 496, 3, 3 }, 1, gfx_rgb(255, 255, 255)))) return rc;
    if ((rc = gfx_fill_round_rect(s, (struct gfx_rect){ 288, 496, 5, 5 }, 2, gfx_rgb(255, 255, 255)))) return rc;
    if ((rc = gfx_fill_round_rect(s, (struct gfx_rect){ 312, 496, 8, 8 }, 4, gfx_rgb(255, 255, 255)))) return rc;
    if ((rc = gfx_stroke_round_rect(s, (struct gfx_rect){ 64, 600, 300, 100 }, 0, 6, gfx_rgb(200, 200, 40)))) return rc; /* square-cornered border */
    if ((rc = gfx_stroke_round_rect(s, (struct gfx_rect){ 400, 600, 100, 100 }, 50, 50, gfx_rgb(40, 200, 200)))) return rc; /* border thicker than half: solid */
    if ((rc = gfx_stroke_round_rect(s, (struct gfx_rect){ 540, 600, 120, 100 }, 30, 1, gfx_rgb(255, 255, 255)))) return rc; /* 1 px outline */
    return GFX_OK;
}
