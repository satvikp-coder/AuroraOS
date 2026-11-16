/* kernel/ui/timersvc.c - the monotonic timer service core (see timersvc.h). No allocation, no libc, no clock of its own. */
#include <timersvc.h>

void tsvc_init(struct tsvc *s)
{
    uint8_t *z = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof *s; i++) z[i] = 0;
}

/* The counter only moves forward: a smaller value than the last one seen is "no time passed" (and counted). */
static uint64_t clamp_now(struct tsvc *s, uint64_t now)
{
    if (s->seen && now < s->last_now) { s->backwards++; return s->last_now; }
    s->seen = 1; s->last_now = now;
    return now;
}

static uint64_t sat_add(uint64_t a, uint64_t b) { return a > 0xFFFFFFFFFFFFFFFFULL - b ? 0xFFFFFFFFFFFFFFFFULL : a + b; }

int tsvc_start(struct tsvc *s, uint64_t now_ms, uint64_t duration_ms, uint32_t pid, uint64_t gen)
{
    if (!s || gen == 0 || duration_ms < TS_MIN_MS || duration_ms > TS_MAX_MS) return TS_E_ARG;
    now_ms = clamp_now(s, now_ms);
    int held = 0;
    for (int i = 0; i < TS_MAX; i++) if (s->t[i].state != TS_FREE && s->t[i].pid == pid && s->t[i].gen == gen) held++;
    if (held >= TS_PER_OWNER) return TS_E_QUOTA;                           /* checked before FULL: a greedy owner is told it is over ITS quota */
    for (int i = 0; i < TS_MAX; i++) {
        if (s->t[i].state != TS_FREE) continue;
        s->t[i].state = TS_RUNNING; s->t[i].pid = pid; s->t[i].gen = gen;
        s->t[i].start_ms = now_ms; s->t[i].duration_ms = duration_ms; s->t[i].deadline_ms = sat_add(now_ms, duration_ms);
        return i;
    }
    return TS_E_FULL;
}

static int owned(const struct tsvc *s, uint32_t id, uint32_t pid, uint64_t gen)
{
    if (id >= TS_MAX || s->t[id].state == TS_FREE) return TS_E_NOENT;
    if (s->t[id].pid != pid || s->t[id].gen != gen) return TS_E_NOENT;       /* someone else's timer looks exactly like no timer */
    return TS_OK;
}

int tsvc_cancel(struct tsvc *s, uint32_t id, uint32_t pid, uint64_t gen)
{
    if (!s) return TS_E_ARG;
    int rc = owned(s, id, pid, gen);
    if (rc) return rc;
    if (s->t[id].state != TS_RUNNING) return TS_E_STATE;                     /* a completed timer cannot be cancelled: it already happened */
    s->t[id].state = TS_CANCELLED;
    return TS_OK;
}

int tsvc_poll(struct tsvc *s, uint64_t now_ms)
{
    int n = 0;
    if (!s) return 0;
    now_ms = clamp_now(s, now_ms);
    for (int i = 0; i < TS_MAX; i++)
        if (s->t[i].state == TS_RUNNING && now_ms >= s->t[i].deadline_ms) { s->t[i].state = TS_DONE; n++; }
    return n;
}

int tsvc_query(struct tsvc *s, uint64_t now_ms, uint32_t id, uint32_t pid, uint64_t gen, uint8_t *state, uint64_t *remaining_ms)
{
    if (!s || !state || !remaining_ms) return TS_E_ARG;
    int rc = owned(s, id, pid, gen);
    if (rc) return rc;
    tsvc_poll(s, now_ms);                                                    /* a late reader still sees completion: the deadline is absolute */
    *state = s->t[id].state;
    *remaining_ms = (s->t[id].state == TS_RUNNING) ? s->t[id].deadline_ms - (s->last_now < s->t[id].deadline_ms ? s->last_now : s->t[id].deadline_ms) : 0;
    return TS_OK;
}

void tsvc_owner_gone(struct tsvc *s, uint32_t pid, uint64_t gen)
{
    if (!s) return;
    for (int i = 0; i < TS_MAX; i++)
        if (s->t[i].state != TS_FREE && s->t[i].pid == pid && s->t[i].gen == gen) {
            uint8_t *z = (uint8_t *)&s->t[i]; for (unsigned k = 0; k < sizeof s->t[i]; k++) z[k] = 0;
        }
}

int tsvc_release(struct tsvc *s, uint32_t id, uint32_t pid, uint64_t gen)
{
    if (!s) return TS_E_ARG;
    int rc = owned(s, id, pid, gen);
    if (rc) return rc;
    if (s->t[id].state == TS_RUNNING) return TS_E_STATE;
    uint8_t *z = (uint8_t *)&s->t[id]; for (unsigned k = 0; k < sizeof s->t[id]; k++) z[k] = 0;
    return TS_OK;
}
