/*
 * kernel/include/fdt.h — flattened device tree parsing interface.
 *
 * Minimal device tree parser for ARM64 boot protocol. Extracts:
 *   - the single /memory node            (chapter 3.4)
 *   - the memory reserve map             (section 5.3)
 *   - /reserved-memory child nodes       (section 3.5.4) that carry a static `reg`
 *
 * Definitions are provided via kernel/include/mm.h (fdt_info, fdt_reservation, etc.)
 * This header is kept for API separation; include mm.h instead.
 */
#ifndef AURORA_FDT_H
#define AURORA_FDT_H

#include <mm.h>

/* All FDT types and functions are declared in mm.h:
 *   struct fdt_reservation, struct fdt_info, fdt_parse(), fdt_strerror()
 *   struct fdt_platform, fdt_platform_parse()
 *   struct fdt_virtio, fdt_virtio_mmio(), fdt_fwcfg_mmio()
 */

#endif