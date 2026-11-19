/* tests/kernel-host/hold_test.c - the hold-to-confirm state machine (kernel/ui/hold.c): the exact 1,499 / 1,500 ms boundary, every cancellation cause, repeats,
 * single confirmation, "a release processed before the poll wins", pointer leave/re-enter, a stuck-key start, backwards time, segment progress, and a 60,000-event
 * randomized run against an independent model. Host, ASan/UBSan. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <hold.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)
static struct hold H;

int main(void)
{
    printf("hold tests\n");

    { /* the boundary, keyboard */
        hold_init(&H); hold_event(&H, HE_KEY_DOWN, 1000, 0);
        CHECK("a key-down starts the hold", H.state == HD_HOLDING);
        CHECK("at 1,499 ms it does NOT confirm", hold_poll(&H, 1000 + 1499) == 0 && H.state == HD_HOLDING);
        CHECK("at 1,500 ms it confirms", hold_poll(&H, 1000 + 1500) == 1 && H.state == HD_CONFIRMED && H.confirmations == 1);
        CHECK("polling again never confirms a second time", hold_poll(&H, 1000 + 1500) == 0 && hold_poll(&H, 9000) == 0 && H.confirmations == 1);
        hold_event(&H, HE_KEY_UP, 9100, 0);
        CHECK("the later release does nothing but end the hold (no second confirmation)", H.state == HD_IDLE && H.confirmations == 1 && hold_poll(&H, 20000) == 0);
        hold_event(&H, HE_KEY_DOWN, 30000, 0);
        CHECK("a NEW press after the release starts a fresh hold", H.state == HD_HOLDING && hold_poll(&H, 31499) == 0 && hold_poll(&H, 31500) == 1 && H.confirmations == 2);
    }
    { /* the boundary, pointer */
        hold_init(&H); hold_event(&H, HE_PTR_DOWN, 500, 0);
        CHECK("pointer: 1,499 no, 1,500 yes", hold_poll(&H, 500 + 1499) == 0 && hold_poll(&H, 500 + 1500) == 1);
    }
    { /* every cancellation */
        struct { int ev; const char *name; } C[] = { { HE_KEY_UP, "release of the starting key" }, { HE_OTHER_KEY, "another key (Tab included)" }, { HE_ESCAPE, "Escape" }, { HE_FOCUS_CHANGE, "a focus change" }, { HE_MODAL_CHANGE, "a modal change" } };
        for (unsigned i = 0; i < sizeof C / sizeof C[0]; i++) {
            hold_init(&H); hold_event(&H, HE_KEY_DOWN, 0, 0); hold_event(&H, C[i].ev, 700, 0);
            char nm[96]; snprintf(nm, sizeof nm, "keyboard hold cancelled by %s: no confirmation even at 5,000 ms", C[i].name);
            CHECK(nm, H.state == HD_CANCELLED && hold_poll(&H, 5000) == 0 && H.confirmations == 0);
        }
        struct { int ev; const char *name; } P[] = { { HE_PTR_UP, "release of the button" }, { HE_PTR_LEAVE, "the pointer leaving the control" }, { HE_OTHER_KEY, "a key" }, { HE_ESCAPE, "Escape" }, { HE_FOCUS_CHANGE, "a focus change" }, { HE_MODAL_CHANGE, "a modal change" } };
        for (unsigned i = 0; i < sizeof P / sizeof P[0]; i++) {
            hold_init(&H); hold_event(&H, HE_PTR_DOWN, 0, 0); hold_event(&H, P[i].ev, 700, 0);
            char nm[96]; snprintf(nm, sizeof nm, "pointer hold cancelled by %s: no confirmation", P[i].name);
            CHECK(nm, H.state == HD_CANCELLED && hold_poll(&H, 5000) == 0 && H.confirmations == 0);
        }
        hold_init(&H); hold_event(&H, HE_PTR_DOWN, 0, 0); hold_event(&H, HE_PTR_LEAVE, 500, 0); hold_event(&H, HE_PTR_ENTER, 600, 0);
        CHECK("re-entering the control does NOT resume a cancelled hold", H.state == HD_CANCELLED && hold_poll(&H, 3000) == 0);
        hold_init(&H); hold_event(&H, HE_KEY_DOWN, 0, 0); hold_event(&H, HE_PTR_LEAVE, 500, 0); hold_event(&H, HE_PTR_UP, 600, 0);
        CHECK("pointer events do not cancel a KEYBOARD hold (and a pointer release does not either)", H.state == HD_HOLDING && hold_poll(&H, 1500) == 1);
        hold_init(&H); hold_event(&H, HE_PTR_DOWN, 0, 0); hold_event(&H, HE_KEY_UP, 500, 0);
        CHECK("a key-up does not cancel a POINTER hold", H.state == HD_HOLDING && hold_poll(&H, 1500) == 1);
    }
    { /* repeats and double presses */
        hold_init(&H); hold_event(&H, HE_KEY_DOWN, 0, 1);
        CHECK("an auto-repeat key-down never STARTS a hold (a stuck or held-down key cannot begin one)", H.state == HD_IDLE && hold_poll(&H, 9000) == 0);
        hold_init(&H); hold_event(&H, HE_KEY_DOWN, 0, 0);
        int ok = 1; for (int i = 0; i < 1000; i++) hold_event(&H, HE_KEY_DOWN, (uint64_t)i, 1);
        for (int i = 0; i < 1000; i++) hold_event(&H, HE_OTHER_KEY, 1000, 1);                      /* repeats of other keys are ignored too */
        CHECK("1,000 repeats neither cancel nor extend nor restart it", H.state == HD_HOLDING && H.t_start == 0 && ok);
        CHECK("... and it still confirms at exactly 1,500 ms from the real start", hold_poll(&H, 1499) == 0 && hold_poll(&H, 1500) == 1);
        hold_init(&H); hold_event(&H, HE_KEY_DOWN, 0, 0); hold_event(&H, HE_KEY_DOWN, 1000, 0);
        CHECK("a second non-repeat press while holding is ignored: it does not restart the clock", H.t_start == 0 && hold_poll(&H, 1500) == 1);
        hold_event(&H, HE_PTR_DOWN, 1600, 0);
        CHECK("after confirming, a different press does not start anything until the first is released", H.state == HD_CONFIRMED && hold_poll(&H, 9000) == 0 && H.confirmations == 1);
    }
    { /* fail closed: a release processed before the poll wins */
        hold_init(&H); hold_event(&H, HE_KEY_DOWN, 0, 0);
        hold_event(&H, HE_KEY_UP, 1500, 0);
        CHECK("the release is processed at 1,500 ms BEFORE the poll: cancelled, never confirmed", H.state == HD_CANCELLED && hold_poll(&H, 1500) == 0 && hold_poll(&H, 2000) == 0 && H.confirmations == 0);
        hold_init(&H); hold_event(&H, HE_PTR_DOWN, 0, 0); hold_event(&H, HE_PTR_LEAVE, 2500, 0);
        CHECK("a leave processed late (after 2.5 s, before any poll) still cancels", hold_poll(&H, 2500) == 0 && H.confirmations == 0);
    }
    { /* time going backwards, segments, NULL */
        hold_init(&H); hold_event(&H, HE_KEY_DOWN, 10000, 0);
        CHECK("a clock reading before the start cannot confirm", hold_poll(&H, 100) == 0 && H.anomalies == 1);
        CHECK("... nor shorten the real wait: still 1,500 ms from the start", hold_poll(&H, 11499) == 0 && hold_poll(&H, 11500) == 1);
        hold_init(&H); hold_event(&H, HE_KEY_DOWN, 0, 0);
        CHECK("segments: 0 at 374 ms, 1 at 375, 2 at 750, 3 at 1,125, 4 at 1,500 (capped)", hold_segments(&H, 374) == 0 && hold_segments(&H, 375) == 1 && hold_segments(&H, 749) == 1 && hold_segments(&H, 750) == 2 && hold_segments(&H, 1125) == 3 && hold_segments(&H, 1499) == 3 && hold_segments(&H, 1500) == 4 && hold_segments(&H, 99999) == 4);
        hold_poll(&H, 1500);
        CHECK("after a confirmation all four segments show", hold_segments(&H, 1600) == 4);
        hold_event(&H, HE_KEY_UP, 1700, 0);
        CHECK("idle and cancelled show none", hold_segments(&H, 1800) == 0 && ({ hold_event(&H, HE_KEY_DOWN, 2000, 0); hold_event(&H, HE_ESCAPE, 2100, 0); hold_segments(&H, 2200) == 0; }));
        CHECK("NULL machine is safe", ({ hold_event(0, HE_KEY_DOWN, 0, 0); hold_poll(0, 0) == 0 && hold_segments(0, 0) == 0; }));
        CHECK("an unknown event is ignored", ({ hold_init(&H); hold_event(&H, HE_KEY_DOWN, 0, 0); hold_event(&H, 99, 100, 0); H.state == HD_HOLDING; }));
    }
    { /* randomized: 60,000 events against an independent model */
        uint32_t s = 31337; int bad = 0; uint64_t now = 0;
        hold_init(&H);
        int m_hold = 0, m_src = 0, m_conf = 0; uint64_t m_start = 0; uint32_t confs = 0, expected_confs = 0;
        for (int i = 0; i < 60000 && !bad; i++) {
            s = s * 1664525u + 1013904223u;
            now += (s >> 22) % 600;
            int ev = 1 + (int)((s >> 8) % 10), repeat = (int)((s >> 4) % 4 == 0);
            if ((s >> 3) % 7 == 0) {                                  /* poll */
                int c = hold_poll(&H, now);
                int want = m_hold && now - m_start >= HOLD_MS;
                if (want) { m_hold = 0; m_conf = 1; expected_confs++; }
                if (c != want) { bad = 1; printf("    poll mismatch at %d: got %d want %d (now %llu start %llu)\n", i, c, want, (unsigned long long)now, (unsigned long long)m_start); }
                confs += (uint32_t)c;
                continue;
            }
            hold_event(&H, ev, now, repeat);
            if (repeat) continue;
            switch (ev) {
            case HE_KEY_DOWN: if (!m_hold && !m_conf) { m_hold = 1; m_src = 1; m_start = now; } break;
            case HE_PTR_DOWN: if (!m_hold && !m_conf) { m_hold = 1; m_src = 2; m_start = now; } break;
            case HE_KEY_UP: if (m_hold && m_src == 1) m_hold = 0; else if (m_conf && m_src == 1) m_conf = 0; break;
            case HE_PTR_UP: if (m_hold && m_src == 2) m_hold = 0; else if (m_conf && m_src == 2) m_conf = 0; break;
            case HE_PTR_LEAVE: if (m_hold && m_src == 2) m_hold = 0; break;
            case HE_OTHER_KEY: case HE_ESCAPE: case HE_FOCUS_CHANGE: case HE_MODAL_CHANGE: m_hold = 0; break;
            default: break;
            }
            if ((H.state == HD_HOLDING) != m_hold) { bad = 2; printf("    state mismatch at %d: machine %s model hold %d\n", i, hold_state_name(H.state), m_hold); }
        }
        CHECK("60,000 random events/polls/repeats: the machine matches the model; every confirmation was a full intact 1,500 ms hold; at most one per hold", bad == 0 && confs == expected_confs && H.confirmations == confs);
    }
    printf("hold: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
