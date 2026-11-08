/*
 * kernel/arch/aarch64/gicv2.c — ARM GICv2 driver (ARM IHI 0048B), single CPU.
 *
 * Distributor (GICD) offsets: CTLR 0x000, TYPER 0x004, ISENABLER 0x100, ICENABLER 0x180, ISPENDR 0x200,
 * ICPENDR 0x280, IPRIORITYR 0x400, ITARGETSR 0x800, SGIR 0xF00.
 * CPU interface (GICC) offsets: CTLR 0x00, PMR 0x04, BPR 0x08, IAR 0x0C, EOIR 0x10.
 *
 * Bases and sizes come from the device tree. Both windows are registered with the physical-target
 * mapping policy and mapped as Device memory; if the policy refuses (e.g. the DT claims RAM), init fails.
 */
#include <console.h>
#include <gic.h>
#include <irq.h>
#include <mm.h>

#define GICD_CTLR      0x000
#define GICD_TYPER     0x004
#define GICD_ISENABLER 0x100
#define GICD_ICENABLER 0x180
#define GICD_ISPENDR   0x200
#define GICD_ICPENDR   0x280
#define GICD_IPRIORITYR 0x400
#define GICD_ITARGETSR 0x800
#define GICD_SGIR      0xF00
#define GICC_CTLR      0x00
#define GICC_PMR       0x04
#define GICC_BPR       0x08
#define GICC_IAR       0x0C
#define GICC_EOIR      0x10

#define IRQ_PRIORITY   0xA0u    /* every enabled interrupt gets the same priority (no nesting yet) */
#define PRIORITY_MASK  0xF0u    /* only priorities numerically below this are signalled */

static volatile uint8_t *gicd, *gicc;
static uint32_t nlines;
static uint32_t gicv2_ack(void);
static void gic_eoi(uint32_t raw);
static void gic_enable(uint32_t id);
static void gic_disable(uint32_t id);
static struct irq_controller_ops ops = { gicv2_ack, gic_eoi, gic_enable, gic_disable, 0 };

static inline uint32_t rd(volatile uint8_t *b, uint32_t off) { return *(volatile uint32_t *)(b + off); }
static inline void wr(volatile uint8_t *b, uint32_t off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }

static int map_window(uint64_t base, uint64_t size)
{
    if (size < 0x1000 || (base | size) & (PAGE_SIZE - 1)) return -1;
    if (vmm_register_mmio(base, size) != VMM_OK) return -2;                 /* policy refuses overlap with RAM/no-map */
    if (vmm_map(base, base, size, VMM_ATTR_DEVICE) != VMM_OK) return -3;
    return 0;
}

int gicv2_init(const struct fdt_platform *p)
{
    if (!p || p->gic_version != 2) return -10;
    if (p->gicd_size < 0x1000 || p->gicc_size < 0x100) return -11;
    int r = map_window(p->gicd_base, p->gicd_size); if (r) return r;
    r = map_window(p->gicc_base, p->gicc_size);     if (r) return r - 10;
    gicd = (volatile uint8_t *)(uintptr_t)p->gicd_base;
    gicc = (volatile uint8_t *)(uintptr_t)p->gicc_base;

    wr(gicd, GICD_CTLR, 0);                                    /* quiesce while configuring */
    nlines = 32 * ((rd(gicd, GICD_TYPER) & 0x1F) + 1);
    if (nlines > IRQ_MAX_ID) nlines = IRQ_MAX_ID;
    for (uint32_t i = 0; i < nlines / 32; i++) {
        wr(gicd, GICD_ICENABLER + 4 * i, 0xFFFFFFFFu);         /* everything off until a driver asks */
        wr(gicd, GICD_ICPENDR + 4 * i, 0xFFFFFFFFu);           /* and nothing left pending from firmware */
    }
    wr(gicc, GICC_PMR, PRIORITY_MASK);
    wr(gicc, GICC_BPR, 0);
    wr(gicc, GICC_CTLR, 1);
    wr(gicd, GICD_CTLR, 1);
    __asm__ volatile ("dsb sy" ::: "memory");
    ops.max_id = nlines;
    irq_set_controller(&ops);
    return 0;
}

uint32_t gicv2_max_id(void) { return nlines; }
uint32_t gicv2_pmr(void) { return rd(gicc, GICC_PMR); }

static void gic_enable(uint32_t id)
{
    if (id >= nlines) return;
    *(volatile uint8_t *)(gicd + GICD_IPRIORITYR + id) = (uint8_t)IRQ_PRIORITY;
    if (id >= 32) *(volatile uint8_t *)(gicd + GICD_ITARGETSR + id) = 0x01;   /* SPIs: CPU 0 (PPIs/SGIs are per-CPU) */
    wr(gicd, GICD_ISENABLER + 4 * (id / 32), 1u << (id % 32));
    __asm__ volatile ("dsb sy" ::: "memory");
}

static void gic_disable(uint32_t id)
{
    if (id >= nlines) return;
    wr(gicd, GICD_ICENABLER + 4 * (id / 32), 1u << (id % 32));
    __asm__ volatile ("dsb sy" ::: "memory");
}

static uint32_t gicv2_ack(void) { return rd(gicc, GICC_IAR); }
uint32_t gicv2_ack_raw(void) { return gicv2_ack(); }
static void gic_eoi(uint32_t raw) { wr(gicc, GICC_EOIR, raw); }

int gicv2_is_pending(uint32_t id) { return id < nlines && ((rd(gicd, GICD_ISPENDR + 4 * (id / 32)) >> (id % 32)) & 1); }
void gicv2_set_pending(uint32_t id) { if (id < nlines) { wr(gicd, GICD_ISPENDR + 4 * (id / 32), 1u << (id % 32)); __asm__ volatile ("dsb sy" ::: "memory"); } }
void gicv2_send_sgi_self(uint32_t sgi) { if (sgi < 16) { wr(gicd, GICD_SGIR, (2u << 24) | sgi); __asm__ volatile ("dsb sy" ::: "memory"); } }


