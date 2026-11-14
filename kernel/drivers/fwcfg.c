/*
 * kernel/drivers/fwcfg.c - QEMU fw_cfg MMIO driver (ARM "virt"). Register layout and DMA protocol: see fwcfg.h.
 *
 * Lifecycle: fwcfg_probe() runs once from the validated device tree. It only READS, so it is safe before the MMU is on
 * (with the MMU off every data access is Device memory). fwcfg_map() registers the window with the MMU policy and maps it
 * Device after vmm_init(). All waits are bounded; every failure is a result code, never a panic.
 *
 * Memory ordering: fw_cfg is a Device-nGnRnE window, so accesses to it are not reordered with each other. The DMA descriptor
 * lives in ordinary RAM, so a DSB SY separates "descriptor written" from "DMA address register written". The device-tree node
 * carries `dma-coherent`: the device reads the descriptor from, and writes its completion to, the same memory the CPU sees,
 * so no cache maintenance is required for the descriptor. (QEMU TCG does not model caches at all; see the plan, section 10.)
 */
#include <fwcfg.h>
#include <mm.h>

#define DMA_SPIN_LIMIT 2000000u   /* bounded wait for the control word to clear; completion is immediate in QEMU */

static struct {
    int probed, mapped, need_dma;
    uint64_t base;
} F;

static inline void dsb(void) { __asm__ volatile ("dsb sy" ::: "memory"); }

static void mmio_select(void *ctx, uint16_t key)
{
    (void)ctx;
    *(volatile uint16_t *)(uintptr_t)(F.base + FWCFG_MMIO_SEL) = __builtin_bswap16(key);   /* selector is big-endian */
    dsb();
}

static void mmio_read(void *ctx, void *buf, uint32_t len)
{
    (void)ctx;
    volatile uint8_t *data = (volatile uint8_t *)(uintptr_t)(F.base + FWCFG_MMIO_DATA);
    uint8_t *o = buf;
    for (uint32_t i = 0; i < len; i++) o[i] = *data;       /* byte reads walk the item in order */
}

static const struct fwcfg_ops OPS = { mmio_select, mmio_read };

uint64_t fwcfg_base(void) { return F.probed ? F.base : 0; }

int fwcfg_probe(const void *fdt, int need_dma)
{
    static struct fdt_virtio nodes[4];
    F.probed = F.mapped = 0; F.base = 0; F.need_dma = need_dma;
    if (!fdt) return FWCFG_ENODEV;
    int n = fdt_fwcfg_mmio(fdt, nodes, 4);
    if (n <= 0) return FWCFG_ENODEV;
    if (n != 1) return FWCFG_ENODE;                         /* two fw_cfg devices would be ambiguous */

    uint64_t base = nodes[0].base, size = nodes[0].size;
    if (size < FWCFG_MMIO_SPAN || base + size < base) return FWCFG_ENODE;
    if (base & 7u) return FWCFG_ENODE;                      /* the 64-bit registers must be naturally aligned */
    if ((base & (PAGE_SIZE - 1)) + FWCFG_MMIO_SPAN > PAGE_SIZE) return FWCFG_ENODE;   /* must fit in the one page we map */
    if (pmm_in_ram((uintptr_t)base) || pmm_in_ram((uintptr_t)(base + FWCFG_MMIO_SPAN - 1))) return FWCFG_ENODE;   /* never RAM */
    F.base = base;

    int rc = fwcfg_check_device(&OPS, 0, need_dma, 0);
    if (rc) { F.base = 0; return rc; }
    if (need_dma) {                                         /* the DMA register reads back "QEMU CFG" (big-endian) */
        uint64_t sig = __builtin_bswap64(*(volatile uint64_t *)(uintptr_t)(base + FWCFG_MMIO_DMA));
        if (sig != FWCFG_DMA_SIGNATURE) { F.base = 0; return FWCFG_ENODMA; }
    }
    F.probed = 1;
    return FWCFG_OK;
}

int fwcfg_lookup(const char *name, struct fwcfg_file *out)
{
    if (!F.probed) return FWCFG_ESTATE;
    return fwcfg_find_file(&OPS, 0, name, out);
}

int fwcfg_map(void)
{
    if (!F.probed) return FWCFG_ESTATE;
    if (F.mapped) return FWCFG_OK;
    uint64_t page = F.base & ~(PAGE_SIZE - 1);
    if (vmm_register_mmio(page, PAGE_SIZE) != VMM_OK || vmm_map(page, page, PAGE_SIZE, VMM_ATTR_DEVICE) != VMM_OK) return FWCFG_EMAP;
    F.mapped = 1;
    return FWCFG_OK;
}

int fwcfg_dma_write(uint16_t key, const void *buf, uint32_t len)
{
    static struct fwcfg_dma d;                              /* kernel BSS: ordinary RAM, identity-mapped, 8-byte aligned */
    if (!F.probed || !F.need_dma) return FWCFG_ESTATE;
    if (vmm_mmu_enabled() && !F.mapped) return FWCFG_ESTATE;
    if (!buf || len == 0 || len > 0x10000u) return FWCFG_EARG;
    if (!pmm_in_ram((uintptr_t)buf) || !pmm_in_ram((uintptr_t)buf + len - 1)) return FWCFG_EARG;   /* the device uses physical addresses */

    fwcfg_dma_fill(&d, key, FWCFG_DMA_WRITE, len, (uint64_t)(uintptr_t)buf);
    dsb();                                                  /* descriptor and payload visible before the device is told */
    *(volatile uint64_t *)(uintptr_t)(F.base + FWCFG_MMIO_DMA) = __builtin_bswap64((uint64_t)(uintptr_t)&d);   /* triggers the request */
    for (uint32_t i = 0; i < DMA_SPIN_LIMIT; i++) {
        dsb();
        int st = fwcfg_dma_status((const volatile uint8_t *)d.b);
        if (st == FWCFG_EDMA) return FWCFG_EDMA;
        if (st == 1) return FWCFG_OK;
    }
    return FWCFG_ETIMEOUT;
}
