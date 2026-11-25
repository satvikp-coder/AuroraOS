/* kernel/ui/broker_kernel.c - kernel binding of the broker policy (see broker_kernel.h). */
#include <broker_kernel.h>
#include <console.h>
#include <ledger.h>
#include <timersys.h>

static struct bp_env ENV;
static struct bp_replay RP;
static const struct aura_caps CAPS = { 1, 1, 0, 0, 0, 0, 0 };          /* Notes launch is advertised only in the trusted shell's local broker environment */
static uint32_t denial_audited, denial_dropped;                    /* denials written to the Trail this boot / counted but not written (a flood must not thrash the disk) */
#define BK_DENIAL_AUDIT_MAX 16

/* The ONLY source of an EL0 requester's rights is the Passport ledger (durable grants by stable principal, instance-bound grants by process generation): there
 * is no second grant table that could outlive a revocation. A grant never carries to another instance of the same pid because lookups are by generation. */
static uint32_t rights_of(uint32_t principal, uint64_t gen)
{
    uint32_t r = 0;
    if (ledger_peek(principal, gen, PP_NAVIGATE)) r |= BP_RIGHT_NAVIGATE;
    if (ledger_peek(principal, gen, PP_SETTINGS_PROPOSE)) r |= BP_RIGHT_SETTINGS;
    if (ledger_peek(principal, gen, PP_TIMER)) r |= BP_RIGHT_TIMER;
    return r;
}

struct pend { uint8_t used; uint32_t pid; uint64_t gen; struct aura_intent in; };
static struct pend Q[BK_QUEUE];
static uint64_t last_exec_tick[PROC_MAX + 1], last_exec_gen[PROC_MAX + 1];      /* per requester instance: when its last request was executed */
#define BK_MIN_GAP_TICKS 100                                                  /* scheduler ticks (1 ms each while EL0 runs): at most about 10 executions per second per requester */
_Static_assert(BP_REPLAY_SLOTS >= PROC_MAX + 1, "the replay table needs a slot for every process plus the shell");


static int lookup(uint32_t pid, uint64_t *gen, uint32_t *principal, uint32_t *rights)
{
    if (pid == BP_SHELL_PID) { *gen = BP_SHELL_GEN; *principal = PRINCIPAL_SHELL; *rights = BP_RIGHTS_ALL; return 1; }   /* the trusted shell (the user at the keyboard) may navigate and PROPOSE settings; a proposal still needs the trusted review, which does not exist yet */
    if (pid > PROC_MAX) return 0;
    const struct proc *p = proc_get((int)pid);
    if (!p || p->gen == 0) return 0;
    if (p->state == PROC_UNUSED || p->state == PROC_TERMINATED || p->state == PROC_FAULTED) return 0;    /* a finished process asks for nothing */
    *gen = p->gen; *principal = p->principal;
    *rights = rights_of(p->principal, p->gen);                       /* re-read from the ledger at EVERY lookup, so a revocation takes effect at the next decision and again at execution */
    return 1;
}

void broker_init(void)
{
    ENV.caps = &CAPS; ENV.lookup = lookup;
    ENV.timer_service = 1;                                            /* the Clock's timer service is attached by the shell */
    ENV.nav_service = 1;                                              /* the shell's navigation executor exists */
    ENV.review_path = 0; ENV.passport = 0; ENV.trail = 0; ENV.hold_input = 0;   /* passport/trail follow the ledger's health (broker_env); the review path and hold input do not exist yet */
    bp_replay_init(&RP);
    denial_audited = denial_dropped = 0;
    for (int i = 0; i < BK_QUEUE; i++) Q[i].used = 0;
    for (int i = 0; i <= PROC_MAX; i++) { last_exec_tick[i] = 0; last_exec_gen[i] = 0; }
}

void broker_set_review_path(int on) { ENV.review_path = on ? 1 : 0; }

const struct bp_env *broker_env(void)
{
    ENV.passport = (uint8_t)ledger_passport_ok();                     /* healthy, verified, writable: otherwise a Passport-dependent action is unavailable */
    ENV.trail = (uint8_t)ledger_trail_ok();
    return &ENV;
}
struct bp_replay *broker_replay(void) { return &RP; }

int broker_grant_navigate(int pid)
{
    struct proc *p = proc_get(pid);
    if (!p || p->gen == 0 || p->principal == PRINCIPAL_NONE) return -1;
    return ledger_grant_instance(p->principal, p->gen, PP_NAVIGATE) == LG_E_OK ? 0 : -1;       /* RAM-only, bound to this process generation, held in the Passport table */
}

int64_t broker_syscall_aura(struct proc *p, uint64_t text_uva, uint64_t len, uint64_t nonce)
{
    if (!p || p->principal == PRINCIPAL_NONE) return AE_EPERM;
    if (len > AURA_CMD_MAX) return AE_E2BIG;
    char buf[AURA_CMD_MAX];
    if (len) { int rc = copy_from_user(buf, text_uva, (size_t)len); if (rc != AE_OK) return rc; }
    /* One outstanding request per requester instance, and no request at all while the queue is full. This is checked BEFORE the policy runs: the policy consumes
     * the nonce, so a request that cannot be queued must not burn it (otherwise one client could make another's natural retry fail as a replay). */
    { int free_slot = 0;
      for (int i = 0; i < BK_QUEUE; i++) { if (!Q[i].used) free_slot = 1; else if (Q[i].pid == (uint32_t)p->pid && Q[i].gen == p->gen) return AE_EAGAIN; }
      if (!free_slot) return AE_EAGAIN; }
    struct bp_request rq; uint8_t *z = (uint8_t *)&rq; for (size_t i = 0; i < sizeof rq; i++) z[i] = 0;
    rq.pid = (uint32_t)p->pid; rq.gen = p->gen; rq.nonce = nonce;          /* identity from the kernel's process table, never from the caller's words */
    aura_parse(buf, (size_t)len, &CAPS, &rq.intent);
    struct bp_result d; bp_decide(broker_env(), &RP, &rq, &d);
    if (d.decision == BP_DENY && d.reason != BPR_NOT_ACTIONABLE && d.reason != BPR_REPLAY) {    /* a security refusal (no right, stale or unknown requester): leave evidence, but at most a few per boot */
        if (denial_audited < BK_DENIAL_AUDIT_MAX && ledger_trail_ok()) {
            struct trail_rec tr; uint8_t *tz = (uint8_t *)&tr; for (size_t i = 0; i < sizeof tr; i++) tz[i] = 0;
            tr.pid = rq.pid; tr.principal = d.principal; tr.kind = rq.intent.kind < 16 ? rq.intent.kind : 0; tr.target = rq.intent.target; tr.risk = d.risk <= 3 ? d.risk : 3;
            tr.decision = d.decision; tr.reason = d.reason; tr.state = AX_DENIED; tr.arg = rq.intent.arg; tr.uptime_ms = (uint32_t)timersys_now_ms();
            int arc = ledger_trail_append(&tr);
            if (arc == LG_E_OK) denial_audited++;
            console_puts("aura: audit denial rc="); console_dec((uint64_t)(uint32_t)(-arc)); console_puts("\n");
        } else { denial_dropped++; console_puts("aura: audit denial dropped total="); console_dec(denial_dropped); console_puts("\n"); }
    }
    console_puts("aura: request pid="); console_dec((uint64_t)p->pid); console_puts(" decision="); console_puts(bp_decision_name(d.decision));
    console_puts(" reason="); console_puts(bp_reason_name(d.reason)); console_puts("\n");
    if (d.decision == BP_ALLOW_NOW) {
        int slot = -1;
        for (int i = 0; i < BK_QUEUE; i++) if (!Q[i].used) { slot = i; break; }
        if (slot < 0) { console_puts("aura: queue full, request dropped\n"); return AE_EAGAIN; }
        Q[slot].used = 1; Q[slot].pid = rq.pid; Q[slot].gen = rq.gen; Q[slot].in = rq.intent;
    }
    return (int64_t)(((uint32_t)d.decision << 8) | d.reason);
}

int broker_drain(struct shell *sh)
{
    int n = 0;
    for (int i = 0; i < BK_QUEUE; i++) {
        if (!Q[i].used) continue;
        if (Q[i].pid <= PROC_MAX && last_exec_gen[Q[i].pid] == Q[i].gen && last_exec_tick[Q[i].pid] && sched_ticks() < last_exec_tick[Q[i].pid] + BK_MIN_GAP_TICKS) continue;   /* too soon: it stays pending (and blocks that requester's next request) */
        struct pend r = Q[i]; Q[i].used = 0; n++;
        if (r.pid <= PROC_MAX) { last_exec_gen[r.pid] = r.gen; last_exec_tick[r.pid] = sched_ticks() ? sched_ticks() : 1; }
        struct aura_outcome o; aura_run_queued(sh, broker_env(), r.pid, r.gen, &r.in, &o);
        console_puts("aura: executed pid="); console_dec((uint64_t)r.pid); console_puts(" state="); console_dec(o.state);
        console_puts(" ["); console_puts(o.msg); console_puts("] screen="); console_puts(shell_screen_name(sh->screen)); console_puts("\n");
    }
    return n;
}
