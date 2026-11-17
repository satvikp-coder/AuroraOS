/* kernel/ui/aura_exec.c - the S0 executor and the Aura pipeline (see aura_exec.h). No allocation, no libc. */
#include <aura_exec.h>

static const uint8_t SCREEN_OF[ASC_COUNT] = { SCR_HOME, SCR_SPACES, SCR_INDEX, SCR_SETTINGS, SCR_PAGES, SCR_CLOCK };

int aura_execute_s0(struct shell *sh, const struct aura_intent *in, const struct aura_caps *caps)
{
    if (!sh || !in || !caps) return 0;
    if (!aura_validate(in, caps) || in->status != AS_ACTIONABLE || in->risk != 0) return 0;      /* only a validated S0 navigation */
    if (sh->about_open || sh->corner_open) return 0;                                            /* a modal owns the screen: never navigate behind it */
    if (in->kind == AK_TIMER) {                                                                 /* success only if the REAL timer service now shows what was asked */
        if (in->target == ATM_START) return shell_clock_start(sh, (uint32_t)in->arg);
        if (in->target == ATM_CANCEL) return shell_clock_cancel(sh);
        return 0;
    }
    if (in->kind == AK_OPEN_SCREEN) {
        if (in->target >= ASC_COUNT) return 0;
        uint8_t want = SCREEN_OF[in->target];
        shell_go(sh, want);
        return sh->screen == want;                                                              /* verified against the real state */
    }
    if (in->kind == AK_SELECT_SPACE) {
        if (in->target >= SHELL_SPACES) return 0;
        shell_select_space(sh, in->target);
        shell_go(sh, SCR_HOME);                                                                 /* so Home shows the chosen Space */
        return sh->space == in->target && sh->screen == SCR_HOME;
    }
    return 0;
}

static void put(char *d, uint32_t cap, const char *a, const char *b)
{
    uint32_t n = 0;
    for (; a && *a && n + 1 < cap; a++) d[n++] = *a;
    for (; b && *b && n + 1 < cap; b++) d[n++] = *b;
    d[n] = 0;
}

void aura_outcome_from(struct aura_outcome *o, const struct bp_result *d, int executed)
{
    if (!o || !d) return;                                                                       /* public helper: never dereference a NULL argument */
    const char *pre = o->intent.preview;
    o->decision = d->decision; o->reason = d->reason; o->executed = 0;
    switch (d->decision) {
    case BP_ALLOW_NOW:
        if (executed) { o->state = AX_DONE; o->executed = 1; put(o->msg, sizeof o->msg, "Done: ", pre); }
        else { o->state = AX_FAILED; put(o->msg, sizeof o->msg, "Could not do that now. Nothing changed.", 0); }
        break;
    case BP_REVIEW:
        o->state = AX_NEEDS_APPROVAL; put(o->msg, sizeof o->msg, "Needs your approval. Nothing changed.", 0); break;
    case BP_HOLD:
        o->state = AX_NEEDS_APPROVAL; put(o->msg, sizeof o->msg, "Needs hold-to-confirm. Nothing changed.", 0); break;
    case BP_UNAVAILABLE:
        o->state = AX_UNAVAILABLE;
        if (d->reason == BPR_UNSUPPORTED || o->intent.kind == AK_DESTRUCTIVE) put(o->msg, sizeof o->msg, pre, 0);
        else if (d->reason == BPR_NO_REVIEW) put(o->msg, sizeof o->msg, "Needs approval, and the trusted review is not available yet. Nothing changed.", 0);
        else put(o->msg, sizeof o->msg, "Not available yet. Nothing changed.", 0);
        break;
    default:                                                                                    /* BP_DENY */
        if (d->reason == BPR_NOT_ACTIONABLE) { o->state = AX_NOT_UNDERSTOOD; put(o->msg, sizeof o->msg, o->intent.preview[0] ? pre : "Not understood. Nothing changed.", 0); }
        else { o->state = AX_DENIED; put(o->msg, sizeof o->msg, "Denied. Nothing changed.", 0); }
        break;
    }
    if (d->decision == BP_UNAVAILABLE && o->msg[0] == 0) put(o->msg, sizeof o->msg, "Not available. Nothing changed.", 0);
}

void aura_run_text(struct shell *sh, const struct bp_env *env, struct bp_replay *rp, uint32_t pid, uint64_t gen, uint64_t nonce,
                   const char *text, size_t len, struct aura_outcome *out)
{
    struct bp_request rq; struct bp_result d;
    if (!out) return;                                                                           /* validate the output pointer BEFORE writing through it */
    uint8_t *z = (uint8_t *)out; for (size_t i = 0; i < sizeof *out; i++) z[i] = 0;
    if (!sh || !env || !env->caps) return;
    aura_parse(text, len, env->caps, &out->intent);
    z = (uint8_t *)&rq; for (size_t i = 0; i < sizeof rq; i++) z[i] = 0;
    rq.pid = pid; rq.gen = gen; rq.nonce = nonce; rq.intent = out->intent;
    bp_decide(env, rp, &rq, &d);
    int ok = 0;
    if (d.decision == BP_ALLOW_NOW) ok = aura_execute_s0(sh, &out->intent, env->caps);
    aura_outcome_from(out, &d, ok);
}

void aura_run_queued(struct shell *sh, const struct bp_env *env, uint32_t pid, uint64_t gen, const struct aura_intent *in, struct aura_outcome *out)
{
    if (!out) return;                                                                           /* validate the output pointer BEFORE writing through it */
    uint8_t *z = (uint8_t *)out; for (size_t i = 0; i < sizeof *out; i++) z[i] = 0;
    if (!sh || !env || !env->caps || !env->lookup || !in) return;
    out->intent = *in;
    struct bp_result d; d.decision = BP_DENY; d.reason = BPR_BAD_REQUESTER; d.risk = 0; d.principal = 0;
    uint64_t g = 0; uint32_t pr = 0, rt = 0;
    if (env->lookup(pid, &g, &pr, &rt) && g != 0 && g == gen && (rt & BP_RIGHT_NAVIGATE) && in->kind != AK_TIMER && aura_validate(in, env->caps) && in->status == AS_ACTIONABLE) {
        int ok = env->nav_service ? aura_execute_s0(sh, in, env->caps) : 0;
        d.decision = BP_ALLOW_NOW; d.reason = BPR_NONE; d.principal = pr;
        aura_outcome_from(out, &d, ok);
        return;
    }
    aura_outcome_from(out, &d, 0);                                                              /* the requester is gone or changed: dropped, nothing executed */
}
