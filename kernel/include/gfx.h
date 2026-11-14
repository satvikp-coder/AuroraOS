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
