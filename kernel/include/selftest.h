#ifndef AURORA_SELFTEST_H
#define AURORA_SELFTEST_H
/* In-kernel memory-management self-tests. Results are printed on the serial console
 * as "[test] <name>: PASS|FAIL"; the QEMU test scripts parse them. */
void selftest_pmm(void);                 /* run with the MMU off, right after pmm_init */
void selftest_mmu(void);                 /* run after vmm_enable */
int  selftest_summary(void);             /* prints totals; returns number of failures */
void selftest_fault(void);               /* only does anything in AURORA_FAULT_TEST builds */
#endif
