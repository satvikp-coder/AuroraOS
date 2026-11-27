/* kernel/ui/broker_policy.c - the pure Action Broker policy (see broker_policy.h). No allocation, no clock, no libc. */
#include <broker_policy.h>

void bp_replay_init(struct bp_replay *r)
{
    for (int i = 0; i < BP_REPLAY_SLOTS; i++) { r->pid[i] = 0; r->gen[i] = 0; r->last[i] = 0; r->used[i] = 0; }
}

uint8_t bp_risk_of_kind(uint8_t kind)
{
    switch (kind) {
    case AK_OPEN_SCREEN: case AK_SELECT_SPACE: case AK_OPEN_APP: return 0;
    case AK_TIMER: return 0;
    case AK_SET_SETTING: return 1;
    case AK_DESTRUCTIVE: return 3;
    default: return 255;
    }
}

static void res(struct bp_result *o, uint8_t d, uint8_t why, uint8_t risk, uint32_t principal)
{
    o->decision = d; o->reason = why; o->risk = risk; o->principal = principal;
}

/* Record (pid, gen, nonce) if the nonce is newer than the last one for that INSTANCE. A slot whose pid is reused with a new generation starts over. */
static int replay_take(struct bp_replay *r, uint32_t pid, uint64_t gen, uint64_t nonce, int *full)
{
    int free_slot = -1;
    *full = 0;
    if (nonce == 0) return 0;
    for (int i = 0; i < BP_REPLAY_SLOTS; i++) {
        if (!r->used[i]) { if (free_slot < 0) free_slot = i; continue; }
        if (r->pid[i] != pid) continue;
        if (r->gen[i] == gen) {
            if (nonce <= r->last[i]) return 0;
            r->last[i] = nonce; return 1;
        }
        r->gen[i] = gen; r->last[i] = nonce; return 1;       /* the same pid with a newer instance (the lookup already proved gen is the live one) */
    }
    if (free_slot < 0) { *full = 1; return 0; }
    r->used[free_slot] = 1; r->pid[free_slot] = pid; r->gen[free_slot] = gen; r->last[free_slot] = nonce;
    return 1;
}

void bp_decide(const struct bp_env *env, struct bp_replay *replay, const struct bp_request *req, struct bp_result *out)
{
    if (!out) return;
    res(out, BP_DENY, BPR_BAD_REQUESTER, 255, 0);
    if (!env || !replay || !req || !env->caps || !env->lookup) return;

    /* 1. the intent: independent validation first. Nothing below trusts a field the validator did not check; the risk class is re-derived. */
    if (req->intent.version != AURA_INTENT_VERSION || !aura_validate(&req->intent, env->caps)) { res(out, BP_DENY, BPR_BAD_INTENT, 255, 0); return; }
    uint8_t kind = req->intent.kind, risk = bp_risk_of_kind(kind);

    /* 2. the requester: live now, same generation, principal and rights from the kernel's table. */
    uint64_t gen = 0; uint32_t principal = 0, rights = 0;
    if (!env->lookup(req->pid, &gen, &principal, &rights)) { res(out, BP_DENY, BPR_BAD_REQUESTER, risk, 0); return; }
    if (gen == 0 || gen != req->gen) { res(out, BP_DENY, gen == 0 ? BPR_BAD_REQUESTER : BPR_STALE_REQUESTER, risk, 0); return; }
    if (rights & ~BP_RIGHTS_ALL) rights &= BP_RIGHTS_ALL;       /* unknown bits grant nothing */

    /* 3. replay: only an identity that has been verified can use up (or be refused) a nonce. */
    int full = 0;
    if (!replay_take(replay, req->pid, req->gen, req->nonce, &full)) { res(out, BP_DENY, full ? BPR_TABLE_FULL : BPR_REPLAY, risk, principal); return; }

    /* 4. the decision, by the intent's STATUS and the policy's own risk class. */
    switch (req->intent.status) {
    case AS_ACTIONABLE:
        if (risk != 0) { res(out, BP_DENY, BPR_BAD_INTENT, risk, principal); return; }          /* only S0 navigation can be actionable */
        if (kind == AK_TIMER) {                                                                 /* the timer belongs to the trusted shell: a client would act on the shell's timer (a confused deputy) */
            if (req->pid != BP_SHELL_PID || !(rights & BP_RIGHT_TIMER)) { res(out, BP_DENY, BPR_NO_RIGHT, risk, principal); return; }
            if (!env->timer_service) { res(out, BP_UNAVAILABLE, BPR_NO_TIMER, risk, principal); return; }
            res(out, BP_ALLOW_NOW, BPR_NONE, risk, principal); return;
        }
        if (!(rights & BP_RIGHT_NAVIGATE)) { res(out, BP_DENY, BPR_NO_RIGHT, risk, principal); return; }
        /* Launching applications and showing system activity are trusted-shell actions. */
        if (req->pid != BP_SHELL_PID &&
            (kind == AK_OPEN_APP || (kind == AK_OPEN_SCREEN && req->intent.target == ASC_TRAIL))) {
            res(out, BP_DENY, BPR_NO_RIGHT, risk, principal); return;
        }
        if (!env->nav_service) { res(out, BP_UNAVAILABLE, BPR_NO_NAVIGATION, risk, principal); return; }
        res(out, BP_ALLOW_NOW, BPR_NONE, risk, principal); return;
    case AS_NEEDS_APPROVAL:
        if (risk != 1) { res(out, BP_DENY, BPR_BAD_INTENT, risk, principal); return; }
        if (!(rights & BP_RIGHT_SETTINGS)) { res(out, BP_DENY, BPR_NO_RIGHT, risk, principal); return; }
        if (!env->review_path) { res(out, BP_UNAVAILABLE, BPR_NO_REVIEW, risk, principal); return; }
        if (!env->trail) { res(out, BP_UNAVAILABLE, BPR_NO_TRAIL, risk, principal); return; }              /* a change that must be recorded cannot run when the Trail cannot record it */
        res(out, BP_REVIEW, BPR_NONE, risk, principal); return;
    case AS_UNSUPPORTED:
        res(out, BP_UNAVAILABLE, BPR_UNSUPPORTED, risk == 255 ? 0 : risk, principal); return;
    case AS_REJECTED:
        if (kind == AK_DESTRUCTIVE) {                                                          /* S3: every dependency, in this order, fails closed */
            if (!env->passport) { res(out, BP_UNAVAILABLE, BPR_NO_PASSPORT, 3, principal); return; }
            if (!env->trail) { res(out, BP_UNAVAILABLE, BPR_NO_TRAIL, 3, principal); return; }
            if (!env->review_path) { res(out, BP_UNAVAILABLE, BPR_NO_REVIEW, 3, principal); return; }
            if (!env->hold_input) { res(out, BP_UNAVAILABLE, BPR_NO_HOLD, 3, principal); return; }
            res(out, BP_HOLD, BPR_NONE, 3, principal); return;
        }
        res(out, BP_DENY, BPR_NOT_ACTIONABLE, risk == 255 ? 0 : risk, principal); return;
    default:                                                                                   /* clarify, or anything unexpected */
        res(out, BP_DENY, BPR_NOT_ACTIONABLE, risk == 255 ? 0 : risk, principal); return;
    }
}

const char *bp_decision_name(uint8_t d)
{
    static const char *const N[BP_COUNT] = { "deny", "allow_now", "review", "hold", "unavailable" };
    return d < BP_COUNT ? N[d] : "?";
}
const char *bp_reason_name(uint8_t r)
{
    static const char *const N[BPR_COUNT] = { "none", "bad_intent", "bad_requester", "stale_requester", "replay", "table_full", "no_right", "not_actionable",
                                              "unsupported", "no_navigation", "no_review", "no_passport", "no_trail", "no_hold", "no_timer" };
    return r < BPR_COUNT ? N[r] : "?";
}
