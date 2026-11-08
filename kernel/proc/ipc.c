/*
 * kernel/proc/ipc.c — bounded message passing.
 *
 * Each process owns ONE mailbox (IPC_QUEUE_DEPTH messages of at most IPC_MSG_MAX bytes) that lives in the kernel's process table.
 * send(dest, buf, len): validates len, that dest is a live process, that the caller is authorised for that destination (an
 * explicit grant made by the kernel with proc_ipc_allow; self-send is always allowed), that the mailbox is not full, and only then copies the
 * bytes from user memory (copy_from_user) into kernel storage. No user pointer is ever retained. A receiver can only dequeue
 * from its OWN mailbox: there is no syscall that names another process's queue, so there is nothing to inspect without authority.
 * receive: copy_to_user into the caller's buffer; if that fails or the buffer is too small the message STAYS queued.
 * AUTHORISATION LIFETIME: a grant stores the generation of the recipient INSTANCE (a counter that is never reused), not just its
 * pid. When a recipient ends and its pid is reused, the new process has a different generation, so every old grant simply fails
 * to match. ipc_cleanup also erases incoming grants explicitly. Grants can only name live processes and only the kernel can make
 * them (there is no system call for it).
 * Messages already queued survive their sender; a receiver's mailbox is discarded when it exits or faults.
 */
#include <proc.h>

static int alive(const struct proc *p) { return p && (p->state == PROC_NEW || p->state == PROC_READY || p->state == PROC_RUNNING || p->state == PROC_BLOCKED); }

int proc_ipc_allow(int from, int to)
{
    struct proc *f = proc_get(from), *t = proc_get(to);
    if (!alive(f) || !alive(t)) return -1;                    /* nonexistent, out of range, terminated or faulted: nothing is recorded */
    f->ipc_grant_gen[to] = t->gen;                            /* bound to this recipient instance; repeating the grant is harmless */
    return 0;
}

int ipc_send(struct proc *from, uint64_t dest, uint64_t ubuf, uint64_t len)
{
    if (len > IPC_MSG_MAX) return AE_E2BIG;
    if (dest < 1 || dest > PROC_MAX) return AE_ESRCH;
    struct proc *to = proc_get((int)dest);
    if (!alive(to)) return AE_ESRCH;
    if (to != from && from->ipc_grant_gen[dest] != to->gen) return AE_EPERM;   /* no grant for THIS instance (stale grants never match) */
    if (to->mbox.count >= IPC_QUEUE_DEPTH) return AE_EAGAIN;
    struct ipc_msg m;
    m.from = (uint8_t)from->pid; m.len = (uint8_t)len;
    for (int i = 0; i < IPC_MSG_MAX; i++) m.data[i] = 0;
    int rc = copy_from_user(m.data, ubuf, (size_t)len);
    if (rc != AE_OK) return rc;
    struct ipc_msg *slot = &to->mbox.q[(to->mbox.head + to->mbox.count) % IPC_QUEUE_DEPTH];
    *slot = m;
    to->mbox.count++;
    return AE_OK;
}

int ipc_dequeue(struct proc *p, uint64_t ubuf, uint64_t max, uint64_t *from)
{
    if (p->mbox.count == 0) return AE_EAGAIN;
    struct ipc_msg *m = &p->mbox.q[p->mbox.head];
    if (m->len > max) return AE_E2BIG;
    int rc = copy_to_user(ubuf, m->data, m->len);
    if (rc != AE_OK) return rc;
    if (from) *from = m->from;
    int len = m->len;
    p->mbox.head = (uint8_t)((p->mbox.head + 1) % IPC_QUEUE_DEPTH);
    p->mbox.count--;
    return len;
}

void ipc_cleanup(struct proc *p)
{
    for (int i = 0; i < IPC_QUEUE_DEPTH; i++) { p->mbox.q[i].len = 0; for (int k = 0; k < IPC_MSG_MAX; k++) p->mbox.q[i].data[k] = 0; }
    p->mbox.head = 0; p->mbox.count = 0; p->rx_pending = 0;
    for (int q = 1; q <= PROC_MAX; q++) { struct proc *o = proc_get(q); if (o) o->ipc_grant_gen[p->pid] = 0; }   /* revoke every grant that named this process */
}
