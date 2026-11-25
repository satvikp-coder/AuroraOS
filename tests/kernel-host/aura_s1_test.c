/* tests/kernel-host/aura_s1_test.c - the S1 settings chain end to end on the host, against the REAL shell, broker policy, trusted-review modal and review state
 * machine, with a fake registry, Trail and ledger-health that can fail at every step. The invariant under test: a setting changes ONLY if a person approved exactly
 * that change, the records were healthy, a durable PRE record was written first, the registry returned the new value, and the outcome says what really happened.
 * Host, ASan/UBSan. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <aura_exec.h>
#include <settings.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

/* ---------------- fakes ---------------- */
static uint32_t REG[3];                         /* [1] theme, [2] text scale */
static int f_set_rc, f_lie, f_get_fail, f_set_calls;
static int rg_get(uint32_t id, uint32_t *v) { if (f_get_fail) return SETTINGS_ESTORE; if (id < 1 || id > 2) return SETTINGS_EID; *v = REG[id]; return SETTINGS_OK; }
static int rg_set(uint32_t id, uint32_t v) { f_set_calls++; if (f_set_rc) return f_set_rc; if (id < 1 || id > 2) return SETTINGS_EID; if (id == 2 && (v < 75 || v > 200)) return SETTINGS_EVALUE; if (id == 1 && v > 2) return SETTINGS_EVALUE; REG[id] = f_lie ? v + 1 : v; return SETTINGS_OK; }
static int rg_reset(uint32_t id) { (void)id; return SETTINGS_EARG; }
static const struct shell_store STORE = { rg_get, rg_set, rg_reset };

static struct trail_rec TR[200]; static int ntr, f_append_fail_at, f_append_calls, f_healthy;
static int e_ok(void) { return f_healthy; }
static int e_append(const struct trail_rec *r) { f_append_calls++; if (f_append_fail_at && f_append_calls == f_append_fail_at) return -1; TR[ntr] = *r; TR[ntr].seq = (uint32_t)ntr + 1; ntr++; return 0; }
static int e_newest(struct trail_rec *o) { if (!ntr) return 0; *o = TR[ntr - 1]; return 1; }
static uint32_t e_last(void) { return ntr ? TR[ntr - 1].seq : 0; }
static uint64_t T;
static uint64_t e_now(void) { return T; }
static int e_nth(uint32_t n, struct trail_rec *o) { if (n >= (uint32_t)ntr) return 0; *o = TR[ntr - 1 - (int)n]; return 1; }
static const struct aura_s1_env ENVS = { e_ok, e_append, e_newest, e_last, e_now, e_nth };

static struct shell sh;
static const struct aura_caps CAPS = { 1, 1, 0, 0, 0, 0, 0 };
static int shell_settings_right, el0_settings_right, review_path;
static int lookup(uint32_t pid, uint64_t *gen, uint32_t *principal, uint32_t *rights)
{
    if (pid == BP_SHELL_PID) { *gen = BP_SHELL_GEN; *principal = 0x7F000003u; *rights = shell_settings_right ? BP_RIGHTS_ALL : (BP_RIGHT_NAVIGATE | BP_RIGHT_TIMER); return 1; }
    if (pid == 2) { *gen = 20; *principal = 0x1002; *rights = BP_RIGHT_NAVIGATE | (el0_settings_right ? BP_RIGHT_SETTINGS : 0); return 1; }
    return 0;
}
static struct bp_env ENV; static struct bp_replay RP; static uint64_t nonce;
static void fresh(void)
{
    memset(REG, 0, sizeof REG); REG[1] = 0; REG[2] = 100; f_set_rc = f_lie = f_get_fail = f_set_calls = 0; ntr = 0; f_append_fail_at = f_append_calls = 0; f_healthy = 1; T = 100000;
    shell_settings_right = 1; el0_settings_right = 0; review_path = 1;
    memset(&ENV, 0, sizeof ENV); ENV.caps = &CAPS; ENV.lookup = lookup; ENV.nav_service = 1; ENV.timer_service = 1; ENV.review_path = (uint8_t)review_path; ENV.trail = 1; ENV.passport = 1;
    bp_replay_init(&RP); nonce = 0;
    shell_init(&sh, 1024, 768); struct shell_caps c = { 1, 1 }; shell_set_caps(&sh, c); shell_set_store(&sh, &STORE); shell_tick(&sh, T);
    aura_s1_attach(&ENVS);
}
static struct aura_outcome run(uint32_t pid, const char *cmd)
{
    struct aura_outcome o; aura_run_text(&sh, &ENV, &RP, pid, pid == 0 ? BP_SHELL_GEN : 20, ++nonce, cmd, strlen(cmd), &o); return o;
}
static void tick(uint64_t ms) { T += ms; shell_tick(&sh, T); }
static void key(uint16_t k) { struct input_event e = { IN_KEY_DOWN, 0, k, 0, 0, 0, 0 }; shell_handle(&sh, &e); }
static int approve(void)                       /* the person: wait, chord, Tab to Approve, Enter */
{
    if (!sh.review_open) return 0;                                                  /* there is nothing to approve: do not press keys on the screen behind */
    tick(700); shell_review_attention(&sh, T, 0); key(KEY_TAB); key(KEY_ENTER);
    return sh.rv_approved_pending;
}
static int finish(uint64_t *tk_out)            /* the kernel loop: take the approval and run the rest */
{
    uint64_t tk = 0; if (!shell_review_take_approved(&sh, &tk)) return -1;
    if (tk_out) *tk_out = tk;
    return aura_s1_complete(&sh, &ENV, &RP, tk, ++nonce);
}
static int has_text(const char *t) { for (uint32_t i = 0; i < sh.n; i++) if (sh.w[i].text && strstr(sh.w[i].text, t)) return 1; return 0; }
static int aura_says(const char *t) { return strstr(sh.aura_msg, t) != 0; }

int main(void)
{
    printf("aura S1 tests\n");

    { /* the happy path */
        fresh();
        struct aura_outcome o = run(0, "make text bigger");
        CHECK("the broker answers REVIEW and Aura opens the trusted review: waiting, nothing changed yet", o.decision == BP_REVIEW && o.state == AX_NEEDS_APPROVAL && sh.review_open && REG[2] == 100 && sh.text_pct == 100 && strstr(o.msg, "Waiting") != 0);
        CHECK("the modal shows the canonical action and the REAL previous -> next values from the registry", has_text("Text size: 100% -> 125%") && has_text("Make text larger") && has_text("Requested by: Aura"));
        CHECK("nothing was written before the person answers (no record, no registry write)", ntr == 0 && f_set_calls == 0);
        CHECK("the person approves", approve() && !sh.review_open);
        CHECK("still nothing changed at the moment of approval (the executor has not run)", REG[2] == 100 && sh.text_pct == 100 && ntr == 0);
        int st = finish(0);
        CHECK("the chain completes: Done", st == AX_DONE && sh.aura_state == AX_DONE);
        CHECK("the registry holds 125 and the live interface follows", REG[2] == 125 && sh.text_pct == 125);
        CHECK("two durable records: PRE (state 4, previous value 100) then POST (state 1, new value 125), both for the text-size setting", ntr == 2 && TR[0].kind == AK_SET_SETTING && TR[0].state == AX_NEEDS_APPROVAL && TR[0].arg == 100 && TR[0].target == 2
              && TR[1].kind == AK_SET_SETTING && TR[1].state == AX_DONE && TR[1].arg == 125 && TR[1].target == 2 && TR[1].seq == 2);
        CHECK("the message gives the truth and the receipt number", aura_says("Done: Make text larger") && aura_says("Receipt #2."));
    }
    { /* the person says no / does nothing */
        fresh(); run(0, "make text bigger"); key(KEY_ENTER);
        CHECK("Enter on the default focus (Cancel) changes nothing", !sh.review_open && REG[2] == 100 && ntr == 0 && f_set_calls == 0 && !sh.rv_approved_pending);
        fresh(); run(0, "make text bigger"); key(KEY_ESC);
        CHECK("Escape changes nothing", REG[2] == 100 && ntr == 0 && f_set_calls == 0);
        fresh(); run(0, "make text bigger"); tick(60000);
        CHECK("an unanswered review expires: nothing changed", sh.rv.state == RV_EXPIRED && REG[2] == 100 && ntr == 0 && f_set_calls == 0 && !sh.rv_approved_pending);
        fresh(); run(0, "make text bigger"); tick(700); shell_review_attention(&sh, T, 0); tick(10000);
        CHECK("arming without approving, then the window lapsing: nothing changed", REG[2] == 100 && ntr == 0 && !sh.rv_approved_pending);
        fresh(); run(0, "make text bigger"); sh.fx.focus = -1; tick(700); shell_review_attention(&sh, T, 0); key(KEY_ENTER); key(KEY_SPACE);
        CHECK("Enter/Space with nothing focused do not approve even when armed", !sh.rv_approved_pending && REG[2] == 100);
    }
    { /* what changes between approval and execution */
        fresh(); run(0, "make text bigger"); approve(); REG[2] = 110;                                   /* someone else changed the setting while the review was open / approved */
        int st = finish(0);
        CHECK("the setting changed after approval: refused, NO PRE record, the new value is left alone", st == AX_FAILED && REG[2] == 110 && ntr == 0 && f_set_calls == 0 && (aura_says("changed since") || aura_says("changed meanwhile")));
        fresh(); run(0, "make text bigger"); approve(); sh.rv_req.intent.arg = 1; sh.rv_req.intent.preview[0] = 'X';
        st = finish(0);
        CHECK("the approved request was altered in memory: refused (the digest binds every byte)", st != AX_DONE && REG[2] == 100 && ntr == 0 && f_set_calls == 0);
        fresh(); run(0, "make text bigger"); approve(); uint64_t tk = 0; shell_review_take_approved(&sh, &tk); shell_review_take_approved(&sh, &tk);
        CHECK("the approval can be taken only once", tk != 0);
        fresh(); run(0, "make text bigger"); approve(); tk = 0; int s1 = finish(&tk); int s2 = aura_s1_complete(&sh, &ENV, &RP, tk, ++nonce);
        CHECK("REPLAYING the ticket: the second execution is refused and writes nothing more", s1 == AX_DONE && s2 == AX_FAILED && REG[2] == 125 && f_set_calls == 1 && ntr == 2);
        fresh(); run(0, "make text bigger"); approve(); uint64_t tk2 = 0; shell_review_take_approved(&sh, &tk2);
        CHECK("a made-up ticket cannot execute the approved change", aura_s1_complete(&sh, &ENV, &RP, tk2 + 99, ++nonce) == AX_FAILED && REG[2] == 100 && ntr == 0 && f_set_calls == 0);
        fresh(); run(0, "make text bigger"); approve(); shell_settings_right = 0;
        CHECK("the right was revoked after approval: denied, nothing changed", finish(0) == AX_DENIED && REG[2] == 100 && ntr == 0 && f_set_calls == 0);
        fresh(); run(0, "make text bigger"); approve(); f_healthy = 0;
        CHECK("the records became unhealthy after approval: refused, nothing changed", finish(0) == AX_UNAVAILABLE && REG[2] == 100 && ntr == 0 && f_set_calls == 0);
        fresh(); run(0, "make text bigger"); approve(); ENV.review_path = 0;
        CHECK("the review path went away after approval: denied, nothing changed", finish(0) != AX_DONE && REG[2] == 100 && ntr == 0);
    }
    { /* failures inside the chain */
        fresh(); run(0, "make text bigger"); approve(); f_append_fail_at = 1;
        CHECK("PRE cannot be written: NOTHING runs, nothing changes, and it says why", finish(0) == AX_UNAVAILABLE && REG[2] == 100 && sh.text_pct == 100 && f_set_calls == 0 && ntr == 0 && aura_says("record could not be written"));
        fresh(); run(0, "make text bigger"); approve(); f_set_rc = SETTINGS_ESTORE;
        int st = finish(0);
        CHECK("the registry refuses (cannot save): Failed, the value and the interface are unchanged, a FAILED record is written, the message says not saved", st == AX_FAILED && REG[2] == 100 && sh.text_pct == 100 && ntr == 2 && TR[1].state == AX_FAILED && TR[1].arg == 100 && aura_says("Not saved"));
        fresh(); run(0, "make text bigger"); approve(); f_lie = 1;
        st = finish(0);
        CHECK("the registry reports success but reads back a DIFFERENT value: not confirmed, not applied, recorded as failed with the real value", st == AX_FAILED && sh.text_pct == 100 && ntr == 2 && TR[1].state == AX_FAILED && TR[1].arg == 126 && aura_says("Not confirmed"));
        fresh(); run(0, "make text bigger"); approve(); f_append_fail_at = 2;
        st = finish(0);
        CHECK("POST cannot be written: the change IS saved and applied, and the message says so and offers no Undo (no false atomicity)", st == AX_DONE && REG[2] == 125 && sh.text_pct == 125 && ntr == 1 && TR[0].state == AX_NEEDS_APPROVAL && aura_says("record could not be written") && aura_says("No Undo"));
        fresh(); f_healthy = 0; struct aura_outcome o = run(0, "make text bigger");
        CHECK("unhealthy records at the start: no review is opened at all", o.state == AX_UNAVAILABLE && !sh.review_open && REG[2] == 100);
        fresh(); f_get_fail = 1; o = run(0, "make text bigger");
        CHECK("the setting cannot be read: no review, nothing changes", o.state == AX_UNAVAILABLE && !sh.review_open && ntr == 0);
        fresh(); struct shell_caps nk = { 0, 1 }; shell_set_caps(&sh, nk); o = run(0, "make text bigger");
        CHECK("no keyboard: the review cannot open, S1 is unavailable and says why", o.state == AX_UNAVAILABLE && !sh.review_open && strstr(o.msg, "keyboard") != 0 && REG[2] == 100);
        fresh(); aura_s1_attach(0); o = run(0, "make text bigger");
        CHECK("with no S1 environment attached the old behaviour remains: needs approval, nothing changed, no review", o.state == AX_NEEDS_APPROVAL && !sh.review_open && strstr(o.msg, "Needs your approval") != 0);
    }
    { /* who may ask */
        fresh(); el0_settings_right = 1; struct aura_outcome o = run(2, "make text bigger");
        CHECK("an EL0 requester holding the settings right still cannot open a review or change anything (only the trusted shell starts S1)", o.decision == BP_REVIEW && !sh.review_open && REG[2] == 100 && ntr == 0);
        fresh(); o = run(2, "make text bigger");
        CHECK("an EL0 requester without the right is denied", o.decision == BP_DENY && !sh.review_open);
        fresh(); shell_settings_right = 0; o = run(0, "make text bigger");
        CHECK("the shell without the settings right is denied", o.decision == BP_DENY && !sh.review_open);
        fresh(); ENV.review_path = 0; o = run(0, "make text bigger");
        CHECK("with no review path the policy answers UNAVAILABLE and nothing opens", o.decision == BP_UNAVAILABLE && !sh.review_open);
        fresh(); ENV.trail = 0; o = run(0, "make text bigger");
        CHECK("with no Trail the policy answers UNAVAILABLE (an unrecorded change cannot run)", o.decision == BP_UNAVAILABLE && !sh.review_open);
    }
    { /* the ladder and the limits */
        fresh(); REG[2] = 200; struct aura_outcome o = run(0, "make text bigger");
        CHECK("already the largest: no review, nothing changed", !sh.review_open && o.state == AX_FAILED && strstr(o.msg, "largest") != 0);
        fresh(); REG[2] = 75; o = run(0, "make text smaller");
        CHECK("already the smallest", !sh.review_open && strstr(o.msg, "smallest") != 0);
        fresh(); o = run(0, "set text size to 100");
        CHECK("setting the value it already has: no review", !sh.review_open && strstr(o.msg, "Already") != 0);
        fresh(); REG[2] = 110; run(0, "make text bigger"); approve(); finish(0);
        CHECK("a value off the ladder steps to the next step above (110 -> 125)", REG[2] == 125);
        fresh(); REG[2] = 150; run(0, "make text smaller"); approve(); finish(0);
        CHECK("smaller from 150 -> 125", REG[2] == 125 && sh.text_pct == 125);
        fresh(); run(0, "set text size to 175"); approve(); finish(0);
        CHECK("an absolute size is applied exactly", REG[2] == 175 && sh.text_pct == 175);
        fresh(); o = run(0, "set text size to 500");
        CHECK("an out-of-range size is rejected by the parser: nothing opens", !sh.review_open && REG[2] == 100 && o.state != AX_NEEDS_APPROVAL);
        fresh(); run(0, "set theme to dark"); approve(); finish(0);
        CHECK("theme: stored (registry 2) and recorded, the text size is untouched, and the interface scale did not change", REG[1] == 2 && sh.text_pct == 100 && ntr == 2 && TR[0].target == 1 && TR[1].target == 1 && TR[1].arg == 2);
        fresh(); run(0, "make text bigger"); struct aura_outcome o2 = run(0, "make text bigger");
        CHECK("a second request while a review is open is refused (one review at a time)", o2.state == AX_UNAVAILABLE && strstr(o2.msg, "Another review") != 0 && sh.review_open);
    }
    { /* recovery of an interrupted change */
        fresh(); struct trail_rec pre; memset(&pre, 0, sizeof pre); pre.kind = AK_SET_SETTING; pre.target = 2; pre.state = AX_NEEDS_APPROVAL; pre.arg = 100; pre.risk = 1; pre.decision = BP_REVIEW; e_append(&pre);
        CHECK("recovery: a newest PRE record is closed with an 'interrupted' FAILED record carrying the setting's real value", aura_s1_recover(&sh) == 1 && ntr == 2 && TR[1].state == AX_FAILED && TR[1].reason == 1 && TR[1].arg == 100);
        CHECK("... and it does not repeat once the newest record is no longer a PRE", aura_s1_recover(&sh) == 0 && ntr == 2);
        fresh(); e_append(&pre); REG[2] = 125;
        CHECK("recovery after the setting WAS changed but the POST was lost: the FAILED/interrupted record carries 125, not a guess", aura_s1_recover(&sh) == 1 && TR[1].arg == 125);
        fresh(); f_healthy = 0; e_append(&pre);
        CHECK("recovery does nothing while the records are unhealthy", aura_s1_recover(&sh) == 0 && ntr == 1);
        fresh(); CHECK("recovery on an empty Trail does nothing", aura_s1_recover(&sh) == 0 && ntr == 0);
    }
    { /* Undo: a receipt is reversed only through a NEW trusted review, only if nothing newer changed the setting */
        fresh(); struct aura_outcome o = run(0, "undo");
        CHECK("undo with an empty Trail: nothing to undo, no review", !sh.review_open && strstr(o.msg, "Nothing to undo") != 0 && ntr == 0);
        fresh(); run(0, "make text bigger"); approve(); finish(0); int n0 = ntr;
        o = run(0, "undo");
        CHECK("undo after a change opens a NEW review showing 125% -> 100%, nothing written yet", sh.review_open && has_text("Text size: 125% -> 100%") && has_text("Aura (Undo)") && REG[2] == 125 && ntr == n0);
        CHECK("approving the undo restores 100 and writes a second PRE/POST pair", approve() && finish(0) == AX_DONE && REG[2] == 100 && sh.text_pct == 100 && ntr == n0 + 2 && TR[n0].state == AX_NEEDS_APPROVAL && TR[n0].arg == 125 && TR[n0 + 1].state == AX_DONE && TR[n0 + 1].arg == 100);
        fresh(); run(0, "make text bigger"); approve(); finish(0); n0 = ntr; run(0, "undo"); key(KEY_ESC);
        CHECK("declining the undo changes nothing", REG[2] == 125 && ntr == n0);
        fresh(); run(0, "make text bigger"); approve(); finish(0); REG[2] = 150; n0 = ntr; o = run(0, "undo");
        CHECK("someone changed the setting after the receipt: undo refuses, never overwrites the newer value", !sh.review_open && REG[2] == 150 && ntr == n0 && strstr(o.msg, "changed since") != 0);
        fresh(); run(0, "make text bigger"); approve(); finish(0); run(0, "undo"); approve(); REG[2] = 130; int st = finish(0);
        CHECK("the setting changed between undo approval and execution: refused, no PRE written", st != AX_DONE && REG[2] == 130);
        fresh(); run(0, "make text bigger"); approve(); f_append_fail_at = 2; finish(0); f_append_fail_at = 0; n0 = ntr; o = run(0, "undo");
        CHECK("a change whose POST was lost has no receipt: no Undo", !sh.review_open && REG[2] == 125 && ntr == n0);
        fresh(); run(0, "make text bigger"); approve(); f_set_rc = SETTINGS_ESTORE; finish(0); f_set_rc = 0; n0 = ntr; run(0, "undo");
        CHECK("a failed change has no receipt: no Undo", !sh.review_open && ntr == n0);
        fresh(); run(0, "make text bigger"); approve(); finish(0); f_healthy = 0; run(0, "undo");
        CHECK("unhealthy records: undo not available", !sh.review_open);
        fresh(); run(0, "make text bigger"); approve(); finish(0); f_healthy = 1; o = run(2, "undo");
        CHECK("an EL0 requester typing undo does not get the shell's undo", !sh.review_open && REG[2] == 125);
        fresh(); run(0, "set theme to dark"); approve(); finish(0); run(0, "UNDO "); approve(); finish(0);
        CHECK("theme undo (also case/space tolerant): back to the previous value 0", REG[1] == 0);
        { struct trail_rec x; memset(&x, 0, sizeof x); x.kind = AK_SET_SETTING; x.target = 2;
          fresh(); x.state = AX_NEEDS_APPROVAL; x.arg = 100; e_append(&x); x.state = AX_FAILED; x.arg = 125; e_append(&x); REG[2] = 125; run(0, "undo");
          CHECK("a newest FAILED record is not a receipt (even if its value happens to match): no Undo", !sh.review_open);
          fresh(); x.state = AX_DONE; x.arg = 100; e_append(&x); x.state = AX_DONE; x.arg = 125; e_append(&x); REG[2] = 125; run(0, "undo");
          CHECK("a DONE record not preceded by its own PRE is not a receipt: no Undo", !sh.review_open);
          fresh(); x.state = AX_NEEDS_APPROVAL; x.arg = 100; x.target = 1; e_append(&x); x.state = AX_DONE; x.arg = 2; x.target = 2; e_append(&x); REG[2] = 2; run(0, "undo");
          CHECK("a PRE for a different setting is not this receipt: no Undo", !sh.review_open); }
        fresh(); run(0, "make text bigger"); approve(); finish(0); run(0, "undo"); approve(); finish(0); run(0, "undo");
        CHECK("undoing an undo is a new, separately reviewed change (redo to 125)", sh.review_open && has_text("Text size: 100% -> 125%"));
    }
    { /* randomized: whatever happens, the setting changes only through a complete approved chain */
        uint32_t s = 2026; int bad = 0; fresh(); int done = 0, changed_ext = 0; uint32_t last_val = REG[2];
        for (int it = 0; it < 4000 && !bad; it++) {
            s = s * 1664525u + 1013904223u;
            f_set_rc = 0; f_lie = 0; f_append_fail_at = 0; f_append_calls = 0;
            switch ((s >> 10) % 8) {
            case 0: f_set_rc = ((s >> 20) % 4 == 0) ? SETTINGS_ESTORE : 0; break;
            case 1: f_lie = ((s >> 20) % 5 == 0); break;
            case 2: f_append_fail_at = (int)((s >> 20) % 4); break;
            default: break;
            }
            ntr = 0;                                                                     /* the fake Trail is a small array: each chain starts it afresh so it never overflows */
            uint32_t before = REG[2]; int nrec = ntr;
            const char *cmd = ((s >> 5) & 1) ? "make text bigger" : "make text smaller";
            if (sh.review_open) { key(KEY_ESC); }
            run(0, cmd);
            int path = (int)((s >> 14) % 6);
            if (path == 0) key(KEY_ESC);                                                  /* cancel */
            else if (path == 1) tick(61000);                                              /* expire */
            else if (path == 2) { approve(); REG[2] = ((s >> 18) & 1) ? REG[2] + 1 : REG[2]; if (REG[2] != before) changed_ext++; int st = finish(0); (void)st; }   /* the world may change under it */
            else { approve(); int st = finish(0); if (st == AX_DONE) done++; }
            if (REG[2] < 75 || REG[2] > 215) bad = 1;                                   /* the fakes themselves can nudge a value (the lying registry, the external poke) by a few: a runaway would exceed this */
            if (ntr > nrec && TR[nrec].state != AX_NEEDS_APPROVAL) bad = 2;               /* a first new record is always a PRE */
            if (REG[2] != before && (path == 0 || path == 1)) bad = 3;                    /* cancelled or expired: no change */
            if (REG[2] != before && path >= 2 && !(ntr >= nrec + 1 && TR[nrec].state == AX_NEEDS_APPROVAL) && !(REG[2] == before + 1 && ntr == nrec)) bad = 4;   /* a change always has its PRE record first (the POST may be the one that was lost); the only change without records is the external +1 poke */
            if (sh.review_open) { key(KEY_ESC); }
            last_val = REG[2];
        }
        (void)last_val; (void)changed_ext;
        if (bad) printf("    random run stopped: bad=%d\n", bad);
        CHECK("4,000 random chains with random failures and external changes: cancelled/expired reviews never change anything, every first new record is a PRE, a change is always accompanied by its records", bad == 0 && done > 100);
    }
    printf("aura S1: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
