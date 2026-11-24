/*
 * kernel/include/mm.h — Common memory management definitions
 *
 * Defines the physical memory map, page size, and allocator interface
 * for the AuroraOS kernel. All addresses are physical unless noted.
 *
 * Target: QEMU ARM64 "virt" machine, 256 MiB RAM at 0x40000000.
 */

#ifndef AURORA_MM_H
#define AURORA_MM_H

#include <stddef.h>
#include <stdint.h>

/* --- Page size --------------------------------------------------------- */

#define AURORA_PAGE_SIZE 4096
#define AURORA_PAGE_SHIFT 12

/* --- Physical memory map (QEMU ARM64 virt, 256 MiB RAM) ----------------
 *
 * QEMU virt places RAM at physical address 0x40000000.  The kernel is
 * loaded by QEMU at 0x40080000 (-kernel load address).  Everything below
 * the kernel load address and above the top of RAM is reserved.
 *
 * PL011 UART (MMIO) lives at 0x09000000, outside the RAM range.
 */

#define AURORA_RAM_BASE     0x40000000UL   /* Start of RAM (QEMU virt)   */
#define AURORA_RAM_SIZE     0x10000000UL   /* 256 MiB                    */
#define AURORA_RAM_END      (AURORA_RAM_BASE + AURORA_RAM_SIZE)

#define AURORA_KERNEL_LOAD  0x40080000UL   /* QEMU -kernel load address  */

#define AURORA_UART_BASE    0x09000000UL   /* PL011 UART (MMIO)          */
#define AURORA_UART_SIZE    0x00001000UL   /* 4 KiB register window      */

/* --- Page-aligned kernel boundaries (from linker.ld) ---------------- */

extern uint8_t __kernel_end[];   /* First page after kernel BSS  */

/* Reserved regions that the allocator must never hand out.  These are
 * expressed as [start, end) physical address pairs. */
#define AURORA_RESERVED_COUNT 3

extern uintptr_t aurora_reserved_ranges[AURORA_RESERVED_COUNT * 2];

/* --- Page allocator interface ---------------------------------------- */

/* Initialize the physical page allocator.  Must be called before any
 * aurora_alloc_page() call.  Returns 0 on success, -1 on failure. */
int aurora_mem_init(void);

/* Allocate one physically contiguous 4 KiB page.  Returns the physical
 * address, or 0 if no pages are available. */
uintptr_t aurora_alloc_page(void);

/* Free a previously allocated page.  The address must be page-aligned
 * and must have been returned by aurora_alloc_page(). */
void aurora_free_page(uintptr_t addr);

/* Return the number of free pages. */
unsigned long aurora_free_page_count(void);

/* Return the total number of managed pages. */
unsigned long aurora_total_page_count(void);

/* Check whether a physical address lies in a reserved region. */
int aurora_is_reserved(uintptr_t addr);

/* --- Page table management ------------------------------------------ */

/* Allocate and zero page table pages. Returns 0 on success. */
int mmu_alloc_tables(void);

/* Identity-map the kernel and RAM regions into the page tables.
   Returns 0 on success. */
int mmu_map_memory(void);

/* Get the physical address of the L0 page table. */
uintptr_t mmu_get_l0_table(void);

#endif /* AURORA_MM_H */
