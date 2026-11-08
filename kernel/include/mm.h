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
#define FDT_MAX_RESERVED 8
struct fdt_info {
    int      valid;                 /* header + memory node parsed OK */
    uint64_t ram_base, ram_size;    /* first /memory reg entry */
    uint32_t totalsize;             /* size of the blob itself */
    int      nreserved;
    struct { uint64_t addr, size; } reserved[FDT_MAX_RESERVED]; /* memory reserve map */
};
int fdt_parse(const void *fdt, struct fdt_info *out);   /* 0 ok, <0 error */

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
int       pmm_reserve(uintptr_t start, uintptr_t end);   /* [start,end), rounded outward */
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

int      vmm_init(void);                         /* build identity tables (MMU still off) */
void     vmm_enable(void);                       /* turn the MMU on */
int      vmm_map(uint64_t va, uint64_t pa, uint64_t size, int attr);
int      vmm_mmu_enabled(void);
/* Hardware translation via AT: returns 0 and *pa on success, -1 if it would fault. */
int      vmm_hw_translate(uint64_t va, int write, uint64_t *pa);

/* Linker-provided section boundaries */
extern uint8_t __image_start[], __text_start[], __text_end[], __rodata_start[], __rodata_end[],
               __data_start[], __data_end[], __stack_guard[], __stack_bottom[], __stack_top[],
               __kernel_end[];

#endif
