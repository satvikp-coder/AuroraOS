/*
 * tests/programs/app.c — one source, built twice (-DAPP_ALPHA / -DAPP_BETA) into two different executables with different data, different files and
 * different secrets. Loaded from AuroraFS by the kernel and run at EL0.
 *
 *  1. proves its sections: initialised data has its value, bss is zero, rodata is readable;
 *  2. reads, increments and rewrites its own counter file (persists across QEMU runs);
 *  3. proves it cannot see the OTHER application's file (the lookup must fail), whichever identity it was started under;
 *  4. isolation from a CONCURRENT process: both applications keep `secret` at the same virtual address; each writes its own value, yields so the other
 *     runs, and checks its own value is still there;
 *  5. probes for kernel privileges: an unknown system call, an unknown file operation, and a system call number that does not exist must all be refused.
 * Exit code 0 = all of that held; any other code names the first failed step.
 */
#include "common.h"

#ifdef APP_ALPHA
#define NAME   "ALPHA"
#define OWN    "alpha.dat"
#define OTHER  "beta.dat"
#define SECRET 0xA1FA0000A1FA0001UL
#define MARK   0xA1FA000000000002UL
#else
#define NAME   "BETA"
#define OWN    "beta.dat"
#define OTHER  "alpha.dat"
#define SECRET 0xBE7A0000BE7A0001UL
#define MARK   0xBE7A000000000002UL
#endif

volatile u64 secret = SECRET;              /* .data: initialised, writable */
volatile u64 zeroed;                       /* .bss: must be zero when the program starts */
static const char banner[] = "AURORA-PROGRAM-" NAME " running from AuroraFS";   /* .rodata */

void __attribute__((section(".text._start"))) _start(void)
{
    say(banner);
    if (zeroed != 0) quit(10);
    if (secret != SECRET) quit(11);
    if (banner[0] != 'A' || banner[15] != NAME[0]) quit(12);

    i64 n = bump(OWN, NAME);
    if (n < 0) quit((int)(20 - n));                           /* 22..26 */
    sayn(NAME " counter now ", (u64)n);

    if (fs_lookup(OTHER) != EFS_ENOENT) quit(30);             /* the other application's private file is invisible to this identity */

    secret = MARK;
    for (int i = 0; i < 4; i++) { yield(); if (secret != MARK) quit(40 + i); }   /* the other process ran meanwhile, at the same virtual address */
    zeroed = MARK;
    yield();
    if (zeroed != MARK || secret != MARK) quit(45);

    if (sc(9, 0, 0, 0, 0, 0) != -4) quit(50);                 /* a system call number that does not exist: AE_ENOSYS */
    if (sc(SYS_FS, 99, 0, 0, 0, 0) != -1) quit(51);           /* an unknown file operation: AE_EINVAL (there is no format or raw-disk operation) */
    if (sc(SYS_FS, 0x464F524D, 0, 0, 0, 0) != -1) quit(52);
    say(NAME " done");
    quit(0);
}
