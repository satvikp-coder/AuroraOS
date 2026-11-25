/*
 * kernel/include/rosys.h - SYS_RO_MAP (12): read-only asset mapping for EL0 processes.
 *
 * Allows EL0 processes to map registered read-only asset regions (firmware blobs,
 * fonts, immutable data) into their address space with PXN/UXN protection.
 *
 * SYS_RO_MAP (x8=12):
 *   x0 = ROOP_MAP (1)     x1 = asset_id, x2 = requested VA (0 = kernel-chosen)
 *                        returns mapped VA in x0, or error code
 *   x0 = ROOP_UNMAP (2)   x1 = VA, x2 = size in pages
 *                        returns 0 on success, error code
 *   x0 = ROOP_INFO (3)    x1 = asset_id
 *                        returns (asset_size_pages << 32) | asset_flags in x0, or error
 */
#ifndef AURORA_ROSYS_H
#define AURORA_ROSYS_H

#include <proc.h>

#define SYS_RO_MAP 12
#define ROOP_MAP   1
#define ROOP_UNMAP 2
#define ROOP_INFO  3

int64_t ro_syscall(struct proc *p, uint64_t op, uint64_t a1, uint64_t a2);

#endif