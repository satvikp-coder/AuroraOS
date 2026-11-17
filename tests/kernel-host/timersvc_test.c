/* tests/kernel-host/timersvc_test.c - the monotonic timer service core (kernel/ui/timersvc.c): start/remaining/cancel/completion, correctness when the caller
 * polls LATE or rarely (delayed scheduling), a counter that goes backwards, saturation at the top of the 64-bit range, ownership by (pid, generation),
 * owner exit, bounds, and a randomised model check. Host, ASan/UBSan. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <timersvc.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)
static struct tsvc S;
static uint64_t xs = 0x0DDBA11C0FFEE123ULL;
static uint64_t rnd(void) { xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17; return xs; }
#define A 3u, 30ULL      /* owner instance A: pid 3, generation 30 */
#define B 4u, 40ULL

int main(void)
{
    printf("timer service tests\n");
    uint8_t st; uint64_t rem;

    tsvc_init(&S);
    int id = tsvc_start(&S, 1000, 25 * 60 * 1000, A);
    CHECK("a 25-minute timer starts and gets slot 0", id == 0);
    CHECK("remaining time right after starting is the full duration", tsvc_query(&S, 1000, 0, A, &st, &rem) == TS_OK && st == TS_RUNNING && rem == 25 * 60 * 1000);
    CHECK("remaining time counts down with the monotonic counter", tsvc_query(&S, 1000 + 60000, 0, A, &st, &rem) == TS_OK && rem == 24 * 60 * 1000);
    CHECK("one millisecond before the deadline it is still running with 1 ms left", tsvc_query(&S, 1000 + 25 * 60 * 1000 - 1, 0, A, &st, &rem) == TS_OK && st == TS_RUNNING && rem == 1);
    CHECK("poll before the deadline completes nothing", tsvc_poll(&S, 1000 + 25 * 60 * 1000 - 1) == 0);
    CHECK("exactly at the deadline the timer completes (once)", tsvc_poll(&S, 1000 + 25 * 60 * 1000) == 1 && tsvc_poll(&S, 1000 + 25 * 60 * 1000 + 5) == 0);
    CHECK("a completed timer reports DONE and 0 remaining", tsvc_query(&S, 1000 + 25 * 60 * 1000 + 5, 0, A, &st, &rem) == TS_OK && st == TS_DONE && rem == 0);
    CHECK("a completed timer cannot be cancelled (it already happened)", tsvc_cancel(&S, 0, A) == TS_E_STATE);
    CHECK("releasing a DONE timer frees its slot", tsvc_release(&S, 0, A) == TS_OK && tsvc_query(&S, 2000000, 0, A, &st, &rem) == TS_E_NOENT);

    /* ---- delayed scheduling: the owner was starved and reads only long after the deadline ---- */
    tsvc_init(&S); tsvc_start(&S, 5000, 10000, A);
    CHECK("a reader that was starved for an hour still sees DONE with 0 remaining (the deadline is absolute)", tsvc_query(&S, 5000 + 3600ULL * 1000, 0, A, &st, &rem) == TS_OK && st == TS_DONE && rem == 0);
    tsvc_init(&S); tsvc_start(&S, 0, 10000, A);
    for (uint64_t t = 0; t < 9000; t += 7) tsvc_poll(&S, t);               /* irregular polling... */
    CHECK("... after thousands of polls it is still running with the exact remaining time (no drift from accumulated deltas)", tsvc_query(&S, 9000, 0, A, &st, &rem) == TS_OK && st == TS_RUNNING && rem == 1000);
    tsvc_poll(&S, 20000);
    CHECK("a single very late poll completes it", tsvc_query(&S, 20000, 0, A, &st, &rem) == TS_OK && st == TS_DONE);

    /* ---- counter going backwards ---- */
    tsvc_init(&S); tsvc_start(&S, 10000, 5000, A); tsvc_poll(&S, 12000);
    CHECK("a smaller counter value is 'no time passed': remaining does not grow", tsvc_query(&S, 500, 0, A, &st, &rem) == TS_OK && rem == 3000 && S.backwards == 1);
    CHECK("... and it never completes early because of it", tsvc_poll(&S, 100) == 0 && S.t[0].state == TS_RUNNING);
    CHECK("then time moving forward completes it normally", tsvc_poll(&S, 15000) == 1);

    /* ---- top of the range ---- */
    tsvc_init(&S);
    int big = tsvc_start(&S, 0xFFFFFFFFFFFFFFFFULL - 10, 5000, A);
    CHECK("a start near 2^64 saturates the deadline instead of wrapping to a tiny one", big == 0 && S.t[0].deadline_ms == 0xFFFFFFFFFFFFFFFFULL && tsvc_poll(&S, 0xFFFFFFFFFFFFFFFFULL - 5) == 0);
    CHECK("... and completes when the counter reaches the maximum", tsvc_poll(&S, 0xFFFFFFFFFFFFFFFFULL) == 1);

    /* ---- limits: refused, never clamped ---- */
    tsvc_init(&S);
    CHECK("0 ms, 999 ms and 24 h + 1 ms are refused", tsvc_start(&S, 0, 0, A) == TS_E_ARG && tsvc_start(&S, 0, 999, A) == TS_E_ARG && tsvc_start(&S, 0, TS_MAX_MS + 1, A) == TS_E_ARG);
    CHECK("1 second and 24 hours are accepted", tsvc_start(&S, 0, 1000, A) >= 0 && tsvc_start(&S, 0, TS_MAX_MS, A) >= 0);
    CHECK("generation 0 (no instance) is refused", tsvc_start(&S, 0, 5000, 3, 0) == TS_E_ARG);
    CHECK("NULL table is refused without a crash", tsvc_start(0, 0, 5000, A) == TS_E_ARG && tsvc_poll(0, 5) == 0);
    tsvc_init(&S); tsvc_start(&S, 0, 5000, A); tsvc_start(&S, 0, 5000, A); tsvc_start(&S, 0, 5000, B); tsvc_start(&S, 0, 5000, B);
    CHECK("quota_per_owner: a third timer for an owner holding TS_PER_OWNER is refused with QUOTA (not FULL)", tsvc_start(&S, 0, 5000, A) == TS_E_QUOTA && tsvc_start(&S, 0, 5000, B) == TS_E_QUOTA);
    CHECK("other_owner_unaffected / the table is bounded: a third owner finds the table FULL", tsvc_start(&S, 0, 5000, 9u, 90ULL) == TS_E_FULL);
    CHECK("cancelling frees nothing by itself, releasing does (quota_freed_by_release)", tsvc_cancel(&S, 1, A) == TS_OK && tsvc_start(&S, 0, 5000, A) == TS_E_QUOTA && tsvc_release(&S, 1, A) == TS_OK && tsvc_start(&S, 0, 5000, A) == 1);
    tsvc_init(&S); tsvc_start(&S, 0, 5000, A); tsvc_start(&S, 0, 5000, A);
    CHECK("quota_freed_by_owner_gone: when the owner exits its quota is free again for the next instance of the same pid", tsvc_start(&S, 0, 5000, A) == TS_E_QUOTA && (tsvc_owner_gone(&S, A), 1) && tsvc_start(&S, 0, 5000, 3u, 31ULL) >= 0 && tsvc_start(&S, 0, 5000, 3u, 31ULL) >= 0 && tsvc_start(&S, 0, 5000, 3u, 31ULL) == TS_E_QUOTA);
    tsvc_init(&S); tsvc_start(&S, 0, 5000, A); tsvc_start(&S, 0, 5000, A);
    CHECK("a quota refusal changes nothing else (the counter and both timers are intact)", tsvc_start(&S, 0, 5000, A) == TS_E_QUOTA && S.t[0].state == TS_RUNNING && S.t[1].state == TS_RUNNING && S.t[2].state == TS_FREE);

    /* ---- ownership ---- */
    tsvc_init(&S); tsvc_start(&S, 0, 60000, A);
    CHECK("another process cannot read, cancel or release it (same answer as 'no such timer')", tsvc_query(&S, 1, 0, B, &st, &rem) == TS_E_NOENT && tsvc_cancel(&S, 0, B) == TS_E_NOENT && tsvc_release(&S, 0, B) == TS_E_NOENT);
    CHECK("a later instance of the same pid (new generation) cannot touch it either", tsvc_cancel(&S, 0, 3, 31) == TS_E_NOENT && tsvc_query(&S, 1, 0, 3, 31, &st, &rem) == TS_E_NOENT);
    CHECK("out-of-range ids and NULL outputs are refused", tsvc_cancel(&S, 99, A) == TS_E_NOENT && tsvc_query(&S, 1, 0, A, 0, &rem) == TS_E_ARG && tsvc_query(&S, 1, 0, A, &st, 0) == TS_E_ARG);
    CHECK("the owner can cancel while running; the state is CANCELLED with 0 remaining", tsvc_cancel(&S, 0, A) == TS_OK && tsvc_query(&S, 2, 0, A, &st, &rem) == TS_OK && st == TS_CANCELLED && rem == 0);
    CHECK("a cancelled timer never completes", tsvc_poll(&S, 10000000) == 0 && S.t[0].state == TS_CANCELLED && tsvc_cancel(&S, 0, A) == TS_E_STATE);
    tsvc_init(&S); tsvc_start(&S, 0, 5000, A); tsvc_start(&S, 0, 5000, B); tsvc_start(&S, 0, 5000, A);
    tsvc_owner_gone(&S, A);
    CHECK("when an instance exits all ITS timers go and nobody else's", S.t[0].state == TS_FREE && S.t[2].state == TS_FREE && S.t[1].state == TS_RUNNING && S.t[1].pid == 4);
    CHECK("an exited process's pid with a new generation cannot see old timers", tsvc_query(&S, 1, 0, 3, 31, &st, &rem) == TS_E_NOENT);

    /* ---- randomised: agree with a model that knows only absolute deadlines ---- */
    {
        tsvc_init(&S);
        struct { int used, state; uint32_t pid; uint64_t gen, deadline; } m[TS_MAX]; memset(m, 0, sizeof m);
        uint64_t now = 100; int bad = 0, reported = 0;
        for (int it = 0; it < 40000; it++) {
            uint64_t r = rnd();
            if (r % 5 == 0) now += r % 90000; else if (r % 41 == 0 && now > 50) { uint64_t back = now - (r % 50); tsvc_poll(&S, back); for (int i = 0; i < TS_MAX; i++) if (m[i].used && m[i].state == TS_RUNNING && S.last_now >= m[i].deadline) m[i].state = TS_DONE; }   /* a stale smaller reading: must change nothing */
            uint32_t pid = 3 + (uint32_t)(r % 3); uint64_t gen = 30 + (r % 3) * 10 + (pid == 3 ? 0 : 0);
            switch ((r >> 8) % 6) {
            case 0: { uint64_t d = TS_MIN_MS + (r >> 20) % (3600ULL * 1000); int id2 = tsvc_start(&S, now, d, pid, gen); int free_ = -1; for (int i = 0; i < TS_MAX; i++) if (!m[i].used) { free_ = i; break; }
                      int held = 0; for (int i = 0; i < TS_MAX; i++) if (m[i].used && m[i].pid == pid && m[i].gen == gen) held++;
                      if (held >= TS_PER_OWNER) { if (id2 != TS_E_QUOTA) bad++; } else if (free_ < 0) { if (id2 != TS_E_FULL) bad++; } else { if (id2 != free_) bad++; else { m[free_].used = 1; m[free_].state = TS_RUNNING; m[free_].pid = pid; m[free_].gen = gen; m[free_].deadline = now + d; } } break; }
            case 1: break;                                                       /* (idle step) */
            case 2: tsvc_poll(&S, now); for (int i = 0; i < TS_MAX; i++) if (m[i].used && m[i].state == TS_RUNNING && now >= m[i].deadline) m[i].state = TS_DONE; break;
            case 3: { uint32_t id2 = (uint32_t)((r >> 24) % TS_MAX); tsvc_poll(&S, now); for (int i = 0; i < TS_MAX; i++) if (m[i].used && m[i].state == TS_RUNNING && now >= m[i].deadline) m[i].state = TS_DONE;
                      int rc = tsvc_query(&S, now, id2, pid, gen, &st, &rem);
                      if (!m[id2].used || m[id2].pid != pid || m[id2].gen != gen) { if (rc != TS_E_NOENT) bad++; }
                      else { if (rc != TS_OK || st != m[id2].state) bad++; else if (st == TS_RUNNING && rem != m[id2].deadline - now) bad++; else if (st != TS_RUNNING && rem != 0) bad++; } break; }
            case 4: { uint32_t id2 = (uint32_t)((r >> 24) % TS_MAX); tsvc_poll(&S, now); for (int i = 0; i < TS_MAX; i++) if (m[i].used && m[i].state == TS_RUNNING && now >= m[i].deadline) m[i].state = TS_DONE;
                      int rc = tsvc_cancel(&S, id2, pid, gen); int want = !m[id2].used || m[id2].pid != pid || m[id2].gen != gen ? TS_E_NOENT : (m[id2].state != TS_RUNNING ? TS_E_STATE : TS_OK);
                      if (rc != want) { bad++; printf("    cancel id %u pid %u gen %llu: rc %d want %d (model used %d state %d pid %u gen %llu; svc state %d)%c", id2, pid, (unsigned long long)gen, rc, want, m[id2].used, m[id2].state, m[id2].pid, (unsigned long long)m[id2].gen, S.t[id2].state, 10); } else if (rc == TS_OK) m[id2].state = TS_CANCELLED; break; }
            default: { tsvc_owner_gone(&S, pid, gen); for (int i = 0; i < TS_MAX; i++) if (m[i].used && m[i].pid == pid && m[i].gen == gen) memset(&m[i], 0, sizeof m[i]); break; }
            }
            for (int i = 0; i < TS_MAX; i++) { if ((S.t[i].state != TS_FREE) != (m[i].used != 0)) bad++; }
            if (bad && !reported) { reported = 1; printf("    first mismatch at iteration %d, case %d, now %llu, last_now %llu\n", it, (int)((r >> 8) % 6), (unsigned long long)now, (unsigned long long)S.last_now); }
        }
        CHECK("40,000 random starts/cancels/polls/queries/exits (with stale counter readings) agree with a deadline-only model", bad == 0);
    }

    { /* tick -> millisecond conversion: checked arithmetic */
        uint64_t ms = 99;
        CHECK("62.5 MHz: 62,500,000 ticks = 1000 ms", tsvc_ticks_to_ms(62500000ULL, 62500000ULL, &ms) == TS_OK && ms == 1000);
        CHECK("62.5 MHz: 0 ticks = 0 ms", tsvc_ticks_to_ms(0, 62500000ULL, &ms) == TS_OK && ms == 0);
        CHECK("sub-second remainder is exact: 31,250,000 ticks = 500 ms", tsvc_ticks_to_ms(31250000ULL, 62500000ULL, &ms) == TS_OK && ms == 500);
        CHECK("one tick below a millisecond rounds down", tsvc_ticks_to_ms(62499ULL, 62500000ULL, &ms) == TS_OK && ms == 0);
        CHECK("zero frequency is refused (no division)", tsvc_ticks_to_ms(5, 0, &ms) == TS_E_ARG);
        CHECK("NULL output is refused", tsvc_ticks_to_ms(5, 1000, 0) == TS_E_ARG);
        CHECK("an absurd frequency is refused (rem*1000 would overflow)", tsvc_ticks_to_ms(5, 0xFFFFFFFFFFFFFFFFULL, &ms) == TS_E_ARG);
        CHECK("a count whose milliseconds exceed 64 bits is refused, not wrapped", tsvc_ticks_to_ms(0xFFFFFFFFFFFFFFFFULL, 1, &ms) == TS_E_ARG);
        CHECK("the largest representable count at 1 kHz converts exactly", tsvc_ticks_to_ms(0xFFFFFFFFFFFFFFFFULL, 1000, &ms) == TS_OK && ms == 0xFFFFFFFFFFFFFFFFULL);
        uint64_t prev = 0; int mono = 1;                                     /* monotonic in the count */
        for (uint64_t c = 0; c < 200000000ULL; c += 7777777ULL) { tsvc_ticks_to_ms(c, 62500000ULL, &ms); if (ms < prev) mono = 0; prev = ms; }
        CHECK("the conversion is monotonic in the counter", mono);
    }

    printf("timer service: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
