/*
 * kernel/irq.c — Interrupt controller dispatch
 *
 * Minimal IRQ dispatch for single-CPU systems.  The controller
 * operations (GICv2, etc.) are registered via irq_set_controller().
 */

#include <irq.h>
#include <stdint.h>

#define MAX_IRQ_HANDLERS 256

static struct {
    void (*handler)(uint32_t, void *);
    void *arg;
} irq_handlers[MAX_IRQ_HANDLERS];

static const struct irq_controller_ops *controller_ops = 0;
static uint64_t irq_mask_state = 0;

void irq_set_controller(const struct irq_controller_ops *ops)
{
    controller_ops = ops;
}

int irq_register(uint32_t id, void (*handler)(uint32_t, void *), void *arg)
{
    if (id >= MAX_IRQ_HANDLERS || !handler)
        return -1;
    irq_handlers[id].handler = handler;
    irq_handlers[id].arg = arg;
    return 0;
}

int irq_enable_line(uint32_t id)
{
    if (controller_ops && controller_ops->enable)
        controller_ops->enable(id);
    return 0;
}

int irq_disable_line(uint32_t id)
{
    if (controller_ops && controller_ops->disable)
        controller_ops->disable(id);
    return 0;
}

void irq_clear_pending(uint32_t id)
{
    if (controller_ops && controller_ops->clear_pending)
        controller_ops->clear_pending(id);
}

void irq_dispatch(void)
{
    if (!controller_ops || !controller_ops->ack)
        return;

    uint32_t raw = controller_ops->ack();
    uint32_t id = raw & 0x3FF;  /* GICv2 IAR: bits 9:0 = interrupt ID */

    if (id < MAX_IRQ_HANDLERS && irq_handlers[id].handler) {
        irq_handlers[id].handler(id, irq_handlers[id].arg);
    }

    if (controller_ops && controller_ops->eoi)
        controller_ops->eoi(raw);
}

uint64_t irq_save(void)
{
    uint64_t state;
    __asm__ volatile ("mrs %0, daif" : "=r"(state));
    __asm__ volatile ("msr daifset, #0xf" ::: "memory");
    return state;
}

void irq_restore(uint64_t state)
{
    __asm__ volatile ("msr daif, %0" :: "r"(state) : "memory");
}