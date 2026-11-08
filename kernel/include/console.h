#ifndef AURORA_CONSOLE_H
#define AURORA_CONSOLE_H
#include <stdint.h>
void console_putc(char c);
void console_puts(const char *s);
void console_hex(uint64_t v);   /* 0x + 16 digits */
void console_dec(uint64_t v);
#endif
