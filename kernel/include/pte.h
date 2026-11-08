/*
 * kernel/include/pte.h — AArch64 stage-1 descriptor bits (4 KiB granule), shared by the kernel mapper (vmm.c) and
 * the per-process address spaces (proc/aspace.c). ARM DDI 0487, D8.3.
 *
 * Access permissions AP[2:1] (bits 7:6), at EL1 / EL0:
 *   00  RW / none     01  RW / RW     10  RO / none     11  RO / RO
 * Kernel mappings use 00 or 10 (EL0 cannot touch them: a permission fault); user mappings use 01 or 11.
 */
#ifndef AURORA_PTE_H
#define AURORA_PTE_H

#define PTE_VALID    (1ULL << 0)
#define PTE_TABLE    (1ULL << 1)            /* level 1/2: table descriptor; level 3: page */
#define PTE_ATTRIDX(n) ((uint64_t)(n) << 2)
#define PTE_AP_RO    (1ULL << 7)            /* AP[2]=1, AP[1]=0: read-only at EL1, no EL0 access (kernel) */
#define PTE_AP_EL0_RW (1ULL << 6)           /* AP=01: read/write at EL1 and EL0 (user data, stack) */
#define PTE_AP_EL0_RO (3ULL << 6)           /* AP=11: read-only at EL1 and EL0 (user code) */
#define PTE_SH_OUTER (2ULL << 8)
#define PTE_SH_INNER (3ULL << 8)
#define PTE_AF       (1ULL << 10)
#define PTE_NG       (1ULL << 11)           /* not global: TLB entries are tagged with the ASID (user pages) */
#define PTE_PXN      (1ULL << 53)
#define PTE_UXN      (1ULL << 54)
#define ADDR_MASK    0x0000FFFFFFFFF000ULL

#endif
