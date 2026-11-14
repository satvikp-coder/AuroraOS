/*
 * tests/kernel-host/gfx_shape_test.c - host tests for kernel/gfx/gfx_shape.c (rounded rectangles and borders).
 *
 * The renderer works with per-row spans. The ORACLE here is a different algorithm: it asks, for every single pixel, whether the pixel
 * centre is inside the shape (the rule documented in gfx_shape.c), using only integer arithmetic and no square roots. The renderer
 * must match the oracle on every pixel of many shapes, including shapes partly or wholly off-screen, plus hand-drawn ASCII art for
 * small shapes, geometric invariants, and guard canaries around every buffer.
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
static struct buf mk(uint64_t len)
{
    struct buf b; b.len = len; b.base = aligned_alloc(16, (GUARD * 2 + len + 15) & ~15ull);
    memset(b.base, CAN, GUARD * 2 + len); b.pix = b.base + GUARD; memset(b.pix, 0, len); return b;
}
static int guards_ok(const struct buf *b) { for (uint32_t i = 0; i < GUARD; i++) if (b->base[i] != CAN || b->pix[b->len + i] != CAN) return 0; return 1; }
static struct gfx_surface surf(const struct buf *b, uint32_t w, uint32_t h, uint32_t stride) { struct gfx_surface s = { b->pix, b->len, w, h, stride, GFX_FMT_XRGB8888 }; return s; }
static uint32_t px(const struct gfx_surface *s, uint32_t x, uint32_t y) { return *(uint32_t *)(void *)(s->pixels + (uint64_t)y * s->stride_bytes + (uint64_t)x * 4); }

/* ---- the oracle: per-pixel centre-in-shape test, integers only ---- */
static int64_t clampr(int64_t w, int64_t h, int64_t r) { if (w <= 0 || h <= 0 || r <= 0) return 0; int64_t m = (w < h ? w : h) / 2; return r < m ? r : m; }
static int inside(int64_t x, int64_t y, int64_t rx, int64_t ry, int64_t w, int64_t h, int64_t r)
{
    if (w <= 0 || h <= 0 || x < rx || x >= rx + w || y < ry || y >= ry + h) return 0;
    r = clampr(w, h, r);
    if (r == 0) return 1;
    int64_t cx, cy;
    if (x < rx + r) cx = rx + r; else if (x >= rx + w - r) cx = rx + w - r; else return 1;
    if (y < ry + r) cy = ry + r; else if (y >= ry + h - r) cy = ry + h - r; else return 1;
    int64_t dx = 2 * x + 1 - 2 * cx, dy = 2 * y + 1 - 2 * cy;
    return dx * dx + dy * dy <= 4 * r * r;
}
static int in_border(int64_t x, int64_t y, int64_t rx, int64_t ry, int64_t w, int64_t h, int64_t r, int64_t b)
{
    if (b <= 0 || !inside(x, y, rx, ry, w, h, r)) return 0;
    r = clampr(w, h, r);
    int64_t ir = r > b ? r - b : 0;
    return !inside(x, y, rx + b, ry + b, w - 2 * b, h - 2 * b, ir);
}

#define FILL 0x00C0FFEEu
#define BG   0x00101010u
static void bg(const struct gfx_surface *s) { gfx_clear(s, BG); }

/* compare the whole surface with the oracle; returns the number of mismatching pixels */
static long diff_fill(const struct gfx_surface *s, int64_t rx, int64_t ry, int64_t w, int64_t h, int64_t r)
{
    long bad = 0;
    for (uint32_t y = 0; y < s->height; y++) for (uint32_t x = 0; x < s->width; x++)
        if ((px(s, x, y) == FILL) != inside(x, y, rx, ry, w, h, r)) bad++;
    return bad;
}
static long diff_stroke(const struct gfx_surface *s, int64_t rx, int64_t ry, int64_t w, int64_t h, int64_t r, int64_t b)
{
    long bad = 0;
    for (uint32_t y = 0; y < s->height; y++) for (uint32_t x = 0; x < s->width; x++)
        if ((px(s, x, y) == FILL) != in_border(x, y, rx, ry, w, h, r, b)) bad++;
    return bad;
}
static long count(const struct gfx_surface *s, uint32_t v) { long n = 0; for (uint32_t y = 0; y < s->height; y++) for (uint32_t x = 0; x < s->width; x++) if (px(s, x, y) == v) n++; return n; }

/* render rows as ASCII ('#' = FILL) for a surface region */
static int art_is(const struct gfx_surface *s, uint32_t x0, uint32_t y0, const char *const *rows, int n)
{
    for (int j = 0; j < n; j++) for (int i = 0; rows[j][i]; i++)
        if ((px(s, x0 + (uint32_t)i, y0 + (uint32_t)j) == FILL) != (rows[j][i] == '#')) return 0;
    return 1;
}

int main(void)
{
    printf("hand-drawn shapes (checked against ASCII art worked out by hand from the coverage rule)\n");
    { struct buf b = mk(16 * 16 * 4); struct gfx_surface s = surf(&b, 16, 16, 64);
      static const char *const c4[] = { ".##.", "####", "####", ".##." };
      bg(&s); CHECK("4x4 radius 2 is a circle of diameter 4", gfx_fill_round_rect(&s, (struct gfx_rect){ 1, 1, 4, 4 }, 2, FILL) == GFX_OK && art_is(&s, 1, 1, c4, 4) && count(&s, FILL) == 12);
      static const char *const c5[] = { ".###.", "#####", "#####", "#####", ".###." };
      bg(&s); CHECK("5x5 radius 2", gfx_fill_round_rect(&s, (struct gfx_rect){ 2, 2, 5, 5 }, 2, FILL) == GFX_OK && art_is(&s, 2, 2, c5, 5) && count(&s, FILL) == 21);
      static const char *const c8[] = { "..####..", ".######.", "########", "########", "########", "########", ".######.", "..####.." };
      bg(&s); CHECK("8x8 radius 4", gfx_fill_round_rect(&s, (struct gfx_rect){ 3, 3, 8, 8 }, 4, FILL) == GFX_OK && art_is(&s, 3, 3, c8, 8) && count(&s, FILL) == 52);
      bg(&s); CHECK("radius 1 trims nothing on shapes of width/height >= 2", gfx_fill_round_rect(&s, (struct gfx_rect){ 1, 1, 6, 5 }, 1, FILL) == GFX_OK && count(&s, FILL) == 30);
      static const char *const pill[] = { ".########.", "##########", "##########", "##########", ".########." };
      bg(&s); CHECK("10x5 with a huge radius is a pill (radius clamped to 2)", gfx_fill_round_rect(&s, (struct gfx_rect){ 1, 1, 10, 5 }, 1000, FILL) == GFX_OK && art_is(&s, 1, 1, pill, 5));
      static const char *const ring[] = { "..####..", ".######.", "###..###", "##....##", "##....##", "###..###", ".######.", "..####.." };
      bg(&s); CHECK("8x8 radius 4 with a 2 px border (concentric, even thickness)", gfx_stroke_round_rect(&s, (struct gfx_rect){ 3, 3, 8, 8 }, 4, 2, FILL) == GFX_OK && art_is(&s, 3, 3, ring, 8) && guards_ok(&b));
      free(b.base); }

    printf("rectangle special cases\n");
    { struct buf b = mk(20 * 20 * 4); struct gfx_surface s = surf(&b, 20, 20, 80);
      bg(&s); gfx_fill_round_rect(&s, (struct gfx_rect){ 3, 4, 9, 7 }, 0, FILL);
      CHECK("radius 0 equals a plain rectangle", count(&s, FILL) == 63 && px(&s, 3, 4) == FILL && px(&s, 11, 10) == FILL && px(&s, 12, 10) == BG && px(&s, 11, 11) == BG);
      bg(&s); gfx_fill_round_rect(&s, (struct gfx_rect){ 3, 4, 9, 7 }, -5, FILL);
      CHECK("negative radius equals a plain rectangle", count(&s, FILL) == 63);
      bg(&s); CHECK("zero/negative sizes draw nothing", gfx_fill_round_rect(&s, (struct gfx_rect){ 3, 4, 0, 7 }, 3, FILL) == GFX_OK && gfx_fill_round_rect(&s, (struct gfx_rect){ 3, 4, 7, -1 }, 3, FILL) == GFX_OK && gfx_stroke_round_rect(&s, (struct gfx_rect){ 3, 4, 0, 7 }, 3, 1, FILL) == GFX_OK && count(&s, FILL) == 0);
      bg(&s); CHECK("border <= 0 draws nothing", gfx_stroke_round_rect(&s, (struct gfx_rect){ 2, 2, 10, 10 }, 3, 0, FILL) == GFX_OK && gfx_stroke_round_rect(&s, (struct gfx_rect){ 2, 2, 10, 10 }, 3, -4, FILL) == GFX_OK && count(&s, FILL) == 0);
      bg(&s); gfx_stroke_round_rect(&s, (struct gfx_rect){ 2, 2, 10, 8 }, 0, 1, FILL);
      CHECK("square border of width 1 is the outline (2*(10+8)-4 = 32 pixels)", count(&s, FILL) == 32 && px(&s, 2, 2) == FILL && px(&s, 5, 5) == BG);
      bg(&s); gfx_stroke_round_rect(&s, (struct gfx_rect){ 2, 2, 10, 8 }, 3, 4, FILL);
      CHECK("a border thicker than half the shape fills it completely (same as the fill)", count(&s, FILL) > 0);
      long a = count(&s, FILL); bg(&s); gfx_fill_round_rect(&s, (struct gfx_rect){ 2, 2, 10, 8 }, 3, FILL);
      CHECK("...exactly", a == count(&s, FILL));
      free(b.base); }

    printf("every shape matches the per-pixel oracle (fill)\n");
    { struct buf b = mk(40 * 40 * 4); struct gfx_surface s = surf(&b, 40, 40, 160); long bad_total = 0; int shapes = 0;
      for (int w = 1; w <= 24; w++) for (int h = 1; h <= 24; h += (h < 8 ? 1 : 3)) for (int r = 0; r <= 14; r++) {
          bg(&s); gfx_fill_round_rect(&s, (struct gfx_rect){ 5, 7, w, h }, r, FILL); bad_total += diff_fill(&s, 5, 7, w, h, r); shapes++; }
      CHECK("fill: all pixels of all shapes inside the surface match", bad_total == 0 && guards_ok(&b));
      printf("    (%d shapes compared)\n", shapes);
      free(b.base); }

    printf("every shape matches the per-pixel oracle (border)\n");
    { struct buf b = mk(40 * 40 * 4); struct gfx_surface s = surf(&b, 40, 40, 160); long bad_total = 0; int shapes = 0;
      for (int w = 1; w <= 22; w += (w < 6 ? 1 : 2)) for (int h = 1; h <= 22; h += (h < 6 ? 1 : 2)) for (int r = 0; r <= 12; r += 1) for (int bw = 1; bw <= 7; bw++) {
          bg(&s); gfx_stroke_round_rect(&s, (struct gfx_rect){ 6, 5, w, h }, r, bw, FILL); bad_total += diff_stroke(&s, 6, 5, w, h, r, bw); shapes++; }
      CHECK("border: all pixels of all shapes match", bad_total == 0 && guards_ok(&b));
      printf("    (%d shapes compared)\n", shapes);
      free(b.base); }

    printf("clipping: shapes partly or wholly off-screen match the oracle restricted to the surface\n");
    { struct buf b = mk(30 * 24 * 4); struct gfx_surface s = surf(&b, 30, 24, 120); long bad_total = 0; int n = 0;
      static const int xs[] = { -40, -20, -13, -5, -1, 0, 3, 17, 24, 29, 30, 31, 60 }, ys[] = { -30, -12, -7, -1, 0, 5, 16, 22, 23, 24, 25, 50 };
      for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; i++) for (unsigned j = 0; j < sizeof ys / sizeof ys[0]; j++) {
          bg(&s); gfx_fill_round_rect(&s, (struct gfx_rect){ xs[i], ys[j], 26, 18 }, 7, FILL); bad_total += diff_fill(&s, xs[i], ys[j], 26, 18, 7);
          bg(&s); gfx_stroke_round_rect(&s, (struct gfx_rect){ xs[i], ys[j], 26, 18 }, 7, 3, FILL); bad_total += diff_stroke(&s, xs[i], ys[j], 26, 18, 7, 3); n += 2; }
      CHECK("clipped fills and borders match everywhere", bad_total == 0 && guards_ok(&b));
      printf("    (%d placements compared)\n", n);
      bg(&s); gfx_fill_round_rect(&s, (struct gfx_rect){ 100, 100, 20, 20 }, 5, FILL); gfx_fill_round_rect(&s, (struct gfx_rect){ -100, -100, 20, 20 }, 5, FILL);
      CHECK("wholly off-screen shapes change nothing", count(&s, FILL) == 0 && guards_ok(&b));
      free(b.base); }

    printf("non-standard strides leave row padding alone\n");
    { static const uint32_t strides[] = { 40 * 4 + 4, 40 * 4 + 28, 256 };
      for (unsigned k = 0; k < 3; k++) {
          struct buf b = mk((uint64_t)strides[k] * 40); struct gfx_surface s = surf(&b, 40, 40, strides[k]); memset(b.pix, 0xFF, b.len); bg(&s); char name[80];
          gfx_fill_round_rect(&s, (struct gfx_rect){ -3, -3, 50, 50 }, 12, FILL); gfx_stroke_round_rect(&s, (struct gfx_rect){ 2, 2, 36, 36 }, 10, 3, 0x00123456);
          int pad_ok = 1; for (uint32_t y = 0; y + 1 < 40; y++) for (uint32_t i = 160; i < strides[k]; i++) if (b.pix[(uint64_t)y * strides[k] + i] != 0xFF) pad_ok = 0;
          snprintf(name, sizeof name, "stride %u: padding untouched, guards intact", strides[k]); CHECK(name, pad_ok && guards_ok(&b));
          free(b.base); } }

    printf("geometric invariants\n");
    { struct buf b = mk(64 * 64 * 4); struct gfx_surface s = surf(&b, 64, 64, 256); int sym = 1, mono = 1, part = 1, nest = 1;
      for (int w = 2; w <= 30; w += 3) for (int h = 2; h <= 30; h += 4) for (int r = 0; r <= 16; r++) {
          bg(&s); gfx_fill_round_rect(&s, (struct gfx_rect){ 10, 10, w, h }, r, FILL);
          for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {   /* mirror symmetry in x and y */
              uint32_t a = px(&s, 10 + x, 10 + y);
              if (a != px(&s, 10 + w - 1 - x, 10 + y) || a != px(&s, 10 + x, 10 + h - 1 - y)) sym = 0; }
          long a0 = count(&s, FILL);
          bg(&s); gfx_fill_round_rect(&s, (struct gfx_rect){ 10, 10, w, h }, r + 1, FILL);
          if (count(&s, FILL) > a0) mono = 0;                          /* a bigger radius never covers more */
          /* border + inner fill = outer fill, no overlap */
          int bw = 1 + (r % 4); bg(&s);
          gfx_stroke_round_rect(&s, (struct gfx_rect){ 10, 10, w, h }, r, bw, 0x00AA0000u);
          gfx_fill_round_rect(&s, (struct gfx_rect){ 10 + bw, 10 + bw, w - 2 * bw, h - 2 * bw }, r > bw ? r - bw : 0, 0x0000BB00u);
          for (uint32_t y = 0; y < 64; y++) for (uint32_t x = 0; x < 64; x++) {
              int out = inside(x, y, 10, 10, w, h, r); uint32_t v = px(&s, x, y);
              if (out != (v == 0x00AA0000u || v == 0x0000BB00u)) part = 0; }
          /* nesting: the inner shape is inside the outer shape */
          for (uint32_t y = 0; y < 64; y++) for (uint32_t x = 0; x < 64; x++)
              if (inside(x, y, 10 + bw, 10 + bw, w - 2 * bw, h - 2 * bw, r > bw ? r - bw : 0) && !inside(x, y, 10, 10, w, h, r)) nest = 0; }
      CHECK("every shape is mirror-symmetric in x and y", sym);
      CHECK("increasing the radius never adds pixels", mono);
      CHECK("border + inner fill == outer fill, with no overlap and no gap", part);
      CHECK("the inset shape always lies inside the outer shape", nest);
      free(b.base); }
    { struct buf b = mk(64 * 64 * 4); struct gfx_surface s = surf(&b, 64, 64, 256);
      bg(&s); gfx_fill_round_rect(&s, (struct gfx_rect){ 8, 8, 40, 40 }, 20, FILL);
      CHECK("a square with radius w/2 is a circle: symmetric under 90 degree rotation", ({ int ok = 1; for (int y = 0; y < 40; y++) for (int x = 0; x < 40; x++) if (px(&s, 8 + x, 8 + y) != px(&s, 8 + y, 8 + x)) ok = 0; ok; }));
      CHECK("circle diameter 40 area is within 1% of pi*r^2 (about 1257)", count(&s, FILL) > 1244 && count(&s, FILL) < 1270);
      CHECK("circle touches the middle of all four sides and misses the corners", px(&s, 8 + 20, 8) == FILL && px(&s, 8 + 20, 8 + 39) == FILL && px(&s, 8, 8 + 20) == FILL && px(&s, 8 + 39, 8 + 20) == FILL && px(&s, 8, 8) == BG && px(&s, 8 + 39, 8 + 39) == BG);
      free(b.base); }

    printf("extreme values (UBSan would trap on any overflow)\n");
    { struct buf b = mk(32 * 32 * 4); struct gfx_surface s = surf(&b, 32, 32, 128);
      bg(&s);
      CHECK("INT32_MAX size at the origin with a huge radius: the corner circle is ~1e9 wide, so nothing of it reaches this surface",
            gfx_fill_round_rect(&s, (struct gfx_rect){ 0, 0, INT32_MAX, INT32_MAX }, INT32_MAX, FILL) == GFX_OK && guards_ok(&b) && count(&s, FILL) == 0);
      bg(&s);
      CHECK("INT32_MIN origin with INT32_MAX size and radius: well-defined, in bounds", gfx_fill_round_rect(&s, (struct gfx_rect){ INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX }, INT32_MAX, FILL) == GFX_OK && guards_ok(&b));
      CHECK("INT32_MAX origin: nothing drawn", gfx_fill_round_rect(&s, (struct gfx_rect){ INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX }, 9, FILL) == GFX_OK && guards_ok(&b));
      bg(&s);
      CHECK("huge border on a huge shape is bounded", gfx_stroke_round_rect(&s, (struct gfx_rect){ -1000000, -1000000, 2000000, 2000000 }, 500000, INT32_MAX, FILL) == GFX_OK && guards_ok(&b));
      bg(&s);
      CHECK("a 2-billion-pixel-wide pill clipped to 32 px: still rounded far away, in bounds", gfx_fill_round_rect(&s, (struct gfx_rect){ -1000000000, 4, 2000000000, 20 }, 10, FILL) == GFX_OK && guards_ok(&b) && px(&s, 15, 14) == FILL && px(&s, 15, 3) == BG);
      CHECK("clamp helper", gfx_round_clamp_radius(10, 6, 99) == 3 && gfx_round_clamp_radius(10, 6, -1) == 0 && gfx_round_clamp_radius(0, 6, 4) == 0 && gfx_round_clamp_radius(INT32_MAX, INT32_MAX, INT32_MAX) == INT32_MAX / 2 && gfx_round_clamp_radius(7, 7, 2) == 2);
      free(b.base);
      b = mk(8192ull * 4 * 3); s = surf(&b, 8192, 3, 8192 * 4);
      bg(&s); CHECK("8192-wide surface: wide rounded rectangle clipped to 3 rows", gfx_fill_round_rect(&s, (struct gfx_rect){ 0, -4, 8192, 20 }, 10, FILL) == GFX_OK && guards_ok(&b) && px(&s, 4096, 1) == FILL);
      free(b.base); }

    printf("invalid surfaces\n");
    { struct buf b = mk(64); struct gfx_surface s = surf(&b, 4, 4, 16);
      CHECK("null surface", gfx_fill_round_rect(0, (struct gfx_rect){ 0, 0, 2, 2 }, 1, FILL) == GFX_EARG && gfx_stroke_round_rect(0, (struct gfx_rect){ 0, 0, 2, 2 }, 1, 1, FILL) == GFX_EARG);
      s.size_bytes = 8; CHECK("short buffer refused, nothing written", gfx_fill_round_rect(&s, (struct gfx_rect){ 0, 0, 4, 4 }, 1, FILL) == GFX_ESIZE && gfx_stroke_round_rect(&s, (struct gfx_rect){ 0, 0, 4, 4 }, 1, 1, FILL) == GFX_ESIZE && px(&(struct gfx_surface){ b.pix, 64, 4, 4, 16, GFX_FMT_XRGB8888 }, 0, 0) == 0 && guards_ok(&b));
      s = surf(&b, 4, 4, 16); s.format = 7; CHECK("unsupported format refused", gfx_fill_round_rect(&s, (struct gfx_rect){ 0, 0, 4, 4 }, 1, FILL) == GFX_EFORMAT);
      free(b.base); }

    printf("the G3 diagnostic image\n");
    { struct buf b = mk(3145728); struct gfx_surface s = surf(&b, 1024, 768, 4096);
      CHECK("draws on a 1024x768 surface", gfx_draw_shapes_pattern(&s) == GFX_OK && guards_ok(&b));
      CHECK("background pixel in the corner is untouched", px(&s, 1000, 20) == gfx_rgb(16, 24, 40) && px(&s, 20, 740) == gfx_rgb(16, 24, 40));
      CHECK("shape A fill: centre and a rounded-off corner", px(&s, 264, 184) == gfx_rgb(240, 240, 240) && px(&s, 64, 64) == gfx_rgb(16, 24, 40) && px(&s, 64 + 32, 64) == gfx_rgb(240, 240, 240));
      CHECK("shape B is border-only: middle is background, rim is orange", px(&s, 736, 184) == gfx_rgb(16, 24, 40) && px(&s, 516, 184) == gfx_rgb(255, 128, 0) && px(&s, 520, 184) == gfx_rgb(16, 24, 40));
      CHECK("clipped shape F covers (0,0) but its rounded bottom-right corner (39,19) is left as background", px(&s, 30, 10) == gfx_rgb(255, 0, 0) && px(&s, 0, 0) == gfx_rgb(255, 0, 0) && px(&s, 39, 19) == gfx_rgb(16, 24, 40) && px(&s, 40, 5) == gfx_rgb(16, 24, 40));
      CHECK("tiny 1x1 shape draws one pixel", px(&s, 240, 496) == gfx_rgb(255, 255, 255) && px(&s, 241, 496) == gfx_rgb(16, 24, 40));
      free(b.base);
      b = mk(1000 * 4 * 700); s = surf(&b, 1000, 700, 4000);
      CHECK("surface smaller than 1024x768 refused", gfx_draw_shapes_pattern(&s) == GFX_ESIZE && px(&s, 0, 0) == 0 && guards_ok(&b));
      free(b.base); }

    printf("\ngfx_shape_test: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
