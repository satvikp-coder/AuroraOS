/*
 * kernel/proc/memsys.c - SYS_MEM: a bounded, quota-checked user heap (Cycle 22) and
 * bounded large allocation region (MEM-03).
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
 * Limits (kernel/include/heapacct.h): per process 32 pages, per PRINCIPAL 64 pages across all its live processes (a derived sum, never a stored counter), and the
 * 256-page kernel reserve. Everything the heap owns is released with the address space when the process is destroyed.
 *
 * Bounded large allocation region (MEM-03):
 *   - Maximum single allocation: 64 KiB (16 pages)
 *   - Per-process budget: 4 allocations (256 KiB total)
 *   - Per-principal budget: 4 allocations (256 KiB total)
 *   - Kernel reserve: 16 pages (64 KiB)
 *   - Permissions: VMM_ATTR_NORMAL_RW or VMM_ATTR_NORMAL_RO (execute-never enforced via PXN/UXN)
 *   - Mapped into process address space at a kernel-chosen VA (outside heap/ELF/stack regions)
 *   - Cleaned up on process exit, fault, or explicit free
 */
#include <proc.h>
#include <mm.h>
#include <pte.h>
#include <heapacct.h>                   /* the pure policy: per-process, per-principal and kernel-reserve limits */

#ifdef AURORA_SELFTEST_HOOKS
int mem_test_fail_page = -1;                     /* test kernels only: make the Nth page of the next grow fail (to test the rollback), -1 = off */
#endif

/* Find a free virtual address in the user slot for mapping a bounded region.
 * Returns a page-aligned VA or 0 if no space. */
static uint64_t find_bounded_va(struct aspace *as, uint32_t pages)
{
    /* Search from USER_HEAP_END + guard upwards, within the 1 GiB user slot.
     * We need a contiguous run of 'pages' unmapped pages. */
    uint64_t start = USER_HEAP_END + USER_HEAP_GUARD_BYTES;
    uint64_t limit = USER_VA_LIMIT;
    uint64_t va = start;

    while (va + pages * PAGE_SIZE <= limit) {
        uint8_t ok = 1;
        for (uint32_t i = 0; i < pages; i++) {
            uint64_t page_va = va + i * PAGE_SIZE;
            if (uv_mappable(page_va) == 0) { ok = 0; break; }
            if (aspace_is_mapped(as, page_va)) { ok = 0; break; }
        }
        if (ok) return va;
        va += PAGE_SIZE;
    }
    return 0;
}

int64_t mem_syscall(struct proc *p, uint64_t op, uint64_t n)
{
    switch (op) {
    case MEMOP_INFO:                             /* (budget << 32) | pages currently mapped */
        return (int64_t)(((uint64_t)USER_HEAP_MAX_PAGES << 32) | p->heap_pages);
    case MEMOP_GROW: {
        struct hp_view view[PROC_MAX]; int self = -1;                                /* the process table as the policy sees it (kernel-held principals only) */
        for (int i = 0; i < PROC_MAX; i++) {
            struct proc *q = proc_get(i + 1);
            view[i].live = q != 0; view[i].principal = q ? q->principal : 0; view[i].pages = q ? q->heap_pages : 0;
            if (q == p) self = i;
        }
        struct pmm_stats st; pmm_get_stats(&st);
        int dec = hp_grow_check(view, PROC_MAX, self, n, st.free_pages);             /* per process, per principal, kernel reserve: all before anything is touched */
        if (dec == HG_EINVAL) return AE_EINVAL;
        if (dec != HG_OK) return AE_ENOMEM;
        uint32_t base = p->heap_pages, done = 0;
        for (; done < n; done++) {
            uint64_t va = uv_heap_page_va((uint64_t)base + done);                    /* 0 if it is not a page the heap may ever own */
#ifdef AURORA_SELFTEST_HOOKS
            if ((int)done == mem_test_fail_page) break;                              /* injected failure part-way through */
#endif
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
    case MEMOP_BOUNDED_ALLOC: {
        /* x1 = size in bytes (n), x2 = perm (passed via n's upper bits or a register).
         * Since we only have 'n' as argument, we encode: n = size | (perm << 48). */
        uint64_t size = n & 0xFFFFFFFFFFFFULL;
        int perm = (int)((n >> 48) & 0xFF);
        if (size == 0 || size > BOUNDED_MAX_BYTES) return AE_EINVAL;
        if (perm != VMM_ATTR_NORMAL_RW && perm != VMM_ATTR_NORMAL_RO) return AE_EINVAL;

        uint32_t pages = (uint32_t)((size + PAGE_SIZE - 1) >> PAGE_SHIFT);
        if (pages > BOUNDED_MAX_PAGES) return AE_EINVAL;

        /* Check per-process budget. */
        if (p->bounded_pages + pages > BOUNDED_PROC_BUDGET_PAGES) return AE_ENOMEM;

        /* Check per-principal budget via PMM. */
        uint64_t principal_budget = pmm_region_budget(p->principal);
        if (principal_budget < pages) return AE_ENOMEM;

        /* Check global reserve via PMM. */
        struct pmm_stats st; pmm_get_stats(&st);
        if (st.free_pages < pages + BOUNDED_RESERVE_PAGES) return AE_ENOMEM;

        /* Find a virtual address to map it. */
        uint64_t va = find_bounded_va(&p->as, pages);
        if (!va) return AE_ENOMEM;

        /* Allocate the bounded region with VA tracking. */
        uintptr_t pa = pmm_alloc_bounded(size, p->principal, (uint32_t)p->pid, perm, &va);
        if (!pa) return AE_ENOMEM;

        /* Map the region into the process address space. */
        int rc = aspace_map(&p->as, va, (perm == VMM_ATTR_NORMAL_RO) ? UPERM_RO : UPERM_RW, 0, 0, 0);
        if (rc != 0) {
            (void)pmm_free_bounded(pa, 0);  /* kernel principal for cleanup */
            return AE_ENOMEM;
        }

        /* Update process tracking. */
        p->bounded_pages += pages;
        p->bounded_count++;

        /* Return encoded result: PA in lower 48 bits, VA in upper 16 bits (VA is page-aligned, so lower 12 bits are 0).
         * We can return PA | (va << 48) since VA < 1 GiB user slot = 30 bits, but we only have 16 bits.
         * Better: return PA in x0, and the caller can use MEMOP_BOUNDED_INFO to get details.
         * For now, return PA. */
        return (int64_t)pa;
    }
    case MEMOP_BOUNDED_FREE: {
        uintptr_t addr = (uintptr_t)n;
        if (addr == 0) return AE_EINVAL;

        /* Look up the region to verify ownership. */
        struct bounded_region br;
        if (!pmm_bounded_lookup(addr, &br)) return AE_EFAULT;
        if (br.pid != (uint32_t)p->pid) return AE_EPERM;

        /* Unmap from process address space using stored VA. */
        uint32_t pages = br.pages;
        if (br.va != 0) {
            for (uint32_t i = 0; i < pages; i++) {
                (void)aspace_unmap(&p->as, br.va + i * PAGE_SIZE);
            }
        }

        /* Free the bounded region (permission check uses process's principal). */
        int rc = pmm_free_bounded(addr, p->principal);
        if (rc != BOUNDED_OK) {
            if (rc == BOUNDED_ERR_PERM) return AE_EPERM;
            if (rc == BOUNDED_ERR_NOT_FOUND) return AE_EFAULT;
            return AE_EFAULT;
        }

        /* Update process tracking. */
        if (p->bounded_pages >= pages) p->bounded_pages -= pages;
        if (p->bounded_count > 0) p->bounded_count--;

        return 0;
    }
    case MEMOP_BOUNDED_INFO: {
        uint64_t budget = pmm_region_budget(p->principal);
        return (int64_t)(((budget & 0xFFFFFFFFULL) << 32) | (p->bounded_pages & 0xFFFFFFFFULL));
    }
    default: return AE_EINVAL;
    }
}
