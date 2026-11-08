#ifndef AURORA_GIC_H
#define AURORA_GIC_H
#include <irq.h>
#include <mm.h>
#include <stdint.h>

/* GICv2 driver (ARM IHI 0048B). Init registers the distributor and CPU-interface windows with the
 * mapping policy (addresses come from the device tree) and maps them as Device memory. */
int      gicv2_init(const struct fdt_platform *plat);       /* 0 or a negative error */
uint32_t gicv2_max_id(void);
uint32_t gicv2_ack_raw(void);                                /* raw GICC_IAR read (acknowledges!) */
int      gicv2_is_pending(uint32_t intid);
void     gicv2_set_pending(uint32_t intid);                  /* software-pend (SPI/PPI) */
void     gicv2_send_sgi_self(uint32_t sgi);                  /* software-generated interrupt to this CPU */
uint32_t gicv2_pmr(void);

#endif
