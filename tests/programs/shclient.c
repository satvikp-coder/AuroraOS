/*
 * tests/programs/shclient.c - EL0 clients that run while the native shell is interactive (tests/qemu/shell-sched-tests.sh). The kernel launch
 * registry passes the mode in x0:
 *   1 SPIN    CPU-bound forever; prints a heartbeat line every 100 scheduler ticks of its own execution
 *   2 YIELD   yields in a loop; prints a line every 200 yields
 *   3 BLOCK   blocks in a receive that nobody will ever satisfy (it must cost no CPU)
 *   4 EXIT    spins for about 300 ticks, then exits with code 7
 *   5 FAULT   spins for about 300 ticks, then writes to address 0 (a fault the kernel must contain)
 *   6 CHURN   spins for about 20 ticks, then exits with code 0
 * Each line is printed by the kernel with a "[user pN] " tag, so a guest log line proves the process really ran at EL0.
 */
#include "common.h"
#define SYS_GETPID 3
#define SYS_TICKS 5
#define SYS_RECV 7

static u64 ticks(void) { return (u64)sc(SYS_TICKS, 0, 0, 0, 0, 0); }
static void burn(int n) { for (int i = 0; i < n; i++) __asm__ volatile ("" ::: "memory"); }      /* spend CPU without a syscall */

void __attribute__((section(".text._start"))) _start(u64 mode)
{
    u64 t0 = ticks(), last = t0, n = 0;
    if (mode == 1) {
        for (;;) {
            burn(20000);                                              /* burn CPU; check the clock now and then */
            u64 t = ticks();
            if (t - last >= 100) { last = t; sayn("spin heartbeat t=", t); }
        }
    }
    if (mode == 2) {
        for (;;) { yield(); if (++n % 200 == 0) sayn("yield count=", n); }
    }
    if (mode == 3) {
        say("block waiting");
        char buf[16];
        for (;;) sc(SYS_RECV, (i64)buf, sizeof buf, 0, 0, 0);
    }
    if (mode == 4 || mode == 5) {
        say(mode == 4 ? "exit soon" : "fault soon");
        for (;;) {
            burn(5000);
            if (ticks() - t0 >= 300) break;
        }
        if (mode == 4) { say("exit now"); quit(7); }
        say("fault now");
        *(volatile u64 *)0 = 1;
        quit(99);
    }
    if (mode == 6) {
        for (;;) { burn(5000); if (ticks() - t0 >= 20) break; }
        quit(0);
    }
    quit(50);
}
