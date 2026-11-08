/* kernel/console.c — PL011 UART console (QEMU virt, 0x09000000). */
#include <console.h>

#define UART_BASE 0x09000000UL
#define UART_DR   (*(volatile uint32_t *)(UART_BASE + 0x00))
#define UART_FR   (*(volatile uint32_t *)(UART_BASE + 0x18))
#define UART_FR_TXFF (1u << 5)

void console_putc(char c)
{
    while (UART_FR & UART_FR_TXFF)
        ;
    UART_DR = (uint32_t)(unsigned char)c;
}

void console_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') console_putc('\r');
        console_putc(*s++);
    }
}

void console_hex(uint64_t v)
{
    static const char d[] = "0123456789abcdef";
    console_puts("0x");
    for (int i = 60; i >= 0; i -= 4) console_putc(d[(v >> i) & 0xF]);
}

void console_dec(uint64_t v)
{
    char buf[21]; int n = 0;
    if (v == 0) { console_putc('0'); return; }
    while (v) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) console_putc(buf[--n]);
}
