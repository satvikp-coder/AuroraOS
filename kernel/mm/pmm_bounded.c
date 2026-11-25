/*
 * kernel/mm/pmm_bounded.c — bounded allocation implementation (MEM-03).
 *
 * This file contains the bounded allocation logic separated from pmm.c
 * so that host tests can compile pmm.c without kernel dependencies.
 */
#include <mm.h>
#include <proc.h>
#include <sched.h>

/* ---- Bounded allocation metadata (MEM-03) ---- */
static struct bounded_region bounded_regions[BOUNDED_MAX_REGIONS];

#define TST(bm, i) (((bm)[(i) >> 6] >> ((i) & 63)) & 1ULL)
#define SET(bm, i) ((bm)[(i) >> 6] |= 1ULL << ((i) & 63))
#define CLR(bm, i) ((bm)[(i) >> 6] &= ~(1ULL << ((i) & 63)))

extern uint64_t used_bm[PMM_MAX_PAGES / 64];
extern uint64_t rsv_bm[PMM_MAX_PAGES / 64];
extern uintptr_t base;
extern uint64_t npages, nfree, nrsv, hint;

static int bounded_perm_to_vmm_attr(uint8_t perm)
{
    switch (perm) {
    case BOUNDED_PERM_RO: return VMM_ATTR_NORMAL_RO;
    case BOUNDED_PERM_RX: return VMM_ATTR_NORMAL_RX;
    case BOUNDED_PERM_RW: return VMM_ATTR_NORMAL_RW;
    default: return 0;
    }
}

static int bounded_find_free_slot(void)
{
    for (uint32_t i = 0; i < BOUNDED_MAX_REGIONS; i++) {
        if (!bounded_regions[i].active) return (int)i;
    }
    return -1;
}

static int bounded_count_active_proc(int pid, uint64_t gen)
{
    int count = 0;
    for (uint32_t i = 0; i < BOUNDED_MAX_REGIONS; i++) {
        if (bounded_regions[i].active &&
            bounded_regions[i].pid == (uint64_t)pid &&
            bounded_regions[i].gen == gen) {
            count++;
        }
    }
    return count;
}

static int bounded_count_active_principal(uint32_t principal)
{
    int count = 0;
    for (uint32_t i = 0; i < BOUNDED_MAX_REGIONS; i++) {
        if (bounded_regions[i].active &&
            bounded_regions[i].principal == principal) {
            count++;
        }
    }
    return count;
}

static uint64_t bounded_alloc_contiguous(uint32_t npages)
{
    if (npages == 0 || npages > BOUNDED_MAX_PAGES) return 0;
    for (uint64_t n = 0; n + npages <= npages; n++) {
        uint64_t i = hint + n; if (i + npages > npages) i = 0;
        int ok = 1;
        for (uint32_t k = 0; k < npages; k++) {
            uint64_t idx = i + k;
            if (TST(used_bm, idx) || TST(rsv_bm, idx)) { ok = 0; break; }
        }
        if (ok) {
            for (uint32_t k = 0; k < npages; k++) {
                uint64_t idx = i + k;
                SET(used_bm, idx); nfree--;
            }
            hint = i + npages;
            volatile uint64_t *p = (volatile uint64_t *)(base + (i << PAGE_SHIFT));
            for (uint64_t k = 0; k < (npages * PAGE_SIZE) / 8; k++) p[k] = 0;
            return base + (i << PAGE_SHIFT);
        }
    }
    return 0;
}

static void bounded_free_contiguous(uint64_t pbase, uint32_t npages)
{
    if (!pbase || npages == 0) return;
    if (!pmm_in_ram(pbase)) return;
    uint64_t i = (pbase - base) >> PAGE_SHIFT;
    for (uint32_t k = 0; k < npages; k++) {
        uint64_t idx = i + k;
        if (!TST(rsv_bm, idx) && TST(used_bm, idx)) {
            CLR(used_bm, idx); nfree++;
        }
    }
    if (i < hint) hint = i;
}

int pmm_bounded_alloc(int pid, uint64_t gen, uint32_t principal,
                      uint32_t npages, uint8_t perm,
                      uint64_t *vbase_out, uint64_t *pbase_out)
{
    if (!vbase_out || !pbase_out) return BOUNDED_ERR_ARG;
    if (pid < 1 || pid > PROC_MAX) return BOUNDED_ERR_ARG;
    if (gen == 0) return BOUNDED_ERR_ARG;
    if (npages == 0 || npages > BOUNDED_MAX_PAGES) return BOUNDED_ERR_ARG;
    if (perm != BOUNDED_PERM_RO && perm != BOUNDED_PERM_RX && perm != BOUNDED_PERM_RW) return BOUNDED_ERR_ARG;

    struct proc *p = proc_get(pid);
    if (!p) return BOUNDED_ERR_ARG;
    if (p->gen != gen) return BOUNDED_ERR_GEN;

    /* Per-process budget check */
    if (bounded_count_active_proc(pid, gen) >= BOUNDED_PROC_BUDGET) return BOUNDED_ERR_BUDGET;
    /* Per-principal budget check */
    if (principal != PRINCIPAL_NONE && bounded_count_active_principal(principal) >= BOUNDED_PRINCIPAL_BUDGET) return BOUNDED_ERR_BUDGET;
    /* Kernel reserve check */
    struct pmm_stats st; pmm_get_stats(&st);
    if (st.free_pages < npages + BOUNDED_RESERVE_PAGES) return BOUNDED_ERR_NOMEM;

    /* Find a free slot */
    int slot = bounded_find_free_slot();
    if (slot < 0) return BOUNDED_ERR_NOSLOT;

    /* Allocate contiguous physical pages */
    uint64_t pbase = bounded_alloc_contiguous(npages);
    if (!pbase) return BOUNDED_ERR_NOMEM;

    /* Assign virtual address: each allocation gets a page-aligned VA in the process's bounded region.
     * We use a simple scheme: USER_BOUNDED_VA_BASE + (slot * BOUNDED_MAX_PAGES * PAGE_SIZE) */
    uint64_t vbase = USER_BOUNDED_VA_BASE + (slot * BOUNDED_MAX_PAGES * PAGE_SIZE);

    /* Map into the process address space */
    int attr = bounded_perm_to_vmm_attr(perm);
    if (attr == 0) {
        bounded_free_contiguous(pbase, npages);
        return BOUNDED_ERR_ARG;
    }
    if (aspace_map_contiguous(&p->as, vbase, attr, 0, npages, 0) != 0) {
        bounded_free_contiguous(pbase, npages);
        return BOUNDED_ERR_NOMEM;
    }

    /* Record the allocation */
    bounded_regions[slot] = (struct bounded_region){
        .pid = pid,
        .gen = gen,
        .principal = principal,
        .vbase = vbase,
        .npages = npages,
        .pbase = pbase,
        .perm = perm,
        .active = 1,
        .alloc_tick = sched_ticks(),
        .free_tick = 0,
    };

    p->bounded_pages += npages;
    p->bounded_count++;

    *vbase_out = vbase;
    *pbase_out = pbase;
    return BOUNDED_OK;
}

int pmm_bounded_free(int pid, uint64_t gen, uint64_t vbase)
{
    if (pid < 1 || pid > PROC_MAX) return BOUNDED_ERR_ARG;
    if (gen == 0) return BOUNDED_ERR_ARG;
    if (vbase & (PAGE_SIZE - 1)) return BOUNDED_ERR_ARG;

    struct proc *p = proc_get(pid);
    if (!p) return BOUNDED_ERR_ARG;
    if (p->gen != gen) return BOUNDED_ERR_GEN;

    /* Find the region by VA */
    for (uint32_t i = 0; i < BOUNDED_MAX_REGIONS; i++) {
        if (bounded_regions[i].active &&
            bounded_regions[i].pid == (uint64_t)pid &&
            bounded_regions[i].gen == gen &&
            bounded_regions[i].vbase == vbase) {

            struct bounded_region *br = &bounded_regions[i];

            /* Unmap from process address space */
            (void)aspace_unmap(&p->as, vbase);

            /* Free physical pages */
            bounded_free_contiguous(br->pbase, br->npages);

            /* Update process accounting */
            if (p->bounded_pages >= br->npages) p->bounded_pages -= br->npages;
            if (p->bounded_count > 0) p->bounded_count--;

            /* Mark region as freed */
            br->active = 0;
            br->free_tick = sched_ticks();

            return BOUNDED_OK;
        }
    }
    return BOUNDED_ERR_NOT_FOUND;
}

int pmm_bounded_lookup_by_va(int pid, uint64_t gen, uint64_t vbase,
                             struct bounded_region *out)
{
    if (!out) return BOUNDED_ERR_ARG;
    if (pid < 1 || pid > PROC_MAX) return BOUNDED_ERR_ARG;
    if (gen == 0) return BOUNDED_ERR_ARG;

    for (uint32_t i = 0; i < BOUNDED_MAX_REGIONS; i++) {
        if (bounded_regions[i].active &&
            bounded_regions[i].pid == (uint64_t)pid &&
            bounded_regions[i].gen == gen &&
            bounded_regions[i].vbase == vbase) {
            *out = bounded_regions[i];
            return BOUNDED_OK;
        }
    }
    return BOUNDED_ERR_NOT_FOUND;
}

int pmm_bounded_lookup_by_generation(int pid, uint64_t gen,
                                     struct bounded_region *out, int max,
                                     int *count)
{
    if (!out || max <= 0 || !count) return BOUNDED_ERR_ARG;
    if (pid < 1 || pid > PROC_MAX) return BOUNDED_ERR_ARG;
    if (gen == 0) return BOUNDED_ERR_ARG;

    *count = 0;
    for (uint32_t i = 0; i < BOUNDED_MAX_REGIONS && *count < max; i++) {
        if (bounded_regions[i].active &&
            bounded_regions[i].pid == (uint64_t)pid &&
            bounded_regions[i].gen == gen) {
            out[*count] = bounded_regions[i];
            (*count)++;
        }
    }
    return BOUNDED_OK;
}

int pmm_bounded_cleanup_proc(int pid, uint64_t gen)
{
    if (pid < 1 || pid > PROC_MAX) return BOUNDED_ERR_ARG;
    if (gen == 0) return BOUNDED_ERR_ARG;

    int freed = 0;
    struct proc *p = proc_get(pid);
    if (!p) return BOUNDED_ERR_ARG;

    for (uint32_t i = 0; i < BOUNDED_MAX_REGIONS; i++) {
        if (bounded_regions[i].active &&
            bounded_regions[i].pid == (uint64_t)pid &&
            bounded_regions[i].gen == gen) {

            struct bounded_region *br = &bounded_regions[i];

            /* Unmap from process address space */
            (void)aspace_unmap(&p->as, br->vbase);

            /* Free physical pages - accounting is released exactly once */
            bounded_free_contiguous(br->pbase, br->npages);

            br->active = 0;
            br->free_tick = sched_ticks();
            freed++;
        }
    }
    return freed;
}

uint32_t pmm_bounded_count_proc(int pid, uint64_t gen)
{
    return (uint32_t)bounded_count_active_proc(pid, gen);
}

uint32_t pmm_bounded_count_principal(uint32_t principal)
{
    return (uint32_t)bounded_count_active_principal(principal);
}

void pmm_bounded_get_stats(struct pmm_stats *s)
{
    pmm_get_stats(s);  /* Reuse the same stats structure */
}