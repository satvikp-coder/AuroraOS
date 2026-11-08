/*
 * kernel/include/mm.h — physical and virtual memory management interfaces.
 * Target: QEMU ARM64 "virt", 4 KiB pages, 39-bit stage-1 translation at EL1.
 */
#ifndef AURORA_MM_H
#define AURORA_MM_H
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE  4096UL
#define PAGE_SHIFT 12

/* ---- Device tree ---------------------------------------------------- */
/* Upper bound on reservations we can represent. A device tree with more is REJECTED
 * (FDT_ERR_TOO_MANY_RESERVATIONS); reservations are never silently dropped. */
#define FDT_MAX_RESERVED 32
enum {
    FDT_OK = 0,
    FDT_ERR_NULL = -1,
    FDT_ERR_MAGIC = -2,                  /* null/unaligned pointer or wrong magic */
    FDT_ERR_HEADER = -3,                 /* header fields inconsistent or out of bounds */
    FDT_ERR_STRUCT = -4,                 /* malformed structure block */
    FDT_ERR_CELLS = -5,                  /* unsupported/malformed #address-cells, #size-cells or reg length */
    FDT_ERR_NO_MEMORY = -6,              /* no usable /memory node */
    FDT_ERR_TOO_MANY_RESERVATIONS = -7,
    FDT_ERR_MULTI_MEMORY = -8,           /* more than one RAM region: unsupported, so rejected */
    FDT_ERR_RESERVATION = -9,            /* zero size or addr+size overflow */
    FDT_ERR_STRING = -10,                /* property name not NUL-terminated inside the strings block */
};
struct fdt_reservation { uint64_t addr, size; int nomap; };
struct fdt_info {
    int      valid;                 /* header + memory node parsed OK */
    uint64_t ram_base, ram_size;    /* the single /memory region */
    uint32_t totalsize;             /* size of the blob itself */
    int      nreserved;
    struct fdt_reservation reserved[FDT_MAX_RESERVED]; /* memory reserve map + /reserved-memory children with reg */
};
int fdt_parse(const void *fdt, struct fdt_info *out);   /* FDT_OK or FDT_ERR_* */
const char *fdt_strerror(int err);

/* Interrupt controller and architected timer, from a blob that fdt_parse() accepts. */
struct fdt_platform {
    int      gic_version;                    /* 0 none found, 2 = GICv2 (supported), 3 = GICv3 (recognised only) */
    uint64_t gicd_base, gicd_size;           /* GICv2 distributor */
    uint64_t gicc_base, gicc_size;           /* GICv2 CPU interface */
    int      have_timer;
    uint32_t timer_intid_phys_ns;            /* GIC interrupt ID of the non-secure EL1 physical timer */
    uint32_t timer_intid_virt;               /* ... of the virtual timer */
};
int fdt_platform_parse(const void *fdt, struct fdt_platform *out);

/* ---- Physical page allocator ---------------------------------------- */
#define PMM_MAX_RAM   (1UL << 30)                 /* managed RAM cap: 1 GiB */
#define PMM_MAX_PAGES (PMM_MAX_RAM >> PAGE_SHIFT)

enum { PMM_OK = 0, PMM_ERR_ALIGN = -1, PMM_ERR_RANGE = -2, PMM_ERR_RESERVED = -3,
       PMM_ERR_DOUBLE = -4, PMM_ERR_STATE = -5 };

struct pmm_stats {
    uint64_t total_pages, free_pages, reserved_pages, allocated_pages;
    uint64_t alloc_failures, free_errors;
};

int       pmm_init(uintptr_t ram_base, uint64_t ram_size);
int       pmm_reserve(uintptr_t start, uintptr_t end);   /* [start,end), rounded outward, clamped to RAM */
int       pmm_reserve_range(uint64_t addr, uint64_t size); /* overflow-checked; -3 if malformed */
uintptr_t pmm_alloc_page(void);                          /* zeroed page, 0 if exhausted */
int       pmm_free_page(uintptr_t addr);                 /* PMM_* error code */
int       pmm_is_reserved(uintptr_t addr);
int       pmm_is_allocated(uintptr_t addr);
int       pmm_in_ram(uintptr_t addr);
uintptr_t pmm_ram_base(void);
uintptr_t pmm_ram_end(void);
void      pmm_get_stats(struct pmm_stats *s);

/* ---- Virtual memory (stage-1, identity map + extra mappings) -------- */
#define VMM_ATTR_NORMAL_RW   1   /* normal WB, read/write, never execute */
#define VMM_ATTR_NORMAL_RO   2   /* normal WB, read-only, never execute  */
#define VMM_ATTR_NORMAL_RX   3   /* normal WB, read-only, executable     */
#define VMM_ATTR_DEVICE      4   /* Device-nGnRnE, read/write, no exec   */

enum { VMM_OK = 0, VMM_ERR_ARG = -1, VMM_ERR_ALIGN = -2, VMM_ERR_RANGE = -3, VMM_ERR_NOMEM = -4,
       VMM_ERR_CONFLICT = -5, /* overlaps an existing mapping or block */
       VMM_ERR_POLICY = -6    /* physical-target policy denied it; see vmm_last_denial() / mmpolicy.h */ };

int      vmm_last_denial(void);                    /* MMP_DENY_* reason of the most recent VMM_ERR_POLICY */
int      vmm_register_mmio(uint64_t addr, uint64_t size); /* declare a device window (only Device mappings may target it) */

int      vmm_add_nomap(uint64_t addr, uint64_t size);   /* device-tree no-map region: left unmapped (call before vmm_init) */
int      vmm_pa_bits(void);                       /* implemented physical address width */
int      vmm_init(void);                         /* build identity tables (MMU still off) */
void     vmm_enable(void);                       /* turn the MMU on */
/* All-or-nothing: returns VMM_OK, or a VMM_ERR_* code with NO descriptors changed. Never remaps. */
int      vmm_map(uint64_t va, uint64_t pa, uint64_t size, int attr);
int      vmm_mmu_enabled(void);
uint64_t vmm_kernel_l1_pa(void);                 /* master table; shared (slots 0-1) into every process */
const uint64_t *vmm_kernel_l1(void);
int      vmm_hw_translate_el0(uint64_t va, int write, uint64_t *pa);   /* as EL0 would see it */
/* Hardware translation via AT: returns 0 and *pa on success, -1 if it would fault. */
int      vmm_hw_translate(uint64_t va, int write, uint64_t *pa);

/* Linker-provided section boundaries */
extern uint8_t __image_start[], __text_start[], __text_end[], __rodata_start[], __rodata_end[],
               __data_start[], __data_end[], __stack_guard[], __stack_bottom[], __stack_top[],
               __exc_guard[], __exc_stack_bottom[], __exc_stack_top[], __kernel_end[];

#endif
