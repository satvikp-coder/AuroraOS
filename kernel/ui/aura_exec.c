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
        else if (d->reason == BPR_NO_TRAIL && o->intent.kind == AK_SET_SETTING) put(o->msg, sizeof o->msg, "Not available: the records are unhealthy. Nothing changed.", 0);   /* an unrecorded change cannot run */
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
    if (d.decision == BP_REVIEW) aura_s1_begin(sh, env, pid, nonce, out);                       /* S1: only the trusted shell opens a review; nothing changes until a person approves */
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

/* ============================ S1 settings execution (see aura_exec.h) ============================ */
#include <settings.h>

_Static_assert(ASET_TEXT_SCALE == SET_TEXT_SCALE && ASET_THEME == SET_THEME, "the Aura setting ids are the registry ids");
static const struct aura_s1_env *S1;
void aura_s1_attach(const struct aura_s1_env *e) { S1 = e; }

static const uint32_t TXT_STEPS[6] = { 75, 100, 125, 150, 175, 200 };

int aura_s1_resolve(const struct aura_intent *in, uint32_t cur, int32_t *next)
{
    if (!in || !next || in->kind != AK_SET_SETTING) return -1;
    if (in->target == ASET_TEXT_SCALE) {
        if (in->arg_kind == AA_ABS) { if (in->arg < 75 || in->arg > 200) return -1; *next = in->arg; }
        else if (in->arg_kind == AA_REL && (in->arg == 1 || in->arg == -1)) {
            int found = 0;
            if (in->arg > 0) { for (int i = 0; i < 6; i++) if (TXT_STEPS[i] > cur) { *next = (int32_t)TXT_STEPS[i]; found = 1; break; } }
            else { for (int i = 5; i >= 0; i--) if (TXT_STEPS[i] < cur) { *next = (int32_t)TXT_STEPS[i]; found = 1; break; } }
            if (!found) return -3;
        } else return -1;
    } else if (in->target == ASET_THEME) {
        if (in->arg_kind != AA_ABS || in->arg < 0 || in->arg > 2) return -1;
        *next = in->arg;
    } else return -1;
    return (uint32_t)*next == cur ? -2 : 0;
}

static void s1_outcome(struct aura_outcome *o, int state, const char *msg) { o->state = (uint8_t)state; o->executed = 0; put(o->msg, sizeof o->msg, msg, 0); }

void aura_s1_begin(struct shell *sh, const struct bp_env *env, uint32_t pid, uint64_t nonce, struct aura_outcome *out)
{
    if (!sh || !env || !out || pid != BP_SHELL_PID || !S1 || out->intent.kind != AK_SET_SETTING) return;       /* keep "Needs your approval. Nothing changed." */
    if (!sh->store || !sh->store->get) { s1_outcome(out, AX_UNAVAILABLE, "Not available: no settings store. Nothing changed."); return; }
    uint32_t cur = 0;
    if (sh->store->get(out->intent.target, &cur) != SETTINGS_OK) { s1_outcome(out, AX_UNAVAILABLE, "Not available: cannot read the setting. Nothing changed."); return; }
    int32_t next = 0; int rc = aura_s1_resolve(&out->intent, cur, &next);
    if (rc == -2) { s1_outcome(out, AX_FAILED, "Already set that way. Nothing changed."); return; }
    if (rc == -3) { s1_outcome(out, AX_FAILED, out->intent.arg > 0 ? "Already the largest size. Nothing changed." : "Already the smallest size. Nothing changed."); return; }
    if (rc != 0) { s1_outcome(out, AX_NOT_UNDERSTOOD, "Not understood. Nothing changed."); return; }
    if (!S1->ledger_ok()) { s1_outcome(out, AX_UNAVAILABLE, "Not available: the records are unhealthy. Nothing changed."); return; }
    struct rv_request q; uint8_t *z = (uint8_t *)&q; for (size_t i = 0; i < sizeof q; i++) z[i] = 0;
    q.intent = out->intent; q.pid = BP_SHELL_PID; q.gen = BP_SHELL_GEN; q.principal = 0x7F000003u; q.nonce = nonce; q.prior = (int32_t)cur; q.next = next;
    int64_t t = shell_review_open(sh, S1->now_ms(), &q, env->caps, "Aura");
    if (t > 0) { s1_outcome(out, AX_NEEDS_APPROVAL, "Waiting for your approval. Nothing changed yet."); return; }
    s1_outcome(out, AX_UNAVAILABLE, t == RV_E_STATE ? "Not available: a review needs a keyboard. Nothing changed." : t == RV_E_BUSY ? "Another review is open. Nothing changed." : "The review could not open. Nothing changed.");
}

static void s1_report(struct shell *sh, int state, const char *msg) { shell_aura_report(sh, (uint8_t)state, msg); }

static int s1_rec(const struct aura_s1_env *e, uint8_t target, uint8_t state, uint8_t reason, int32_t arg)
{
    struct trail_rec r; uint8_t *z = (uint8_t *)&r; for (size_t i = 0; i < sizeof r; i++) z[i] = 0;
    r.pid = BP_SHELL_PID; r.principal = 0x7F000003u; r.kind = AK_SET_SETTING; r.target = target; r.risk = 1; r.decision = BP_REVIEW; r.reason = reason; r.state = state; r.arg = arg;
    r.uptime_ms = (uint32_t)e->now_ms();
    return e->trail_append(&r);
}

int aura_s1_complete(struct shell *sh, const struct bp_env *env, struct bp_replay *rp, uint64_t ticket, uint64_t fresh_nonce)
{
    if (!sh || !env || !rp || !S1) return AX_FAILED;
    const struct rv_request basis = sh->rv_req;                                   /* exactly what the person approved */
    const uint8_t id = basis.intent.target;
    /* 1. the records must be healthy NOW; 2. the broker must still say REVIEW for this requester (a revoked right or an unhealthy service ends it here) */
    if (!S1->ledger_ok()) { rv_decline(&sh->rv, ticket); s1_report(sh, AX_UNAVAILABLE, "Not done: the records became unavailable. Nothing changed."); return AX_UNAVAILABLE; }
    { struct bp_request rq; uint8_t *z = (uint8_t *)&rq; for (size_t i = 0; i < sizeof rq; i++) z[i] = 0;
      rq.pid = BP_SHELL_PID; rq.gen = BP_SHELL_GEN; rq.nonce = fresh_nonce; rq.intent = basis.intent;
      struct bp_result d; bp_decide(env, rp, &rq, &d);
      if (d.decision != BP_REVIEW) { rv_decline(&sh->rv, ticket); s1_report(sh, AX_DENIED, "Not done: no longer allowed. Nothing changed."); return AX_DENIED; } }
    /* 3. read the setting NOW; the approval only counts if the world is as it was reviewed */
    uint32_t cur = 0;
    if (!sh->store || !sh->store->get || sh->store->get(id, &cur) != SETTINGS_OK) { rv_decline(&sh->rv, ticket); s1_report(sh, AX_UNAVAILABLE, "Not done: cannot read the setting. Nothing changed."); return AX_UNAVAILABLE; }
    struct rv_request now_q = basis; now_q.prior = (int32_t)cur;
    { int32_t nx = 0; int rc = aura_s1_resolve(&basis.intent, cur, &nx); if (rc != 0) { rv_decline(&sh->rv, ticket); s1_report(sh, AX_FAILED, "Not done: the setting changed meanwhile. Nothing changed."); return AX_FAILED; } now_q.next = nx; }
    int vr = shell_review_verify(sh, S1->now_ms(), ticket, &now_q, env->caps);              /* consumes the approval; compares every byte, the requester, both values */
    if (vr != RV_OK) { s1_report(sh, AX_FAILED, vr == RV_E_CHANGED ? "Not done: the request changed since you approved it. Nothing changed." : "Not done: the approval is no longer valid. Nothing changed."); return AX_FAILED; }
    const int32_t next = now_q.next;
    /* 4. the PRE record: no record, no change */
    if (s1_rec(S1, id, AX_NEEDS_APPROVAL, 0, (int32_t)cur) != 0) { s1_report(sh, AX_UNAVAILABLE, "Not done: the record could not be written. Nothing changed."); return AX_UNAVAILABLE; }
    /* 5. the setting, then 6. read it back from the registry */
    int src = sh->store->set ? sh->store->set(id, (uint32_t)next) : SETTINGS_EARG;
    uint32_t back = 0; int grc = sh->store->get(id, &back);
    if (src != SETTINGS_OK) {
        int still = grc == SETTINGS_OK && back == cur;
        (void)s1_rec(S1, id, AX_FAILED, 2, grc == SETTINGS_OK ? (int32_t)back : (int32_t)cur);
        s1_report(sh, AX_FAILED, still ? "Not saved. Nothing changed." : "Not confirmed. Check Settings for the real value.");
        return AX_FAILED;
    }
    if (grc != SETTINGS_OK || back != (uint32_t)next) {
        (void)s1_rec(S1, id, AX_FAILED, 3, grc == SETTINGS_OK ? (int32_t)back : (int32_t)cur);
        s1_report(sh, AX_FAILED, "Not confirmed. Check Settings for the real value.");
        return AX_FAILED;
    }
    /* 7. only a verified value is applied to the live interface */
    if (id == ASET_TEXT_SCALE) shell_set_text_scale(sh, (uint32_t)next);
    /* 8. the POST record, and the truth about it */
    if (s1_rec(S1, id, AX_DONE, 0, next) != 0) { s1_report(sh, AX_DONE, "Saved, but its record could not be written. No Undo."); return AX_DONE; }
    char msg[64]; msg[0] = 0; put(msg, sizeof msg, "Done: ", basis.intent.preview);
    { uint32_t n = 0; while (msg[n]) n++; uint32_t seq = S1->trail_last_seq(); char t[12]; int k = 0; if (!seq) t[k++] = '0'; while (seq && k < 11) { t[k++] = (char)('0' + seq % 10); seq /= 10; }
      const char *pre = " Receipt #"; for (; *pre && n + 1 < sizeof msg; pre++) msg[n++] = *pre; while (k && n + 1 < sizeof msg) msg[n++] = t[--k]; if (n + 1 < sizeof msg) msg[n++] = '.'; msg[n] = 0; }
    s1_report(sh, AX_DONE, msg);
    return AX_DONE;
}

int aura_s1_recover(struct shell *sh)
{
    if (!sh || !S1 || !S1->ledger_ok || !S1->ledger_ok()) return 0;
    struct trail_rec r;
    if (!S1->trail_newest(&r)) return 0;
    if (r.kind != AK_SET_SETTING || r.state != AX_NEEDS_APPROVAL) return 0;           /* the newest record is not an unfinished S1 change */
    uint32_t cur = (uint32_t)r.arg;
    if (sh->store && sh->store->get && sh->store->get(r.target, &cur) != SETTINGS_OK) cur = (uint32_t)r.arg;
    return s1_rec(S1, r.target, AX_FAILED, 1, (int32_t)cur) == 0;                    /* "interrupted", with the value the setting really has now */
}
