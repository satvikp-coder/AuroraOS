/*
 * kernel/gfx/gfx_text.c - monospaced bitmap text: UTF-8 decoding, measuring, word wrap, clipped drawing (stage G4).
 * Pure: no hardware access, no allocation, no floating point. Coordinates are int64 internally; every pixel write is clipped.
 * The glyph data is generated (font_noto_mono.c); this file only knows the data format (gfx_font.h). Layout rules are in gfx.h.
 *
 * Known limits (not hidden): printable ASCII only; no shaping; no bidirectional text; no combining marks; no kerning (the font is
 * monospaced); 1-bit glyphs (no anti-aliasing); no keyboard, focus or screen-reader support exists in the native stack yet, so text
 * drawn here is visual only.
 *
 * Wrapping rules (greedy, by cells; every code point is one cell):
 *   - '\n' ends a line (an empty line is a line). A final empty line after a trailing '\n' is not a line.
 *   - A cell that does not fit moves to the next line. If the line holds a run of spaces that is not at its very start, the line
 *     breaks at the LAST such run and the spaces are dropped; otherwise (one long word) it breaks between cells.
 *   - A space that would be the first cell past the edge is the break: it and any spaces after it are dropped, and a '\n' directly
 *     after them is part of the same break (no spurious empty line).
 *   - Trailing spaces never count towards a line's cells. At least one cell fits on every line, however narrow the box.
 */
#include <gfx.h>

extern const struct gfx_font *const gfx_fonts[];
extern const int gfx_font_count;

const struct gfx_font *gfx_font_find(int weight, int size)
{
    for (int i = 0; i < gfx_font_count; i++)
        if (gfx_fonts[i]->weight == weight && gfx_fonts[i]->size == size) return gfx_fonts[i];
    return 0;
}

void gfx_utf8_next(const char *str, size_t n, size_t *i, uint32_t *cp)
{
    const uint8_t *s = (const uint8_t *)str;
    size_t p = *i;
    uint8_t b0 = s[p];
    *cp = 0xFFFD;
    if (b0 < 0x80) { *cp = b0; *i = p + 1; return; }
    int len; uint32_t v; uint8_t lo = 0x80, hi = 0xBF;     /* allowed range of the SECOND byte */
    if (b0 >= 0xC2 && b0 <= 0xDF) { len = 2; v = b0 & 0x1Fu; }
    else if (b0 >= 0xE0 && b0 <= 0xEF) { len = 3; v = b0 & 0x0Fu; if (b0 == 0xE0) lo = 0xA0; if (b0 == 0xED) hi = 0x9F; }
    else if (b0 >= 0xF0 && b0 <= 0xF4) { len = 4; v = b0 & 0x07u; if (b0 == 0xF0) lo = 0x90; if (b0 == 0xF4) hi = 0x8F; }
    else { *i = p + 1; return; }                           /* continuation byte, overlong lead (C0, C1) or F5..FF */
    if (n - p < (size_t)len) { *i = p + 1; return; }       /* truncated */
    for (int k = 1; k < len; k++) {
        uint8_t b = s[p + (size_t)k];
        uint8_t l = (k == 1) ? lo : 0x80, h = (k == 1) ? hi : 0xBF;
        if (b < l || b > h) { *i = p + 1; return; }
        v = (v << 6) | (b & 0x3Fu);
    }
    *cp = v; *i = p + (size_t)len;
}

static int check_font(const struct gfx_font *f) { return f && f->glyphs && f->bits && f->advance > 0 && f->first <= f->last && f->line_height > 0; }

int gfx_text_measure(const struct gfx_font *f, const char *s, size_t n, int32_t *w, int32_t *h)
{
    if (!check_font(f) || (!s && n) || !w || !h) return GFX_EARG;
    if (n > GFX_TEXT_MAX_BYTES) return GFX_ESIZE;
    uint64_t best = 0, cols = 0, lines = 0; int pending = 0;
    size_t i = 0;
    while (i < n) {
        uint32_t cp; gfx_utf8_next(s, n, &i, &cp);
        if (cp == '\n') { if (cols > best) best = cols; cols = 0; lines++; pending = 0; }
        else { cols++; pending = 1; }
    }
    if (pending) { if (cols > best) best = cols; lines++; }
    *w = (int32_t)(best * f->advance);
    *h = (int32_t)(lines * f->line_height);
    return GFX_OK;
}

typedef void (*emit_fn)(void *ctx, size_t start, size_t len, uint32_t cells);

static void emit_trimmed(emit_fn emit, void *ctx, const char *s, size_t a, size_t b, uint32_t cells)
{
    while (b > a && s[b - 1] == ' ') { b--; cells--; }       /* spaces are single bytes and single cells */
    emit(ctx, a, b - a, cells);
}

static void wrap_core(const struct gfx_font *f, const char *s, size_t n, int32_t max_w, emit_fn emit, void *ctx)
{
    uint32_t cols_max = max_w > 0 ? (uint32_t)max_w / f->advance : 0;
    if (cols_max < 1) cols_max = 1;
    size_t i = 0, line_start = 0;
    uint32_t cols = 0;
    int in_run = 0, have_run = 0;
    size_t run_start = 0, run_end = 0; uint32_t run_before = 0, run_after = 0;

    while (i < n) {
        size_t cell = i; uint32_t cp;
        gfx_utf8_next(s, n, &i, &cp);
        if (cp == '\n') {
            emit_trimmed(emit, ctx, s, line_start, cell, cols);
            line_start = i; cols = 0; have_run = in_run = 0;
            continue;
        }
        if (cols + 1 > cols_max) {                           /* this cell does not fit on the line */
            if (cp == ' ') {                                 /* a space at the edge is the break itself */
                emit_trimmed(emit, ctx, s, line_start, cell, cols);
                while (i < n && s[i] == ' ') i++;
                if (i < n && s[i] == '\n') i++;
                line_start = i; cols = 0; have_run = in_run = 0;
                continue;
            }
            if (have_run) {                                  /* break at the last run of spaces */
                emit_trimmed(emit, ctx, s, line_start, run_start, run_before);
                line_start = run_end; cols -= run_after; have_run = in_run = 0;
            } else {                                         /* one long word: break between cells */
                emit_trimmed(emit, ctx, s, line_start, cell, cols);
                line_start = cell; cols = 0; in_run = 0;
            }
        }
        cols++;
        if (cp == ' ') {
            if (!in_run) { in_run = 1; run_start = cell; run_before = cols - 1; }
            run_end = i; run_after = cols;
            have_run = run_before > 0;                       /* a run at the very start of a line is not a break opportunity */
        } else {
            in_run = 0;
        }
    }
    if (line_start < n) emit_trimmed(emit, ctx, s, line_start, n, cols);
}

struct collect { struct gfx_text_line *out; size_t max, total; };
static void collect_line(void *c, size_t start, size_t len, uint32_t cells)
{
    struct collect *k = c;
    if (k->total < k->max) { k->out[k->total].start = start; k->out[k->total].len = len; k->out[k->total].cells = cells; }
    k->total++;
}

int gfx_text_wrap(const struct gfx_font *f, const char *s, size_t n, int32_t max_w, struct gfx_text_line *out, size_t max_out, size_t *total)
{
    if (!check_font(f) || (!s && n) || !total || (max_out && !out)) return GFX_EARG;
    if (n > GFX_TEXT_MAX_BYTES) return GFX_ESIZE;
    struct collect k = { out, max_out, 0 };
    wrap_core(f, s, n, max_w, collect_line, &k);
    *total = k.total;
    return GFX_OK;
}

/* ---- drawing ------------------------------------------------------------------------------------------------------------------ */
struct clipbox { int64_t x0, y0, x1, y1; };

static void put(const struct gfx_surface *s, const struct clipbox *c, int64_t x, int64_t y, uint32_t color)
{
    if (x < c->x0 || x >= c->x1 || y < c->y0 || y >= c->y1) return;
    *(uint32_t *)(void *)(s->pixels + (uint64_t)y * s->stride_bytes + (uint64_t)x * GFX_BPP_BYTES) = color;
}

static void draw_cell(const struct gfx_surface *s, const struct gfx_font *f, const struct clipbox *c, int64_t pen, int64_t baseline, uint32_t cp, uint32_t color)
{
    if (cp >= f->first && cp <= f->last) {
        const struct gfx_glyph *g = &f->glyphs[cp - f->first];
        if (g->w == 0 || g->h == 0) return;                  /* blank glyph (space) */
        uint32_t stride = ((uint32_t)g->w + 7u) / 8u;
        if ((uint64_t)g->off + (uint64_t)stride * g->h > f->bits_len) return;     /* corrupt table: draw nothing rather than read out of bounds */
        for (uint32_t r = 0; r < g->h; r++) {
            int64_t py = baseline - g->top + r;
            if (py < c->y0 || py >= c->y1) continue;
            const uint8_t *row = f->bits + g->off + (size_t)r * stride;
            for (uint32_t col = 0; col < g->w; col++)
                if (row[col >> 3] & (0x80u >> (col & 7u))) put(s, c, pen + g->xoff + col, py, color);
        }
        return;
    }
    /* missing glyph: a hollow box standing on the baseline, one pixel in from each side of the cell */
    int64_t bh = (int64_t)f->ascent * 3 / 4; if (bh < 5) bh = 5;
    int64_t left = pen + 1, right = pen + f->advance - 2, bottom = baseline - 1, top = baseline - bh;
    if (right < left + 2) return;
    for (int64_t x = left; x <= right; x++) { put(s, c, x, top, color); put(s, c, x, bottom, color); }
    for (int64_t y = top; y <= bottom; y++) { put(s, c, left, y, color); put(s, c, right, y, color); }
}

static void draw_line(const struct gfx_surface *s, const struct gfx_font *f, const struct clipbox *c, int64_t x, int64_t y, const char *str, size_t len, uint32_t color)
{
    if (y + f->line_height <= c->y0 || y >= c->y1) return;
    int64_t pen = x, baseline = y + f->ascent;
    size_t i = 0;
    while (i < len) {
        uint32_t cp; gfx_utf8_next(str, len, &i, &cp);
        if (pen >= c->x1) break;
        if (pen + f->advance > c->x0) draw_cell(s, f, c, pen, baseline, cp, color);
        pen += f->advance;
    }
}

static int make_clip(const struct gfx_surface *s, const struct gfx_rect *clip, struct clipbox *c)
{
    c->x0 = 0; c->y0 = 0; c->x1 = s->width; c->y1 = s->height;
    if (clip) {
        struct gfx_rect r;
        if (!gfx_clip_rect(s->width, s->height, *clip, &r)) return 0;
        c->x0 = r.x; c->y0 = r.y; c->x1 = (int64_t)r.x + r.w; c->y1 = (int64_t)r.y + r.h;
    }
    return 1;
}

int gfx_draw_text(const struct gfx_surface *s, const struct gfx_font *f, int32_t x, int32_t y, const char *str, size_t n, uint32_t color, const struct gfx_rect *clip)
{
    int rc = gfx_surface_check(s);
    if (rc) return rc;
    if (!check_font(f) || (!str && n)) return GFX_EARG;
    if (n > GFX_TEXT_MAX_BYTES) return GFX_ESIZE;
    struct clipbox c;
    if (!make_clip(s, clip, &c)) return GFX_OK;
    size_t len = 0;
    while (len < n && str[len] != '\n') len++;               /* the first line only */
    draw_line(s, f, &c, x, y, str, len, color);
    return GFX_OK;
}

struct blockctx { const struct gfx_surface *s; const struct gfx_font *f; struct clipbox c; const char *str; int64_t x, y; uint32_t color; size_t total, drawn; };
static void block_line(void *p, size_t start, size_t len, uint32_t cells)
{
    struct blockctx *b = p; (void)cells;
    int64_t ly = b->y + (int64_t)b->total * b->f->line_height;
    b->total++;
    if (ly >= b->c.y1 || ly + b->f->line_height <= b->c.y0) return;
    draw_line(b->s, b->f, &b->c, b->x, ly, b->str + start, len, b->color);
    b->drawn++;
}

int gfx_draw_text_block(const struct gfx_surface *s, const struct gfx_font *f, struct gfx_rect box, const char *str, size_t n, uint32_t color, size_t *lines_total, size_t *lines_drawn)
{
    int rc = gfx_surface_check(s);
    if (rc) return rc;
    if (!check_font(f) || (!str && n)) return GFX_EARG;
    if (n > GFX_TEXT_MAX_BYTES) return GFX_ESIZE;
    if (lines_total) *lines_total = 0;
    if (lines_drawn) *lines_drawn = 0;
    if (box.w <= 0 || box.h <= 0) return GFX_OK;
    struct blockctx b = { s, f, { 0, 0, 0, 0 }, str, box.x, box.y, color, 0, 0 };
    struct gfx_rect vis;
    int visible = gfx_clip_rect(s->width, s->height, box, &vis);
    if (visible) { b.c.x0 = vis.x; b.c.y0 = vis.y; b.c.x1 = (int64_t)vis.x + vis.w; b.c.y1 = (int64_t)vis.y + vis.h; }
    wrap_core(f, str, n, box.w, block_line, &b);
    if (lines_total) *lines_total = b.total;
    if (lines_drawn) *lines_drawn = b.drawn;
    return GFX_OK;
}

/*
 * The G4 diagnostic image (1024x768), drawn by the kernel only in a test build (AURORA_GFX_TEXT=1). tests/qemu/gfx_check.py
 * re-renders it with an independent Python implementation (own UTF-8 decoder, own word wrap, own blit) over the glyph data parsed
 * from kernel/gfx/font_noto_mono.c, and requires the screenshot to match byte for byte. It exercises every size and weight, the full
 * ASCII range, missing glyphs (Devanagari, Gujarati, controls, DEL, accented Latin, invalid UTF-8), wrapping with explicit newlines,
 * double spaces, one overlong word and a blank line, narrow boxes, vertical clipping, and clipping at all four screen edges and to
 * an explicit clip rectangle.
 */
int gfx_draw_text_pattern(const struct gfx_surface *s)
{
    int rc = gfx_surface_check(s);
    if (rc) return rc;
    if (s->width < 1024u || s->height < 768u) return GFX_ESIZE;
    static const int sizes[5] = { 12, 14, 16, 18, 24 };
    static const char para[] = "Wrapping: every word wraps at the box edge, a very-long-word-without-any-spaces-breaks-between-cells,\nand a newline starts a new line.  Two  spaces  survive  inside  lines.\n\nBlank line above.";
    static const char miss[] = "Missing: \xE0\xA4\xB9 \xE0\xAA\x97 \x01 \x7F \xC3\xA9 bad:\xFF\xC3 end";
    const uint32_t BG = gfx_rgb(250, 250, 246), INK = gfx_rgb(20, 20, 30), RULE = gfx_rgb(150, 150, 150);
    const struct gfx_font *f24b = gfx_font_find(GFX_FONT_BOLD, 24), *f16 = gfx_font_find(GFX_FONT_REGULAR, 16), *f16b = gfx_font_find(GFX_FONT_BOLD, 16);
    const struct gfx_font *f14 = gfx_font_find(GFX_FONT_REGULAR, 14), *f12 = gfx_font_find(GFX_FONT_REGULAR, 12), *f18b = gfx_font_find(GFX_FONT_BOLD, 18), *f18 = gfx_font_find(GFX_FONT_REGULAR, 18);
    if (!f24b || !f16 || !f16b || !f14 || !f12 || !f18b || !f18) return GFX_EARG;
    if ((rc = gfx_clear(s, BG))) return rc;
    static const char title[] = "AuroraOS native text";
    if ((rc = gfx_draw_text(s, f24b, 32, 16, title, sizeof title - 1, INK, 0))) return rc;
    int32_t y = 64;
    static const char fox[] = "The quick brown fox jumps over the lazy dog 0123456789";
    for (int i = 0; i < 5; i++) {
        const struct gfx_font *f = gfx_font_find(GFX_FONT_REGULAR, sizes[i]);
        if (!f) return GFX_EARG;
        if ((rc = gfx_draw_text(s, f, 32, y, fox, sizeof fox - 1, INK, 0))) return rc;
        y += f->line_height + 2;
    }
    char all[96];
    for (int c = 0x20; c <= 0x7E; c++) all[c - 0x20] = (char)c;
    if ((rc = gfx_draw_text(s, f16b, 32, y, all, 95, INK, 0))) return rc;
    y += f16b->line_height + 2;
    if ((rc = gfx_draw_text(s, f16, 32, y, miss, sizeof miss - 1, INK, 0))) return rc;
    y += f16->line_height + 12;
    struct gfx_rect a = { 32, y, 300, 150 }, b = { 360, y, 120, 200 }, c = { 520, y, 260, 50 };
    if ((rc = gfx_stroke_round_rect(s, (struct gfx_rect){ a.x - 1, a.y - 1, a.w + 2, a.h + 2 }, 0, 1, RULE))) return rc;
    if ((rc = gfx_stroke_round_rect(s, (struct gfx_rect){ b.x - 1, b.y - 1, b.w + 2, b.h + 2 }, 0, 1, RULE))) return rc;
    if ((rc = gfx_stroke_round_rect(s, (struct gfx_rect){ c.x - 1, c.y - 1, c.w + 2, c.h + 2 }, 0, 1, RULE))) return rc;
    if ((rc = gfx_draw_text_block(s, f14, a, para, sizeof para - 1, INK, 0, 0))) return rc;
    if ((rc = gfx_draw_text_block(s, f12, b, para, sizeof para - 1, INK, 0, 0))) return rc;
    if ((rc = gfx_draw_text_block(s, f18b, c, para, sizeof para - 1, INK, 0, 0))) return rc;
    int32_t yc = y + 220;
    static const char l1[] = "CLIPPED LEFT EDGE", l2[] = "CLIPPED RIGHT EDGE", l3[] = "TOP EDGE", l4[] = "BOTTOM EDGE", l5[] = "clip rectangle test: only a slice shows";
    if ((rc = gfx_draw_text(s, f16, -25, yc, l1, sizeof l1 - 1, INK, 0))) return rc;
    if ((rc = gfx_draw_text(s, f16, 940, yc, l2, sizeof l2 - 1, INK, 0))) return rc;
    if ((rc = gfx_draw_text(s, f16, 700, -8, l3, sizeof l3 - 1, INK, 0))) return rc;
    if ((rc = gfx_draw_text(s, f16, 32, 760, l4, sizeof l4 - 1, INK, 0))) return rc;
    struct gfx_rect clip = { 80, yc + 44, 120, 12 };
    if ((rc = gfx_draw_text(s, f18, 32, yc + 40, l5, sizeof l5 - 1, INK, &clip))) return rc;
    return GFX_OK;
}
