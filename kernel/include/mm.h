/*
 * kernel/include/mm.h — physical and virtual memory management interfaces.
 * Target: QEMU ARM64 "virt", 4 KiB pages, 39-bit stage-1 translation at EL1.
 */
#ifndef AURORA_MM_H
#define AURORA_MM_H
#include <stddef.h>
#include <stdint.h>

/* Forward declaration for struct aspace (defined in proc.h) */
struct aspace;

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

/* virtio-mmio transports described by the device tree (count returned, or a negative FDT_ERR_*). */
struct fdt_virtio { uint64_t base, size; uint32_t intid; };
int fdt_virtio_mmio(const void *fdt, struct fdt_virtio *out, int max);
int fdt_fwcfg_mmio(const void *fdt, struct fdt_virtio *out, int max);   /* QEMU fw_cfg MMIO nodes (compatible "qemu,fw-cfg-mmio") */

/* ---- Physical page allocator ---------------------------------------- */
#define PMM_MAX_RAM   (1UL << 30)                 /* managed RAM cap: 1 GiB */
#define PMM_MAX_PAGES (PMM_MAX_RAM >> PAGE_SHIFT)

enum { PMM_OK = 0, PMM_ERR_ALIGN = -1, PMM_ERR_RANGE = -2, PMM_ERR_RESERVED = -3,
       PMM_ERR_DOUBLE = -4, PMM_ERR_STATE = -5 };

struct pmm_stats {
    uint64_t total_pages, free_pages, reserved_pages, allocated_pages;
    uint64_t alloc_failures, free_errors;
};

/* ---- Bounded large allocation region (MEM-03) ----------------------- */
/* Maximum size for a single bounded allocation: 64 KiB = 16 pages. */
#define BOUNDED_MAX_PAGES   16u
#define BOUNDED_MAX_BYTES   (BOUNDED_MAX_PAGES * PAGE_SIZE)

/* Per-process budget: maximum 4 bounded allocations (64 KiB each = 256 KiB). */
#define BOUNDED_PROC_BUDGET_PAGES  (4u * BOUNDED_MAX_PAGES)

/* Per-principal budget: same as process budget (shared across processes). */
#define BOUNDED_PRINCIPAL_BUDGET_PAGES  (4u * BOUNDED_MAX_PAGES)

/* Kernel reserve for bounded allocations: 16 pages = 64 KiB. */
#define BOUNDED_RESERVE_PAGES  16u

enum {
    BOUNDED_OK = 0,
    BOUNDED_ERR_ARG = -1,          /* invalid arguments (size, alignment, principal) */
    BOUNDED_ERR_NOMEM = -2,        /* out of physical pages or budget exceeded */
    BOUNDED_ERR_BUDGET = -3,       /* per-process or per-principal budget exceeded */
    BOUNDED_ERR_NOT_FOUND = -4,    /* address not found in caller's allocations */
    BOUNDED_ERR_PERM = -5,         /* permission check failed */
    BOUNDED_ERR_STATE = -6,        /* allocator not initialized or invalid state */
    BOUNDED_ERR_ALIGN = -7,        /* address not page-aligned */
    BOUNDED_ERR_RANGE = -8,        /* address outside managed RAM */
    BOUNDED_ERR_RESERVED = -9,     /* page is permanently reserved */
    BOUNDED_ERR_DOUBLE = -10,      /* double-free or free of unallocated page */
};

struct bounded_region {
    uintptr_t pa;              /* physical base address (page-aligned) */
    uint64_t  va;              /* virtual address in process address space (page-aligned) */
    uint32_t  pages;           /* number of pages in this region */
    uint32_t  perm;            /* VMM_ATTR_* permissions for mapping */
    uint32_t  principal;       /* owning principal */
    uint32_t  pid;             /* owning process PID */
    uint8_t   in_use;          /* 1 = active, 0 = free */
};

/* Allocate a bounded region: size bytes (rounded up to pages, max BOUNDED_MAX_PAGES),
 * principal = owning principal (0 = kernel, PRINCIPAL_NONE = unprivileged),
 * pid = owning process PID (0 for kernel allocations),
 * va_out = receives the virtual address where the region is mapped.
 * Returns physical base address, or 0 on failure. */
uintptr_t pmm_alloc_bounded(uint64_t size, uint32_t principal, uint32_t pid, int perm, uint64_t *va_out);

/* Free a bounded region by its physical base address. Only the owning process
 * or kernel may free it. Returns BOUNDED_OK or error code. */
int pmm_free_bounded(uintptr_t addr, uint32_t caller_principal);

/* Return remaining budget (in pages) for a principal across all live processes.
 * Returns 0 if principal has no budget or budget exhausted. */
uint64_t pmm_region_budget(uint32_t principal);

/* Get bounded region info by physical address. Returns 1 if found, 0 otherwise. */
int pmm_bounded_lookup(uintptr_t addr, struct bounded_region *out);

/* Get bounded region info by virtual address. Returns 1 if found, 0 otherwise. */
int pmm_bounded_lookup_by_va(uint64_t va, struct bounded_region *out);

/* Initialize bounded allocator (called from pmm_init). */
void pmm_bounded_init(void);

/* Cleanup all bounded regions owned by a process (called on process exit/fault).
 * Unmaps VAs from the provided address space before freeing physical pages. */
void pmm_bounded_cleanup_proc(uint32_t pid, struct aspace *as);

/* Count bounded pages currently allocated by a process. */
uint32_t pmm_bounded_proc_pages(uint32_t pid);

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

/* ---- Read-only asset mapping (MEM-04) --------------------------------- */
/* Asset region tracking. Assets are reserved physical pages pre-populated with data. */
#define RO_MAX_ASSETS    32u
#define RO_MAX_PAGES     64u   /* max pages per asset (256 KiB) */

struct ro_asset {
    uintptr_t pa;              /* physical base address (page-aligned) */
    uint32_t  pages;           /* number of pages in this asset */
    uint32_t  flags;           /* RO_ASSET_* flags */
    uint8_t   in_use;          /* 1 = active, 0 = free */
};
#define RO_ASSET_NONE     0
#define RO_ASSET_KERNEL   (1u << 0)  /* kernel-owned, cannot be freed by processes */

/* Map physical pages read-only into kernel virtual address space.
 * va, pa, size must be page-aligned. Returns 0 on success, VMM_ERR_* on failure. */
int      mmu_map_ro(uint64_t va, uint64_t pa, uint64_t size);

/* Unmap read-only region from kernel virtual address space.
 * va, size must be page-aligned. Returns 0 on success, VMM_ERR_* on failure. */
int      mmu_unmap_ro(uint64_t va, uint64_t size);

/* Register a read-only asset region (kernel-only, called during init).
 * pa must be page-aligned, size > 0. Returns asset_id (1..RO_MAX_ASSETS) or 0 on failure. */
int      ro_register_asset(uintptr_t pa, uint32_t pages, uint32_t flags);

/* Look up an asset by ID. Returns 1 if found, 0 otherwise. */
int      ro_lookup_asset(int asset_id, struct ro_asset *out);

/* Map a registered asset into a process address space read-only.
 * Returns mapped VA on success, 0 on failure. */
uint64_t ro_map_asset(struct aspace *as, int asset_id, uint64_t requested_va);

/* Unmap an asset from a process address space. Returns 0 on success, error code on failure. */
int      ro_unmap_asset(struct aspace *as, uint64_t va, uint32_t pages);

/* Clean up all RO mappings for a process (called on exit/fault). */
void     ro_cleanup_proc(struct aspace *as);

/* Linker-provided section boundaries */
extern uint8_t __image_start[], __text_start[], __text_end[], __rodata_start[], __rodata_end[],
               __data_start[], __data_end[], __stack_guard[], __stack_bottom[], __stack_top[],
               __exc_guard[], __exc_stack_bottom[], __exc_stack_top[], __kernel_end[];

#endif
