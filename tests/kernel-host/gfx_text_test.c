/*
 * tests/kernel-host/gfx_text_test.c - host tests for the bitmap font data (kernel/gfx/font_noto_mono.c) and the text engine
 * (kernel/gfx/gfx_text.c): data integrity and provenance, UTF-8 decoding, measuring, word wrapping, clipped drawing.
 * Built with ASan + UBSan. Drawing is verified with a METAMORPHIC oracle: a text drawn with clipping must equal the same pixels
 * cropped out of the same text drawn on a canvas big enough that nothing is clipped; the unclipped rendering is itself checked
 * against the glyph table pixel by pixel.
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

#define INK 0x00112233u
#define BG  0x00FFFFFFu

static const struct gfx_font *F(int w, int s) { const struct gfx_font *f = gfx_font_find(w, s); if (!f) { printf("missing font %d/%d\n", w, s); exit(2); } return f; }
static int glyph_bit(const struct gfx_font *f, uint32_t cp, int col, int row)
{
    const struct gfx_glyph *g = &f->glyphs[cp - f->first];
    uint32_t stride = ((uint32_t)g->w + 7u) / 8u;
    return (f->bits[g->off + (uint32_t)row * stride + (uint32_t)(col >> 3)] >> (7 - (col & 7))) & 1;
}
static long count(const struct gfx_surface *s, uint32_t v) { long n = 0; for (uint32_t y = 0; y < s->height; y++) for (uint32_t x = 0; x < s->width; x++) if (px(s, x, y) == v) n++; return n; }

/* collect wrap result as "line|line|..." */
static void wrapstr(const struct gfx_font *f, const char *s, int cols, char *out, size_t outsz, size_t *total)
{
    struct gfx_text_line L[64]; size_t t = 0;
    int rc = gfx_text_wrap(f, s, strlen(s), cols * f->advance, L, 64, &t);
    out[0] = 0; if (total) *total = t;
    if (rc) { snprintf(out, outsz, "<rc %d>", rc); return; }
    for (size_t i = 0; i < t && i < 64; i++) {
        char tmp[200]; snprintf(tmp, sizeof tmp, "%s%.*s", i ? "|" : "", (int)L[i].len, s + L[i].start); strncat(out, tmp, outsz - strlen(out) - 1);
    }
}
#define WRAP(txt, cols, expect) do { char o_[512]; size_t t_; wrapstr(f16, txt, cols, o_, sizeof o_, &t_); \
    if (strcmp(o_, expect) == 0) { passed++; printf("  PASS: wrap %d cols %s -> %s\n", cols, #txt, expect); } else { failed++; printf("  FAIL: wrap %d cols %s: got '%s' expected '%s' (line %d)\n", cols, #txt, o_, expect, __LINE__); } } while (0)

static void decode(const char *s, size_t n, uint32_t *cp, size_t *len) { size_t i = 0; gfx_utf8_next(s, n, &i, cp); *len = i; }
#define UTF(bytes, n, expcp, explen, name) do { uint32_t c_; size_t l_; decode(bytes, n, &c_, &l_); CHECK(name, c_ == (expcp) && l_ == (explen)); } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

int main(void)
{
    const struct gfx_font *f16 = F(GFX_FONT_REGULAR, 16);

    printf("font data: integrity\n");
    { int ok_count = gfx_font_count == 10, ok_meta = 1, ok_glyph = 1, ok_blank = 1, ok_nonblank = 1, ok_bounds = 1, ok_unique = 1, ok_adv = 1;
      static const int sizes[5] = { 12, 14, 16, 18, 24 };
      for (int i = 0; i < gfx_font_count; i++) {
          const struct gfx_font *f = gfx_fonts[i];
          if (!f->name || strcmp(f->name, "Noto Sans Mono") || f->first != 0x20 || f->last != 0x7E || f->advance == 0 || f->ascent == 0 || f->line_height < f->ascent + f->descent) ok_meta = 0;
          if (f->weight != 400 && f->weight != 700) ok_meta = 0;
          for (int j = i + 1; j < gfx_font_count; j++) if (gfx_fonts[j]->weight == f->weight && gfx_fonts[j]->size == f->size) ok_unique = 0;
          if (f->advance != (uint8_t)((600 * f->size + 500) / 1000)) ok_adv = 0;        /* 0.6 em, rounded */
          for (uint32_t c = f->first; c <= f->last; c++) {
              const struct gfx_glyph *g = &f->glyphs[c - f->first];
              uint32_t stride = ((uint32_t)g->w + 7u) / 8u;
              if ((uint64_t)g->off + (uint64_t)stride * g->h > f->bits_len) ok_glyph = 0;
              if (c == 0x20 && (g->w || g->h)) ok_blank = 0;
              if (c != 0x20) {
                  long on = 0; for (int r = 0; r < g->h; r++) for (int k = 0; k < g->w; k++) on += glyph_bit(f, c, k, r);
                  if (on == 0) ok_nonblank = 0;
                  if (g->xoff < -1 || g->xoff + g->w > f->advance + 1 || g->top > f->ascent || g->top - g->h < -f->descent) ok_bounds = 0;
                  /* the box is tight: the first and last row and the first and last column all hold ink (interior blank rows are legitimate: i, j, :, ;, !) */
                  int top_row = 0, bot_row = 0, left_col = 0, right_col = 0;
                  for (int k = 0; k < g->w; k++) { top_row |= glyph_bit(f, c, k, 0); bot_row |= glyph_bit(f, c, k, g->h - 1); }
                  for (int r = 0; r < g->h; r++) { left_col |= glyph_bit(f, c, 0, r); right_col |= glyph_bit(f, c, g->w - 1, r); }
                  if (!top_row || !bot_row || !left_col || !right_col) ok_bounds = 0;
              }
          }
      }
      for (int w = 0; w < 2; w++) for (int k = 0; k < 5; k++) if (!gfx_font_find(w ? 700 : 400, sizes[k])) ok_count = 0;
      CHECK("ten faces: regular and bold at 12, 14, 16, 18 and 24", ok_count);
      CHECK("metadata: name, ASCII range, advance is 0.6 em rounded, line height covers ascent + descent", ok_meta && ok_adv);
      CHECK("every (weight, size) pair is unique", ok_unique);
      CHECK("every glyph's bitmap lies inside the face's bit array", ok_glyph);
      CHECK("space is blank, every other glyph has ink", ok_blank && ok_nonblank);
      CHECK("glyph boxes sit inside the cell (+-1 px) and between ascent and descent, and the box is tight", ok_bounds);
      CHECK("lookup of a missing face is NULL", gfx_font_find(400, 13) == 0 && gfx_font_find(500, 16) == 0);
      CHECK("letter shapes differ sensibly: W is at least as wide as i, and the full stop is shorter than A", f16->glyphs['W' - 0x20].w >= f16->glyphs['i' - 0x20].w && f16->glyphs['.' - 0x20].h < f16->glyphs['A' - 0x20].h);
    }
    { /* provenance: the committed generated file must carry the pinned hashes and licence notices */
      FILE *fp = fopen("kernel/gfx/font_noto_mono.c", "rb"); static char hdr[4096]; size_t n = 0; if (fp) { n = fread(hdr, 1, sizeof hdr - 1, fp); fclose(fp); } hdr[n] = 0;
      CHECK("generated file header: regular font SHA-256 pinned", strstr(hdr, "74fd536351d0f30a73410e1bd223a0ebf763dd4808eb844aa2aed18c0d6e3c84") != 0);
      CHECK("generated file header: bold font SHA-256 pinned", strstr(hdr, "4d063be01a00694bf7183d6799d5611760121dcfd526f8b07280ba79115ce02a") != 0);
      CHECK("generated file header: copyright, SIL OFL 1.1, modified version, no Reserved Font Name", strstr(hdr, "Copyright 2022 The Noto Project Authors") && strstr(hdr, "SIL Open Font License, Version 1.1") && strstr(hdr, "MODIFIED VERSION") && strstr(hdr, "No Reserved Font Name"));
      CHECK("generated file header: upstream release tag", strstr(hdr, "NotoSansMono-v2.014") != 0);
      FILE *ofl = fopen("third_party/fonts/NotoSansMono/OFL.txt", "rb"); static char lic[8192]; size_t ln = 0; if (ofl) { ln = fread(lic, 1, sizeof lic - 1, ofl); fclose(ofl); } lic[ln] = 0;
      CHECK("third_party OFL.txt present with the upstream copyright line and OFL 1.1", ln > 4000 && strstr(lic, "Copyright 2022 The Noto Project Authors") && strstr(lic, "SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007"));
      FILE *rd = fopen("third_party/fonts/NotoSansMono/README.md", "rb"); CHECK("third_party README.md (provenance) present", rd != 0); if (rd) fclose(rd);
      FILE *au = fopen("third_party/fonts/NotoSansMono/AUTHORS.txt", "rb"); CHECK("third_party AUTHORS.txt present", au != 0); if (au) fclose(au); }

    printf("UTF-8 decoding\n");
    UTF("A", 1, 'A', 1, "ASCII");
    UTF("\xC3\xA9", 2, 0xE9, 2, "two bytes: U+00E9");
    UTF("\xE0\xA4\xB9", 3, 0x939, 3, "three bytes: Devanagari HA U+0939");
    UTF("\xE0\xAA\x97", 3, 0xA97, 3, "three bytes: Gujarati GA U+0A97");
    UTF("\xF0\x9F\x98\x80", 4, 0x1F600, 4, "four bytes: U+1F600");
    UTF("\xF4\x8F\xBF\xBF", 4, 0x10FFFF, 4, "highest code point U+10FFFF");
    UTF("\xED\x9F\xBF", 3, 0xD7FF, 3, "U+D7FF just below the surrogates");
    UTF("\xEE\x80\x80", 3, 0xE000, 3, "U+E000 just above the surrogates");
    UTF("\x80", 1, 0xFFFD, 1, "lone continuation byte is invalid (1 byte)");
    UTF("\xC0\x80", 2, 0xFFFD, 1, "overlong C0 80 is invalid (1 byte)");
    UTF("\xC1\xBF", 2, 0xFFFD, 1, "overlong C1 BF is invalid");
    UTF("\xE0\x80\x80", 3, 0xFFFD, 1, "overlong 3-byte E0 80 80 is invalid");
    UTF("\xF0\x80\x80\x80", 4, 0xFFFD, 1, "overlong 4-byte F0 80 80 80 is invalid");
    UTF("\xED\xA0\x80", 3, 0xFFFD, 1, "surrogate U+D800 is invalid");
    UTF("\xED\xBF\xBF", 3, 0xFFFD, 1, "surrogate U+DFFF is invalid");
    UTF("\xF4\x90\x80\x80", 4, 0xFFFD, 1, "above U+10FFFF is invalid");
    UTF("\xF5\x80\x80\x80", 4, 0xFFFD, 1, "lead byte F5 is invalid");
    UTF("\xFF", 1, 0xFFFD, 1, "byte FF is invalid");
    UTF("\xE0\xA4", 2, 0xFFFD, 1, "truncated 3-byte sequence is invalid");
    UTF("\xC3", 1, 0xFFFD, 1, "truncated 2-byte sequence at the end is invalid");
    UTF("\xC3\x28", 2, 0xFFFD, 1, "bad continuation byte is invalid and only the lead byte is consumed");
    { int ok = 1; for (int t = 0; t < 200000; t++) {
          char b[8]; size_t n = 1 + rnd() % 6; for (size_t k = 0; k < n; k++) b[k] = (char)(rnd() & 0xFF);
          size_t i = 0; while (i < n) { size_t before = i; uint32_t cp; gfx_utf8_next(b, n, &i, &cp);
              if (i <= before || i > n || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) ok = 0; } }
      CHECK("200,000 random byte strings: always progresses, never overruns, never yields a surrogate or a value above U+10FFFF", ok); }

    printf("measuring\n");
    { int32_t w, h; const char *t = "abc\nde\n\nlong line here";
      CHECK("empty text is 0x0", gfx_text_measure(f16, "", 0, &w, &h) == GFX_OK && w == 0 && h == 0);
      CHECK("one cell", gfx_text_measure(f16, "x", 1, &w, &h) == GFX_OK && w == f16->advance && h == f16->line_height);
      CHECK("lines: widest line and 4 lines (a blank one counts)", gfx_text_measure(f16, t, strlen(t), &w, &h) == GFX_OK && w == 14 * f16->advance && h == 4 * f16->line_height);
      CHECK("a trailing newline adds no line", gfx_text_measure(f16, "ab\n", 3, &w, &h) == GFX_OK && h == f16->line_height);
      CHECK("a lone newline is one empty line", gfx_text_measure(f16, "\n", 1, &w, &h) == GFX_OK && w == 0 && h == f16->line_height);
      CHECK("non-ASCII is one cell per code point (Devanagari HA = 1)", gfx_text_measure(f16, "\xE0\xA4\xB9", 3, &w, &h) == GFX_OK && w == f16->advance);
      CHECK("invalid bytes are one cell each", gfx_text_measure(f16, "\xFF\xFF\xC3", 3, &w, &h) == GFX_OK && w == 3 * f16->advance);
      CHECK("bad arguments", gfx_text_measure(0, "a", 1, &w, &h) == GFX_EARG && gfx_text_measure(f16, 0, 1, &w, &h) == GFX_EARG && gfx_text_measure(f16, "a", 1, 0, &h) == GFX_EARG);
      CHECK("input over the 1 MiB cap refused", gfx_text_measure(f16, "a", GFX_TEXT_MAX_BYTES + 1, &w, &h) == GFX_ESIZE); }

    printf("word wrapping (hand-worked cases)\n");
    WRAP("hello world", 5, "hello|world");
    WRAP("hello world", 11, "hello world");
    WRAP("hello world", 10, "hello|world");
    WRAP("hello  world", 5, "hello|world");
    WRAP("hello   world", 7, "hello|world");
    WRAP("abcdefghij", 4, "abcd|efgh|ij");
    WRAP("ab cdefghij", 4, "ab|cdef|ghij");
    WRAP("  abcdef", 4, "  ab|cdef");
    WRAP("a b c d e", 3, "a b|c d|e");
    WRAP("a\nb", 10, "a|b");
    WRAP("a\n\nb", 10, "a||b");
    WRAP("a\n", 10, "a");
    WRAP("\n", 10, "");
    WRAP("", 10, "");
    WRAP("abc \ndef", 3, "abc|def");
    WRAP("abc  \n\ndef", 3, "abc||def");
    WRAP("ab   ", 10, "ab");
    WRAP("abc", 0, "a|b|c");
    WRAP("the quick brown fox", 9, "the quick|brown fox");
    WRAP("the quick brown fox", 8, "the|quick|brown|fox");
    WRAP("the quick brown fox", 20, "the quick brown fox");
    { struct gfx_text_line L[2]; size_t t = 99;
      CHECK("max_out smaller than the total: total still counted, only 2 recorded", gfx_text_wrap(f16, "a b c d", 7, 10, L, 2, &t) == GFX_OK && t == 4 && L[0].len == 1 && L[1].start == 2);
      t = 99; CHECK("counting only (NULL out, max_out 0)", gfx_text_wrap(f16, "a b c d", 7, 10, 0, 0, &t) == GFX_OK && t == 4);
      CHECK("bad wrap arguments", gfx_text_wrap(0, "a", 1, 10, L, 2, &t) == GFX_EARG && gfx_text_wrap(f16, "a", 1, 10, L, 2, 0) == GFX_EARG && gfx_text_wrap(f16, "a", 1, 10, 0, 2, &t) == GFX_EARG && gfx_text_wrap(f16, "a", GFX_TEXT_MAX_BYTES + 1, 10, L, 2, &t) == GFX_ESIZE);
      struct gfx_text_line two[2]; t = 0;
      CHECK("cells count code points, not bytes: ab<space><Devanagari>z in 3 columns", gfx_text_wrap(f16, "ab \xE0\xA4\xB9z", 7, 30, two, 2, &t) == GFX_OK && t == 2 && two[0].len == 2 && two[0].cells == 2 && two[1].start == 3 && two[1].len == 4 && two[1].cells == 2); }

    printf("word wrapping (invariants on 20,000 random texts)\n");
    { static const char alpha[] = "ab cd  e\nfghij k"; int ok_cells = 1, ok_text = 1, ok_trim = 1, ok_total = 1;
      for (int t = 0; t < 20000; t++) {
          char s[80]; size_t n = rnd() % 70; for (size_t i = 0; i < n; i++) s[i] = alpha[rnd() % (sizeof alpha - 1)];
          int cols = 1 + (int)(rnd() % 9);
          struct gfx_text_line L[80]; size_t total = 0;
          if (gfx_text_wrap(f16, s, n, cols * f16->advance, L, 80, &total) != GFX_OK) { ok_total = 0; continue; }
          char cat[200]; size_t cn = 0, np = 0;
          for (size_t i = 0; i < total; i++) {
              if (L[i].cells > (uint32_t)cols || L[i].cells != L[i].len) ok_cells = 0;           /* ASCII: cells == bytes */
              if (L[i].len && s[L[i].start + L[i].len - 1] == ' ') ok_trim = 0;
              for (size_t k = 0; k < L[i].len; k++) if (s[L[i].start + k] != ' ') cat[cn++] = s[L[i].start + k];
          }
          for (size_t i = 0; i < n; i++) if (s[i] != ' ' && s[i] != '\n') { if (np >= cn || cat[np] != s[i]) ok_text = 0; np++; }
          if (np != cn) ok_text = 0;
          int32_t w, h; (void)w; (void)h;
      }
      CHECK("no line exceeds the box width, and each line's cell count equals its bytes", ok_cells);
      CHECK("no line ends with a space", ok_trim);
      CHECK("all visible characters survive wrapping, in order, none added", ok_text);
      CHECK("wrapping never fails on valid input", ok_total); }

    printf("drawing: glyph pixels\n");
    { struct buf b = mk(64 * 48 * 4); struct gfx_surface s = surf(&b, 64, 48, 256);
      gfx_clear(&s, BG);
      const struct gfx_font *f = f16; CHECK("draws 'A'", gfx_draw_text(&s, f, 5, 3, "A", 1, INK, 0) == GFX_OK && guards_ok(&b));
      const struct gfx_glyph *g = &f->glyphs['A' - 0x20]; int ok = 1;
      for (int y = 0; y < 48; y++) for (int x = 0; x < 64; x++) {
          int gx = x - (5 + g->xoff), gy = y - (3 + f->ascent - g->top);
          int on = (gx >= 0 && gx < g->w && gy >= 0 && gy < g->h) ? glyph_bit(f, 'A', gx, gy) : 0;
          if ((px(&s, x, y) == INK) != on) ok = 0; }
      CHECK("every pixel equals the glyph bitmap placed at pen + xoff, baseline - top", ok);
      CHECK("the baseline is y + ascent: the lowest ink row of 'A' is on the baseline - 1", ({ int low = -1; for (int y = 0; y < 48; y++) for (int x = 0; x < 64; x++) if (px(&s, x, y) == INK) low = y; low == 3 + f->ascent - 1; }));
      gfx_clear(&s, BG); gfx_draw_text(&s, f, 2, 2, "  ", 2, INK, 0); CHECK("spaces draw nothing", count(&s, INK) == 0);
      gfx_clear(&s, BG); gfx_draw_text(&s, f, 2, 2, "ab\ncd", 5, INK, 0);
      { long a = count(&s, INK); gfx_clear(&s, BG); gfx_draw_text(&s, f, 2, 2, "ab", 2, INK, 0); CHECK("drawing stops at the first newline", a == count(&s, INK) && a > 0); }
      gfx_clear(&s, BG); gfx_draw_text(&s, f, 0, 0, "\x01", 1, INK, 0);
      { int64_t bh = f->ascent * 3 / 4; if (bh < 5) bh = 5; long expect = 2 * (f->advance - 2 - 1 + 1) + 2 * (bh - 2);
        CHECK("a missing glyph is a hollow box of the documented size", count(&s, INK) == expect && px(&s, 1, f->ascent - bh) == INK && px(&s, f->advance - 2, f->ascent - 1) == INK && px(&s, 3, f->ascent - bh + 2) == BG); }
      gfx_clear(&s, BG); gfx_draw_text(&s, f, 0, 0, "\xE0\xA4\xB9", 3, INK, 0); { long a = count(&s, INK); gfx_clear(&s, BG); gfx_draw_text(&s, f, 0, 0, "\x7F", 1, INK, 0); CHECK("Devanagari draws the same single box as DEL (no shaping, one cell)", a == count(&s, INK) && a > 0); }
      free(b.base); }

    printf("drawing: clipping equals the crop of an unclipped rendering\n");
    { const char *txts[] = { "Hello, World!", "WWWW iiii @@@@", "x", "A\xE0\xA4\xB9z\x01Q", "multi\nline" };
      int ok = 1, cnt = 0, nonblank = 0;
      for (int wt = 0; wt < 2; wt++) for (int sz = 12; sz <= 24; sz += (sz < 18 ? 2 : 6)) {
          const struct gfx_font *f = gfx_font_find(wt ? 700 : 400, sz); if (!f) continue;
          for (unsigned ti = 0; ti < 5; ti++) {
              const char *t = txts[ti]; size_t n = strlen(t);
              struct buf big = mk(400 * 120 * 4); struct gfx_surface B = surf(&big, 400, 120, 1600); gfx_clear(&B, BG);
              gfx_draw_text(&B, f, 100, 30, t, n, INK, 0);
              if (count(&B, INK)) nonblank++;
              static const int xs[] = { -60, -30, -9, -1, 0, 3, 40 }, ys[] = { -25, -10, -1, 0, 7, 25 };
              for (unsigned xi = 0; xi < 7; xi++) for (unsigned yi = 0; yi < 6; yi++) {
                  struct buf sm = mk(70 * 40 * 4); struct gfx_surface S = surf(&sm, 70, 40, 280); gfx_clear(&S, BG);
                  /* put the text so that its origin lands at (100 + xs - 100 + ..): draw at (xs, ys) clipped; compare with big drawn at (100,30) cropped at offset (100 - xs, 30 - ys) */
                  gfx_draw_text(&S, f, xs[xi], ys[yi], t, n, INK, 0);
                  for (int y = 0; y < 40; y++) for (int x = 0; x < 70; x++) {
                      int bx = x + 100 - xs[xi], by = y + 30 - ys[yi];
                      uint32_t want = (bx >= 0 && bx < 400 && by >= 0 && by < 120) ? px(&B, bx, by) : BG;
                      if (px(&S, x, y) != want) ok = 0; }
                  if (!guards_ok(&sm)) { ok = 0; }
                  free(sm.base); cnt++; }
              free(big.base); } }
      CHECK("clipped output equals the crop of the unclipped output at every offset, size, weight and text", ok && nonblank > 0);
      printf("    (%d clipped renderings compared)\n", cnt); }
    { struct buf b = mk(80 * 40 * 4); struct gfx_surface s = surf(&b, 80, 40, 320); const struct gfx_font *f = f16;
      gfx_clear(&s, BG); struct gfx_rect clip = { 10, 8, 30, 12 }; gfx_draw_text(&s, f, 0, 0, "WWWWWWWWWWWW", 12, INK, &clip);
      int outside = 0, inside = 0; for (int y = 0; y < 40; y++) for (int x = 0; x < 80; x++) if (px(&s, x, y) == INK) { if (x >= 10 && x < 40 && y >= 8 && y < 20) inside++; else outside++; }
      CHECK("an explicit clip rectangle: ink inside it, none outside", inside > 0 && outside == 0);
      gfx_clear(&s, BG); struct gfx_rect none = { 200, 200, 10, 10 }; gfx_draw_text(&s, f, 0, 0, "WWWW", 4, INK, &none); CHECK("a clip rectangle off the surface draws nothing", count(&s, INK) == 0);
      gfx_clear(&s, BG); struct gfx_rect zero = { 0, 0, 0, 10 }; gfx_draw_text(&s, f, 0, 0, "WWWW", 4, INK, &zero); CHECK("an empty clip rectangle draws nothing", count(&s, INK) == 0);
      free(b.base); }

    printf("drawing: non-tight stride and extreme positions\n");
    { static const uint32_t strides[] = { 64 * 4 + 4, 64 * 4 + 28, 512 };
      for (int k = 0; k < 3; k++) { struct buf b = mk((uint64_t)strides[k] * 32); memset(b.pix, 0xFF, b.len); struct gfx_surface s = surf(&b, 64, 32, strides[k]); char nm[80];
          gfx_draw_text(&s, f16, -3, -2, "Stride test", 11, INK, 0);
          int pad = 1; for (uint32_t y = 0; y + 1 < 32; y++) for (uint32_t i = 256; i < strides[k]; i++) if (b.pix[(uint64_t)y * strides[k] + i] != 0xFF) pad = 0;
          snprintf(nm, sizeof nm, "stride %u: padding untouched, guards intact", strides[k]); CHECK(nm, pad && guards_ok(&b)); free(b.base); } }
    { struct buf b = mk(64 * 32 * 4); struct gfx_surface s = surf(&b, 64, 32, 256); gfx_clear(&s, BG);
      CHECK("INT32_MIN / INT32_MAX positions are accepted and draw nothing",
            gfx_draw_text(&s, f16, INT32_MIN, INT32_MIN, "abc", 3, INK, 0) == GFX_OK && gfx_draw_text(&s, f16, INT32_MAX, INT32_MAX, "abc", 3, INK, 0) == GFX_OK &&
            gfx_draw_text(&s, f16, INT32_MAX - 4, 0, "abc", 3, INK, 0) == GFX_OK && gfx_draw_text(&s, f16, 0, INT32_MIN, "abc", 3, INK, 0) == GFX_OK && count(&s, INK) == 0 && guards_ok(&b));
      CHECK("invalid arguments", gfx_draw_text(0, f16, 0, 0, "a", 1, INK, 0) == GFX_EARG && gfx_draw_text(&s, 0, 0, 0, "a", 1, INK, 0) == GFX_EARG && gfx_draw_text(&s, f16, 0, 0, 0, 1, INK, 0) == GFX_EARG && gfx_draw_text(&s, f16, 0, 0, "a", GFX_TEXT_MAX_BYTES + 1, INK, 0) == GFX_ESIZE);
      CHECK("zero-length text with a NULL pointer is fine", gfx_draw_text(&s, f16, 0, 0, 0, 0, INK, 0) == GFX_OK);
      struct gfx_font bad = *f16; bad.advance = 0; CHECK("a font with advance 0 is refused", gfx_draw_text(&s, &bad, 0, 0, "a", 1, INK, 0) == GFX_EARG);
      struct gfx_font corrupt = *f16; corrupt.bits_len = 2; gfx_clear(&s, BG);
      CHECK("a corrupt glyph table never reads out of bounds (glyph skipped)", gfx_draw_text(&s, &corrupt, 0, 0, "WWWW", 4, INK, 0) == GFX_OK && guards_ok(&b));
      struct gfx_surface badsurf = s; badsurf.size_bytes = 8; CHECK("an invalid surface is refused", gfx_draw_text(&badsurf, f16, 0, 0, "a", 1, INK, 0) == GFX_ESIZE);
      free(b.base); }

    printf("drawing: text blocks\n");
    { struct buf b = mk(200 * 120 * 4); struct gfx_surface s = surf(&b, 200, 120, 800); const struct gfx_font *f = F(400, 12);
      const char *t = "alpha beta gamma delta epsilon zeta eta theta iota kappa";
      size_t total = 0, drawn = 0; gfx_clear(&s, BG);
      struct gfx_rect box = { 20, 10, 100, 60 };
      CHECK("block draws and reports", gfx_draw_text_block(&s, f, box, t, strlen(t), INK, &total, &drawn) == GFX_OK && total >= 4 && drawn >= 3 && drawn <= total && guards_ok(&b));
      int outside = 0; for (int y = 0; y < 120; y++) for (int x = 0; x < 200; x++) if (px(&s, x, y) == INK && (x < 20 || x >= 120 || y < 10 || y >= 70)) outside++;
      CHECK("not a single pixel outside the box", outside == 0 && count(&s, INK) > 0);
      struct gfx_text_line L[16]; size_t tt = 0; gfx_text_wrap(f, t, strlen(t), 100, L, 16, &tt);
      CHECK("the block uses exactly the same line breaks as gfx_text_wrap", tt == total);
      size_t expect_drawn = 0; for (size_t i = 0; i < tt; i++) { int64_t ly = 10 + (int64_t)i * f->line_height; if (ly < 70 && ly + f->line_height > 10) expect_drawn++; }
      CHECK("lines whose top is above the box bottom are drawn (the last may be cut off)", drawn == expect_drawn);
      gfx_clear(&s, BG); CHECK("tall box shows every line", gfx_draw_text_block(&s, f, (struct gfx_rect){ 0, 0, 100, 120 }, t, strlen(t), INK, &total, &drawn) == GFX_OK && drawn == total);
      gfx_clear(&s, BG); CHECK("empty/zero box draws nothing and reports 0", gfx_draw_text_block(&s, f, (struct gfx_rect){ 5, 5, 0, 50 }, t, strlen(t), INK, &total, &drawn) == GFX_OK && total == 0 && drawn == 0 && count(&s, INK) == 0);
      gfx_clear(&s, BG); gfx_draw_text_block(&s, f, (struct gfx_rect){ -30, -20, 100, 60 }, t, strlen(t), INK, &total, &drawn); CHECK("a box partly off the surface is clipped safely", guards_ok(&b) && count(&s, INK) > 0);
      gfx_clear(&s, BG); CHECK("a box wholly off the surface counts lines, draws none", gfx_draw_text_block(&s, f, (struct gfx_rect){ 500, 500, 100, 60 }, t, strlen(t), INK, &total, &drawn) == GFX_OK && total >= 4 && count(&s, INK) == 0);
      gfx_clear(&s, BG); CHECK("INT32 box extremes are safe", gfx_draw_text_block(&s, f, (struct gfx_rect){ INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX }, t, strlen(t), INK, 0, 0) == GFX_OK && gfx_draw_text_block(&s, f, (struct gfx_rect){ INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX }, t, strlen(t), INK, 0, 0) == GFX_OK && guards_ok(&b));
      CHECK("block invalid arguments", gfx_draw_text_block(0, f, box, t, 1, INK, 0, 0) == GFX_EARG && gfx_draw_text_block(&s, 0, box, t, 1, INK, 0, 0) == GFX_EARG && gfx_draw_text_block(&s, f, box, 0, 1, INK, 0, 0) == GFX_EARG);
      free(b.base); }

    printf("the G4 diagnostic image\n");
    { struct buf b = mk(3145728); struct gfx_surface s = surf(&b, 1024, 768, 4096);
      CHECK("draws on a 1024x768 surface", gfx_draw_text_pattern(&s) == GFX_OK && guards_ok(&b));
      CHECK("background corner and ink present", px(&s, 1020, 760) == gfx_rgb(250, 250, 246) && count(&s, gfx_rgb(20, 20, 30)) > 3000);
      struct buf b2 = mk(3145728); struct gfx_surface s2 = surf(&b2, 1024, 768, 4096);
      gfx_draw_text_pattern(&s2); CHECK("deterministic: two renderings are identical", memcmp(b.pix, b2.pix, 3145728) == 0);
      free(b2.base); free(b.base);
      b = mk(1000 * 4 * 700); s = surf(&b, 1000, 700, 4000);
      CHECK("surface smaller than 1024x768 refused, nothing written", gfx_draw_text_pattern(&s) == GFX_ESIZE && px(&s, 0, 0) == 0 && guards_ok(&b)); free(b.base); }

    printf("\ngfx_text_test: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
