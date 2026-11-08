/*
 * kernel/include/irq.h — generic interrupt dispatch, independent of the interrupt controller.
 *
 * A controller driver (kernel/arch/aarch64/gicv2.c) supplies irq_controller_ops; this layer owns the
 * handler table, statistics and the policy for spurious/unknown interrupts:
 *   - spurious (ID >= IRQ_MAX_ID, e.g. GICv2 ID 1023): counted, never acknowledged with EOI
 *   - unknown (valid ID, no handler): counted, the line is DISABLED (so a level-triggered source cannot
 *     storm the CPU), and the interrupt is completed with EOI
 *   - handled: handler runs with IRQs masked, then EOI
 * IRQs are masked on exception entry, so handlers never nest.
 */
#ifndef AURORA_IRQ_H
#define AURORA_IRQ_H
#include <stdint.h>

#define IRQ_MAX_ID 1020u            /* GICv2: IDs 1020..1023 are special */

typedef void (*irq_handler_t)(uint32_t intid, void *arg);

struct irq_controller_ops {
    uint32_t (*ack)(void);                  /* returns the raw acknowledge value (ID in bits [9:0]) */
    void     (*eoi)(uint32_t raw);
    void     (*enable)(uint32_t intid);
    void     (*disable)(uint32_t intid);
    uint32_t max_id;                        /* number of interrupt IDs the hardware implements */
};

struct irq_stats {
    uint64_t handled, unknown, spurious;
    uint32_t last_unknown;
};

enum { IRQ_OK = 0, IRQ_ERR_RANGE = -1, IRQ_ERR_BUSY = -2, IRQ_ERR_NOCTRL = -3, IRQ_ERR_NULL = -4 };

void irq_set_controller(const struct irq_controller_ops *ops);
int  irq_register(uint32_t intid, irq_handler_t h, void *arg);
int  irq_unregister(uint32_t intid);
int  irq_enable_line(uint32_t intid);       /* controller-level unmask of one interrupt ID */
void irq_dispatch(void);                    /* called from the IRQ vector */
void irq_dispatch_raw(uint32_t raw);        /* same, for an already-acknowledged value (tests) */
const struct irq_stats *irq_get_stats(void);

/* CPU-level masking (PSTATE.I) */
static inline void irq_cpu_enable(void)  { __asm__ volatile ("msr daifclr, #2" ::: "memory"); }
static inline void irq_cpu_disable(void) { __asm__ volatile ("msr daifset, #2" ::: "memory"); }
static inline int  irq_cpu_enabled(void) { uint64_t d; __asm__ volatile ("mrs %0, daif" : "=r"(d)); return !(d & 0x80); }

#endif
