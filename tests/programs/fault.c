/*
 * tests/programs/fault.c - a program that misbehaves on purpose, in the way selected by the number the kernel launch registry passes in x0.
 * The kernel must contain each one (the process is FAULTED, never resumed) and keep running. If it ever gets past its fault it exits with
 * code 77, which the test treats as a failure.
 */
#include "common.h"

static const char banner[] = "AURORA-PROGRAM-FAULT about to misbehave";

void __attribute__((section(".text._start"))) _start(u64 kind)
{
    say(banner);
    if (kind == 1) *(volatile u64 *)(void *)_start = 0;                        /* write to its own (read-only, executable) code */
    else if (kind == 2) ((void (*)(void))0x4000001000UL)();                    /* jump to an address that is not mapped in a loaded process (the fixed low data page) */
    else if (kind == 3) { u64 v; __asm__ volatile ("mrs %0, ttbr0_el1" : "=r"(v)); sayn("read TTBR0: ", v); }   /* privileged register read */
    else if (kind == 4) sayn("kernel word: ", *(volatile u64 *)0x40080000UL);  /* a kernel address */
    else if (kind == 5) *(volatile u64 *)(void *)banner = 0;                   /* write to its own read-only data */
    quit(77);
}
