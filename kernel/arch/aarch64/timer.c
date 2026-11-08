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

int timer_start_periodic(uint32_t hz)
{
    if (hz == 0 || freq / hz == 0) return TIMER_ERR_ARG;
    period = freq / hz; periodic = 1; hist_n = 0;
    deadline = timer_count() + period;
    write_cval(deadline);
    write_ctl(CTL_ENABLE);
    return TIMER_OK;
}

int timer_oneshot(uint64_t delta)
{
    if (delta == 0) return TIMER_ERR_ARG;
    periodic = 0; period = delta; hist_n = 0;
    deadline = timer_count() + delta;
    write_cval(deadline);
    write_ctl(CTL_ENABLE);
    return TIMER_OK;
}

void timer_stop(void) { write_ctl(0); }
uint64_t timer_ticks(void) { return *(volatile uint64_t *)&ticks; }
uint64_t timer_missed(void) { return missed; }
uint64_t timer_period(void) { return period; }
uint32_t timer_intid(void) { return intid; }
int timer_ctl_istatus(void) { return (read_ctl() & CTL_ISTATUS) != 0; }
uint64_t timer_current_deadline(void) { return deadline; }
const struct timer_tick_record *timer_history(uint32_t *n) { *n = hist_n; return hist; }
void timer_set_tick_hook(void (*fn)(void)) { hook = fn; }
uint64_t timer_last_handler_sp(void) { return last_sp; }
