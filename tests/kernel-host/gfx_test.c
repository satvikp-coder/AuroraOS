/*
 * tests/kernel-host/gfx_test.c - host tests for the pure renderer (kernel/gfx/gfx_draw.c) and the framebuffer placement
 * (kernel/gfx/gfx_region.c). Pixel buffers sit between guard regions filled with a canary pattern, so any out-of-bounds write is
 * detected even when ASan cannot see it (padding bytes inside one allocation).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gfx.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; printf("  PASS: %s\n", name); } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

#define GUARD 256u
#define CAN 0xA5u
struct buf { uint8_t *base; uint8_t *pix; uint64_t len; };
/* one allocation: GUARD canary bytes, the pixel area, GUARD canary bytes. Pixels start as 0xFF (a recognisable "untouched" value). */
static struct buf mk(uint64_t len)
{
    struct buf b; b.len = len; b.base = aligned_alloc(16, (GUARD * 2 + len + 15) & ~15ull);
    memset(b.base, CAN, GUARD * 2 + len); b.pix = b.base + GUARD; memset(b.pix, 0xFF, len); return b;
}
static int guards_ok(const struct buf *b)
{
    for (uint32_t i = 0; i < GUARD; i++) if (b->base[i] != CAN || b->pix[b->len + i] != CAN) return 0;
    return 1;
}
static struct gfx_surface surf(const struct buf *b, uint32_t w, uint32_t h, uint32_t stride)
{ struct gfx_surface s = { b->pix, b->len, w, h, stride, GFX_FMT_XRGB8888 }; return s; }
static uint32_t px(const struct gfx_surface *s, uint32_t x, uint32_t y) { return *(uint32_t *)(void *)(s->pixels + (uint64_t)y * s->stride_bytes + (uint64_t)x * 4); }
static int rgb_is(const struct gfx_surface *s, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t bl) { return px(s, x, y) == gfx_rgb(r, g, bl); }

/* count pixels in the visible area with a value, and bytes in row padding that changed from 0xFF */
static uint64_t count_val(const struct gfx_surface *s, uint32_t v)
{ uint64_t n = 0; for (uint32_t y = 0; y < s->height; y++) for (uint32_t x = 0; x < s->width; x++) if (px(s, x, y) == v) n++; return n; }
static int padding_untouched(const struct gfx_surface *s)
{
    for (uint32_t y = 0; y + 1 < s->height; y++)
        for (uint32_t i = s->width * 4; i < s->stride_bytes; i++) if (s->pixels[(uint64_t)y * s->stride_bytes + i] != 0xFF) return 0;
    return 1;
}

static int busy_set[64]; static int busy_calls;
static int busy_fn(uint64_t page, void *ctx) { (void)ctx; busy_calls++; uint64_t i = (page - 0x40000000ULL) >> 12; return i < 64 && busy_set[i]; }

int main(void)
{
    printf("colour packing and size arithmetic\n");
    CHECK("rgb packs 0x00RRGGBB", gfx_rgb(0x12, 0x34, 0x56) == 0x00123456u);
    CHECK("pure red/green/blue", gfx_rgb(255, 0, 0) == 0xFF0000u && gfx_rgb(0, 255, 0) == 0x00FF00u && gfx_rgb(0, 0, 255) == 0x0000FFu);
    { uint64_t n;
      CHECK("1024x768 tight = 3,145,728", gfx_buffer_bytes(1024, 768, 0, &n) == GFX_OK && n == 3145728ull);
      CHECK("explicit stride 4096 same", gfx_buffer_bytes(1024, 768, 4096, &n) == GFX_OK && n == 3145728ull);
      CHECK("wider stride adds only between rows", gfx_buffer_bytes(10, 4, 64, &n) == GFX_OK && n == 64ull * 3 + 40);
      CHECK("stride below width*4 refused", gfx_buffer_bytes(10, 4, 39, &n) == GFX_ESIZE);
      CHECK("stride not a multiple of 4 refused", gfx_buffer_bytes(10, 4, 42, &n) == GFX_ESIZE);
      CHECK("zero width/height refused", gfx_buffer_bytes(0, 4, 0, &n) == GFX_ESIZE && gfx_buffer_bytes(4, 0, 0, &n) == GFX_ESIZE);
      CHECK("over GFX_MAX_DIM refused", gfx_buffer_bytes(GFX_MAX_DIM + 1, 4, 0, &n) == GFX_ESIZE && gfx_buffer_bytes(4, GFX_MAX_DIM + 1, 0, &n) == GFX_ESIZE);
      CHECK("maximum 8192x8192 fits 64-bit", gfx_buffer_bytes(GFX_MAX_DIM, GFX_MAX_DIM, 0, &n) == GFX_OK && n == 8192ull * 8192 * 4);
      CHECK("huge stride x height does not wrap", gfx_buffer_bytes(8192, 8192, 0xFFFFFFFCu, &n) == GFX_OK && n == 0xFFFFFFFCull * 8191 + 8192ull * 4);
      CHECK("null result pointer refused", gfx_buffer_bytes(4, 4, 0, 0) == GFX_ESIZE); }

    printf("surface validation\n");
    { struct buf b = mk(64 * 4 * 4); struct gfx_surface s = surf(&b, 64, 4, 256);
      CHECK("valid surface accepted", gfx_surface_check(&s) == GFX_OK);
      CHECK("null surface", gfx_surface_check(0) == GFX_EARG);
      struct gfx_surface t = s; t.pixels = 0; CHECK("null pixels", gfx_surface_check(&t) == GFX_EARG);
      t = s; t.pixels = b.pix + 1; CHECK("misaligned pixels", gfx_surface_check(&t) == GFX_EARG);
      t = s; t.format = 99; CHECK("unsupported format", gfx_surface_check(&t) == GFX_EFORMAT);
      t = s; t.stride_bytes = 0; CHECK("zero stride", gfx_surface_check(&t) == GFX_ESIZE);
      t = s; t.stride_bytes = 100; CHECK("stride < width*4", gfx_surface_check(&t) == GFX_ESIZE);
      t = s; t.size_bytes = s.size_bytes - 1; CHECK("buffer one byte too short", gfx_surface_check(&t) == GFX_ESIZE);
      t = s; t.height = 5; CHECK("height larger than the buffer allows", gfx_surface_check(&t) == GFX_ESIZE);
      t = s; t.width = 0; CHECK("zero width", gfx_surface_check(&t) == GFX_ESIZE);
      t = s; t.size_bytes = 256ull * 3 + 64 * 4; CHECK("last row needs only width*4 bytes", gfx_surface_check(&t) == GFX_OK);
      CHECK("drawing on an invalid surface fails and writes nothing", gfx_fill_rect(&(struct gfx_surface){ b.pix, 4, 64, 4, 256, GFX_FMT_XRGB8888 }, (struct gfx_rect){ 0, 0, 5, 5 }, 0) == GFX_ESIZE && guards_ok(&b) && px(&s, 0, 0) == 0xFFFFFFFFu);
      free(b.base); }

    printf("clipping\n");
    { struct gfx_rect o;
      CHECK("inside unchanged", gfx_clip_rect(100, 50, (struct gfx_rect){ 10, 20, 30, 10 }, &o) == 1 && o.x == 10 && o.y == 20 && o.w == 30 && o.h == 10);
      CHECK("negative origin trimmed", gfx_clip_rect(100, 50, (struct gfx_rect){ -5, -7, 20, 20 }, &o) == 1 && o.x == 0 && o.y == 0 && o.w == 15 && o.h == 13);
      CHECK("right/bottom trimmed", gfx_clip_rect(100, 50, (struct gfx_rect){ 90, 45, 20, 20 }, &o) == 1 && o.x == 90 && o.w == 10 && o.y == 45 && o.h == 5);
      CHECK("covering everything", gfx_clip_rect(100, 50, (struct gfx_rect){ -1000, -1000, 5000, 5000 }, &o) == 1 && o.x == 0 && o.y == 0 && o.w == 100 && o.h == 50);
      CHECK("entirely left/above", gfx_clip_rect(100, 50, (struct gfx_rect){ -30, 0, 30, 10 }, &o) == 0 && gfx_clip_rect(100, 50, (struct gfx_rect){ 0, -10, 10, 10 }, &o) == 0);
      CHECK("entirely right/below", gfx_clip_rect(100, 50, (struct gfx_rect){ 100, 0, 10, 10 }, &o) == 0 && gfx_clip_rect(100, 50, (struct gfx_rect){ 0, 50, 10, 10 }, &o) == 0);
      CHECK("zero and negative sizes are empty", gfx_clip_rect(100, 50, (struct gfx_rect){ 5, 5, 0, 5 }, &o) == 0 && gfx_clip_rect(100, 50, (struct gfx_rect){ 5, 5, 5, -1 }, &o) == 0);
      CHECK("INT32_MIN origin with INT32_MAX size does not overflow", gfx_clip_rect(100, 50, (struct gfx_rect){ INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX }, &o) == 0);
      CHECK("INT32_MAX origin is off-screen", gfx_clip_rect(100, 50, (struct gfx_rect){ INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX }, &o) == 0);
      CHECK("x + w overflow of int32 handled", gfx_clip_rect(100, 50, (struct gfx_rect){ 50, 10, INT32_MAX, 5 }, &o) == 1 && o.x == 50 && o.w == 50);
      CHECK("zero-size surface", gfx_clip_rect(0, 50, (struct gfx_rect){ 0, 0, 5, 5 }, &o) == 0);
      CHECK("null out is allowed for a pure test", gfx_clip_rect(100, 50, (struct gfx_rect){ 0, 0, 5, 5 }, 0) == 1); }

    printf("drawing: tight stride\n");
    { struct buf b = mk(16 * 8 * 4); struct gfx_surface s = surf(&b, 16, 8, 64);
      CHECK("clear fills every pixel", gfx_clear(&s, 0x112233) == GFX_OK && count_val(&s, 0x112233) == 128 && guards_ok(&b));
      CHECK("repeated clears are idempotent", gfx_clear(&s, 0x112233) == GFX_OK && gfx_clear(&s, 0x445566) == GFX_OK && count_val(&s, 0x445566) == 128 && guards_ok(&b));
      CHECK("fill inside: exact area", gfx_fill_rect(&s, (struct gfx_rect){ 2, 3, 5, 2 }, 0xABCDEF) == GFX_OK && count_val(&s, 0xABCDEF) == 10);
      CHECK("fill edges exact", px(&s, 2, 3) == 0xABCDEF && px(&s, 6, 4) == 0xABCDEF && px(&s, 1, 3) == 0x445566 && px(&s, 7, 3) == 0x445566 && px(&s, 2, 2) == 0x445566 && px(&s, 2, 5) == 0x445566);
      CHECK("repeated fills (same rect) stay exact", gfx_fill_rect(&s, (struct gfx_rect){ 2, 3, 5, 2 }, 0xABCDEF) == GFX_OK && count_val(&s, 0xABCDEF) == 10 && guards_ok(&b));
      gfx_clear(&s, 0);
      CHECK("zero-size fill draws nothing", gfx_fill_rect(&s, (struct gfx_rect){ 3, 3, 0, 4 }, 1) == GFX_OK && gfx_fill_rect(&s, (struct gfx_rect){ 3, 3, 4, 0 }, 1) == GFX_OK && count_val(&s, 1) == 0);
      CHECK("fully off-screen fills draw nothing", gfx_fill_rect(&s, (struct gfx_rect){ -50, -50, 10, 10 }, 1) == GFX_OK && gfx_fill_rect(&s, (struct gfx_rect){ 16, 0, 10, 10 }, 1) == GFX_OK && gfx_fill_rect(&s, (struct gfx_rect){ 0, 8, 10, 10 }, 1) == GFX_OK && count_val(&s, 1) == 0 && guards_ok(&b));
      CHECK("partially off-screen top-left clips", gfx_fill_rect(&s, (struct gfx_rect){ -3, -3, 6, 6 }, 7) == GFX_OK && count_val(&s, 7) == 9 && px(&s, 0, 0) == 7 && px(&s, 2, 2) == 7 && px(&s, 3, 3) == 0);
      CHECK("partially off-screen bottom-right clips", gfx_fill_rect(&s, (struct gfx_rect){ 14, 6, 10, 10 }, 8) == GFX_OK && count_val(&s, 8) == 4 && px(&s, 15, 7) == 8 && px(&s, 13, 7) == 0);
      CHECK("huge rectangle clips to the surface", gfx_fill_rect(&s, (struct gfx_rect){ INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX }, 9) == GFX_OK && guards_ok(&b));
      CHECK("INT32_MAX-size rectangle at origin fills exactly the surface", gfx_clear(&s, 0) == GFX_OK && gfx_fill_rect(&s, (struct gfx_rect){ 0, 0, INT32_MAX, INT32_MAX }, 9) == GFX_OK && count_val(&s, 9) == 128 && guards_ok(&b));
      gfx_clear(&s, 0);
      CHECK("set_pixel in bounds", gfx_set_pixel(&s, 15, 7, 5) == GFX_OK && px(&s, 15, 7) == 5 && count_val(&s, 5) == 1);
      CHECK("set_pixel out of bounds is a no-op", gfx_set_pixel(&s, -1, 0, 5) == GFX_OK && gfx_set_pixel(&s, 0, -1, 5) == GFX_OK && gfx_set_pixel(&s, 16, 0, 5) == GFX_OK && gfx_set_pixel(&s, 0, 8, 5) == GFX_OK && gfx_set_pixel(&s, INT32_MIN, INT32_MAX, 5) == GFX_OK && count_val(&s, 5) == 1 && guards_ok(&b));
      CHECK("hline", gfx_hline(&s, 2, 1, 4, 3) == GFX_OK && count_val(&s, 3) == 4 && px(&s, 2, 1) == 3 && px(&s, 5, 1) == 3 && px(&s, 6, 1) == 0);
      CHECK("vline", gfx_vline(&s, 10, 2, 3, 4) == GFX_OK && count_val(&s, 4) == 3 && px(&s, 10, 2) == 4 && px(&s, 10, 4) == 4 && px(&s, 10, 5) == 0);
      CHECK("lines clip", gfx_hline(&s, -2, 0, 5, 6) == GFX_OK && count_val(&s, 6) == 3 && gfx_vline(&s, 0, 6, 100, 2) == GFX_OK && count_val(&s, 2) == 2 && guards_ok(&b));
      CHECK("null surface everywhere", gfx_clear(0, 0) == GFX_EARG && gfx_set_pixel(0, 0, 0, 0) == GFX_EARG && gfx_fill_rect(0, (struct gfx_rect){ 0, 0, 1, 1 }, 0) == GFX_EARG);
      free(b.base); }

    printf("drawing: non-standard strides leave padding alone\n");
    { static const uint32_t strides[] = { 64 + 4, 64 + 28, 128, 4096 };
      for (unsigned k = 0; k < sizeof strides / sizeof strides[0]; k++) {
          uint32_t st = strides[k]; uint64_t len = (uint64_t)st * 7 + 64;
          struct buf b = mk(len); struct gfx_surface s = surf(&b, 16, 8, st);
          char name[96];
          int ok = gfx_clear(&s, 0x00FFFFFEu) == GFX_OK && gfx_fill_rect(&s, (struct gfx_rect){ -4, -4, 40, 40 }, 0x00ABCDEFu) == GFX_OK && gfx_hline(&s, 0, 7, 100, 0x123456) == GFX_OK;
          snprintf(name, sizeof name, "stride %u: all %u visible pixels set", st, 16u * 8u);
          CHECK(name, ok && count_val(&s, 0x123456) == 16 && count_val(&s, 0xABCDEF) == 16 * 7);
          snprintf(name, sizeof name, "stride %u: row padding bytes untouched, guards intact", st);
          CHECK(name, padding_untouched(&s) && guards_ok(&b));
          free(b.base);
      } }

    printf("drawing: size limits\n");
    { /* minimum: 1x1 */
      struct buf b = mk(4); struct gfx_surface s = surf(&b, 1, 1, 4);
      CHECK("1x1 surface: fill and clip", gfx_clear(&s, 0x010203) == GFX_OK && px(&s, 0, 0) == 0x010203 && gfx_fill_rect(&s, (struct gfx_rect){ -5, -5, 10, 10 }, 0x040506) == GFX_OK && px(&s, 0, 0) == 0x040506 && guards_ok(&b));
      free(b.base);
      /* maximum width: 8192 x 2 */
      b = mk(8192ull * 4 * 2); s = surf(&b, GFX_MAX_DIM, 2, GFX_MAX_DIM * 4);
      CHECK("8192x2 surface: clear + last pixel", gfx_clear(&s, 0x0A0B0C) == GFX_OK && px(&s, 8191, 1) == 0x0A0B0C && count_val(&s, 0x0A0B0C) == 16384 && guards_ok(&b));
      free(b.base);
      /* maximum height: 2 x 8192 */
      b = mk(8ull * 8192); s = surf(&b, 2, GFX_MAX_DIM, 8);
      CHECK("2x8192 surface: clear + last pixel", gfx_clear(&s, 0x0D0E0F) == GFX_OK && px(&s, 1, 8191) == 0x0D0E0F && count_val(&s, 0x0D0E0F) == 16384 && guards_ok(&b));
      free(b.base);
      /* oversized dimension refused without touching memory */
      b = mk(64); s = surf(&b, GFX_MAX_DIM + 1, 1, (GFX_MAX_DIM + 1) * 4);
      CHECK("width over the cap refused", gfx_clear(&s, 1) == GFX_ESIZE && px(&(struct gfx_surface){ b.pix, 64, 4, 1, 16, GFX_FMT_XRGB8888 }, 0, 0) == 0xFFFFFFFFu && guards_ok(&b));
      free(b.base); }

    printf("test pattern (the same expectations tests/qemu/gfx_check.py applies to the QEMU screenshot)\n");
    { struct buf b = mk(3145728); struct gfx_surface s = surf(&b, 1024, 768, 4096);
      CHECK("draws", gfx_draw_test_pattern(&s) == GFX_OK && guards_ok(&b));
      CHECK("background", rgb_is(&s, 64, 700, 32, 48, 64) && rgb_is(&s, 1000, 400, 32, 48, 64) && rgb_is(&s, 127, 300, 32, 48, 64) && rgb_is(&s, 896, 300, 32, 48, 64) && rgb_is(&s, 300, 95, 32, 48, 64) && rgb_is(&s, 300, 672, 32, 48, 64));
      CHECK("panel edges exact", rgb_is(&s, 128, 96, 224, 224, 224) && rgb_is(&s, 895, 671, 224, 224, 224) && rgb_is(&s, 128, 671, 224, 224, 224) && rgb_is(&s, 895, 96, 224, 224, 224));
      CHECK("panel interior", rgb_is(&s, 200, 300, 224, 224, 224) && rgb_is(&s, 700, 500, 224, 224, 224));
      CHECK("horizontal rule y 383..384", rgb_is(&s, 300, 382, 224, 224, 224) && rgb_is(&s, 300, 383, 0, 0, 0) && rgb_is(&s, 300, 384, 0, 0, 0) && rgb_is(&s, 300, 385, 224, 224, 224) && rgb_is(&s, 128, 383, 0, 0, 0) && rgb_is(&s, 895, 384, 0, 0, 0));
      CHECK("vertical rule x 511..512", rgb_is(&s, 510, 200, 224, 224, 224) && rgb_is(&s, 511, 200, 0, 0, 0) && rgb_is(&s, 512, 200, 0, 0, 0) && rgb_is(&s, 513, 200, 224, 224, 224) && rgb_is(&s, 511, 96, 0, 0, 0) && rgb_is(&s, 512, 671, 0, 0, 0));
      CHECK("swatches (channel order)", rgb_is(&s, 192, 160, 255, 0, 0) && rgb_is(&s, 272, 160, 0, 255, 0) && rgb_is(&s, 352, 160, 0, 0, 255) && rgb_is(&s, 159, 160, 224, 224, 224) && rgb_is(&s, 160, 128, 255, 0, 0) && rgb_is(&s, 223, 191, 255, 0, 0) && rgb_is(&s, 224, 160, 224, 224, 224));
      CHECK("corner markers", rgb_is(&s, 31, 20, 255, 0, 0) && rgb_is(&s, 1000, 5, 0, 255, 0) && rgb_is(&s, 20, 745, 0, 0, 255) && rgb_is(&s, 1000, 745, 255, 255, 0));
      CHECK("marker boundaries", rgb_is(&s, 32, 20, 32, 48, 64) && rgb_is(&s, 20, 32, 32, 48, 64) && rgb_is(&s, 991, 5, 32, 48, 64) && rgb_is(&s, 20, 735, 32, 48, 64) && rgb_is(&s, 991, 745, 32, 48, 64));
      CHECK("clipped cyan from (-20,-20): visible 0..15", rgb_is(&s, 0, 0, 0, 255, 255) && rgb_is(&s, 15, 15, 0, 255, 255) && rgb_is(&s, 16, 16, 255, 0, 0) && rgb_is(&s, 20, 5, 255, 0, 0) && rgb_is(&s, 5, 20, 255, 0, 0));
      CHECK("clipped magenta from (1008,752): visible 1008..1023", rgb_is(&s, 1023, 767, 255, 0, 255) && rgb_is(&s, 1008, 752, 255, 0, 255) && rgb_is(&s, 1007, 760, 255, 255, 0) && rgb_is(&s, 1015, 751, 255, 255, 0));
      CHECK("bottom row and right column reach the corners", rgb_is(&s, 0, 767, 0, 0, 255) && rgb_is(&s, 1023, 0, 0, 255, 0));
      CHECK("an untouched background location far from everything", rgb_is(&s, 600, 740, 32, 48, 64));
      free(b.base);
      b = mk(1000 * 4 * 700); s = surf(&b, 1000, 700, 4000);
      CHECK("surface smaller than 1024x768 refused, nothing written", gfx_draw_test_pattern(&s) == GFX_ESIZE && px(&s, 0, 0) == 0xFFFFFFFFu && guards_ok(&b));
      free(b.base);
      b = mk(1100ull * 4 * 800); s = surf(&b, 1100, 800, 4400);
      CHECK("larger surface: pattern drawn from the origin, rest cleared", gfx_draw_test_pattern(&s) == GFX_OK && rgb_is(&s, 1099, 799, 32, 48, 64) && rgb_is(&s, 1023, 767, 255, 0, 255) && guards_ok(&b));
      free(b.base); }

    printf("framebuffer placement\n");
    { uint64_t out; const uint64_t RB = 0x40000000ULL, RE = RB + 64 * 4096;
      memset(busy_set, 0, sizeof busy_set);
      CHECK("free RAM: highest region at the very top", gfx_region_find(RB, RE, 8 * 4096, busy_fn, 0, &out) == GFX_OK && out == RE - 8 * 4096);
      CHECK("size rounds up to whole pages", gfx_region_find(RB, RE, 8 * 4096 - 5, busy_fn, 0, &out) == GFX_OK && out == RE - 8 * 4096);
      busy_set[63] = 1;
      CHECK("top page busy: region slides below it", gfx_region_find(RB, RE, 8 * 4096, busy_fn, 0, &out) == GFX_OK && out == RB + 55 * 4096);
      busy_set[58] = 1;
      CHECK("busy page in the middle: region ends below it", gfx_region_find(RB, RE, 8 * 4096, busy_fn, 0, &out) == GFX_OK && out == RB + 50 * 4096);
      { int mask_ok = 1; for (int i = 0; i < 8; i++) if (busy_set[50 + i]) mask_ok = 0; CHECK("the chosen region contains no busy page", mask_ok); }
      memset(busy_set, 0, sizeof busy_set); busy_set[0] = 1;
      CHECK("only the very bottom busy: top region still chosen", gfx_region_find(RB, RE, 8 * 4096, busy_fn, 0, &out) == GFX_OK && out == RE - 8 * 4096);
      memset(busy_set, 0, sizeof busy_set); for (int i = 3; i < 64; i += 8) busy_set[i] = 1;
      CHECK("fragmented RAM (a busy page every 8): 8 pages cannot fit", gfx_region_find(RB, RE, 8 * 4096, busy_fn, 0, &out) == GFX_EMEM);
      CHECK("...but 4 pages do, at the top-most gap", gfx_region_find(RB, RE, 4 * 4096, busy_fn, 0, &out) == GFX_OK && out == RB + 60 * 4096);
      memset(busy_set, 0, sizeof busy_set);
      CHECK("exactly the whole RAM", gfx_region_find(RB, RE, 64 * 4096, busy_fn, 0, &out) == GFX_OK && out == RB);
      CHECK("larger than RAM", gfx_region_find(RB, RE, 65 * 4096, busy_fn, 0, &out) == GFX_EMEM);
      busy_set[0] = 1; CHECK("whole-RAM request with one busy page fails", gfx_region_find(RB, RE, 64 * 4096, busy_fn, 0, &out) == GFX_EMEM);
      memset(busy_set, 0, sizeof busy_set);
      CHECK("zero size", gfx_region_find(RB, RE, 0, busy_fn, 0, &out) == GFX_EARG);
      CHECK("unaligned RAM bounds", gfx_region_find(RB + 1, RE, 4096, busy_fn, 0, &out) == GFX_EARG && gfx_region_find(RB, RE + 8, 4096, busy_fn, 0, &out) == GFX_EARG);
      CHECK("empty/inverted RAM", gfx_region_find(RE, RB, 4096, busy_fn, 0, &out) == GFX_EARG && gfx_region_find(RB, RB, 4096, busy_fn, 0, &out) == GFX_EARG);
      CHECK("null callback/out", gfx_region_find(RB, RE, 4096, 0, 0, &out) == GFX_EARG && gfx_region_find(RB, RE, 4096, busy_fn, 0, 0) == GFX_EARG);
      CHECK("size near 2^64 rejected, no wrap", gfx_region_find(RB, RE, ~0ULL, busy_fn, 0, &out) == GFX_EMEM && gfx_region_find(RB, RE, ~0ULL - 100, busy_fn, 0, &out) == GFX_EMEM);
      { const uint64_t HI = 0xFFFFFFFFFFFFF000ULL - 16 * 4096; /* RAM at the very top of the address space: no overflow in the scan */
        CHECK("RAM near the top of the 64-bit space", gfx_region_find(HI, HI + 16 * 4096, 4 * 4096, busy_fn, 0, &out) == GFX_OK && out == HI + 12 * 4096); }
      memset(busy_set, 0, sizeof busy_set); busy_calls = 0;
      gfx_region_find(RB, RE, 8 * 4096, busy_fn, 0, &out);
      CHECK("work is bounded: a free region needs only size/4096 callbacks", busy_calls == 8);
      { /* 256 MiB RAM, the real layout: 3 MiB at the very top */
        const uint64_t R0 = 0x40000000ULL, R1 = R0 + 0x10000000ULL; uint64_t bytes;
        gfx_buffer_bytes(1024, 768, 0, &bytes);
        CHECK("real 256 MiB layout: 0x4FD00000", gfx_region_find(R0, R1, bytes, busy_fn, 0, &out) == GFX_OK && out == 0x4FD00000ULL); } }

    printf("error strings\n");
    { int ok = 1; for (int rc = 0; rc >= -6; rc--) { const char *s = gfx_strerror(rc); if (!s || !*s || !strcmp(s, "unknown graphics error")) ok = 0; }
      CHECK("every code has a message", ok); CHECK("unknown handled", strcmp(gfx_strerror(-42), "unknown graphics error") == 0); }

    printf("\ngfx_test: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
