/*
 * kernel/mm/ro.c — Read-only asset management (MEM-04).
 *
 * Asset region tracking and mapping into EL0 process address spaces.
 * Assets are reserved physical pages pre-populated with immutable data
 * (firmware blobs, fonts, etc.) that can be mapped read-only into processes.
 */
#include <mm.h>
#include <proc.h>
#include <uvlayout.h>

/* ---- Read-only asset tracking (MEM-04) --------------------------------- */
static struct ro_asset ro_assets[RO_MAX_ASSETS];
static uint32_t ro_asset_count = 0;

/* Register a read-only asset region (kernel-only, called during init).
 * pa must be page-aligned, size > 0. Returns asset_id (1..RO_MAX_ASSETS) or 0 on failure. */
int ro_register_asset(uintptr_t pa, uint32_t pages, uint32_t flags)
{
    if (!pa || pages == 0 || pages > RO_MAX_PAGES) return 0;
    if (ro_asset_count >= RO_MAX_ASSETS) return 0;

    /* Verify the physical pages are reserved (in rsv_bm). */
    if (!pmm_in_ram(pa)) return 0;
    uint64_t i = (pa - pmm_ram_base()) >> PAGE_SHIFT;
    struct pmm_stats st; pmm_get_stats(&st);
    if (i + pages > st.total_pages) return 0;  /* simplified check */
    for (uint32_t k = 0; k < pages; k++) {
        if (!pmm_is_reserved(pa + k * PAGE_SIZE)) return 0;  /* not reserved: not a valid asset */
    }

    int slot = -1;
    for (uint32_t j = 0; j < RO_MAX_ASSETS; j++) {
        if (!ro_assets[j].in_use) { slot = j; break; }
    }
    if (slot < 0) return 0;

    ro_assets[slot].pa = pa;
    ro_assets[slot].pages = pages;
    ro_assets[slot].flags = flags;
    ro_assets[slot].in_use = 1;
    ro_asset_count++;

    return slot + 1;  /* asset_id is 1-based */
}

/* Look up an asset by ID. Returns 1 if found, 0 otherwise. */
int ro_lookup_asset(int asset_id, struct ro_asset *out)
{
    if (asset_id <= 0 || (uint32_t)asset_id > RO_MAX_ASSETS) return 0;
    if (!ro_assets[asset_id - 1].in_use) return 0;
    if (out) *out = ro_assets[asset_id - 1];
    return 1;
}

/* Map a registered asset into a process address space read-only.
 * Returns mapped VA on success, 0 on failure. */
uint64_t ro_map_asset(struct aspace *as, int asset_id, uint64_t requested_va)
{
    struct ro_asset asset;
    if (!ro_lookup_asset(asset_id, &asset)) return 0;
    if (!as || !as->live) return 0;

    uint64_t size = (uint64_t)asset.pages * PAGE_SIZE;
    uint64_t va;

    if (requested_va != 0) {
        /* User requested a specific VA: validate it. */
        if ((requested_va & (PAGE_SIZE - 1)) || requested_va < USER_VA_BASE || requested_va >= USER_VA_LIMIT)
            return 0;
        if ((requested_va >> 30) != (USER_VA_BASE >> 30)) return 0;
        /* Check all pages are mappable and unmapped. */
        for (uint32_t p = 0; p < asset.pages; p++) {
            uint64_t page_va = requested_va + p * PAGE_SIZE;
            if (!uv_mappable(page_va)) return 0;
            if (aspace_is_mapped(as, page_va)) return 0;
        }
        va = requested_va;
    } else {
        /* Kernel chooses VA: search from USER_HEAP_END + guard upwards. */
        uint64_t start = USER_HEAP_END + USER_HEAP_GUARD_BYTES;
        uint64_t limit = USER_VA_LIMIT;
        uint8_t found = 0;
        for (va = start; va + size <= limit; va += PAGE_SIZE) {
            uint8_t ok = 1;
            for (uint32_t p = 0; p < asset.pages; p++) {
                uint64_t page_va = va + p * PAGE_SIZE;
                if (!uv_mappable(page_va) || aspace_is_mapped(as, page_va)) { ok = 0; break; }
            }
            if (ok) { found = 1; break; }
        }
        if (!found) return 0;
    }

    /* Map the asset pages read-only into the process address space. */
    uint64_t pa = asset.pa;
    for (uint32_t p = 0; p < asset.pages; p++) {
        int rc = aspace_map_ro(as, va + p * PAGE_SIZE, pa + p * PAGE_SIZE);
        if (rc != 0) {
            /* Rollback on failure. */
            for (uint32_t q = 0; q < p; q++)
                (void)aspace_unmap(as, va + q * PAGE_SIZE);
            return 0;
        }
    }
    return va;
}

/* Unmap an asset from a process address space. Returns 0 on success, error code on failure. */
int ro_unmap_asset(struct aspace *as, uint64_t va, uint32_t pages)
{
    if (!as || !as->live) return -1;
    if ((va & (PAGE_SIZE - 1)) || pages == 0) return -2;
    if (va < USER_VA_BASE || va >= USER_VA_LIMIT) return -2;
    if ((va >> 30) != (USER_VA_BASE >> 30)) return -2;

    for (uint32_t p = 0; p < pages; p++) {
        int rc = aspace_unmap_ro(as, va + p * PAGE_SIZE);
        if (rc != 0) return rc;
    }
    return 0;
}

/* Clean up all RO mappings for a process (called on exit/fault). */
void ro_cleanup_proc(struct aspace *as)
{
    if (!as || !as->live) return;
    /* The aspace_destroy() will free all user pages including RO mappings.
     * No additional cleanup needed for the asset registry itself (assets are global). */
    (void)as;
}