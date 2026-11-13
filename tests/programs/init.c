/*
 * tests/programs/init.c - a minimal "init": the first user program the kernel starts at boot from an installer-owned init.elf on the disk.
 * It reports that it is running, keeps a boot counter in its own private file "init.log" (so state written by init survives a restart), and exits 0.
 * The kernel gives it identity PRINCIPAL_INIT from its registry; init cannot start other programs (no such system call exists).
 */
#include "common.h"

void __attribute__((section(".text._start"))) _start(u64 arg)
{
    (void)arg;
    say("AURORA-INIT started by the kernel");
    i64 n = bump("init.log", "INIT");
    if (n < 0) quit((int)(10 - n));
    sayn("INIT boot count ", (u64)n);
    quit(0);
}
