/* kernel/ui/ui_sys.c - connects the pure EL0 UI protocol (ui_proto.c) to processes: the SYS_UI call, event queues, capability, cleanup, routing. */
#include <console.h>
#include <launch.h>
#include <ui_sys.h>
#include <broker_kernel.h>

#define MAX_GRANTS 8

static struct uip_state G;
static uint32_t grants[MAX_GRANTS];

struct evq { uint64_t gen; uint8_t head, count; uint8_t len[UI_EVQ_DEPTH]; uint8_t buf[UI_EVQ_DEPTH][16]; };
static struct evq Q[UIP_MAX_CLIENTS + 1];

void ui_sys_init(int16_t rx, int16_t ry, int16_t rw, int16_t rh)
{
    uip_init(&G, rx, ry, rw, rh);
    for (int i = 0; i < MAX_GRANTS; i++) grants[i] = 0;
    for (int i = 0; i <= UIP_MAX_CLIENTS; i++) { Q[i].gen = 0; Q[i].head = Q[i].count = 0; }
}

const struct uip_state *ui_sys_state(void) { return &G; }

int ui_sys_grant(uint32_t principal)
{
    if (!principal_assignable(principal)) return -1;                       /* the kernel, none and every reserved service identity (installer, settings, shell, Passport, Trail) */
    for (int i = 0; i < MAX_GRANTS; i++) if (grants[i] == principal) return 0;
    for (int i = 0; i < MAX_GRANTS; i++) if (!grants[i]) { grants[i] = principal; return 0; }
    return -1;
}

static int has_cap(uint32_t principal)
{
    for (int i = 0; i < MAX_GRANTS; i++) if (grants[i] && grants[i] == principal) return 1;
    return 0;
}

/* ---- per-client event queues: bounded, oldest dropped when a client does not drain them ---- */
static void q_reset(struct evq *q, uint64_t gen) { q->gen = gen; q->head = q->count = 0; }
static void q_push(uint8_t pid, uint64_t gen, const uint8_t *ev, uint32_t n)
{
    if (pid < 1 || pid > UIP_MAX_CLIENTS || n == 0 || n > 16) return;
    struct evq *q = &Q[pid];
    if (q->gen != gen) q_reset(q, gen);                                  /* a new instance never sees the previous one's events */
    if (q->count == UI_EVQ_DEPTH) { q->head = (uint8_t)((q->head + 1) % UI_EVQ_DEPTH); q->count--; }   /* drop the oldest */
    uint8_t slot = (uint8_t)((q->head + q->count) % UI_EVQ_DEPTH);
    for (uint32_t i = 0; i < n; i++) q->buf[slot][i] = ev[i];
    q->len[slot] = (uint8_t)n; q->count++;
}

int ui_sys_post(int pid, uint64_t gen, const uint8_t *ev, uint32_t n)
{
    if (pid < 1 || pid > UIP_MAX_CLIENTS || gen == 0 || !ev || n == 0 || n > 16) return 0;
    struct proc *p = proc_get(pid);
    if (!p || p->gen != gen || (p->state == PROC_UNUSED || p->state == PROC_TERMINATED || p->state == PROC_FAULTED)) return 0;   /* never queue for an instance that is gone */
    struct evq *q = &Q[pid];
    if (q->gen != gen) q_reset(q, gen);
    if (q->count == UI_EVQ_DEPTH) return 0;                              /* a service event never evicts another event */
    q_push((uint8_t)pid, gen, ev, n);
    return 1;
}

void ui_sys_client_gone(int pid, uint64_t gen)
{
    if (pid < 1 || pid > UIP_MAX_CLIENTS) return;
    int dropped = 0;
    for (int i = 0; i < UIP_MAX_SURFACES; i++) if (G.s[i].live && G.s[i].owner_pid == (uint8_t)pid && G.s[i].owner_gen == gen) dropped++;
    uip_client_gone(&G, (uint8_t)pid, gen);
    if (dropped) { console_puts("ui: client gone pid="); console_dec((uint64_t)pid); console_puts(" surfaces dropped="); console_dec((uint64_t)dropped); console_puts("\n"); }
    if (Q[pid].gen == gen) q_reset(&Q[pid], 0);
}

/* The compositor decides where a new surface goes (a cascade inside the allowed region); the client never chooses. */
static void autoplace(int slot)
{
    int idx = slot;
    int16_t x = (int16_t)(G.rx + 8 + (idx % 3) * 300), y = (int16_t)(G.ry + (idx / 3) * 200);
    if (uip_place(&G, slot, x, y) == UIP_OK) return;
    if (uip_place(&G, slot, G.rx, G.ry) == UIP_OK) return;                 /* does not fit the cascade: the region's corner */
    console_puts("ui: surface in slot "); console_dec((uint64_t)slot); console_puts(" could not be placed (too large for the region); it stays hidden\n");
}

int64_t ui_syscall(struct proc *p, uint64_t op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    if (!p || p->principal == PRINCIPAL_NONE) return AE_EPERM;           /* a process without an identity has no UI */
    if (op == SYS_UI_SEND) {
        if (a2 > UIP_MSG_MAX) return AE_E2BIG;
        uint8_t m[UIP_MSG_MAX];
        if (a2) { int rc = copy_from_user(m, a1, (size_t)a2); if (rc != AE_OK) return rc; }
        if (a4 && !user_writable(a3, (size_t)a4)) return AE_EFAULT;      /* refuse before anything changes */
        struct uip_caller who = { (uint8_t)p->pid, p->gen, p->principal, (uint8_t)has_cap(p->principal) };
        uint8_t r[UIP_MSG_MAX]; uint32_t rl = 0;
        int code = uip_handle(&G, &who, m, (uint32_t)a2, r, sizeof r, &rl);
        uip_reply_consumed(&G, who.pid, who.gen);                          /* the reply is delivered right now: no backlog can form */
        if (code == UIP_OK && rl >= 8 && m[1] == UIP_CREATE_SURFACE) {
            uint32_t h = (uint32_t)r[4] | ((uint32_t)r[5] << 8) | ((uint32_t)r[6] << 16) | ((uint32_t)r[7] << 24);
            int slot = uip_slot_of(h);
            if (slot >= 0) {
                autoplace(slot);
                const struct uip_surface *s = uip_surface(&G, slot);
                console_puts("ui: surface slot="); console_dec((uint64_t)slot); console_puts(" pid="); console_dec((uint64_t)p->pid);
                if (s && s->placed) { console_puts(" at "); console_dec((uint64_t)(uint16_t)s->sx); console_puts(","); console_dec((uint64_t)(uint16_t)s->sy); console_puts(" size "); console_dec(UIP_SIZE_W[s->size_class]); console_puts("x"); console_dec(UIP_SIZE_H[s->size_class]); }
                else console_puts(" hidden");
                console_puts("\n");
            }
        }
        if (rl && a4) { uint32_t n = rl < (uint32_t)a4 ? rl : (uint32_t)a4; (void)copy_to_user(a3, r, n); }
        return code == UIP_OK ? 0 : AE_UI_BASE + code;
    }
    if (op == SYS_UI_AURA) return broker_syscall_aura(p, a1, a2, a3);      /* a request to the Action Broker: the policy decides, the shell executes */
    if (op == SYS_UI_POLL) {
        if (a2 < 14) return AE_EINVAL;
        if (!user_writable(a1, (size_t)a2)) return AE_EFAULT;            /* a bad buffer is always EFAULT, whether or not an event is waiting */
        struct evq *q = &Q[p->pid];
        if (q->gen != p->gen) q_reset(q, p->gen);
        if (!q->count) return AE_EAGAIN;
        uint8_t slot = q->head; uint32_t n = q->len[slot];
        int rc = copy_to_user(a1, q->buf[slot], n);
        if (rc != AE_OK) return rc;                                       /* the event stays queued */
        q->head = (uint8_t)((q->head + 1) % UI_EVQ_DEPTH); q->count--;
        return (int64_t)n;
    }
    return AE_EINVAL;
}

/* ---- routing from the shell loop ---- */
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

int ui_sys_pointer(int x, int y, uint8_t kind, uint8_t buttons)
{
    uint8_t ev[16]; struct uip_target to;
    uint32_t n = uip_route_pointer(&G, x, y, kind, buttons, &to, ev, sizeof ev);
    if (!n) return 0;
    if (kind == 1) { int slot = uip_slot_of(rd32(ev + 4)); if (slot >= 0) uip_set_focus(&G, slot); }   /* a press gives the surface focus: the COMPOSITOR decides */
    q_push(to.pid, to.gen, ev, n);
    return 1;
}

int ui_sys_key(uint16_t code, uint16_t mods, uint8_t down, uint8_t repeat)
{
    uint8_t ev[16]; struct uip_target to;
    uint32_t n = uip_route_key(&G, code, mods, down, repeat, &to, ev, sizeof ev);
    if (!n) return 0;
    q_push(to.pid, to.gen, ev, n);
    return 1;
}

int ui_sys_has_focus(void) { return G.focus >= 0; }
void ui_sys_focus_clear(void) { (void)uip_set_focus(&G, -1); }
