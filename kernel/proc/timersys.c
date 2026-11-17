/* kernel/proc/timersys.c - SYS_TIMER and the completion poller (see timersys.h). The table and every rule about time live in the pure core (timersvc.c). */
#include <timer.h>
#include <timersys.h>
#include <ui_sys.h>

#define MAX_GRANTS 8

static struct tsvc S;
static uint32_t grants[MAX_GRANTS];
static uint8_t posted[TS_MAX];                               /* a DONE timer whose completion event was accepted by its owner's queue */

void timersys_init(void)
{
    tsvc_init(&S);
    for (int i = 0; i < MAX_GRANTS; i++) grants[i] = 0;
    for (int i = 0; i < TS_MAX; i++) posted[i] = 0;
}

int timersys_grant(uint32_t principal)
{
    if (principal == 0 || principal == PRINCIPAL_NONE || (principal & 0xFF000000u) == 0x7F000000u) return -1;   /* kernel, none and every reserved service identity */
    for (int i = 0; i < MAX_GRANTS; i++) if (grants[i] == principal) return 0;
    for (int i = 0; i < MAX_GRANTS; i++) if (!grants[i]) { grants[i] = principal; return 0; }
    return -1;
}
static int has_cap(uint32_t principal)
{
    for (int i = 0; i < MAX_GRANTS; i++) if (grants[i] && grants[i] == principal) return 1;
    return 0;
}

static uint64_t now_ms(void)
{
    uint64_t ms = 0;
    if (tsvc_ticks_to_ms(timer_count(), timer_freq(), &ms) != TS_OK) return 0;      /* a broken counter reads as "no time": nothing completes early */
    return ms;
}

int64_t timersys_syscall(struct proc *p, uint64_t op, uint64_t a1, uint64_t a2)
{
    if (!p || p->principal == PRINCIPAL_NONE) return AE_EPERM;
    if (!has_cap(p->principal)) return AE_EPERM;                                    /* no capability, no timers: refused before anything is looked at */
    uint32_t pid = (uint32_t)p->pid; uint64_t gen = p->gen;
    switch (op) {
    case TIMER_START: {
        int rc = tsvc_start(&S, now_ms(), a1, pid, gen);
        if (rc >= 0) { posted[rc] = 0; return rc; }
        return AE_TM_BASE + rc;
    }
    case TIMER_CANCEL: {
        if (a1 >= TS_MAX) return AE_TM_BASE + TS_E_NOENT;
        int rc = tsvc_cancel(&S, (uint32_t)a1, pid, gen);
        return rc == TS_OK ? 0 : AE_TM_BASE + rc;
    }
    case TIMER_QUERY: {
        if (!user_writable(a2, 16)) return AE_EFAULT;                               /* a bad buffer is EFAULT before any state is read */
        if (a1 >= TS_MAX) return AE_TM_BASE + TS_E_NOENT;
        uint8_t st = 0; uint64_t rem = 0;
        int rc = tsvc_query(&S, now_ms(), (uint32_t)a1, pid, gen, &st, &rem);
        if (rc != TS_OK) return AE_TM_BASE + rc;
        uint8_t out[16]; for (int i = 0; i < 16; i++) out[i] = 0;
        out[0] = st; for (int i = 0; i < 8; i++) out[8 + i] = (uint8_t)(rem >> (8 * i));
        int c = copy_to_user(a2, out, sizeof out);
        return c == AE_OK ? 0 : c;
    }
    case TIMER_RELEASE: {
        if (a1 >= TS_MAX) return AE_TM_BASE + TS_E_NOENT;
        int rc = tsvc_release(&S, (uint32_t)a1, pid, gen);
        if (rc == TS_OK) posted[a1] = 0;
        return rc == TS_OK ? 0 : AE_TM_BASE + rc;
    }
    case TIMER_NOW: return (int64_t)now_ms();
    default: return AE_EINVAL;
    }
}

void timersys_tick(void)
{
    tsvc_poll(&S, now_ms());
    for (int i = 0; i < TS_MAX; i++) {
        if (S.t[i].state != TS_DONE) { posted[i] = 0; continue; }
        if (posted[i]) continue;
        uint8_t ev[12] = { 1, UIP_EVENT_TIMER, 0, 0, 0, 0, 0, 0, (uint8_t)i, TS_DONE, 0, 0 };
        if (ui_sys_post((int)S.t[i].pid, S.t[i].gen, ev, sizeof ev)) posted[i] = 1;   /* a full queue: not delivered, retried next tick */
    }
}

void timersys_owner_gone(int pid, uint64_t gen)
{
    tsvc_owner_gone(&S, (uint32_t)pid, gen);
    for (int i = 0; i < TS_MAX; i++) if (S.t[i].state == TS_FREE) posted[i] = 0;
}
