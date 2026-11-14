/*
 * kernel/gfx/gfx.c - graphics backend for QEMU ramfb (stage G2).
 *
 * Boot flow:
 *   gfx_reserve(fdt)   BEFORE vmm_init: probe fw_cfg, require DMA and the file etc/ramfb, then claim a physically contiguous
 *                      region of guest RAM from the page allocator. Done early because the allocator has no contiguous
 *                      allocation and page-table setup consumes pages. If there is no display, NOTHING is reserved.
 *   gfx_init()         AFTER vmm_enable: map fw_cfg as Device memory, write the ramfb configuration with one DMA request,
 *                      then expose the surface. Any failure leaves graphics disabled; the kernel keeps booting on the UART.
 *
 * ramfb has no present step: QEMU samples the guest memory on its own refresh timer. gfx_present() is therefore only a
 * barrier. A backend with explicit presentation (virtio-gpu) would implement it for real; the renderer would not change.
 *
 * Cache coherence: the framebuffer is ordinary Normal write-back memory. QEMU TCG reads guest RAM directly and does not model
 * caches, so no cache maintenance is performed or needed here. This says NOTHING about real hardware or accelerators:
 * any scan-out engine that is not cache-coherent would require a clean (dc cvac) of each damaged range in gfx_present().
 */
#include <console.h>
#include <fwcfg.h>
#include <gfx.h>
#include <mm.h>

static struct {
    int state;                         /* 0 none, 1 reserved, 2 up, 3 disabled */
    uint64_t fb_pa, fb_bytes;
    struct fwcfg_file ramfb;
    struct gfx_surface surf;
} G;

static void off(const char *why, int rc)
{
    console_puts("gfx: disabled ("); console_puts(why);
    if (rc) { console_puts(": "); console_puts(fwcfg_strerror(rc)); }
    console_puts(")\n");
}

static int page_busy(uint64_t page, void *ctx)
{
    (void)ctx;
    return pmm_is_reserved((uintptr_t)page) || pmm_is_allocated((uintptr_t)page);
}

int gfx_reserve(const void *fdt)
{
    G.state = 3; G.fb_pa = G.fb_bytes = 0;
    if (!fdt) { off("no device tree", 0); return GFX_ENODEV; }

    int rc = fwcfg_probe(fdt, 1);
    if (rc) { off("fw_cfg", rc); return GFX_ENODEV; }
    rc = fwcfg_lookup(RAMFB_FILE_NAME, &G.ramfb);
    if (rc) { off("no ramfb display", rc); return GFX_ENODEV; }
    if (G.ramfb.size < RAMFB_CFG_SIZE) { off("etc/ramfb is smaller than the configuration structure", 0); return GFX_ENODEV; }

    uint64_t bytes;
    if (gfx_buffer_bytes(GFX_FB_WIDTH, GFX_FB_HEIGHT, 0, &bytes)) { off("framebuffer size arithmetic failed", 0); return GFX_ESIZE; }
    uint64_t pa;
    rc = gfx_region_find(pmm_ram_base(), pmm_ram_end(), bytes, page_busy, 0, &pa);
    if (rc) { off("no contiguous free RAM for the framebuffer", 0); return GFX_EMEM; }
    if (pmm_reserve_range(pa, bytes)) { off("could not reserve the framebuffer region", 0); return GFX_EMEM; }

    G.fb_pa = pa; G.fb_bytes = bytes; G.state = 1;
    console_puts("gfx: reserved framebuffer "); console_hex(pa); console_puts(" - "); console_hex(pa + ((bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)));
    console_puts(" ("); console_dec(bytes); console_puts(" bytes)\n");
    return GFX_OK;
}

int gfx_init(void)
{
    if (G.state != 1) return G.state == 2 ? GFX_OK : GFX_ENODEV;
    int rc = fwcfg_map();
    if (rc) { off("fw_cfg window", rc); G.state = 3; return GFX_ENODEV; }

    struct gfx_surface s;
    s.pixels = (uint8_t *)(uintptr_t)G.fb_pa; s.size_bytes = G.fb_bytes;
    s.width = GFX_FB_WIDTH; s.height = GFX_FB_HEIGHT; s.stride_bytes = GFX_FB_WIDTH * GFX_BPP_BYTES; s.format = GFX_FMT_XRGB8888;
    if (gfx_surface_check(&s)) { off("surface failed validation", 0); G.state = 3; return GFX_ESIZE; }
    gfx_clear(&s, gfx_rgb(0, 0, 0));                         /* the display starts black, never with stale RAM contents */

    static uint8_t cfg[RAMFB_CFG_SIZE] __attribute__((aligned(8)));
    if (ramfb_build_cfg(cfg, G.fb_pa, RAMFB_FOURCC_XR24, s.width, s.height, s.stride_bytes)) { off("bad ramfb configuration", 0); G.state = 3; return GFX_ESIZE; }
    rc = fwcfg_dma_write(G.ramfb.select, cfg, RAMFB_CFG_SIZE);
    if (rc) { off("ramfb configuration rejected", rc); G.state = 3; return GFX_ENODEV; }

    G.surf = s; G.state = 2;
    console_puts("gfx: framebuffer "); console_dec(s.width); console_puts("x"); console_dec(s.height);
    console_puts(" XRGB8888 stride "); console_dec(s.stride_bytes); console_puts(" at "); console_hex(G.fb_pa); console_puts("\n");
    return GFX_OK;
}

const struct gfx_surface *gfx_get_surface(void) { return G.state == 2 ? &G.surf : 0; }

void gfx_present(const struct gfx_rect *damage)
{
    (void)damage;                                            /* ramfb has no present step; see the file header on caches */
    __asm__ volatile ("dsb sy" ::: "memory");
}

void gfx_shutdown(void)
{
    if (G.state == 2) G.state = 3;                           /* the region stays reserved; ramfb keeps showing the last frame */
}
