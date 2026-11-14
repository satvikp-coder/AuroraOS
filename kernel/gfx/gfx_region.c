/*
 * kernel/gfx/gfx_region.c - pure placement of the framebuffer: the highest contiguous run of free, page-aligned RAM.
 * No hardware or allocator access; the caller supplies busy(page).
 */
#include <gfx.h>

#define PG 4096ULL

int gfx_region_find(uint64_t ram_base, uint64_t ram_end, uint64_t size, int (*busy)(uint64_t page, void *ctx), void *ctx, uint64_t *out)
{
    if (!busy || !out || size == 0 || ram_end <= ram_base) return GFX_EARG;
    if ((ram_base | ram_end) & (PG - 1)) return GFX_EARG;
    if (size > ram_end - ram_base) return GFX_EMEM;
    size = (size + PG - 1) & ~(PG - 1);                       /* whole pages (size <= RAM, so this cannot overflow) */
    if (size > ram_end - ram_base) return GFX_EMEM;

    uint64_t top = ram_end;                                  /* candidate is [top - size, top) */
    while (top - ram_base >= size) {
        uint64_t lo = top - size, bad = 0; int have = 0;
        for (uint64_t p = top; p > lo; ) {                   /* find the highest busy page in the candidate */
            p -= PG;
            if (busy(p, ctx)) { bad = p; have = 1; break; }
        }
        if (!have) { *out = lo; return GFX_OK; }
        top = bad;                                           /* slide the window to end just below the busy page */
        if (top < ram_base + size) break;
    }
    return GFX_EMEM;
}
