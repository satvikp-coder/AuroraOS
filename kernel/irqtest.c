/*
 * kernel/irqtest.c — interrupt, timer and exception-stack tests (QEMU virt, GICv2 + ARM generic timer).
 *
 * Deterministic by construction: no test depends on how long something takes except as a generous upper
 * bound for "the event happened at all" (2 s of counter time). Properties asserted are ordering
 * properties (never early, exactly one delivery, masked means pending) rather than wall-clock accuracy.
 */
#include <arch.h>
#include <console.h>
#include <gic.h>
#include <irq.h>
#include <irqtest.h>
#include <mm.h>
#include <mmpolicy.h>
#include <timer.h>

static int npass, nfail;

static void check(const char *name, int ok)
{
    console_puts("[irq-test] "); console_puts(name); console_puts(ok ? ": PASS\n" : ": FAIL\n");
    if (ok) npass++; else nfail++;
}

static volatile uint32_t sgi_hits;
static void sgi_handler(uint32_t id, void *arg) { (void)id; (void)arg; sgi_hits++; }

/* Observations made from inside the real timer interrupt handler (via the tick hook). */
static volatile uint64_t h_spsel, h_daif, h_sp;
static volatile uint32_t h_calls;
static void observe(void)
{
    uint64_t v;
    __asm__ volatile ("mrs %0, spsel" : "=r"(v)); h_spsel = v;
    __asm__ volatile ("mrs %0, daif" : "=r"(v)); h_daif = v;
    __asm__ volatile ("mov %0, sp" : "=r"(v)); h_sp = v;
    h_calls++;
}

static void spin_counter(uint64_t ticks) { uint64_t end = timer_count() + ticks; while (!timer_reached(timer_count(), end)) { } }

/* Wait with IRQs enabled until `cond` returns true or 2 s of counter time pass. */
static int wait_ticks_at_least(uint64_t target)
{
    uint64_t limit = timer_count() + 2 * timer_freq();
    while (timer_ticks() < target) {
        if (timer_reached(timer_count(), limit)) return 0;
        __asm__ volatile ("wfi");
    }
    return 1;
}

static int history_sane(void)
{   /* every tick was seen at or after its deadline; consecutive deadlines are one period apart (more only if ticks were missed) */
    uint32_t n; const struct timer_tick_record *h = timer_history(&n);
    if (n < 2) return 0;
    uint64_t period = timer_period();
    for (uint32_t i = 0; i < n; i++) if (!timer_reached(h[i].seen_at, h[i].deadline)) return 0;
    for (uint32_t i = 1; i < n; i++) {
        uint64_t dd = h[i].deadline - h[i - 1].deadline;
        if (dd < period) return 0;
        if (timer_missed() == 0 && dd != period) return 0;
    }
    return 1;
}

static void periodic_test(uint32_t hz, const char *label_a, const char *label_b)
{
    irq_cpu_disable();
    uint64_t base = timer_ticks();
    int rc = timer_start_periodic(hz);
    irq_cpu_enable();
    int got = rc == TIMER_OK && wait_ticks_at_least(base + 5);
    timer_stop();
    irq_cpu_disable();
    check(label_a, got && timer_ticks() >= base + 5 && timer_period() == timer_freq() / hz);
    check(label_b, history_sane());
}

void irqtest_run(const struct fdt_platform *plat)
{
    uint64_t v;

    /* ---- exception vectors and stacks ---- */
    __asm__ volatile ("mrs %0, vbar_el1" : "=r"(v));
    check("exceptions: VBAR_EL1 points at the vector table and is 2 KiB aligned", v == (uint64_t)(uintptr_t)exception_vectors && !(v & 0x7FF));
    uint64_t spsel, sp; __asm__ volatile ("mrs %0, spsel" : "=r"(spsel)); __asm__ volatile ("mov %0, sp" : "=r"(sp));
    check("exceptions: kernel runs on SP_EL0 inside the kernel stack; the exception stack is a separate region",
          spsel == 0 && sp >= (uint64_t)(uintptr_t)__stack_bottom && sp < (uint64_t)(uintptr_t)__stack_top &&
          (uint64_t)(uintptr_t)__exc_stack_bottom >= (uint64_t)(uintptr_t)__stack_top);
    check("exceptions: both stack guard pages are unmapped", vmm_hw_translate((uint64_t)(uintptr_t)__stack_guard, 0, 0) == -1 && vmm_hw_translate((uint64_t)(uintptr_t)__exc_guard, 0, 0) == -1 &&
          vmm_hw_translate((uint64_t)(uintptr_t)__exc_stack_bottom, 1, 0) == 0);

    /* ---- interrupt controller ---- */
    check("irq: the device tree describes a GICv2 at the QEMU virt addresses", plat->gic_version == 2 && plat->gicd_base == 0x08000000ULL && plat->gicc_base == 0x08010000ULL);
    check("irq: GIC initialised (priority mask set, 64+ interrupt IDs)", gicv2_pmr() == 0xF0 && gicv2_max_id() >= 64);
    check("irq: GIC windows are policy-registered Device memory (cacheable alias refused, neighbouring space refused)",
          vmm_map(0x1D0000000ULL, plat->gicd_base, PAGE_SIZE, VMM_ATTR_NORMAL_RW) == VMM_ERR_POLICY && vmm_last_denial() == MMP_DENY_NORMAL_ON_MMIO &&
          vmm_map(0x1D0001000ULL, plat->gicc_base + plat->gicc_size, PAGE_SIZE, VMM_ATTR_DEVICE) == VMM_ERR_POLICY);

    /* ---- timer basics ---- */
    uint64_t f = timer_freq(), a = timer_count(); spin_counter(f / 10000); uint64_t b = timer_count();
    check("timer: frequency is reported and the counter advances monotonically", f >= 1000 && timer_reached(b, a) && b != a);
    check("timer: the interrupt ID comes from the device tree (non-secure physical timer PPI)", plat->have_timer && timer_intid() == plat->timer_intid_phys_ns && timer_intid() >= 16 && timer_intid() < 32);
    check("timer: zero-rate, zero-length and sub-tick-rate requests are rejected", timer_start_periodic(0) == TIMER_ERR_ARG && timer_oneshot(0) == TIMER_ERR_ARG && (f / 0xFFFFFFFFu != 0 || timer_start_periodic(0xFFFFFFFFu) == TIMER_ERR_ARG));
    timer_stop();
    timer_set_tick_hook(observe);

    /* ---- masking: the interrupt is raised and pending, but must not be taken while masked ---- */
    irq_cpu_disable();
    const struct irq_stats *st = irq_get_stats();
    uint64_t ticks0 = timer_ticks(), handled0 = st->handled;
    uint32_t calls0 = h_calls;
    timer_oneshot(f / 1000);
    spin_counter(f / 200);                                   /* 5 ms: well past the 1 ms deadline */
    check("masking: with IRQs masked the compare condition is met and the line is pending at the GIC, yet the handler has NOT run",
          !irq_cpu_enabled() && timer_ctl_istatus() && gicv2_is_pending(timer_intid()) && timer_ticks() == ticks0 && h_calls == calls0);
    irq_cpu_enable();
    uint64_t limit = timer_count() + f / 10;
    while (timer_ticks() == ticks0 && !timer_reached(timer_count(), limit)) { }
    check("unmasking: enabling IRQs delivers exactly one interrupt, acknowledged and completed", timer_ticks() == ticks0 + 1 && st->handled == handled0 + 1 && h_calls == calls0 + 1);
    spin_counter(f / 200);
    check("timer: a one-shot disables itself (no second interrupt)", timer_ticks() == ticks0 + 1 && !timer_ctl_istatus() && !gicv2_is_pending(timer_intid()));
    check("handler context: runs on the separate exception stack (SP_EL1), 16-byte aligned, with IRQs masked",
          h_spsel == 1 && (h_sp & 15) == 0 && h_sp >= (uint64_t)(uintptr_t)__exc_stack_bottom && h_sp < (uint64_t)(uintptr_t)__exc_stack_top && (h_daif & 0x80));

    /* ---- periodic ticks at two rates ---- */
    periodic_test(1000, "timer: 1 kHz periodic ticks are delivered by real interrupts (5+ ticks)", "timer: 1 kHz ticks never fire early and deadlines are exactly one period apart");
    periodic_test(250, "timer: 250 Hz periodic ticks are delivered by real interrupts (5+ ticks)", "timer: 250 Hz ticks never fire early and deadlines are exactly one period apart");

    /* ---- masking again, with a running periodic timer ---- */
    irq_cpu_disable();
    uint64_t t1 = timer_ticks();
    timer_start_periodic(1000);
    spin_counter(f / 125);                                   /* 8 ms with IRQs masked: ~8 deadlines pass */
    int quiet = timer_ticks() == t1 && gicv2_is_pending(timer_intid());
    irq_cpu_enable();
    int resumed = wait_ticks_at_least(t1 + 3);
    timer_stop(); irq_cpu_disable();
    check("masking: a running periodic timer produces no ticks while masked, and resumes after unmasking", quiet && resumed);
    check("timer: time arithmetic is wraparound-safe", timer_reached(5, 0xFFFFFFFFFFFFFFF0ULL) && !timer_reached(0xFFFFFFFFFFFFFFF0ULL, 5) && timer_reached(7, 7));

    /* ---- software-generated interrupts: registered handler ---- */
    check("irq: registering a handler validates its arguments", irq_register(5000, sgi_handler, 0) == IRQ_ERR_RANGE && irq_register(IRQ_MAX_ID, sgi_handler, 0) == IRQ_ERR_RANGE && irq_register(0, 0, 0) == IRQ_ERR_NULL);
    check("irq: a handler can be registered once", irq_register(0, sgi_handler, 0) == IRQ_OK && irq_register(0, sgi_handler, 0) == IRQ_ERR_BUSY && irq_enable_line(0) == IRQ_OK);
    irq_cpu_disable();
    sgi_hits = 0; handled0 = st->handled;
    gicv2_send_sgi_self(0);
    spin_counter(f / 1000);
    int masked_ok = sgi_hits == 0 && gicv2_is_pending(0);
    irq_cpu_enable();
    limit = timer_count() + f / 10;
    while (sgi_hits == 0 && !timer_reached(timer_count(), limit)) { }
    check("irq: a software-generated interrupt stays pending while masked and is dispatched exactly once when unmasked", masked_ok && sgi_hits == 1 && st->handled == handled0 + 1);

    /* ---- unknown interrupts ---- */
    uint64_t unknown0 = st->unknown; uint32_t hits0 = sgi_hits; ticks0 = timer_ticks();
    irq_enable_line(1);                                      /* enabled at the GIC but with no handler: an "unknown" source */
    irq_enable_line(40);                                     /* an SPI nobody registered */
    gicv2_send_sgi_self(1);
    gicv2_set_pending(40);
    limit = timer_count() + f / 10;
    while (st->unknown < unknown0 + 2 && !timer_reached(timer_count(), limit)) { }
    check("irq: unknown interrupts (SGI 1 and SPI 40) are counted, completed and do not touch other state",
          st->unknown == unknown0 + 2 && st->last_unknown == 40 && sgi_hits == hits0 && timer_ticks() == ticks0 && vmm_mmu_enabled());
    /* SGIs cannot be disabled on a GICv2 (their enable bits are fixed), but an SPI can: the disabled SPI must stay pending and undelivered. */
    gicv2_set_pending(40);
    spin_counter(f / 500);
    check("irq: an unknown SPI is disabled after its first interrupt: re-raised, it stays pending and is not delivered (no storm)", st->unknown == unknown0 + 2 && gicv2_is_pending(40));

    /* ---- spurious interrupts ---- */
    irq_cpu_disable();
    uint32_t raw = gicv2_ack_raw();
    check("irq: acknowledging with nothing pending returns the spurious ID (1023)", (raw & 0x3FF) == 1023);
    uint64_t sp0 = st->spurious, h0 = st->handled, u0 = st->unknown;
    irq_dispatch_raw(raw); irq_dispatch_raw(1020); irq_dispatch_raw(1022 | (3u << 10));
    check("irq: spurious/reserved IDs are counted and change nothing else", st->spurious == sp0 + 3 && st->handled == h0 && st->unknown == u0);
    irq_cpu_enable();
    hits0 = sgi_hits; gicv2_send_sgi_self(0);
    limit = timer_count() + f / 10;
    while (sgi_hits == hits0 && !timer_reached(timer_count(), limit)) { }
    check("irq: normal delivery still works after spurious interrupts", sgi_hits == hits0 + 1);
    irq_cpu_disable();
    check("irq: a handler can be unregistered", irq_unregister(0) == IRQ_OK && irq_unregister(5000) == IRQ_ERR_RANGE);

    /* ---- memory protections still intact ---- */
    check("memory: MMU still on, text/rodata still read-only, serial output works",
          vmm_mmu_enabled() && vmm_hw_translate((uint64_t)(uintptr_t)__text_start, 1, 0) == -1 && vmm_hw_translate((uint64_t)(uintptr_t)__rodata_start, 1, 0) == -1);
    timer_set_tick_hook(0);
}

int irqtest_summary(void)
{
    console_puts("AURORA-IRQ-SELFTEST: "); console_dec((uint64_t)npass); console_puts(" passed, ");
    console_dec((uint64_t)nfail); console_puts(" failed\n");
    return nfail;
}

#ifdef AURORA_FAULT_TEST
static uint64_t (*volatile recurse_fp)(uint64_t);
static uint64_t recurse(uint64_t n)
{   /* burns kernel stack until it hits the guard page; the call through a pointer prevents tail-call/loop conversion */
    volatile uint64_t pad[32];
    pad[0] = n;
    uint64_t r = recurse_fp(n + 1);
    return r + pad[0];
}
static void crash_in_handler(void) { (void)*(volatile uint32_t *)0xDEAD0000UL; }

void irqtest_fault(void)
{
    if (AURORA_FAULT_TEST == 6) {
        console_puts("[fault-test] case 6: kernel stack overflow into the guard page\n");
        recurse_fp = recurse;
        (void)recurse(0);
        console_puts("ERROR: fault test did not fault!\n");
    } else if (AURORA_FAULT_TEST == 7) {
        console_puts("[fault-test] case 7: fault inside the timer interrupt handler (nested exception)\n");
        timer_set_tick_hook(crash_in_handler);
        irq_cpu_disable();
        timer_start_periodic(1000);
        irq_cpu_enable();
        for (;;) __asm__ volatile ("wfi");
    }
}
#else
void irqtest_fault(void) {}
#endif
