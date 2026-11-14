/*
 * kernel/include/virtio_mmio.h - the small part of the virtio-mmio transport shared by every virtio driver (issue #7, G5-B).
 * Scope is deliberately narrow: register offsets, status bits, and ONE idempotent routine that declares and maps the device
 * tree's virtio-mmio window. Queue handling stays in each driver (block: one request in flight; input: pre-posted buffers).
 */
#ifndef AURORA_VIRTIO_MMIO_H
#define AURORA_VIRTIO_MMIO_H
#include <mm.h>

#define VIO_R_MAGIC 0x000
#define VIO_R_VERSION 0x004
#define VIO_R_DEVID 0x008
#define VIO_R_DEVFEAT 0x010
#define VIO_R_DEVFEATSEL 0x014
#define VIO_R_DRVFEAT 0x020
#define VIO_R_DRVFEATSEL 0x024
#define VIO_R_QSEL 0x030
#define VIO_R_QNUMMAX 0x034
#define VIO_R_QNUM 0x038
#define VIO_R_QREADY 0x044
#define VIO_R_QNOTIFY 0x050
#define VIO_R_INTSTATUS 0x060
#define VIO_R_INTACK 0x064
#define VIO_R_STATUS 0x070
#define VIO_R_QDESCLO 0x080
#define VIO_R_QDESCHI 0x084
#define VIO_R_QDRVLO 0x090
#define VIO_R_QDRVHI 0x094
#define VIO_R_QDEVLO 0x0a0
#define VIO_R_QDEVHI 0x0a4
#define VIO_R_CFGGEN 0x0fc
#define VIO_R_CONFIG 0x100

#define VIO_ST_ACK 1u
#define VIO_ST_DRIVER 2u
#define VIO_ST_DRIVER_OK 4u
#define VIO_ST_FEATURES_OK 8u
#define VIO_ST_NEEDS_RESET 0x40u
#define VIO_ST_FAILED 0x80u
#define VIO_F_VERSION_1_HI 1u        /* feature bit 32 = bit 0 of the high word */
#define VIO_MAGIC 0x74726976u        /* "virt" */
#define VIO_DEV_BLOCK 2u
#define VIO_DEV_INPUT 18u

/* Read the virtio-mmio nodes from the device tree (up to max) and, once, declare + map their pages as Device memory.
 * Returns the node count (>= 0), or a negative VMM error / -1 for an implausible window. Safe to call repeatedly. */
int virtio_map_window(const void *fdt, struct fdt_virtio *nodes, int max);

#endif
