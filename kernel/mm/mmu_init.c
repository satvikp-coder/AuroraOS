/*
 * kernel/mm/mmu_init.c — MMU initialization
 *
 * Allocates page tables, maps memory, sets up exception vectors,
 * and enables the MMU.  Called from kernel_main() after the UART
 * console is ready.
 */

#include <stdint.h>

#include <mm/mm.h>
#include <aarch64/mmu.h>
#include <aarch64/exceptions.h>

/* External: L0 table physical address storage (defined in mmu.S) */
extern uintptr_t mmu_l0_table_storage;

/* Set the L0 table address in the mmu.S assembly */
extern void mmu_set_l0_table(uintptr_t addr);

/* Set VBAR_EL2 to the exception vector table address */
extern void mmu_set_vbar(uintptr_t addr);

/* Enable the MMU (assembly in mmu.S) */
extern void mmu_enable(void);

/* --- MMU initialization --------------------------------------------- */

int mmu_init(void)
{
    /* 1. Allocate page table pages */
    if (mmu_alloc_tables() < 0)
        return -1;

    /* 2. Identity-map RAM and MMIO */
    if (mmu_map_memory() < 0)
        return -1;

    /* 3. Set the L0 table address in assembly */
    mmu_set_l0_table(mmu_get_l0_table());

    /* 4. Set VBAR_EL2 to our exception vector table */
    mmu_set_vbar((uintptr_t)exception_vector_table);

    /* 5. Enable the MMU */
    mmu_enable();

    return 0;
}
