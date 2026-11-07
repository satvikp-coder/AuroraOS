/*
 * kernel/kernel.c — AuroraOS kernel entry (C)
 *
 * Minimal first boot: initializes the early UART console and prints a
 * boot message, then halts. This is the first milestone of the
 * independent ARM64 kernel. No Linux or Android code is used.
 *
 * Target: QEMU ARM64 "virt" machine, PL011 UART at 0x09000000.
 */

#define UART_DR  (*(volatile unsigned int *)(0x09000000 + 0x00))
#define UART_FR  (*(volatile unsigned int *)(0x09000000 + 0x18))
#define UART_FR_TXFF (1 << 5)

/* --- Early UART (PL011) console ----------------------------------------- */

static void uart_putc(char c)
{
    /* Wait until transmit FIFO is not full */
    while (UART_FR & UART_FR_TXFF)
        ;
    UART_DR = (unsigned int)c;
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*(s++));
}

/* --- Kernel main -------------------------------------------------------- */

void kernel_main(void)
{
    uart_puts("AuroraOS booting...\r\n");
    uart_puts("ARM64 initialized\r\n");
    uart_puts("Serial console ready\r\n");
    uart_puts("Welcome to AuroraOS\r\n");

    /* Halt: wait for interrupts forever */
    for (;;) {
        __asm__ volatile ("wfi" ::: "memory");
    }
}
