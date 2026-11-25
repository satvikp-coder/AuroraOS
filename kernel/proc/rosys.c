/*
 * kernel/proc/rosys.c — read-only asset mapping system call (SYS_RO_MAP).
 *
 * This syscall allows EL0 processes to map registered read-only asset regions
 * (firmware blobs, fonts, immutable data) into their address space.
 *
 * SYS_RO_MAP (x8=12):
 *   x0 = ROOP_MAP (1)     x1 = asset_id, x2 = requested VA (0 = kernel-chosen)
 *                        returns mapped VA in x0, or error code
 *   x0 = ROOP_UNMAP (2)   x1 = VA, x2 = size in pages
 *                        returns 0 on success, error code
 *   x0 = ROOP_INFO (3)    x1 = asset_id
 *                        returns (asset_size_pages << 32) | asset_flags in x0, or error
 */
#include <mm.h>
#include <proc.h>

static inline int ro_err(int e) { return e; }

/* Map a registered asset into the current process's address space.
 * Returns mapped VA on success, or negative error code. */
static int64_t ro_map(struct proc *p, int asset_id, uint64_t requested_va)
{
    if (asset_id <= 0 || (uint32_t)asset_id > RO_MAX_ASSETS) return ro_err(AE_EINVAL);

    uint64_t va = ro_map_asset(&p->as, asset_id, requested_va);
    if (!va) return ro_err(AE_ENOMEM);
    return (int64_t)va;
}

/* Unmap an asset region from the current process's address space.
 * Returns 0 on success, negative error code on failure. */
static int64_t ro_unmap(struct proc *p, uint64_t va, uint32_t pages)
{
    if (!p || pages == 0) return ro_err(AE_EINVAL);
    if (va & (PAGE_SIZE - 1)) return ro_err(AE_EINVAL);
    if (va < USER_VA_BASE || va >= USER_VA_LIMIT) return ro_err(AE_EINVAL);
    if ((va >> 30) != (USER_VA_BASE >> 30)) return ro_err(AE_EINVAL);

    int rc = ro_unmap_asset(&p->as, va, pages);
    if (rc != 0) {
        if (rc == -1) return ro_err(AE_EINVAL);
        if (rc == -2) return ro_err(AE_EINVAL);
        return ro_err(AE_EFAULT);
    }
    return 0;
}

/* Get information about a registered asset.
 * Returns (size_pages << 32) | flags on success, negative error code on failure. */
static int64_t ro_info(int asset_id)
{
    struct ro_asset asset;
    if (!ro_lookup_asset(asset_id, &asset)) return ro_err(AE_EINVAL);
    return ((int64_t)asset.pages << 32) | (int64_t)asset.flags;
}

/* Main syscall entry point for SYS_RO_MAP. */
int64_t ro_syscall(struct proc *p, uint64_t op, uint64_t a1, uint64_t a2)
{
    if (!p) return ro_err(AE_EFAULT);

    switch (op) {
    case ROOP_MAP:    return ro_map(p, (int)a1, a2);
    case ROOP_UNMAP:  return ro_unmap(p, a1, (uint32_t)a2);
    case ROOP_INFO:   return ro_info((int)a1);
    default:          return ro_err(AE_EINVAL);
    }
}