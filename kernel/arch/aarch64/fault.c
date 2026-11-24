/*
 * kernel/arch/aarch64/fault.c — Synchronous fault handler
 *
 * Called from the assembly exception vector on a synchronous exception.
 * Reads ESR_EL2 and FAR_EL2 to diagnose the fault, prints diagnostic
 * information via the UART console, and halts.
 */

#include <stdint.h>
#include <stddef.h>

#include <aarch64/exceptions.h>
#include <aarch64/mmu.h>

/* --- UART (same addresses as kernel.c) ------------------------------- */

#define UART_DR   (*(volatile unsigned int *)(0x09000000 + 0x00))
#define UART_FR   (*(volatile unsigned int *)(0x09000000 + 0x18))
#define UART_FR_TXFF (1 << 5)

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

/* --- EC value names ------------------------------------------------- */

static const char *ec_name(uint64_t ec)
{
    switch (ec) {
    case 0x20: return "Instruction abort (lower EL)";
    case 0x21: return "Instruction abort (same EL)";
    case 0x24: return "Data abort (lower EL)";
    case 0x25: return "Data abort (same EL)";
    default:   return "Unknown exception";
    }
}

/* --- Fault handler -------------------------------------------------- */

void fault_handler(uint64_t esr, uint64_t far)
{
    uint64_t ec = (esr >> 26) & 0x3F;
    uint64_t dfsc = esr & ESR_DFSC_STATUS_MASK;

    uart_puts("\r\n*** SYNCHRONOUS EXCEPTION ***\r\n");
    uart_puts("EC: ");
    uart_puts(ec_name(ec));
    uart_puts("\r\n");
    uart_puts("FAR: ");
    uart_hex64(far);
    uart_puts("\r\n");
    uart_puts("DFSC: ");
    uart_hex64(dfsc);
    uart_puts("\r\n");
    uart_puts("Fault is fatal. Halting.\r\n");

    /* Halt forever */
    for (;;) {
        __asm__ volatile ("wfi" ::: "memory");
    }
}
