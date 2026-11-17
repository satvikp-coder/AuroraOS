/*
 * kernel/include/timersys.h - SYS_TIMER (42): the native timer SERVICE for EL0 clients, binding the pure core (timersvc.c) to the architectural counter.
 *   x0 = operation, x1 / x2 = arguments. Results: >= 0, or a negative error (AE_* for the call itself, AE_TM_BASE + TS_E_* for the service).
 *   TIMER_START   (1): x1 = duration in milliseconds (1000 .. 86,400,000; anything else is REFUSED, never clamped). Returns the timer id.
 *   TIMER_CANCEL  (2): x1 = id. Only the owner instance, only while running.
 *   TIMER_QUERY   (3): x1 = id, x2 = user buffer of 16 bytes: u8 state (1 running, 2 done, 3 cancelled), 7 zero bytes, u64 remaining ms (little endian).
 *   TIMER_RELEASE (4): x1 = id. Frees a finished or cancelled timer's slot (a timer slot counts against the owner's quota until released).
 *   TIMER_NOW     (5): monotonic milliseconds since the counter started (CNTPCT_EL0 / CNTFRQ_EL0). It is NOT a wall-clock time and not SYS_TICKS.
 *
 * Authority: the service is a kernel-granted CAPABILITY on a principal (timersys_grant). No system call grants, lists or revokes it, nobody holds it by
 * default, and the kernel, "none" and every reserved 0x7Fxxxxxx principal are refused. Timers belong to the kernel-attested (pid, generation): a
 * recycled pid never sees an earlier instance's timers, and a foreign id looks exactly like no timer. Every user pointer is validated BEFORE
 * anything changes.
 *
 * Completion: timersys_tick() (shell loop) polls the monotonic counter and offers the owner ONE event per completed timer through its UI event queue
 * (type 0x85, 12 bytes: 1, 0x85, six zero bytes, id, state, two zero bytes). The event is a HINT: the state stays readable with TIMER_QUERY. A full
 * queue is never overwritten by a timer event: the offer is retried on later ticks until it fits or the owner is gone.
 */
#ifndef AURORA_TIMERSYS_H
#define AURORA_TIMERSYS_H
#include <proc.h>
#include <timersvc.h>

#define SYS_TIMER 42
#define TIMER_START 1
#define TIMER_CANCEL 2
#define TIMER_QUERY 3
#define TIMER_RELEASE 4
#define TIMER_NOW 5
#define AE_TM_BASE (-400)            /* system-call result = AE_TM_BASE + TS_E* (e.g. -406 = quota) */
#define UIP_EVENT_TIMER 0x85

void timersys_init(void);
int  timersys_grant(uint32_t principal);                 /* kernel code only: 0, or -1 for a refused principal or a full table */
int64_t timersys_syscall(struct proc *p, uint64_t op, uint64_t a1, uint64_t a2);
void timersys_tick(void);                                 /* poll the counter; deliver completions */
void timersys_owner_gone(int pid, uint64_t gen);          /* the instance exited or faulted: its timers go */
/* The trusted shell's own entries (kernel code, NOT system calls). The shell is the owner (pid 0, generation 1): it is not a process, no process can be it,
 * and it holds the same quota (2) as any owner. Completion is read by polling (shell_tick), so no event is posted for it. */
#define TIMER_SHELL_PID 0u
#define TIMER_SHELL_GEN 1u
int      timersys_shell_start(uint64_t duration_ms);                       /* id >= 0, or AE_TM_BASE + TS_E* */
int      timersys_shell_query(int id, uint8_t *state, uint64_t *remaining_ms);   /* 0, or AE_TM_BASE + TS_E* */
int      timersys_shell_cancel(int id);
int      timersys_shell_release(int id);
uint64_t timersys_now_ms(void);                                            /* monotonic milliseconds (0 if the counter is unusable) */
#endif
