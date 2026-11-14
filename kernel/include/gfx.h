/*
 * kernel/include/gfx.h - native graphics: a backend-independent surface, bounds-checked drawing, and the QEMU ramfb backend.
 *
 * Scope (stage G2 of issue #7): QEMU "virt" ONLY. Nothing here has run on real hardware. The renderer (gfx_draw.c) knows
 * nothing about devices: it draws into a struct gfx_surface. The backend (gfx.c + fwcfg.c) owns the display device and the
 * physical memory behind the surface. A future virtio-gpu backend must be able to replace the backend without touching gfx_draw.c.
 *
 * Ownership and concurrency: the kernel owns the one framebuffer (a physically contiguous region reserved from the page
 * allocator at boot). It is never mapped into an EL0 address space. There is one CPU and one caller at a time; no locking.
 */
#ifndef AURORA_GFX_H
#define AURORA_GFX_H
#include <stddef.h>
#include <stdint.h>
#include <gfx_font.h>

#define GFX_FMT_XRGB8888 1u        /* 32 bits per pixel: 0x00RRGGBB as a little-endian u32 (bytes B,G,R,X); DRM fourcc XR24 */
#define GFX_BPP_BYTES    4u
#define GFX_MAX_DIM      8192u     /* hard cap on width and height */

struct gfx_surface {
    uint8_t *pixels;               /* first pixel; 4-byte aligned */
    uint64_t size_bytes;           /* length of the buffer behind `pixels` */
    uint32_t width, height;        /* pixels */
    uint32_t stride_bytes;         /* bytes from one row to the next; need not equal width * 4 */
    uint32_t format;               /* GFX_FMT_* */
};
struct gfx_rect { int32_t x, y, w, h; };   /* w or h <= 0 is an empty rectangle */

enum { GFX_OK = 0, GFX_EARG = -1, GFX_EFORMAT = -2, GFX_ESIZE = -3, GFX_ENODEV = -4, GFX_ESTATE = -5, GFX_EMEM = -6 };
const char *gfx_strerror(int rc);

/* ---- pure rendering (kernel/gfx/gfx_draw.c): host-tested under ASan/UBSan ------------------------------------------------ */
uint32_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b);                       /* 0x00RRGGBB */
/* Full validation: non-null, aligned, supported format, 1..GFX_MAX_DIM dimensions, stride >= width*4 and a multiple of 4,
 * and (height-1)*stride + width*4 <= size_bytes, all in 64-bit arithmetic. GFX_OK or an error. */
int  gfx_surface_check(const struct gfx_surface *s);
int  gfx_clear(const struct gfx_surface *s, uint32_t color);
int  gfx_set_pixel(const struct gfx_surface *s, int32_t x, int32_t y, uint32_t color);   /* off-screen: GFX_OK, no write */
int  gfx_fill_rect(const struct gfx_surface *s, struct gfx_rect r, uint32_t color);      /* clipped; empty/off-screen: GFX_OK */
int  gfx_hline(const struct gfx_surface *s, int32_t x, int32_t y, int32_t len, uint32_t color);
int  gfx_vline(const struct gfx_surface *s, int32_t x, int32_t y, int32_t len, uint32_t color);
/* Intersect r with [0,w) x [0,h). Returns 1 and the clipped rectangle when non-empty, 0 when empty. Overflow-safe for any int32. */
int  gfx_clip_rect(uint32_t w, uint32_t h, struct gfx_rect r, struct gfx_rect *out);
int  gfx_draw_test_pattern(const struct gfx_surface *s);                 /* the G2 diagnostic image; needs width,height >= 1024x768 */

/* ---- rounded rectangles and borders (kernel/gfx/gfx_shape.c, stage G3): exact integer coverage rule documented in that file ---- */
int32_t gfx_round_clamp_radius(int32_t w, int32_t h, int32_t r);          /* r clamped to [0, min(w,h)/2] */
int  gfx_fill_round_rect(const struct gfx_surface *s, struct gfx_rect r, int32_t radius, uint32_t color);
/* The border is drawn INSIDE r: outer shape minus the shape inset by `border` with radius max(radius - border, 0). border <= 0 draws nothing. */
int  gfx_stroke_round_rect(const struct gfx_surface *s, struct gfx_rect r, int32_t radius, int32_t border, uint32_t color);
int  gfx_draw_shapes_pattern(const struct gfx_surface *s);               /* the G3 diagnostic image (test builds only) */

/* ---- text (kernel/gfx/gfx_text.c, stage G4): printable ASCII only, monospaced bitmap font, 1-bit, opaque ---------------------------------
 * Layout model: one UTF-8 code point = one cell of `advance` pixels. U+0020..U+007E draw their glyph; EVERYTHING ELSE (controls, DEL,
 * non-ASCII such as Devanagari or Gujarati, invalid UTF-8: one cell per bad byte) draws a visible hollow missing-glyph box; there is no
 * shaping, no bidirectional text and no combining marks. '
' ends a line; '' and '	' are ordinary controls (boxes). Lines are
 * `line_height` apart; a line's top is y, its baseline is y + ascent. Text input is bytes with an explicit length (no NUL needed). */
#define GFX_TEXT_MAX_BYTES (1u << 20)             /* longer inputs are refused (GFX_ESIZE) */
struct gfx_text_line { size_t start, len; uint32_t cells; };   /* byte range in the input (trailing spaces at a wrap trimmed) and its cell count */

const struct gfx_font *gfx_font_find(int weight, int size);    /* exact weight and size, or NULL */
/* Decode one code point at s[*i] (requires *i < n): advances *i by the sequence length, or by exactly 1 byte for an invalid or
 * overlong sequence, a surrogate or a value above U+10FFFF, for which *cp is U+FFFD. */
void gfx_utf8_next(const char *s, size_t n, size_t *i, uint32_t *cp);
/* Size of the text with explicit newlines only (no wrapping): width of the longest line, height = lines * line_height. */
int  gfx_text_measure(const struct gfx_font *f, const char *s, size_t n, int32_t *w, int32_t *h);
/* Word wrap to max_w pixels (at least one cell per line even if max_w is smaller). Breaks at spaces (the spaces at the break are dropped),
 * breaks overlong words between cells, honours '
'. Fills up to max_out entries of `out` (may be NULL with max_out 0) and always
 * stores the total number of lines in *total. A final empty line after a trailing '
' is not counted. */
int  gfx_text_wrap(const struct gfx_font *f, const char *s, size_t n, int32_t max_w, struct gfx_text_line *out, size_t max_out, size_t *total);
/* Draw the first line of the text (up to the first '
') with its top-left at (x, y), clipped to the surface and to `clip` (may be NULL). */
int  gfx_draw_text(const struct gfx_surface *s, const struct gfx_font *f, int32_t x, int32_t y, const char *str, size_t n, uint32_t color, const struct gfx_rect *clip);
/* Wrap into `box` and draw every line whose top is above the box bottom, clipped to the box. *lines_total / *lines_drawn may be NULL. */
int  gfx_draw_text_block(const struct gfx_surface *s, const struct gfx_font *f, struct gfx_rect box, const char *str, size_t n, uint32_t color, size_t *lines_total, size_t *lines_drawn);
int  gfx_draw_text_pattern(const struct gfx_surface *s);       /* the G4 diagnostic image (test builds only) */

/* ---- placement of the framebuffer in RAM (kernel/gfx/gfx_region.c): pure ------------------------------------------------ */
/* Find the HIGHEST page-aligned region of `size` bytes inside [ram_base, ram_end) for which busy(page) is false for every
 * page. 0 on success (*out set), GFX_EMEM if none, GFX_EARG on bad arguments or overflow. */
int  gfx_region_find(uint64_t ram_base, uint64_t ram_end, uint64_t size, int (*busy)(uint64_t page, void *ctx), void *ctx, uint64_t *out);
/* Bytes needed for a w x h x 4 buffer with the given stride (0 = tight). GFX_OK or GFX_ESIZE on overflow/invalid. */
int  gfx_buffer_bytes(uint32_t w, uint32_t h, uint32_t stride_bytes, uint64_t *bytes);

/* ---- backend (kernel/gfx/gfx.c): QEMU ramfb ------------------------------------------------------------------------------ */
#define GFX_FB_WIDTH  1024u
#define GFX_FB_HEIGHT 768u
/* Early (before vmm_init, after the page-allocator reservations): probe for a display and reserve its memory. Never panics.
 * Returns GFX_OK (display found and memory reserved) or a negative reason; the OS keeps booting either way. */
int  gfx_reserve(const void *fdt);
/* After vmm_enable(): map fw_cfg, configure ramfb, expose the surface. */
int  gfx_init(void);
const struct gfx_surface *gfx_get_surface(void);                        /* NULL unless the display is up */
void gfx_present(const struct gfx_rect *damage);                        /* NULL = whole surface. ramfb: ordering barrier only */
void gfx_shutdown(void);                                                /* stop using the surface (the memory stays reserved) */

#endif
