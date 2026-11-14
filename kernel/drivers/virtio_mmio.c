/* kernel/drivers/virtio_mmio.c - shared virtio-mmio window mapping (see kernel/include/virtio_mmio.h). */
#include <virtio_mmio.h>

int virtio_map_window(const void *fdt, struct fdt_virtio *nodes, int max)
{
    static int mapped;
    if (!fdt) return 0;
    int n = fdt_virtio_mmio(fdt, nodes, max);
    if (n <= 0) return 0;
    uint64_t lo = nodes[0].base, hi = nodes[0].base + nodes[0].size;
    for (int i = 1; i < n; i++) {
        if (nodes[i].base < lo) lo = nodes[i].base;
        if (nodes[i].base + nodes[i].size > hi) hi = nodes[i].base + nodes[i].size;
    }
    lo &= ~(PAGE_SIZE - 1); hi = (hi + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (hi - lo > 64 * PAGE_SIZE) return -1;                     /* implausible window: refuse */
    if (!mapped) {
        if (vmm_register_mmio(lo, hi - lo) != VMM_OK || vmm_map(lo, lo, hi - lo, VMM_ATTR_DEVICE) != VMM_OK) return -1;
        mapped = 1;
    }
    return n;
}
