#ifndef AURORA_VIRTQUEUE_H
#define AURORA_VIRTQUEUE_H
#include <stddef.h>
#include <stdint.h>

/*
 * Split virtqueue ring bookkeeping (virtio 1.x), shared by every virtio driver, freestanding and host-testable.
 * It owns only the avail/used index arithmetic and the checks on what the DEVICE writes into the used ring. Everything the device writes is hostile:
 * the used index may jump, the id may be out of range or not an id we submitted, the length may exceed what we gave it. Those are refused with VQ_EPROTO
 * and the queue is marked broken; a broken queue accepts no more work. Descriptor contents stay with the driver (it owns the buffers).
 *
 * Not yet used by virtio_blk.c / virtio_input.c: migrating them is a separate change guarded by their QEMU suites (docs/kernel/audio-and-drivers-research.md).
 */
#define VQ_MAX 256u                      /* queue size bound; must be a power of two */

#define VQ_OK      0
#define VQ_EARG   -1
#define VQ_EFULL  -2                     /* no free ring slot / id already in flight */
#define VQ_EEMPTY -3                     /* nothing completed */
#define VQ_EPROTO -4                     /* the device broke the protocol; the queue is now unusable */

struct vq {
    uint16_t qn;                         /* queue size, power of two, <= VQ_MAX */
    uint16_t last_used;                  /* used entries consumed so far (wraps like the device's index) */
    uint16_t avail_idx;                  /* avail entries published so far */
    uint8_t  broken;
    uint8_t  inflight[VQ_MAX];           /* 1 while a head id is submitted and not yet completed */
    uint32_t cap[VQ_MAX];                /* bytes the device may write for that head (0 = device-read only) */
    uint16_t outstanding;
    volatile uint16_t *avail;            /* flags, idx, ring[qn] */
    volatile uint16_t *used;             /* flags, idx, then {u32 id, u32 len}[qn] */
    void (*barrier)(void);               /* full memory barrier; NULL on a host test with no concurrency */
};

int vq_init(struct vq *q, uint16_t qn, volatile uint16_t *avail, volatile uint16_t *used, void (*barrier)(void));
/* Publish descriptor chain `head` (< qn). `device_write_cap` = the most bytes the device may report writing. */
int vq_submit(struct vq *q, uint16_t head, uint32_t device_write_cap);
/* Take one completion: *id is a head we submitted, *len <= the cap given at submit. */
int vq_complete(struct vq *q, uint16_t *id, uint32_t *len);
#endif