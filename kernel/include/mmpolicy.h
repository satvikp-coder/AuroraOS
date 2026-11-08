/*
 * kernel/include/mmpolicy.h — physical-target policy for virtual mappings.
 *
 * vmm.c builds page tables; this module decides WHICH physical ranges may be mapped with WHICH
 * attributes. It is pure (no hardware access), so it is unit-tested on the host and used
 * unchanged by the kernel. Every mapping, including the boot identity map, goes through it.
 *
 * Scope: it enforces correct mapping CONTRACTS. It does not protect against malicious EL1 code,
 * which can rewrite its own page tables.
 */
#ifndef AURORA_MMPOLICY_H
#define AURORA_MMPOLICY_H
#include <stdint.h>

#define MMP_MAX_NOMAP 32
#define MMP_MAX_MMIO  8

/* Attribute numbers are shared with vmm_map() (kernel/include/mm.h). */
#define MMP_ATTR_NORMAL_RW 1
#define MMP_ATTR_NORMAL_RO 2
#define MMP_ATTR_NORMAL_RX 3
#define MMP_ATTR_DEVICE    4

struct mm_range { uint64_t start, end; };       /* [start, end) */

struct mm_policy {
    struct mm_range ram;                        /* managed RAM */
    struct mm_range text;                       /* kernel text: never writable, the only executable memory */
    struct mm_range rodata;                     /* kernel read-only data: never writable */
    struct mm_range nomap[MMP_MAX_NOMAP];       /* device-tree no-map regions: never mappable by any API */
    int nnomap;
    struct mm_range mmio[MMP_MAX_MMIO];         /* registered device windows: Device attributes only */
    int nmmio;
};

enum {
    MMP_OK = 0,
    MMP_DENY_OVERFLOW = -1,                     /* size 0 or pa+size overflows */
    MMP_DENY_ATTR = -2,                         /* unknown attribute */
    MMP_DENY_NOMAP = -3,                        /* touches a no-map region */
    MMP_DENY_NORMAL_ON_MMIO = -4,               /* cacheable mapping of a device window */
    MMP_DENY_NORMAL_OUTSIDE_RAM = -5,           /* cacheable mapping of something that is not wholly RAM */
    MMP_DENY_DEVICE_ON_RAM = -6,                /* device attributes on RAM */
    MMP_DENY_DEVICE_OUTSIDE_WINDOW = -7,        /* device mapping not wholly inside a registered window */
    MMP_DENY_TEXT_WRITE = -8,                   /* writable alias of kernel text */
    MMP_DENY_RODATA_WRITE = -9,                 /* writable alias of kernel rodata */
    MMP_DENY_EXEC_OUTSIDE_TEXT = -10,           /* executable mapping of anything but kernel text */
    MMP_DENY_BAD_REGISTRATION = -11,            /* mm_policy_add_*: bad or conflicting range */
};

/* Check whether [pa, pa+size) may be mapped with `attr`. MMP_OK or an MMP_DENY_* reason. */
int mm_policy_check(const struct mm_policy *p, uint64_t pa, uint64_t size, int attr);
const char *mm_policy_reason(int rc);

/* Registration (boot time). Both reject empty/overflowing ranges and ranges that conflict with RAM/no-map. */
int mm_policy_add_nomap(struct mm_policy *p, uint64_t addr, uint64_t size);
int mm_policy_add_mmio(struct mm_policy *p, uint64_t addr, uint64_t size);

#endif
