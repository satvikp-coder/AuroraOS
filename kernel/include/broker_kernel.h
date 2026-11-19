/*
 * kernel/include/broker_kernel.h - binds the pure broker policy (broker_policy.c) to the kernel: the live requester lookup, the rights table, the
 * replay state, the SYS_UI "aura request" operation and the queue the shell drains. Nothing here executes an effect: a request that the policy ALLOWs is
 * queued with its requester's (pid, generation), and the shell loop executes it later through aura_run_queued(), which re-verifies the requester.
 *
 * Rights are granted by KERNEL code only (broker_grant_navigate); there is no system call that grants, lists or revokes them, a grant is bound to the
 * process INSTANCE (generation) it was given to, and nobody holds any by default.
 */
#ifndef AURORA_BROKER_KERNEL_H
#define AURORA_BROKER_KERNEL_H
#include <proc.h>
#include <aura_exec.h>

#define SYS_UI_AURA 3          /* SYS_UI operation 3: x1 = user buffer with the command text, x2 = length (<= 64), x3 = nonce. Returns (decision << 8) | reason, or a negative AE_* error */
#define BK_QUEUE 4

void broker_init(void);
const struct bp_env *broker_env(void);
struct bp_replay *broker_replay(void);
int  broker_grant_navigate(int pid);
void broker_set_review_path(int on);       /* kernel code: 1 only when the trusted review modal, its input path and the S1 executor exist (a keyboard is present) */                                 /* kernel code only: the CURRENT instance of this live process may navigate; 0 or -1 */
int64_t broker_syscall_aura(struct proc *p, uint64_t text_uva, uint64_t len, uint64_t nonce);
/* The shell loop calls this each iteration: executes every queued, still-valid request. Returns how many were handled. */
int  broker_drain(struct shell *sh);
#endif
