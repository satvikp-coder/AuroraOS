/*
 * kernel/proc/memsys.c - SYS_MEM: a bounded, quota-checked user heap (Cycle 22).
 *
 * A process may grow and shrink one contiguous heap at USER_HEAP_VA (kernel/include/uvlayout.h), in whole pages, up to USER_HEAP_MAX_PAGES (a per-process
 * page budget). The heap is bracketed by 16-page GUARDS that are never mapped (aspace_map refuses them), and it lies outside the ELF load window. Rules:
 *   - heap pages are RW and never executable (W^X: aspace_map's RW attribute sets UXN and PXN) and are zero when first mapped (the allocator hands out zeroed pages);
 *   - growth is ATOMIC: if any page cannot be mapped, every page this call mapped is unmapped again and the heap is unchanged;
 *   - the kernel keeps HEAP_RESERVE_PAGES free for itself, the compositor and the loader: a grow that would dig into the reserve fails with AE_ENOMEM;
 *   - a request is validated before anything is touched (0 pages, more than the budget, or an arithmetic overflow are AE_EINVAL, never wrapped);
 *   - shrinking unmaps from the top, flushes the TLB entry and frees the page; the freed addresses fault (translation fault) afterwards;
 *   - there is no way to name a physical address, map the framebuffer or another process's memory, or change permissions: the only addresses involved are the
 *     heap's own, computed here from the page count.
 * Everything the heap owns is released with the address space when the process exits or faults.
 */
#include <proc.h>
#include <mm.h>

#define HEAP_RESERVE_PAGES 256u                  /* 1 MiB of RAM the heap can never take */

int64_t mem_syscall(struct proc *p, uint64_t op, uint64_t n)
{
    switch (op) {
    case MEMOP_INFO:                             /* (budget << 32) | pages currently mapped */
        return (int64_t)(((uint64_t)USER_HEAP_MAX_PAGES << 32) | p->heap_pages);
    case MEMOP_GROW: {
        if (n == 0 || n > USER_HEAP_MAX_PAGES) return AE_EINVAL;                     /* also rejects any value that could overflow later arithmetic */
        if (p->heap_pages + n > USER_HEAP_MAX_PAGES) return AE_ENOMEM;               /* over the budget */
        struct pmm_stats st; pmm_get_stats(&st);
        if (st.free_pages < n + HEAP_RESERVE_PAGES) return AE_ENOMEM;                /* would dig into the reserve */
        uint32_t base = p->heap_pages, done = 0;
        for (; done < n; done++) {
            uint64_t va = uv_heap_page_va((uint64_t)base + done);                    /* 0 if it is not a page the heap may ever own */
            if (!va || aspace_map(&p->as, va, UPERM_RW, 0, 0, 0) != 0) break;
        }
        if (done < n) {                                                              /* atomic: undo what this call mapped */
            while (done--) (void)aspace_unmap(&p->as, uv_heap_page_va((uint64_t)base + done));
            return AE_ENOMEM;
        }
        p->heap_pages = base + (uint32_t)n;
        return (int64_t)p->heap_pages;
    }
    case MEMOP_SHRINK: {
        if (n == 0 || n > p->heap_pages) return AE_EINVAL;
        for (uint64_t i = 0; i < n; i++) {
            p->heap_pages--;
            (void)aspace_unmap(&p->as, uv_heap_page_va(p->heap_pages));
        }
        return (int64_t)p->heap_pages;
    }
    default: return AE_EINVAL;
    }
}
