/* kernel/proc/procsnap.c - read-only process snapshot (see procsnap.h). */
#include <procsnap.h>

static uint8_t map_state(enum proc_state s)
{
    switch (s) {
    case PROC_NEW: return PSS_NEW;
    case PROC_READY: return PSS_READY;
    case PROC_RUNNING: return PSS_RUNNING;
    case PROC_BLOCKED: return PSS_BLOCKED;
    case PROC_TERMINATED: return PSS_EXITED;
    case PROC_FAULTED: return PSS_FAULTED;
    default: return 0;                                  /* unused, or a corrupt value: not listed */
    }
}

uint32_t procsnap_take(struct procsnap *out, procsnap_get_fn get)
{
    if (!out || !get) return 0;
    out->n = 0;
    for (int pid = 1; pid <= PROC_MAX; pid++) {
        const struct proc *p = get(pid);
        if (!p) continue;
        uint8_t st = map_state(p->state);
        if (!st || p->pid != pid) continue;             /* a slot that does not describe itself is not shown */
        struct procsnap_entry *e = &out->e[out->n++];
        e->pid = (uint8_t)pid; e->state = st; e->gen = p->gen;
        uint32_t i = 0;
        for (; i + 1 < PROCSNAP_NAME && p->name[i]; i++) { unsigned char c = (unsigned char)p->name[i]; e->name[i] = (c >= 0x20 && c <= 0x7e) ? (char)c : '?'; }
        e->name[i] = 0;
        if (!i) { e->name[0] = '?'; e->name[1] = 0; }   /* an unnamed process still gets a readable label */
        e->ticks = p->ticks_run; e->switches = p->switches_in; e->yields = p->yields; e->preemptions = p->preemptions;
        e->pages = p->as.npages < 0 ? 0u : (uint32_t)p->as.npages;
        e->exit_code = st == PSS_EXITED ? p->exit_code : 0;
    }
    return out->n;
}


const char *procsnap_state_name(uint8_t s)
{
    switch (s) {
    case PSS_NEW: return "new";
    case PSS_READY: return "ready";
    case PSS_RUNNING: return "running";
    case PSS_BLOCKED: return "blocked";
    case PSS_EXITED: return "exited";
    case PSS_FAULTED: return "faulted";
    default: return "?";
    }
}

uint64_t procsnap_digest(const struct procsnap *s)
{
    uint64_t h = 1469598103934665603ULL;
#define MIX(v) do { h ^= (uint64_t)(v); h *= 1099511628211ULL; } while (0)
    MIX(s->n);
    for (uint32_t i = 0; i < s->n && i < PROC_MAX; i++) {
        const struct procsnap_entry *e = &s->e[i];
        MIX(e->pid); MIX(e->state); MIX(e->gen); MIX(e->ticks); MIX(e->switches); MIX(e->yields); MIX(e->preemptions); MIX(e->pages); MIX(e->exit_code);
        for (uint32_t k = 0; k < PROCSNAP_NAME && e->name[k]; k++) MIX((unsigned char)e->name[k]);
    }
#undef MIX
    return h;
}
