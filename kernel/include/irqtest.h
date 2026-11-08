#ifndef AURORA_IRQTEST_H
#define AURORA_IRQTEST_H
#include <mm.h>
/* In-kernel interrupt/timer tests: "[irq-test] <name>: PASS|FAIL". Each test waits for the real,
 * hardware-emulated event; nothing is simulated by calling the handler directly (except the
 * explicitly named spurious-ID dispatch test). */
void irqtest_run(const struct fdt_platform *plat);
int  irqtest_summary(void);
void irqtest_fault(void);          /* deliberate-fault cases 6 and 7 (AURORA_FAULT_TEST builds only) */
#endif
