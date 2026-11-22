#include <virtqueue.h>

static int pow2(uint32_t n) { return n && !(n & (n - 1)); }

int vq_init(struct vq *q, uint16_t qn, volatile uint16_t *avail, volatile uint16_t *used, void (*barrier)(void))
{
    if (!q || !avail || !used || qn == 0 || qn > VQ_MAX || !pow2(qn)) return VQ_EARG;
    q->qn = qn; q->last_used = 0; q->avail_idx = 0; q->broken = 0; q->outstanding = 0; q->barrier = barrier;
    for (uint32_t i = 0; i < VQ_MAX; i++) { q->inflight[i] = 0; q->cap[i] = 0; }
    q->avail = avail; q->used = used;
    return VQ_OK;
}

int vq_submit(struct vq *q, uint16_t head, uint32_t device_write_cap)
{
    if (!q || head >= q->qn) return VQ_EARG;
    if (q->broken) return VQ_EPROTO;
    if (q->outstanding >= q->qn || q->inflight[head]) return VQ_EFULL;
    q->avail[2 + (q->avail_idx & (q->qn - 1))] = head;
    if (q->barrier) q->barrier();                                 /* ring entry visible before the index */
    q->avail_idx++;
    q->avail[1] = q->avail_idx;
    q->inflight[head] = 1; q->cap[head] = device_write_cap; q->outstanding++;
    if (q->barrier) q->barrier();
    return VQ_OK;
}

int vq_complete(struct vq *q, uint16_t *id, uint32_t *len)
{
    if (!q || !id || !len) return VQ_EARG;
    if (q->broken) return VQ_EPROTO;
    uint16_t dev_idx = q->used[1];
    if (q->barrier) q->barrier();
    uint16_t ahead = (uint16_t)(dev_idx - q->last_used);          /* wraps like the index */
    if (ahead == 0) return VQ_EEMPTY;
    if (ahead > q->outstanding) { q->broken = 1; return VQ_EPROTO; }   /* the device claims more completions than we submitted */
    volatile const uint32_t *e = (volatile const uint32_t *)(volatile const void *)(q->used + 2) + 2u * (q->last_used & (q->qn - 1));
    uint32_t eid = e[0], elen = e[1];
    if (eid >= q->qn || !q->inflight[eid]) { q->broken = 1; return VQ_EPROTO; }   /* not a chain we have in flight (also catches a repeated id) */
    if (elen > q->cap[eid]) { q->broken = 1; return VQ_EPROTO; }                  /* wrote more than we allowed */
    q->inflight[eid] = 0; q->outstanding--; q->last_used++;
    *id = (uint16_t)eid; *len = elen;
    return VQ_OK;
}
