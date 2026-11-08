/* kernel/arch/aarch64/timer.c — ARM generic timer driver (see include/timer.h). */
#include <gic.h>
#include <irq.h>
#include <timer.h>

#define CTL_ENABLE  (1u << 0)
#define CTL_IMASK   (1u << 1)
#define CTL_ISTATUS (1u << 2)

static uint32_t intid;
static uint64_t freq, period, deadline, ticks, missed, last_sp;
static int periodic;
static void (*hook)(void);
static struct timer_tick_record hist[TIMER_HISTORY];
static uint32_t hist_n;

static inline void write_ctl(uint64_t v) { __asm__ volatile ("msr cntp_ctl_el0, %0\n isb" :: "r"(v) : "memory"); }
static inline uint64_t read_ctl(void) { uint64_t v; __asm__ volatile ("mrs %0, cntp_ctl_el0" : "=r"(v)); return v; }
static inline void write_cval(uint64_t v) { __asm__ volatile ("msr cntp_cval_el0, %0\n isb" :: "r"(v) : "memory"); }

uint64_t timer_freq(void) { uint64_t v; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
uint64_t timer_count(void) { uint64_t v; __asm__ volatile ("isb\n mrs %0, cntpct_el0" : "=r"(v) :: "memory"); return v; }

static void on_tick(uint32_t id, void *arg)
{
    (void)id; (void)arg;
    uint64_t now = timer_count();
    __asm__ volatile ("mov %0, sp" : "=r"(last_sp));
    if (hist_n == TIMER_HISTORY) { for (uint32_t i = 1; i < TIMER_HISTORY; i++) hist[i - 1] = hist[i]; hist_n--; }
    hist[hist_n].deadline = deadline; hist[hist_n].seen_at = now; hist_n++;
    ticks++;
    if (periodic) {
        deadline += period;                                   /* from the previous deadline: no drift */
        if (timer_reached(now, deadline)) {                   /* more than a period late: skip, do not burst */
            missed += (uint64_t)(((int64_t)(now - deadline)) / (int64_t)period) + 1;
            deadline = now + period;
        }
        write_cval(deadline);                                 /* writing CVAL clears the level-sensitive condition */
    } else {
        write_ctl(0);                                         /* one-shot: disable (and thus deassert) */
    }
    if (hook) hook();
}

int timer_init(const struct fdt_platform *p)
{
    freq = timer_freq();
    if (freq == 0) return TIMER_ERR_NO_FREQ;
    if (!p || !p->have_timer) return TIMER_ERR_NO_DT;
    intid = p->timer_intid_phys_ns;
    write_ctl(0);                                             /* stopped, unmasked at the timer */
    if (irq_register(intid, on_tick, 0) != IRQ_OK) return TIMER_ERR_IRQ;
    if (irq_enable_line(intid) != IRQ_OK) return TIMER_ERR_IRQ;
    return TIMER_OK;
}

/*
 * Reprogramming protocol (single CPU). The tick handler also writes period/deadline/periodic and CNTP_*, so
 * every state change runs inside irq_save()/irq_restore(): the handler cannot observe a half-updated state.
 * Inside the critical section the hardware is stopped FIRST (CTL=0 deasserts the level-sensitive line), any
 * interrupt that was already pending for the OLD configuration is cleared at the GIC, then CVAL is written and
 * the timer is enabled last. Validation happens before the critical section changes anything, so an invalid
 * request leaves the previous timer running. The caller's mask state is restored exactly.
 * SMP would additionally need: one timer state per CPU (CNTP_* are per-CPU registers) or a lock around shared
 * state, and a cross-CPU protocol if one CPU may reprogram another's timer.
 */
static int min_period_ok(uint64_t p) { uint64_t min = freq / TIMER_MIN_PERIOD_HZ; return p >= (min ? min : 1); }

static void (*crit_probe)(void);
void timer_set_critical_probe(void (*fn)(void)) { crit_probe = fn; }

static void arm_locked(uint64_t first_delta)
{
    if (crit_probe) crit_probe();                             /* test hook: observes the CPU state inside the critical section */
    write_ctl(0);                                             /* stop and deassert the old condition */
    irq_clear_pending(intid);                                 /* an interrupt raised for the old deadline must not fire */
    deadline = timer_count() + first_delta;
    write_cval(deadline);
    write_ctl(CTL_ENABLE);
}

int timer_start_periodic(uint32_t hz)
{
    if (freq == 0) return TIMER_ERR_NO_FREQ;
    if (hz == 0) return TIMER_ERR_ARG;
    uint64_t p = freq / hz;
    if (p == 0 || !min_period_ok(p) || p > TIMER_MAX_INTERVAL) return TIMER_ERR_ARG;
    uint64_t fl = irq_save();
    period = p; periodic = 1; hist_n = 0;
    arm_locked(p);
    irq_restore(fl);
    return TIMER_OK;
}

int timer_oneshot(uint64_t delta)
{
    if (freq == 0) return TIMER_ERR_NO_FREQ;
    if (delta == 0 || delta > TIMER_MAX_INTERVAL) return TIMER_ERR_ARG;
    uint64_t fl = irq_save();
    periodic = 0; period = delta; hist_n = 0;
    arm_locked(delta);
    irq_restore(fl);
    return TIMER_OK;
}

void timer_stop(void)
{
    uint64_t fl = irq_save();
    if (crit_probe) crit_probe();
    write_ctl(0);
    irq_clear_pending(intid);
    irq_restore(fl);
}

int timer_enabled(void) { return (read_ctl() & CTL_ENABLE) != 0; }
int timer_is_periodic(void) { return periodic; }

uint64_t timer_ticks(void) { return *(volatile uint64_t *)&ticks; }
uint64_t timer_missed(void) { return missed; }
uint64_t timer_period(void) { return period; }
uint32_t timer_intid(void) { return intid; }
int timer_ctl_istatus(void) { return (read_ctl() & CTL_ISTATUS) != 0; }
uint64_t timer_current_deadline(void) { return deadline; }
const struct timer_tick_record *timer_history(uint32_t *n) { *n = hist_n; return hist; }
void timer_set_tick_hook(void (*fn)(void)) { hook = fn; }
uint64_t timer_last_handler_sp(void) { return last_sp; }
