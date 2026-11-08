#ifndef AURORA_PROCTEST_H
#define AURORA_PROCTEST_H
/* In-kernel userspace tests: "[proc-test] <name>: PASS|FAIL". Every scenario runs REAL EL0 programs
 * (kernel/proc/user_progs.S) and checks what the hardware actually reported (exception syndrome, fault address,
 * SPSR) and what the processes actually did to memory. */
void proctest_run(void);
int  proctest_summary(void);
void proctest_fault(void);       /* deliberate-fault case 8: a KERNEL fault after processes have run (AURORA_FAULT_TEST builds) */
#endif
