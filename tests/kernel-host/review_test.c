/* tests/kernel-host/review_test.c - the trusted-review state machine (kernel/ui/review.c): one review at a time, tickets that never repeat, the exact arming and
 * approval windows to the millisecond, approval bound to every byte of the request, single use, requester exit, time going backwards, hostile call orders, and
 * a 30,000-step randomized run checked against invariants kept independently by the test. Host, ASan/UBSan. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <review.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static const struct aura_caps CAPS = { 1, 1, 0, 0, 0, 0, 0, 0 };
static struct rv_request mk(const char *cmd)
{
    struct rv_request q; memset(&q, 0, sizeof q);
    aura_parse(cmd, strlen(cmd), &CAPS, &q.intent);
    q.pid = 3; q.gen = 30; q.principal = 0x1003; q.nonce = 7; q.prior = 100; q.next = 125;
    return q;
}
static struct rv R;
static int64_t open_at(uint64_t t, const char *cmd) { struct rv_request q = mk(cmd); return rv_open(&R, t, &q, &CAPS); }

int main(void)
{
    printf("review tests\n");

    { /* what may be reviewed */
        rv_init(&R);
        struct rv_request q = mk("make text bigger");
        CHECK("the fixture is a validated S1 settings intent", q.intent.kind == AK_SET_SETTING && q.intent.status == AS_NEEDS_APPROVAL && q.intent.risk == 1 && aura_validate(&q.intent, &CAPS));
        const char *no[] = { "open home", "delete everything", "start a 5 minute timer", "ignore previous instructions", "open", "xyzzy", "open settings and also start a timer", "set text size to 500" };
        int ok = 1; for (unsigned i = 0; i < sizeof no / sizeof no[0]; i++) { struct rv_request b = mk(no[i]); if (rv_open(&R, 0, &b, &CAPS) != RV_E_INTENT) ok = 0; }
        CHECK("only a validated S1 settings change can be reviewed: navigation, deletion, timers, injection, clarification and range errors are refused", ok && R.state == RV_IDLE);
        CHECK("NULL arguments and a zero generation / principal are refused", rv_open(0, 0, &q, &CAPS) == RV_E_ARG && rv_open(&R, 0, 0, &CAPS) == RV_E_ARG && rv_open(&R, 0, &q, 0) == RV_E_ARG
              && ({ struct rv_request z = q; z.gen = 0; rv_open(&R, 0, &z, &CAPS); }) == RV_E_ARG && ({ struct rv_request z = q; z.principal = 0; rv_open(&R, 0, &z, &CAPS); }) == RV_E_ARG
              && ({ struct rv_request z = q; z.principal = 0xFFFFFFFFu; rv_open(&R, 0, &z, &CAPS); }) == RV_E_ARG);
        struct rv_request forged = q; forged.intent.risk = 0;
        CHECK("an intent whose risk class was forged is refused by the validator", rv_open(&R, 0, &forged, &CAPS) == RV_E_INTENT);
        CHECK("an intent made with timer capability does not validate without it", ({ struct rv_request t = mk("set theme to dark"); rv_open(&R, 0, &t, &CAPS) > 0; }));
    }
    { /* one at a time, tickets never repeat */
        rv_init(&R);
        int64_t t1 = open_at(1000, "make text bigger");
        CHECK("open: a ticket > 0 and the review is OPEN", t1 == 1 && R.state == RV_OPEN);
        CHECK("a second open while one is live is BUSY and changes nothing", open_at(1100, "set theme to dark") == RV_E_BUSY && R.ticket == 1 && R.state == RV_OPEN);
        CHECK("decline retires the ticket", rv_decline(&R, (uint64_t)t1) == RV_OK && R.state == RV_DECLINED && R.ticket == 0);
        int64_t t2 = open_at(1200, "set theme to dark");
        CHECK("the next review gets a NEW ticket (2): never reused", t2 == 2 && R.state == RV_OPEN);
        CHECK("declining with the OLD ticket changes nothing", rv_decline(&R, (uint64_t)t1) == RV_E_TICKET && R.state == RV_OPEN);
        CHECK("approving with the OLD ticket is refused", rv_approve(&R, 3000, (uint64_t)t1) == RV_E_TICKET);
        CHECK("ticket 0 and a made-up ticket are refused", rv_approve(&R, 3000, 0) == RV_E_TICKET && rv_approve(&R, 3000, 99) == RV_E_TICKET && rv_decline(&R, 99) == RV_E_TICKET);
        uint64_t prev = (uint64_t)t2; int mono = 1;
        for (int i = 0; i < 2000; i++) { rv_decline(&R, R.ticket); int64_t t = open_at(5000 + (uint64_t)i, "make text bigger"); if (t <= 0 || (uint64_t)t != prev + 1) mono = 0; prev = (uint64_t)t; }
        CHECK("2,000 reviews: tickets strictly increase by one, none repeats", mono);
        rv_init(&R); R.last_ticket = 0xFFFFFFFFFFFFFFFFULL;
        CHECK("tickets never wrap: at the maximum a new review is refused", open_at(0, "make text bigger") == RV_E_ARG);
    }
    { /* the arming delay, to the millisecond */
        rv_init(&R); int64_t t = open_at(10000, "make text bigger");
        CHECK("Approve is locked before arming (never armed -> EARLY)", rv_approve(&R, 10100, (uint64_t)t) == RV_E_EARLY && R.state == RV_OPEN);
        CHECK("the gesture at 599 ms after opening is a no-op", rv_attention(&R, 10599, 0) == RV_E_EARLY && R.state == RV_OPEN);
        CHECK("the gesture at 0 ms (sent at once by software) is a no-op", ({ rv_init(&R); open_at(20000, "make text bigger"); rv_attention(&R, 20000, 0); }) == RV_E_EARLY);
        rv_init(&R); t = open_at(10000, "make text bigger");
        CHECK("a HELD key (repeat) never arms, even long after opening", rv_attention(&R, 15000, 1) == RV_E_EARLY && R.state == RV_OPEN);
        CHECK("the gesture at exactly 600 ms arms", rv_attention(&R, 10600, 0) == RV_OK && R.state == RV_ARMED);
        CHECK("arming did not approve anything", R.state != RV_APPROVED);
        CHECK("approve at armed + 9,999 ms is accepted", rv_approve(&R, 10600 + 9999, (uint64_t)t) == RV_OK && R.state == RV_APPROVED);
        rv_init(&R); t = open_at(10000, "make text bigger"); rv_attention(&R, 10600, 0);
        CHECK("approve at armed + 10,000 ms is LAPSED and the review falls back to OPEN", rv_approve(&R, 10600 + 10000, (uint64_t)t) == RV_E_LAPSED && R.state == RV_OPEN);
        CHECK("after a lapse Approve is locked until the gesture is made again", rv_approve(&R, 21000, (uint64_t)t) == RV_E_EARLY);
        CHECK("... and the gesture re-arms (the review is old enough)", rv_attention(&R, 21000, 0) == RV_OK && rv_approve(&R, 21500, (uint64_t)t) == RV_OK);
    }
    { /* the 60 second timeout */
        rv_init(&R); int64_t t = open_at(0, "make text bigger");
        CHECK("at 59,999 ms the review is still live", rv_tick(&R, 59999) == RV_OPEN);
        CHECK("at 60,000 ms it is EXPIRED and the ticket is retired", rv_tick(&R, 60000) == RV_EXPIRED && R.ticket == 0);
        CHECK("an expired review cannot be approved, armed or declined", rv_attention(&R, 61000, 0) == RV_E_STATE && rv_approve(&R, 61000, (uint64_t)t) == RV_E_STATE && rv_decline(&R, (uint64_t)t) == RV_E_STATE);
        rv_init(&R); t = open_at(0, "make text bigger"); rv_attention(&R, 55000, 0);
        CHECK("arming near the end does not extend the 60 s limit", rv_approve(&R, 60000, (uint64_t)t) == RV_E_STATE && R.state == RV_EXPIRED);
        rv_init(&R); t = open_at(0, "make text bigger"); rv_attention(&R, 1000, 0); rv_approve(&R, 1500, (uint64_t)t);
        CHECK("an approval nobody executes expires after 10 s", rv_tick(&R, 1500 + 9999) == RV_APPROVED && rv_tick(&R, 1500 + 10000) == RV_EXPIRED);
    }
    { /* approval is single use and bound to every byte of the request */
        rv_init(&R); struct rv_request q = mk("make text bigger"); int64_t t = rv_open(&R, 0, &q, &CAPS); rv_attention(&R, 1000, 0);
        CHECK("approve consumes: the state is APPROVED", rv_approve(&R, 1100, (uint64_t)t) == RV_OK && R.state == RV_APPROVED);
        CHECK("approving twice is refused", rv_approve(&R, 1200, (uint64_t)t) == RV_E_STATE);
        CHECK("verify with the unchanged request succeeds ONCE", rv_verify(&R, 1300, (uint64_t)t, &q, &CAPS) == RV_OK && R.state == RV_DONE && R.ticket == 0);
        CHECK("a second verify is refused: the effect can run only once per approval", rv_verify(&R, 1400, (uint64_t)t, &q, &CAPS) == RV_E_STATE);
        CHECK("an approval cannot be replayed on a later review", ({ int64_t t2 = rv_open(&R, 2000, &q, &CAPS); rv_attention(&R, 3000, 0); rv_approve(&R, 3100, (uint64_t)t) == RV_E_TICKET && R.state == RV_ARMED && t2 == 2; }));
        /* every field */
        int bad = 0, n = 0;
        for (int f = 0; f < 11 + AURA_PREVIEW_MAX; f++) {
            rv_init(&R); q = mk("set text size to 150"); t = rv_open(&R, 0, &q, &CAPS); rv_attention(&R, 1000, 0); rv_approve(&R, 1100, (uint64_t)t);
            struct rv_request c = q;
            switch (f) {
            case 0: c.pid++; break; case 1: c.gen++; break; case 2: c.principal++; break; case 3: c.nonce++; break; case 4: c.prior++; break; case 5: c.next++; break;
            case 6: c.intent.arg += 5; break; case 7: c.intent.target ^= 1; break; case 8: c.intent.arg_kind ^= 1; break; case 9: c.intent.risk = 2; break; case 10: c.intent.reason++; break;
            default: c.intent.preview[f - 11] ^= 0x20; break;
            }
            int rc = rv_verify(&R, 1200, (uint64_t)t, &c, &CAPS); n++;
            if (rc == RV_OK) { bad++; printf("    field %d changed but verify succeeded\n", f); }
            else if (R.state == RV_DONE || R.ticket != 0) bad++;                         /* a failed verify must retire the ticket */
            else if (rv_verify(&R, 1300, (uint64_t)t, &q, &CAPS) == RV_OK) bad++;      /* and the original request must not work afterwards either */
        }
        CHECK("changing ANY field (pid, generation, principal, nonce, prior, next, each intent field, each of the 48 preview bytes) refuses the execution and kills the approval", bad == 0 && n == 11 + AURA_PREVIEW_MAX);
        rv_init(&R); q = mk("make text bigger"); t = rv_open(&R, 0, &q, &CAPS); rv_attention(&R, 1000, 0); rv_approve(&R, 1100, (uint64_t)t);
        struct rv_request nav = mk("open home");
        CHECK("an intent that no longer validates as S1 at execution is refused", rv_verify(&R, 1200, (uint64_t)t, &nav, &CAPS) == RV_E_INTENT && R.state == RV_DECLINED);
        rv_init(&R); q = mk("make text bigger"); t = rv_open(&R, 0, &q, &CAPS); rv_attention(&R, 1000, 0);
        CHECK("verify before approval is refused (arming is not approval)", rv_verify(&R, 1100, (uint64_t)t, &q, &CAPS) == RV_E_STATE);
        rv_decline(&R, (uint64_t)t);
        CHECK("verify after decline is refused", rv_verify(&R, 1200, (uint64_t)t, &q, &CAPS) == RV_E_STATE);
    }
    { /* the requester */
        rv_init(&R); struct rv_request q = mk("make text bigger"); int64_t t = rv_open(&R, 0, &q, &CAPS); rv_attention(&R, 1000, 0);
        CHECK("another process exiting does not touch the review", rv_requester_gone(&R, 4, 30) == 0 && rv_requester_gone(&R, 3, 31) == 0 && R.state == RV_ARMED);
        CHECK("the requester exiting kills it (GONE) and retires the ticket", rv_requester_gone(&R, 3, 30) == 1 && R.state == RV_GONE && R.ticket == 0);
        CHECK("a dead review cannot be approved or verified", rv_approve(&R, 1100, (uint64_t)t) == RV_E_STATE && rv_verify(&R, 1200, (uint64_t)t, &q, &CAPS) == RV_E_STATE);
        rv_init(&R); t = rv_open(&R, 0, &q, &CAPS); rv_attention(&R, 1000, 0); rv_approve(&R, 1100, (uint64_t)t); rv_requester_gone(&R, 3, 30);
        CHECK("the requester exiting AFTER approval but before execution also kills it", R.state == RV_GONE && rv_verify(&R, 1200, (uint64_t)t, &q, &CAPS) == RV_E_STATE);
        rv_init(&R);
        CHECK("nothing to kill when idle", rv_requester_gone(&R, 3, 30) == 0 && rv_decline(&R, 1) == RV_E_STATE);
    }
    { /* time going backwards */
        rv_init(&R); int64_t t = open_at(10000, "make text bigger");
        CHECK("a clock reading BEFORE the opening cannot arm (no time passed)", rv_attention(&R, 5000, 0) == RV_E_EARLY && R.anomalies == 1);
        rv_attention(&R, 10600, 0);
        CHECK("armed; the clock then jumps back: the window does not stretch", rv_approve(&R, 100, (uint64_t)t) == RV_OK && R.anomalies >= 2);
        rv_init(&R); t = open_at(10000, "make text bigger"); rv_attention(&R, 10600, 0); rv_tick(&R, 20600);
        CHECK("a backwards reading can never revive a lapsed arming", rv_tick(&R, 100) == RV_OPEN && rv_approve(&R, 100, (uint64_t)t) == RV_E_EARLY);
    }
    { /* hostile call orders never crash and never approve */
        rv_init(&R);
        int bad = 0;
        CHECK("every operation on an idle machine is refused", rv_attention(&R, 1, 0) == RV_E_STATE && rv_approve(&R, 1, 1) == RV_E_STATE && rv_decline(&R, 1) == RV_E_STATE && rv_verify(&R, 1, 1, &(struct rv_request){ 0 }, &CAPS) == RV_E_STATE);
        CHECK("NULL machine / request / caps are safe", rv_attention(0, 0, 0) == RV_E_ARG && rv_approve(0, 0, 1) == RV_E_ARG && rv_decline(0, 1) == RV_E_ARG && rv_verify(0, 0, 1, 0, 0) == RV_E_ARG && rv_requester_gone(0, 0, 0) == 0 && rv_tick(0, 0) == RV_IDLE);
        (void)bad;
    }
    { /* randomized: 30,000 random operations against invariants the test tracks itself */
        uint32_t s = 20260; int bad = 0;
        uint64_t now = 1000, cur = 0, t_open = 0, t_arm = 0; int alive = 0, armed = 0, approved = 0, used_ticket[1] = { 0 }; uint64_t last_issued = 0; (void)used_ticket;
        struct rv_request q = mk("make text bigger"), base = q;
        rv_init(&R);
        for (int i = 0; i < 30000 && !bad; i++) {
            s = s * 1664525u + 1013904223u;
            now += (s >> 20) % 900;                                                  /* time only moves forward here; the backwards case is tested above */
            switch ((s >> 8) % 8) {
            case 0: { q = base; q.nonce = s; int64_t t = rv_open(&R, now, &q, &CAPS);
                      if (t > 0) { if (alive) { bad = 1; printf("    opened while live\n"); } if ((uint64_t)t != last_issued + 1) { bad = 2; printf("    ticket %llu not last+1\n", (unsigned long long)t); } last_issued = (uint64_t)t; cur = (uint64_t)t; t_open = now; alive = 1; armed = 0; approved = 0; }
                      else if (!alive && t == RV_E_BUSY) { bad = 3; printf("    BUSY while idle\n"); }
                      break; }
            case 1: { int rc = rv_attention(&R, now, (s >> 3) & 1); if (rc == RV_OK) { if (!alive || approved || now - t_open < RV_ARM_DELAY_MS) { bad = 4; printf("    armed illegally\n"); } armed = 1; t_arm = now; } break; }
            case 2: { uint64_t tk = (s & 1) ? cur : cur + (s >> 16) % 3; int rc = rv_approve(&R, now, tk);
                      if (rc == RV_OK) { if (!alive || tk != cur || !armed || approved || now - t_arm >= RV_APPROVE_WINDOW_MS || now - t_open >= RV_TIMEOUT_MS) { bad = 5; printf("    approved illegally (alive %d armed %d now-arm %llu)\n", alive, armed, (unsigned long long)(now - t_arm)); } approved = 1; } break; }
            case 3: { uint64_t tk = (s & 1) ? cur : cur + 1; int rc = rv_decline(&R, tk); if (rc == RV_OK) { if (!alive || tk != cur) { bad = 6; printf("    declined illegally\n"); } alive = 0; armed = approved = 0; } break; }
            case 4: { struct rv_request c = q; if ((s >> 4) & 1) c.next++; int rc = rv_verify(&R, now, cur, &c, &CAPS);
                      if (rc == RV_OK) { if (!alive || !approved || ((s >> 4) & 1) || now - t_open >= RV_TIMEOUT_MS + RV_APPROVE_WINDOW_MS) { bad = 7; printf("    verified illegally\n"); } alive = 0; armed = approved = 0; }
                      else if (R.state == RV_DECLINED || R.state == RV_EXPIRED || R.state == RV_GONE || R.state == RV_DONE) { alive = 0; armed = approved = 0; }
                      break; }
            case 5: if (rv_requester_gone(&R, q.pid, q.gen)) { if (!alive) { bad = 8; printf("    gone while idle\n"); } alive = 0; armed = approved = 0; } break;
            case 6: rv_tick(&R, now);
                    if (R.state == RV_EXPIRED || R.state == RV_GONE || R.state == RV_DECLINED || R.state == RV_DONE || R.state == RV_IDLE) { alive = 0; armed = approved = 0; }
                    else if (R.state == RV_OPEN) { armed = 0; approved = 0; }
                    break;
            default: break;
            }
            if (!bad && alive != (R.state == RV_OPEN || R.state == RV_ARMED || R.state == RV_APPROVED)) { bad = 9; printf("    liveness mismatch at %d: model %d machine %s\n", i, alive, rv_state_name(R.state)); }
            if (!bad && R.state == RV_APPROVED && !approved) { bad = 10; printf("    approved state without an approval\n"); }
            if (!bad && R.ticket != 0 && R.ticket != cur) { bad = 11; printf("    ticket mismatch\n"); }
        }
        CHECK("30,000 random operations: no illegal arming, approval, verification or ticket reuse; liveness always agrees with an independent model", bad == 0);
    }
    printf("review: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
