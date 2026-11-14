/*
 * kernel/gfx/gfx_draw.c - pure, bounds-checked software rendering into a struct gfx_surface. No hardware access, no floating
 * point, no libc. All size arithmetic is 64-bit and checked; every public function validates its surface first.
 */
#include <gfx.h>

const char *gfx_strerror(int rc)
{
    switch (rc) {
    case GFX_OK: return "ok";
    case GFX_EARG: return "bad argument";
    case GFX_EFORMAT: return "unsupported pixel format";
    case GFX_ESIZE: return "invalid or overflowing size";
    case GFX_ENODEV: return "no display device";
    case GFX_ESTATE: return "graphics not in the right state";
    case GFX_EMEM: return "no suitable memory";
    default: return "unknown graphics error";
    }
}

uint32_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; }

int gfx_buffer_bytes(uint32_t w, uint32_t h, uint32_t stride_bytes, uint64_t *bytes)
{
    if (!bytes || w == 0 || h == 0 || w > GFX_MAX_DIM || h > GFX_MAX_DIM) return GFX_ESIZE;
    uint64_t row = (uint64_t)w * GFX_BPP_BYTES;
    uint64_t stride = stride_bytes ? stride_bytes : row;
    if (stride < row || (stride & 3u)) return GFX_ESIZE;
    *bytes = stride * (h - 1) + row;
    return GFX_OK;
}

int gfx_surface_check(const struct gfx_surface *s)
{
    if (!s || !s->pixels) return GFX_EARG;
    if (((uintptr_t)s->pixels & 3u) != 0) return GFX_EARG;
    if (s->format != GFX_FMT_XRGB8888) return GFX_EFORMAT;
    if (s->stride_bytes == 0) return GFX_ESIZE;
    uint64_t need;
    int rc = gfx_buffer_bytes(s->width, s->height, s->stride_bytes, &need);
    if (rc) return rc;
    if (need > s->size_bytes) return GFX_ESIZE;
    return GFX_OK;
}

int gfx_clip_rect(uint32_t w, uint32_t h, struct gfx_rect r, struct gfx_rect *out)
{
    if (r.w <= 0 || r.h <= 0 || w == 0 || h == 0) return 0;
    int64_t x0 = r.x, y0 = r.y, x1 = (int64_t)r.x + r.w, y1 = (int64_t)r.y + r.h;   /* int64: no overflow for any int32 inputs */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int64_t)w) x1 = w;
    if (y1 > (int64_t)h) y1 = h;
    if (x1 <= x0 || y1 <= y0) return 0;
    if (out) { out->x = (int32_t)x0; out->y = (int32_t)y0; out->w = (int32_t)(x1 - x0); out->h = (int32_t)(y1 - y0); }
    return 1;
}

static uint32_t *row_ptr(const struct gfx_surface *s, uint32_t y, uint32_t x)
{
    return (uint32_t *)(void *)(s->pixels + (uint64_t)y * s->stride_bytes + (uint64_t)x * GFX_BPP_BYTES);
}

int gfx_fill_rect(const struct gfx_surface *s, struct gfx_rect r, uint32_t color)
{
    int rc = gfx_surface_check(s);
    if (rc) return rc;
    struct gfx_rect c;
    if (!gfx_clip_rect(s->width, s->height, r, &c)) return GFX_OK;
    for (int32_t yy = 0; yy < c.h; yy++) {
        uint32_t *p = row_ptr(s, (uint32_t)(c.y + yy), (uint32_t)c.x);
        for (int32_t xx = 0; xx < c.w; xx++) p[xx] = color;
    }
    return GFX_OK;
}

int gfx_clear(const struct gfx_surface *s, uint32_t color)
{
    int rc = gfx_surface_check(s);
    if (rc) return rc;
    struct gfx_rect all = { 0, 0, (int32_t)s->width, (int32_t)s->height };
    return gfx_fill_rect(s, all, color);          /* only the visible columns of each row; padding bytes are left alone */
}

int gfx_set_pixel(const struct gfx_surface *s, int32_t x, int32_t y, uint32_t color)
{
    int rc = gfx_surface_check(s);
    if (rc) return rc;
    if (x < 0 || y < 0 || (uint32_t)x >= s->width || (uint32_t)y >= s->height) return GFX_OK;
    *row_ptr(s, (uint32_t)y, (uint32_t)x) = color;
    return GFX_OK;
}

int gfx_hline(const struct gfx_surface *s, int32_t x, int32_t y, int32_t len, uint32_t color)
{
    struct gfx_rect r = { x, y, len, 1 };
    return gfx_fill_rect(s, r, color);
}

int gfx_vline(const struct gfx_surface *s, int32_t x, int32_t y, int32_t len, uint32_t color)
{
    struct gfx_rect r = { x, y, 1, len };
    return gfx_fill_rect(s, r, color);
}

/*
 * The G2 diagnostic image (1024x768). Every region has a known colour so that a screenshot can prove channel order, row stride,
 * width, height, clipping and fill. The expected pixels are listed in tests/qemu/gfx_check.py and tests/kernel-host/gfx_test.c.
 *   background (32,48,64); panel (224,224,224) at x 128..895, y 96..671; black rules 2 px wide inside the panel;
 *   red/green/blue swatches (pure primaries) at y 128..191; corner markers 32x32: TL red, TR green, BL blue, BR yellow;
 *   then two CLIPPED fills that start off-screen: cyan from (-20,-20) and magenta from (1008,752) with 40x40 sizes.
 */
int gfx_draw_test_pattern(const struct gfx_surface *s)
{
    int rc = gfx_surface_check(s);
    if (rc) return rc;
    if (s->width < 1024u || s->height < 768u) return GFX_ESIZE;
    int32_t W = (int32_t)s->width, H = (int32_t)s->height;
    if ((rc = gfx_clear(s, gfx_rgb(32, 48, 64)))) return rc;
    if ((rc = gfx_fill_rect(s, (struct gfx_rect){ 128, 96, 768, 576 }, gfx_rgb(224, 224, 224)))) return rc;
    if ((rc = gfx_fill_rect(s, (struct gfx_rect){ 128, 383, 768, 2 }, gfx_rgb(0, 0, 0)))) return rc;        /* horizontal rule: y 383..384 */
    if ((rc = gfx_fill_rect(s, (struct gfx_rect){ 511, 96, 2, 576 }, gfx_rgb(0, 0, 0)))) return rc;        /* vertical rule: x 511..512 */
    if ((rc = gfx_fill_rect(s, (struct gfx_rect){ 160, 128, 64, 64 }, gfx_rgb(255, 0, 0)))) return rc;     /* swatches */
    if ((rc = gfx_fill_rect(s, (struct gfx_rect){ 240, 128, 64, 64 }, gfx_rgb(0, 255, 0)))) return rc;
    if ((rc = gfx_fill_rect(s, (struct gfx_rect){ 320, 128, 64, 64 }, gfx_rgb(0, 0, 255)))) return rc;
    if ((rc = gfx_fill_rect(s, (struct gfx_rect){ 0, 0, 32, 32 }, gfx_rgb(255, 0, 0)))) return rc;         /* corner markers on the 1024x768 screen */
    if ((rc = gfx_fill_rect(s, (struct gfx_rect){ 992, 0, 32, 32 }, gfx_rgb(0, 255, 0)))) return rc;
    if ((rc = gfx_fill_rect(s, (struct gfx_rect){ 0, 736, 32, 32 }, gfx_rgb(0, 0, 255)))) return rc;
    if ((rc = gfx_fill_rect(s, (struct gfx_rect){ 992, 736, 32, 32 }, gfx_rgb(255, 255, 0)))) return rc;
    if ((rc = gfx_fill_rect(s, (struct gfx_rect){ -20, -20, 36, 36 }, gfx_rgb(0, 255, 255)))) return rc;   /* clipped: visible (0..15,0..15) */
    if ((rc = gfx_fill_rect(s, (struct gfx_rect){ 1008, 752, 40, 40 }, gfx_rgb(255, 0, 255)))) return rc;  /* clipped: visible (1008..1023,752..767) */
    (void)W; (void)H;
    return GFX_OK;
}
