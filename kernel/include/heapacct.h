#ifndef AURORA_HEAPACCT_H
#define AURORA_HEAPACCT_H
#include <stdint.h>
#include <uvlayout.h>

/*
 * Heap accounting policy for SYS_MEM, pure and header-only so the host tests run the REAL decision code against a reference model.
 *
 * Three limits, checked in this order, all before anything is touched:
 *   1. per process:   a process owns at most USER_HEAP_MAX_PAGES heap pages (32 = 128 KiB);
 *   2. per principal: ALL live processes holding the same kernel-chosen principal together own at most HEAP_PRINCIPAL_BUDGET_PAGES (64 = 256 KiB), so starting
 *                     more instances does not multiply a principal's memory. Processes that hold NO principal (PRINCIPAL_NONE) are one shared group, so not
 *                     having an identity is never a way around the budget;
 *   3. the machine:   the kernel keeps HEAP_RESERVE_PAGES (256 = 1 MiB) free for itself, the compositor and the loader; a heap can never take them.
 *
 * There is NO stored counter. A principal's usage is the SUM of heap_pages over every process slot that is not UNUSED and holds that principal, computed
 * from the process table each time. A charge can therefore never go stale, underflow, be released twice or survive a pid reuse: it exists exactly while
 * the pages do (a FAULTED or TERMINATED process still owns its pages until it is destroyed, so it still counts). The principal is chosen by kernel code
 * (the launch registry or proc_set_principal), never by EL0. The sum is over at most PROC_MAX slots, so the cost is negligible.
 *
 * The principal budget is a POLICY number, not derived from the RAM size: it is twice the per-process budget, small enough that even the smallest supported
 * RAM (128 MiB = 32768 pages) loses at most 0.2 percent per principal, and the physical check (3) is what actually protects the kernel on small machines.
 * proc_set_principal does not retroactively revoke pages a process already holds; it only stops further growth over the new group's budget.
 */
#define HEAP_PRINCIPAL_BUDGET_PAGES (2u * USER_HEAP_MAX_PAGES)
#define HEAP_RESERVE_PAGES 256u

struct hp_view { uint8_t live; uint32_t principal; uint32_t pages; };    /* one process slot as the policy sees it */
enum { HG_OK = 0, HG_EINVAL = -1, HG_ENOMEM = -2 };

/* Pages owned by all live slots holding `principal`. */
static inline uint64_t hp_principal_total(const struct hp_view *v, int n, uint32_t principal)
{
    uint64_t t = 0;
    for (int i = 0; i < n; i++) if (v[i].live && v[i].principal == principal) t += v[i].pages;
    return t;
}

/* May slot `self` grow its heap by `pages`, given `free_pages` physical pages free right now? */
static inline int hp_grow_check(const struct hp_view *v, int n, int self, uint64_t pages, uint64_t free_pages)
{
    if (pages == 0 || pages > USER_HEAP_MAX_PAGES) return HG_EINVAL;                                  /* also rejects any value that could overflow later arithmetic */
    if (!v || self < 0 || self >= n || !v[self].live) return HG_EINVAL;
    if ((uint64_t)v[self].pages + pages > USER_HEAP_MAX_PAGES) return HG_ENOMEM;                      /* 1. per process */
    if (hp_principal_total(v, n, v[self].principal) + pages > HEAP_PRINCIPAL_BUDGET_PAGES) return HG_ENOMEM;   /* 2. per principal */
    if (free_pages < pages + HEAP_RESERVE_PAGES) return HG_ENOMEM;                                    /* 3. the kernel's reserve */
    return HG_OK;
}
#endif
