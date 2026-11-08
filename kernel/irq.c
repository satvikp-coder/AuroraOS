/* kernel/irq.c — generic IRQ dispatch (see include/irq.h). */
#include <irq.h>

static struct { irq_handler_t fn; void *arg; } table[IRQ_MAX_ID];
static const struct irq_controller_ops *ctl;
static struct irq_stats stats;

void irq_set_controller(const struct irq_controller_ops *ops) { ctl = ops; }

static int valid_id(uint32_t id) { return ctl && id < IRQ_MAX_ID && id < ctl->max_id; }

int irq_register(uint32_t intid, irq_handler_t h, void *arg)
{
    if (!h) return IRQ_ERR_NULL;
    if (!ctl) return IRQ_ERR_NOCTRL;
    if (!valid_id(intid)) return IRQ_ERR_RANGE;
    if (table[intid].fn) return IRQ_ERR_BUSY;
    table[intid].arg = arg; table[intid].fn = h;
    return IRQ_OK;
}

int irq_unregister(uint32_t intid)
{
    if (!valid_id(intid)) return IRQ_ERR_RANGE;
    if (ctl->disable) ctl->disable(intid);
    table[intid].fn = 0; table[intid].arg = 0;
    return IRQ_OK;
}

int irq_enable_line(uint32_t intid)
{
    if (!valid_id(intid)) return IRQ_ERR_RANGE;
    ctl->enable(intid);
    return IRQ_OK;
}

void irq_clear_pending(uint32_t intid)
{
    if (valid_id(intid) && ctl->clear_pending) ctl->clear_pending(intid);
}

void irq_dispatch_raw(uint32_t raw)
{
    uint32_t id = raw & 0x3FF;
    if (id >= IRQ_MAX_ID) { stats.spurious++; return; }          /* nothing to complete: no EOI for spurious */
    if (id >= ctl->max_id || !table[id].fn) {
        stats.unknown++; stats.last_unknown = id;
        if (id < ctl->max_id) ctl->disable(id);                   /* stop a possible interrupt storm */
        ctl->eoi(raw);
        return;
    }
    table[id].fn(id, table[id].arg);
    stats.handled++;
    ctl->eoi(raw);
}

void irq_dispatch(void)
{
    if (!ctl) return;                                             /* no controller: nothing can have been acknowledged */
    irq_dispatch_raw(ctl->ack());
}

const struct irq_stats *irq_get_stats(void) { return &stats; }
