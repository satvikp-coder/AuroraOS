/*
 * kernel/include/timer.h — ARM architected generic timer (EL1 physical timer, CNTP_*).
 *
 * The counter (CNTPCT_EL0) runs at CNTFRQ_EL0 Hz. A deadline is an ABSOLUTE counter value written to
 * CNTP_CVAL_EL0; the interrupt fires when the counter reaches it. Periodic ticks are rearmed from the
 * PREVIOUS DEADLINE (not from "now"), so they do not drift; if the CPU fell more than one period behind,
 * the missed ticks are counted and the deadline is moved to now+period instead of firing a burst.
 * All comparisons use signed differences so 64-bit wraparound cannot mis-order two counter values.
 */
#ifndef AURORA_TIMER_H
#define AURORA_TIMER_H
#include <mm.h>
#include <stdint.h>

#define TIMER_HISTORY 16

/* Interval limits. Deadlines are compared with signed 64-bit differences, which is only correct while the
 * distance between "now" and a deadline is below 2^63 ticks; we accept at most 2^62 (leaving a full 2^62 of
 * margin for the time spent late). The minimum period (10 microseconds) stops a caller from turning the
 * timer into an interrupt storm that starves the CPU. */
#define TIMER_MAX_INTERVAL (1ULL << 62)
#define TIMER_MIN_PERIOD_HZ 100000u      /* periods shorter than 1/100000 s are rejected */

enum { TIMER_OK = 0, TIMER_ERR_NO_FREQ = -1, TIMER_ERR_NO_DT = -2, TIMER_ERR_ARG = -3, TIMER_ERR_IRQ = -4 };

struct timer_tick_record { uint64_t deadline, seen_at; };

int      timer_init(const struct fdt_platform *plat);   /* registers + enables the interrupt line; timer stays stopped */
uint64_t timer_freq(void);                              /* Hz, from CNTFRQ_EL0 */
uint64_t timer_count(void);                             /* CNTPCT_EL0, ordered by an ISB */
/* All three are safe against the timer handler on this CPU and PRESERVE the caller's IRQ mask state (called
 * with IRQs masked, including from the tick handler itself, they leave them masked). An invalid request returns
 * TIMER_ERR_ARG and changes nothing: a running timer keeps running unchanged. */
int      timer_start_periodic(uint32_t hz);             /* TIMER_ERR_ARG: hz == 0, period < 10 us or period > TIMER_MAX_INTERVAL */
int      timer_oneshot(uint64_t delta_ticks);           /* fires once, then disables itself; TIMER_ERR_ARG: 0 or > TIMER_MAX_INTERVAL */
void     timer_stop(void);
int      timer_enabled(void);                           /* CNTP_CTL_EL0.ENABLE */
int      timer_is_periodic(void);
uint64_t timer_ticks(void);                             /* interrupts handled since boot */
uint64_t timer_missed(void);                            /* periods skipped because the CPU fell behind */
uint64_t timer_period(void);
uint32_t timer_intid(void);
int      timer_ctl_istatus(void);                       /* CNTP_CTL_EL0.ISTATUS: the compare condition is met */
uint64_t timer_current_deadline(void);
const struct timer_tick_record *timer_history(uint32_t *count);   /* last TIMER_HISTORY ticks, oldest first */
void     timer_set_critical_probe(void (*fn)(void));     /* called inside every reprogramming critical section (tests only) */
void     timer_set_tick_hook(void (*fn)(void));          /* called from the tick handler (tests only) */
uint64_t timer_last_handler_sp(void);                    /* SP observed inside the handler */

/* Wraparound-safe ordering: true when a is at or after b. */
static inline int timer_reached(uint64_t a, uint64_t b) { return (int64_t)(a - b) >= 0; }

#endif
