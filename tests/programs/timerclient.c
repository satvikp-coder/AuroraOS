/*
 * tests/programs/timerclient.c - EL0 clients of SYS_TIMER (42) for tests/qemu/timer-tests.sh. The kernel launch registry passes the mode in x0:
 *   1 BASIC    a 2 s timer: never early (NOW), running -> done, completion EVENT (0x85) delivered, release, then every use-after-release is refused;
 *              quota (third timer), cancel twice, 25 minute and 24 hour timers are accepted and cancelled
 *   2 HOSTILE  bad durations, bad ops, bad pointers (unmapped and kernel), unknown ids, then a 20,000-iteration start/cancel/release flood
 *   3 NOCAP    holds no timer capability: every operation is refused
 *   4 HOLDER   starts a 3 s timer and keeps polling it: it must finish (state 2), never cancelled by anybody else
 *   5 SPY      tries to cancel / query / release every timer id and to take every slot: it must never touch the holder's timer
 *   6 EXITER   starts two timers and exits
 *   7 FAULTER  starts two timers and faults
 *   8 LATE     starts after the exiter and faulter are gone: it must be able to take its full quota (their timers died with them)
 * Output goes through SYS_WRITE ("[user pN] " prefix added by the kernel).
 */
#include "common.h"
#define SYS_TICKS 5
#define SYS_UI 41
#define SYS_TIMER 42
typedef unsigned char u8;

static i64 tm(i64 op, i64 a, i64 b) { return sc(SYS_TIMER, op, a, b, 0, 0); }
static i64 ui_poll(void *buf, u64 cap) { return sc(SYS_UI, 2, (i64)buf, (i64)cap, 0, 0); }
static void sayi(const char *s, i64 v)
{
    char b[96]; u64 n = 0; b[n++] = 't'; b[n++] = 'm'; b[n++] = ':'; b[n++] = ' '; for (u64 i = 0; s[i] && i < 60; i++) b[n++] = s[i];
    if (v < 0) { b[n++] = '-'; v = -v; }
    char d[24]; int k = 0; u64 u = (u64)v; if (!u) d[k++] = '0'; while (u) { d[k++] = (char)('0' + u % 10); u /= 10; }
    while (k) b[n++] = d[--k];
    b[n] = 0; say(b);
}
#define START 1
#define CANCEL 2
#define QUERY 3
#define RELEASE 4
#define NOW 5
#define E_ARG (-401)
#define E_FULL (-402)
#define E_NOENT (-403)
#define E_STATE (-405)
#define E_QUOTA (-406)
#define EFAULT_ (-2)
#define EINVAL_ (-1)
#define EPERM_ (-7)

struct q { u8 state; u8 z[7]; u64 rem; };
static i64 query(i64 id, u8 *state, u64 *rem)
{
    struct q q; q.state = 0xEE; q.rem = 0xEEEEEEEEEEEEEEEEULL;
    i64 rc = tm(QUERY, id, (i64)&q);
    if (rc == 0) { *state = q.state; *rem = q.rem; }
    return rc;
}
static int ok_(const char *what, int cond)             /* ONE SYS_WRITE per line: the kernel tags each write as a line */
{
    char b[200]; u64 n = 0; const char *p = cond ? "tm: PASS " : "tm: FAIL ";
    while (*p) b[n++] = *p++;
    while (*what && n < 190) b[n++] = *what++;
    b[n] = 0; say(b); return cond;
}

void __attribute__((section(".text._start"))) _start(u64 mode)
{
    u8 st = 0; u64 rem = 0;
    if (mode == 3) {                                                      /* no capability */
        sayi("nocap start=", tm(START, 1000, 0)); sayi("nocap now=", tm(NOW, 0, 0)); sayi("nocap query=", tm(QUERY, 0, (i64)&rem));
        ok_("nocap: every operation refused with EPERM", tm(START, 2000, 0) == EPERM_ && tm(NOW, 0, 0) == EPERM_ && tm(CANCEL, 0, 0) == EPERM_ && tm(RELEASE, 0, 0) == EPERM_);
        say("tm: done nocap"); quit(0);
    }
    if (mode == 1) {
        i64 t0 = tm(NOW, 0, 0);
        i64 id = tm(START, 2000, 0);
        sayi("basic start id=", id);
        ok_("basic: a 2 s timer is accepted", id >= 0);
        ok_("basic: it is RUNNING with at most 2000 ms left", query(id, &st, &rem) == 0 && st == 1 && rem <= 2000 && rem > 1000);
        i64 t1 = t0; int ev = 0; i64 evt = 0; u8 evb[16]; i64 evid = -1; i64 evst = -1;
        for (;;) {
            i64 rc = query(id, &st, &rem);
            if (rc != 0 || st != 1) break;
            i64 n = ui_poll(evb, sizeof evb);
            if (n >= 10 && evb[0] == 1 && evb[1] == 0x85 && !ev) { ev = 1; evt = tm(NOW, 0, 0); evid = evb[8]; evst = evb[9]; }   /* it may legitimately arrive between our last query and this poll, but never before the deadline */
            yield();
        }
        t1 = tm(NOW, 0, 0);
        sayi("basic elapsed_ms=", t1 - t0);
        ok_("basic: the timer was never early (>= 2000 ms of the monotonic counter)", t1 - t0 >= 2000);
        ok_("basic: it completed in a reasonable time (< 2600 ms)", t1 - t0 < 2600);
        ok_("basic: state is DONE with 0 remaining", query(id, &st, &rem) == 0 && st == 2 && rem == 0);
        ok_("basic: no completion event arrived before the deadline", !ev || evt - t0 >= 2000);
        /* the completion event: the owner may have to wait a tick or two */
        for (int i = 0; i < 20000 && evid < 0; i++) { i64 n = ui_poll(evb, sizeof evb); if (n >= 10 && evb[0] == 1 && evb[1] == 0x85) { evid = evb[8]; evst = evb[9]; } else yield(); }
        sayi("basic event id=", evid); sayi("basic event state=", evst);
        ok_("basic: exactly the owner got the completion event for its id (state DONE)", evid == id && evst == 2);
        { i64 n = ui_poll(evb, sizeof evb); ok_("basic: only ONE event per completed timer", !(n > 0 && evb[1] == 0x85)); }
        ok_("basic: cancelling a finished timer is refused (STATE)", tm(CANCEL, id, 0) == E_STATE);
        ok_("basic: release works once", tm(RELEASE, id, 0) == 0);
        ok_("basic: release twice -> NOENT", tm(RELEASE, id, 0) == E_NOENT);
        ok_("basic: query after release -> NOENT", query(id, &st, &rem) == E_NOENT);
        ok_("basic: cancel after release -> NOENT", tm(CANCEL, id, 0) == E_NOENT);
        /* quota and cancellation */
        i64 a = tm(START, 1500000, 0);                                    /* 25 minutes */
        i64 b = tm(START, 86400000, 0);                                   /* the maximum, 24 h */
        sayi("basic 25min id=", a); sayi("basic 24h id=", b);
        ok_("basic: a 25 minute and a 24 hour timer are both accepted", a >= 0 && b >= 0 && a != b);
        ok_("basic: the 25 minute timer shows ~25 minutes left", query(a, &st, &rem) == 0 && st == 1 && rem > 1499000 && rem <= 1500000);
        ok_("basic: a THIRD timer exceeds the per-owner quota", tm(START, 5000, 0) == E_QUOTA);
        ok_("basic: cancel works", tm(CANCEL, a, 0) == 0);
        ok_("basic: cancel twice -> STATE", tm(CANCEL, a, 0) == E_STATE);
        ok_("basic: a cancelled timer reads CANCELLED with 0 remaining", query(a, &st, &rem) == 0 && st == 3 && rem == 0);
        ok_("basic: a cancelled slot still counts until released (third start still QUOTA)", tm(START, 5000, 0) == E_QUOTA);
        ok_("basic: release the cancelled timer", tm(RELEASE, a, 0) == 0);
        ok_("basic: ... then a new timer fits again", tm(START, 5000, 0) >= 0);
        say("tm: done basic"); quit(0);
    }
    if (mode == 2) {
        ok_("hostile: 0 ms refused", tm(START, 0, 0) == E_ARG);
        ok_("hostile: 999 ms refused (not clamped to 1000)", tm(START, 999, 0) == E_ARG);
        ok_("hostile: 24 h + 1 ms refused (not clamped)", tm(START, 86400001, 0) == E_ARG);
        ok_("hostile: 2^64-1 refused", tm(START, -1, 0) == E_ARG);
        ok_("hostile: 2^63 refused", tm(START, (i64)0x8000000000000000ULL, 0) == E_ARG);
        ok_("hostile: invalid operation 99 -> EINVAL", tm(99, 0, 0) == EINVAL_);
        ok_("hostile: operation 0 -> EINVAL", tm(0, 0, 0) == EINVAL_);
        ok_("hostile: unknown id 0 -> NOENT (nothing started yet)", tm(CANCEL, 0, 0) == E_NOENT);
        ok_("hostile: id 4 (out of range) -> NOENT", tm(CANCEL, 4, 0) == E_NOENT && tm(RELEASE, 4, 0) == E_NOENT);
        ok_("hostile: id 2^32 -> NOENT (no truncation to a valid id)", tm(CANCEL, 0x100000000LL, 0) == E_NOENT);
        ok_("hostile: id -1 -> NOENT", tm(CANCEL, -1, 0) == E_NOENT);
        i64 id = tm(START, 5000, 0);
        ok_("hostile: a valid timer starts", id >= 0);
        ok_("hostile: query into an unmapped address -> EFAULT", tm(QUERY, id, 0x10) == EFAULT_);
        ok_("hostile: query into a kernel address -> EFAULT", tm(QUERY, id, (i64)0xFFFF000040080000ULL) == EFAULT_);
        ok_("hostile: query into the kernel's identity-mapped RAM -> EFAULT", tm(QUERY, id, 0x40080000) == EFAULT_);
        ok_("hostile: a 16-byte buffer that crosses the end of user memory -> EFAULT", tm(QUERY, id, (i64)0x00007FFFFFFFFFF8ULL) == EFAULT_);
        ok_("hostile: the failed queries changed nothing (still RUNNING)", query(id, &st, &rem) == 0 && st == 1);
        { i64 n1 = tm(NOW, 0, 0), n2 = tm(NOW, 0, 0); ok_("hostile: NOW is monotonic and non-negative", n1 >= 0 && n2 >= n1); }
        ok_("hostile: cancel it", tm(CANCEL, id, 0) == 0 && tm(RELEASE, id, 0) == 0);
        u64 bad = 0;
        for (u64 i = 0; i < 20000; i++) {
            i64 x = tm(START, 1000 + (i64)(i % 5000), 0);
            if (x < 0) { bad++; continue; }
            if (tm(CANCEL, x, 0) != 0) bad++;
            if (tm(RELEASE, x, 0) != 0) bad++;
        }
        sayi("hostile flood bad=", (i64)bad);
        ok_("hostile: 20,000 start/cancel/release cycles never leak a slot or fail", bad == 0);
        ok_("hostile: afterwards both quota slots are still available", tm(START, 2000, 0) >= 0 && tm(START, 2000, 0) >= 0 && tm(START, 2000, 0) == E_QUOTA);
        say("tm: done hostile"); quit(0);
    }
    if (mode == 4) {                                                      /* HOLDER */
        i64 t0 = tm(NOW, 0, 0);                                            /* BEFORE the start: reading the clock after it lets a 1 ms deschedule make a correct 3000 ms timer measure 2999 */
        i64 id = tm(START, 3000, 0);
        sayi("holder start id=", id);
        for (;;) {
            if (query(id, &st, &rem) != 0) { say("tm: FAIL holder lost its timer"); quit(1); }
            if (st == 2) break;
            if (st == 3) { say("tm: FAIL holder's timer was CANCELLED by somebody"); quit(1); }
            yield();
        }
        sayi("holder elapsed_ms=", tm(NOW, 0, 0) - t0);
        ok_("holder: its timer finished normally and was not touched by the spy", st == 2 && tm(NOW, 0, 0) - t0 >= 3000);
        say("tm: done holder"); quit(0);
    }
    if (mode == 5) {                                                      /* SPY */
        u64 touched = 0;
        for (int round = 0; round < 3; round++)
            for (i64 id = 0; id < 4; id++) {
                if (tm(CANCEL, id, 0) == 0) touched++;
                if (query(id, &st, &rem) == 0) touched++;
                if (tm(RELEASE, id, 0) == 0) touched++;
                yield();
            }
        sayi("spy touched=", (i64)touched);
        ok_("spy: no operation on any id succeeded for a timer it does not own", touched == 0);
        ok_("spy: a foreign id looks exactly like no timer (NOENT, not OWNER)", tm(CANCEL, 0, 0) == E_NOENT && tm(CANCEL, 1, 0) == E_NOENT);
        /* it may take its own two slots, but never more, whatever the others hold */
        i64 a = tm(START, 2000, 0), b = tm(START, 2000, 0), c = tm(START, 2000, 0);
        sayi("spy own a=", a); sayi("spy own b=", b); sayi("spy own c=", c);
        ok_("spy: never more than its own quota of 2 timers", c < 0);
        say("tm: done spy"); quit(0);
    }
    if (mode == 6) { i64 a = tm(START, 60000, 0), b = tm(START, 60000, 0); sayi("exiter a=", a); sayi("exiter b=", b); say("tm: done exiter"); quit(0); }
    if (mode == 7) { i64 a = tm(START, 60000, 0), b = tm(START, 60000, 0); sayi("faulter a=", a); sayi("faulter b=", b); say("tm: faulting"); *(volatile u64 *)0 = 1; quit(99); }
    if (mode == 8) {                                                      /* LATE: wait for the exiter and faulter to be reaped */
        { i64 w = tm(NOW, 0, 0); while (tm(NOW, 0, 0) - w < 6000) yield(); }
        i64 a = tm(START, 2000, 0), b = tm(START, 2000, 0);
        sayi("late a=", a); sayi("late b=", b);
        ok_("late: a new instance gets its full quota: the dead owners' timers were dropped", a >= 0 && b >= 0);
        ok_("late: ... and sees only its own", query(a, &st, &rem) == 0 && query(b, &st, &rem) == 0);
        say("tm: done late"); quit(0);
    }
    say("tm: unknown mode"); quit(2);
}
