/* kernel/ui/review.c - the trusted-review state machine (see kernel/include/review.h). Pure, bounded, no libc. */
#include <review.h>

static void zero(void *p, unsigned n) { uint8_t *b = p; for (unsigned i = 0; i < n; i++) b[i] = 0; }

void rv_init(struct rv *r) { zero(r, sizeof *r); r->state = RV_IDLE; }

static uint64_t fnv_byte(uint64_t h, uint8_t b) { h ^= b; h *= 1099511628211ULL; return h; }
static uint64_t fnv_u(uint64_t h, uint64_t v, int bytes) { for (int i = 0; i < bytes; i++) h = fnv_byte(h, (uint8_t)(v >> (8 * i))); return h; }

uint64_t rv_digest(const struct rv_request *q)
{
    uint64_t h = 1469598103934665603ULL;
    const struct aura_intent *in = &q->intent;
    h = fnv_u(h, in->version, 1); h = fnv_u(h, in->kind, 1); h = fnv_u(h, in->status, 1); h = fnv_u(h, in->reason, 1); h = fnv_u(h, in->risk, 1);
    h = fnv_u(h, in->target, 1); h = fnv_u(h, in->arg_kind, 1); h = fnv_u(h, (uint32_t)in->arg, 4);
    for (int i = 0; i < AURA_PREVIEW_MAX; i++) h = fnv_byte(h, (uint8_t)in->preview[i]);        /* all 48 bytes, including everything after the NUL: nothing is left unbound */
    h = fnv_u(h, q->pid, 4); h = fnv_u(h, q->gen, 8); h = fnv_u(h, q->principal, 4); h = fnv_u(h, q->nonce, 8);
    h = fnv_u(h, (uint32_t)q->prior, 4); h = fnv_u(h, (uint32_t)q->next, 4);
    return h;
}

/* time that goes backwards is "no time passed" */
static uint64_t clock_of(struct rv *r, uint64_t now) { if (now < r->last_now) { r->anomalies++; return r->last_now; } r->last_now = now; return now; }
static int live(const struct rv *r) { return r->state == RV_OPEN || r->state == RV_ARMED || r->state == RV_APPROVED; }
static void retire(struct rv *r, int to) { r->state = (uint8_t)to; r->ticket = 0; r->digest = 0; }

static int s1_ok(const struct rv_request *q, const struct aura_caps *caps)
{
    if (!q || !caps) return 0;
    const struct aura_intent *in = &q->intent;
    if (!aura_validate(in, caps)) return 0;
    if (in->kind != AK_SET_SETTING || in->status != AS_NEEDS_APPROVAL || in->risk != 1) return 0;          /* only a validated S1 settings change is ever reviewed */
    return 1;
}

int64_t rv_open(struct rv *r, uint64_t now_ms, const struct rv_request *q, const struct aura_caps *caps)
{
    if (!r || !q || !caps) return RV_E_ARG;
    if (live(r)) return RV_E_BUSY;
    if (q->gen == 0 || q->principal == 0 || q->principal == 0xFFFFFFFFu) return RV_E_ARG;
    if (!s1_ok(q, caps)) return RV_E_INTENT;
    if (r->last_ticket == 0xFFFFFFFFFFFFFFFFULL) return RV_E_ARG;                                   /* tickets never wrap */
    uint64_t now = clock_of(r, now_ms);
    r->last_ticket++; r->ticket = r->last_ticket;
    r->digest = rv_digest(q); r->pid = q->pid; r->gen = q->gen;
    r->t_open = now; r->t_armed = r->t_approved = 0; r->state = RV_OPEN;
    return (int64_t)r->ticket;
}

int rv_tick(struct rv *r, uint64_t now_ms)
{
    if (!r) return RV_IDLE;
    uint64_t now = clock_of(r, now_ms);
    if (r->state == RV_OPEN || r->state == RV_ARMED) {
        if (now - r->t_open >= RV_TIMEOUT_MS) { retire(r, RV_EXPIRED); return r->state; }
        if (r->state == RV_ARMED && now - r->t_armed >= RV_APPROVE_WINDOW_MS) r->state = RV_OPEN;        /* the arming lapsed: press the gesture again */
    } else if (r->state == RV_APPROVED && now - r->t_approved >= RV_APPROVE_WINDOW_MS) retire(r, RV_EXPIRED);   /* an approval nobody executed does not wait forever */
    return r->state;
}

int rv_attention(struct rv *r, uint64_t now_ms, int repeat)
{
    if (!r) return RV_E_ARG;
    rv_tick(r, now_ms);
    if (r->state != RV_OPEN && r->state != RV_ARMED) return RV_E_STATE;
    if (repeat) return RV_E_EARLY;                                                  /* a held key is not a deliberate gesture */
    uint64_t now = r->last_now;
    if (now - r->t_open < RV_ARM_DELAY_MS) return RV_E_EARLY;                      /* too soon after the review opened: nothing changes */
    r->state = RV_ARMED; r->t_armed = now;
    return RV_OK;
}

int rv_approve(struct rv *r, uint64_t now_ms, uint64_t ticket)
{
    if (!r) return RV_E_ARG;
    int was_armed = r->state == RV_ARMED;
    rv_tick(r, now_ms);
    if (was_armed && r->state == RV_OPEN && ticket == r->ticket) return RV_E_LAPSED;       /* the 10 s window ended before this click: say so, and Approve is locked again */
    if (ticket == 0) return RV_E_TICKET;
    if (!live(r)) return RV_E_STATE;                                                 /* nothing is waiting for an approval: a retired ticket approves nothing */
    if (ticket != r->ticket) return RV_E_TICKET;                                    /* an approval for another (older) review */
    if (r->state == RV_OPEN) return RV_E_EARLY;                                      /* never armed (or the arming lapsed): Approve is locked */
    if (r->state != RV_ARMED) return RV_E_STATE;
    r->state = RV_APPROVED; r->t_approved = r->last_now;
    return RV_OK;
}

int rv_decline(struct rv *r, uint64_t ticket)
{
    if (!r) return RV_E_ARG;
    if (!live(r)) return RV_E_STATE;
    if (ticket != r->ticket) return RV_E_TICKET;
    retire(r, RV_DECLINED);
    return RV_OK;
}

int rv_requester_gone(struct rv *r, uint32_t pid, uint64_t gen)
{
    if (!r || !live(r)) return 0;
    if (r->pid != pid || r->gen != gen) return 0;
    retire(r, RV_GONE);
    return 1;
}

int rv_verify(struct rv *r, uint64_t now_ms, uint64_t ticket, const struct rv_request *now_q, const struct aura_caps *caps)
{
    if (!r || !now_q || !caps) return RV_E_ARG;
    rv_tick(r, now_ms);
    if (r->state != RV_APPROVED) return RV_E_STATE;                                 /* not approved, already consumed, declined, expired or gone */
    if (ticket == 0 || ticket != r->ticket) return RV_E_TICKET;
    if (!s1_ok(now_q, caps)) { retire(r, RV_DECLINED); return RV_E_INTENT; }
    if (rv_digest(now_q) != r->digest) { retire(r, RV_DECLINED); return RV_E_CHANGED; }
    retire(r, RV_DONE);                                                             /* consumed: the approval can never be used again */
    return RV_OK;
}

const char *rv_state_name(int s)
{
    static const char *const N[] = { "idle", "open", "armed", "approved", "declined", "expired", "gone", "done" };
    return s >= 0 && s <= RV_DONE ? N[s] : "?";
}
