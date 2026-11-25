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
 * Limits (kernel/include/heapacct.h): per process 32 pages, per PRINCIPAL 64 pages across all its live processes (a derived sum, never a stored counter), and the
 * 256-page kernel reserve. Everything the heap owns is released with the address space when the process is destroyed.
 *
 * Bounded allocations (MEM-03): separate from the heap. A process can allocate up to BOUNDED_PROC_BUDGET
 * regions of up to BOUNDED_MAX_PAGES pages each (max 64 KiB), with per-principal budget BOUNDED_PRINCIPAL_BUDGET.
 * These are for read-only asset windows, model data staging, etc. EL0 receives only a virtual address.
 */
#include <proc.h>
#include <mm.h>
#include <heapacct.h>                   /* the pure policy: per-process, per-principal and kernel-reserve limits */

#ifdef AURORA_SELFTEST_HOOKS
int mem_test_fail_page = -1;                     /* test kernels only: make the Nth page of the next grow fail (to test the rollback), -1 = off */
#endif

/* Packed arguments for MEMOP_BOUNDED_ALLOC: x1 = npages, x2 = perm, x3 = principal (or PRINCIPAL_NONE).
 * Returns (vbase << 32) | npages on success, or AE_* error. EL0 must never learn physical addresses. */
static int64_t mem_bounded_alloc(struct proc *p, uint64_t npages, uint64_t perm, uint64_t principal)
{
    if (npages == 0 || npages > BOUNDED_MAX_PAGES) return AE_EINVAL;
    if (perm != BOUNDED_PERM_RO && perm != BOUNDED_PERM_RX && perm != BOUNDED_PERM_RW) return AE_EINVAL;
    if (principal != PRINCIPAL_NONE && !principal_assignable((uint32_t)principal)) return AE_EPERM;

    uint64_t vbase, pbase;
    int rc = pmm_bounded_alloc(p->pid, p->gen, (uint32_t)principal, (uint32_t)npages, (uint8_t)perm, &vbase, &pbase);
    if (rc != BOUNDED_OK) {
        switch (rc) {
        case BOUNDED_ERR_BUDGET: return AE_ENOMEM;
        case BOUNDED_ERR_NOMEM: return AE_ENOMEM;
        case BOUNDED_ERR_GEN: return AE_EINVAL;
        case BOUNDED_ERR_PERM: return AE_EPERM;
        default: return AE_EINVAL;
        }
    }
    /* Return vbase in upper 32 bits, npages in lower 32 bits. EL0 must never learn physical addresses. */
    return (int64_t)((vbase << 32) | npages);
}

/* MEMOP_BOUNDED_FREE: x1 = vbase. Returns 0 on success, AE_* error. */
static int64_t mem_bounded_free(struct proc *p, uint64_t vbase)
{
    int rc = pmm_bounded_free(p->pid, p->gen, vbase);
    if (rc != BOUNDED_OK) {
        switch (rc) {
        case BOUNDED_ERR_NOT_FOUND: return AE_EFAULT;
        case BOUNDED_ERR_GEN: return AE_EINVAL;
        case BOUNDED_ERR_STATE: return AE_EINVAL;
        default: return AE_EINVAL;
        }
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
        /* n = npages, x2 = perm, x3 = principal (passed via registers in sched.c) */
        return AE_ENOSYS;  /* Will be handled in sched.c with extended arguments */
    }
    case MEMOP_BOUNDED_FREE: {
        return AE_ENOSYS;  /* Will be handled in sched.c with extended arguments */
    }
    default: return AE_EINVAL;
    }
}

/* Extended mem_syscall for bounded operations with full register access. */
int64_t mem_syscall_extended(struct proc *p, uint64_t op, uint64_t n, uint64_t arg2, uint64_t arg3)
{
    switch (op) {
    case MEMOP_BOUNDED_ALLOC:
        return mem_bounded_alloc(p, n, arg2, arg3);
    case MEMOP_BOUNDED_FREE:
        return mem_bounded_free(p, n);
    default:
        return mem_syscall(p, op, n);
    }
}
