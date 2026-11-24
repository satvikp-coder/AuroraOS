/*
 * kernel/kernel.c — AuroraOS kernel entry (C)
 *
 * Minimal first boot: initializes the early UART console, prints a
 * boot message, initializes the physical page allocator and MMU,
 * then runs memory management tests.  No Linux or Android code used.
 *
 * Target: QEMU ARM64 "virt", PL011 UART at 0x09000000.
 */

#define UART_DR   (*(volatile unsigned int *)(0x09000000 + 0x00))
#define UART_FR   (*(volatile unsigned int *)(0x09000000 + 0x18))
#define UART_FR_TXFF (1 << 5)

#include <stdint.h>
#include <mm/mm.h>

/* --- Early UART (PL011) console ----------------------------------------- */

static void uart_putc(char c)
{
    while (UART_FR & UART_FR_TXFF)
        ;
    UART_DR = (unsigned int)c;
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*(s++));
}

static void uart_hex64(uint64_t val)
{
    static const char hex[] = "0123456789abcdef";
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        uart_putc(hex[(val >> i) & 0xF]);
}

/* --- Memory management (from mm/) ------------------------------------- */

extern int aurora_mem_init(void);
extern uintptr_t aurora_alloc_page(void);
extern void aurora_free_page(uintptr_t addr);
extern unsigned long aurora_free_page_count(void);
extern unsigned long aurora_total_page_count(void);
extern int aurora_is_reserved(uintptr_t addr);
extern uint8_t __kernel_end[];

extern int mmu_init(void);

/* --- Fault test --------------------------------------------------------
 *
 * Deliberately triggers a synchronous data abort by accessing an
 * unmapped virtual address.  The exception vector routes to
 * fault_handler(), which prints diagnostics and halts.  We never
 * return from this function on success of the fault.
 */

static void fault_test(void)
{
    uart_puts("Fault test: accessing unmapped address 0xDEAD0000...\r\n");

    /* Deliberate invalid memory access — triggers a data abort */
    volatile uint32_t *unmapped = (volatile uint32_t *)0xDEAD0000UL;
    (void)*unmapped;

    /* Should never reach here */
    uart_puts("ERROR: fault test did not trigger exception!\r\n");
}

/* --- Kernel main -------------------------------------------------------- */

void kernel_main(void)
{
    uart_puts("AuroraOS booting...\r\n");
    uart_puts("ARM64 initialized\r\n");
    uart_puts("Serial console ready\r\n");
    uart_puts("Welcome to AuroraOS\r\n");

    /* 1. Initialize the physical page allocator */
    uart_puts("Initializing physical page allocator...\r\n");
    if (aurora_mem_init() < 0) {
        uart_puts("ERROR: allocator init failed!\r\n");
        for (;;) __asm__ volatile ("wfi" ::: "memory");
    }
    uart_puts("Allocator initialized.\r\n");
    uart_puts("Total pages: ");
    uart_hex64(aurora_total_page_count());
    uart_puts("\r\n");
    uart_puts("Free pages:  ");
    uart_hex64(aurora_free_page_count());
    uart_puts("\r\n");

    /* 2. Allocate and free a page */
    uart_puts("Allocating a test page...\r\n");
    uintptr_t p = aurora_alloc_page();
    if (p == 0) {
        uart_puts("ERROR: page allocation failed!\r\n");
        for (;;) __asm__ volatile ("wfi" ::: "memory");
    }
    uart_puts("Allocated page at ");
    uart_hex64(p);
    uart_puts("\r\n");
    uart_puts("Free pages after alloc: ");
    uart_hex64(aurora_free_page_count());
    uart_puts("\r\n");

    aurora_free_page(p);
    uart_puts("Freed test page.\r\n");
    uart_puts("Free pages after free:  ");
    uart_hex64(aurora_free_page_count());
    uart_puts("\r\n");

    /* 3. Verify reserved memory is protected */
    uart_puts("Verifying reserved memory protection...\r\n");
    uart_puts("Kernel load addr: 0x");
    uart_hex64(AURORA_KERNEL_LOAD);
    uart_puts("\r\n");
    uart_puts("Kernel end addr: 0x");
    uart_hex64((uintptr_t)__kernel_end);
    uart_puts("\r\n");
    if (aurora_is_reserved(AURORA_KERNEL_LOAD))
        uart_puts("Kernel load address: reserved OK\r\n");
    else
        uart_puts("ERROR: kernel load address not reserved!\r\n");
    if (aurora_is_reserved((uintptr_t)__kernel_end - 4096))
        uart_puts("Kernel end-1 page: reserved OK\r\n");
    else
        uart_puts("Kernel end-1 page: NOT reserved!\r\n");
    if (!aurora_is_reserved(AURORA_RAM_BASE + 4096 * 200))
        uart_puts("Page at index 200: free OK\r\n");
    else
        uart_puts("Page at index 200: NOT free!\r\n");

    /* 4. Initialize the MMU */
    uart_puts("Initializing MMU...\r\n");
    if (mmu_init() < 0) {
        uart_puts("ERROR: MMU init failed!\r\n");
        for (;;) __asm__ volatile ("wfi" ::: "memory");
    }
    uart_puts("MMU enabled with identity mapping.\r\n");

    /* 5. Verify memory still accessible with MMU on */
    uart_puts("Verifying mapped memory access with MMU on...\r\n");
    volatile uint32_t *test_addr = (volatile uint32_t *)0x40080000UL;
    (void)*test_addr;
    uart_puts("Mapped memory access OK.\r\n");

    /* 6. Run the fault test (halts on fault) */
    uart_puts("Starting fault test...\r\n");
    fault_test();

    /* Should never reach here */
    uart_puts("ERROR: returned from fault test!\r\n");
    for (;;) __asm__ volatile ("wfi" ::: "memory");
}
