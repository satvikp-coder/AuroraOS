/* kernel/ui/broker_kernel.c - kernel binding of the broker policy (see broker_kernel.h). */
#include <broker_kernel.h>
#include <console.h>

static struct bp_env ENV;
static struct bp_replay RP;
static const struct aura_caps CAPS = { 1, 0, 0, 0, 0 };          /* Pages exists now; no timer, Notes, messaging or hardware services */
static uint64_t grant_gen[PROC_MAX + 1];                           /* the generation each pid's navigate right was granted to (0 = none) */
static uint32_t grant_rights[PROC_MAX + 1];

struct pend { uint8_t used; uint32_t pid; uint64_t gen; struct aura_intent in; };
static struct pend Q[BK_QUEUE];
static uint64_t last_exec_tick[PROC_MAX + 1], last_exec_gen[PROC_MAX + 1];      /* per requester instance: when its last request was executed */
#define BK_MIN_GAP_TICKS 100                                                  /* scheduler ticks (1 ms each while EL0 runs): at most about 10 executions per second per requester */
_Static_assert(BP_REPLAY_SLOTS >= PROC_MAX + 1, "the replay table needs a slot for every process plus the shell");

#define PRINCIPAL_SHELL 0x7F000003u

static int lookup(uint32_t pid, uint64_t *gen, uint32_t *principal, uint32_t *rights)
{
    if (pid == BP_SHELL_PID) { *gen = BP_SHELL_GEN; *principal = PRINCIPAL_SHELL; *rights = BP_RIGHTS_ALL; return 1; }   /* the trusted shell (the user at the keyboard) may navigate and PROPOSE settings; a proposal still needs the trusted review, which does not exist yet */
    if (pid > PROC_MAX) return 0;
    const struct proc *p = proc_get((int)pid);
    if (!p || p->gen == 0) return 0;
    if (p->state == PROC_UNUSED || p->state == PROC_TERMINATED || p->state == PROC_FAULTED) return 0;    /* a finished process asks for nothing */
    *gen = p->gen; *principal = p->principal;
    *rights = (grant_gen[pid] == p->gen) ? grant_rights[pid] : 0;     /* a grant never carries over to a later instance of the same pid */
    return 1;
}

void broker_init(void)
{
    ENV.caps = &CAPS; ENV.lookup = lookup;
    ENV.nav_service = 1;                                              /* the shell's navigation executor exists */
    ENV.review_path = 0; ENV.passport = 0; ENV.trail = 0; ENV.hold_input = 0;   /* none of these exist yet: S1 and above stay unavailable */
    bp_replay_init(&RP);
    for (int i = 0; i <= PROC_MAX; i++) { grant_gen[i] = 0; grant_rights[i] = 0; }
    for (int i = 0; i < BK_QUEUE; i++) Q[i].used = 0;
    for (int i = 0; i <= PROC_MAX; i++) { last_exec_tick[i] = 0; last_exec_gen[i] = 0; }
}

const struct bp_env *broker_env(void) { return &ENV; }
struct bp_replay *broker_replay(void) { return &RP; }

int broker_grant_navigate(int pid)
{
    struct proc *p = proc_get(pid);
    if (!p || p->gen == 0 || p->principal == PRINCIPAL_NONE) return -1;
    grant_gen[pid] = p->gen; grant_rights[pid] = BP_RIGHT_NAVIGATE;
    return 0;
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
    struct bp_result d; bp_decide(&ENV, &RP, &rq, &d);
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
        struct aura_outcome o; aura_run_queued(sh, &ENV, r.pid, r.gen, &r.in, &o);
        console_puts("aura: executed pid="); console_dec((uint64_t)r.pid); console_puts(" state="); console_dec(o.state);
        console_puts(" ["); console_puts(o.msg); console_puts("] screen="); console_puts(shell_screen_name(sh->screen)); console_puts("\n");
    }
    return n;
}
