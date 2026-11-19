/* kernel/ui/hold.c - the hold-to-confirm state machine (see kernel/include/hold.h). Pure, bounded, no libc. */
#include <hold.h>

void hold_init(struct hold *h)
{
    uint8_t *z = (uint8_t *)h;
    for (unsigned i = 0; i < sizeof *h; i++) z[i] = 0;
    h->state = HD_IDLE; h->source = HS_NONE;
}

/* time that goes backwards is "no time passed" */
static uint64_t clock_of(struct hold *h, uint64_t now) { if (now < h->last_now) { h->anomalies++; return h->last_now; } h->last_now = now; return now; }

static void cancel(struct hold *h) { if (h->state == HD_HOLDING) { h->state = HD_CANCELLED; h->source = HS_NONE; } }
static void start(struct hold *h, int src, uint64_t now)
{
    if (h->state == HD_HOLDING || h->state == HD_CONFIRMED) return;   /* a second press neither restarts nor extends a hold, and after a confirmation nothing starts until the starting key/button is released */
    h->state = HD_HOLDING; h->source = (uint8_t)src; h->t_start = now;
}

void hold_event(struct hold *h, int ev, uint64_t now_ms, int repeat)
{
    if (!h) return;
    uint64_t now = clock_of(h, now_ms);
    if (repeat) return;                                         /* auto-repeat: ignored, whatever the key */
    switch (ev) {
    case HE_KEY_DOWN:   start(h, HS_KEY, now); break;
    case HE_PTR_DOWN:   start(h, HS_POINTER, now); break;
    case HE_KEY_UP:     if (h->state == HD_HOLDING && h->source == HS_KEY) cancel(h); else if (h->state == HD_CONFIRMED && h->source == HS_KEY) { h->state = HD_IDLE; h->source = HS_NONE; } break;
    case HE_PTR_UP:     if (h->state == HD_HOLDING && h->source == HS_POINTER) cancel(h); else if (h->state == HD_CONFIRMED && h->source == HS_POINTER) { h->state = HD_IDLE; h->source = HS_NONE; } break;
    case HE_PTR_LEAVE:  if (h->state == HD_HOLDING && h->source == HS_POINTER) cancel(h); break;
    case HE_OTHER_KEY: case HE_ESCAPE: case HE_FOCUS_CHANGE: case HE_MODAL_CHANGE: cancel(h); break;
    case HE_PTR_ENTER: default: break;                          /* re-entering never resumes a cancelled hold */
    }
}

int hold_poll(struct hold *h, uint64_t now_ms)
{
    if (!h) return 0;
    uint64_t now = clock_of(h, now_ms);
    if (h->state != HD_HOLDING) return 0;
    if (now - h->t_start < HOLD_MS) return 0;
    h->state = HD_CONFIRMED; h->confirmations++;                /* once: the state is no longer HOLDING */
    return 1;
}

uint32_t hold_segments(const struct hold *h, uint64_t now_ms)
{
    if (!h) return 0;
    if (h->state == HD_CONFIRMED) return HOLD_SEGMENTS;
    if (h->state != HD_HOLDING) return 0;
    uint64_t now = now_ms < h->last_now ? h->last_now : now_ms;
    uint64_t seg = (now - h->t_start) / HOLD_SEGMENT_MS;
    return seg > HOLD_SEGMENTS ? HOLD_SEGMENTS : (uint32_t)seg;
}

const char *hold_state_name(int s)
{
    static const char *const N[] = { "idle", "holding", "confirmed", "cancelled" };
    return s >= 0 && s <= HD_CANCELLED ? N[s] : "?";
}
