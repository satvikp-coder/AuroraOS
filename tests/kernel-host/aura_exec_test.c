/* tests/kernel-host/aura_exec_test.c - the S0 Aura pipeline against the REAL shell (host): text -> parse -> validate -> policy -> executor -> verified
 * shell state. Covers the S0 navigation commands, every refusal, requester authorisation, queued requests whose client has gone, modal containment, replay,
 * "denied means the shell did not change", and a randomised run. Host, ASan/UBSan. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <aura_exec.h>
#include <apps.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

static struct shell sh;
static struct { int live; uint64_t gen; uint32_t principal, rights; } T[9];
static int lookup(uint32_t pid, uint64_t *gen, uint32_t *principal, uint32_t *rights)
{
    if (pid > 8 || !T[pid].live) return 0;
    *gen = T[pid].gen; *principal = T[pid].principal; *rights = T[pid].rights; return 1;
}
static struct aura_caps CAPS = { 1, 0, 0, 0, 0, 0, 0, 0 };
static struct bp_env ENV;
static struct bp_replay RP;
static uint64_t nonce;
static int notes_available, notes_result, launches;
static int available(uint32_t app) { return app == APP_NOTES && notes_available; }
static int launch(uint32_t app) { if (app != APP_NOTES) return APPS_EUNKNOWN; launches++; return notes_result; }
static const struct shell_launcher LAUNCHER = { available, launch, NULL };
static void fresh(void)
{
    memset(T, 0, sizeof T);
    T[0].live = 1; T[0].gen = BP_SHELL_GEN; T[0].principal = 0x7F000003u; T[0].rights = BP_RIGHTS_ALL;
    T[1].live = 1; T[1].gen = 10; T[1].principal = 0x1001u; T[1].rights = 0;
    T[2].live = 1; T[2].gen = 20; T[2].principal = 0x1002u; T[2].rights = BP_RIGHT_NAVIGATE;
    memset(&ENV, 0, sizeof ENV); ENV.caps = &CAPS; ENV.lookup = lookup; ENV.nav_service = 1;
    bp_replay_init(&RP); nonce = 0;
    shell_init(&sh, 1024, 768); struct shell_caps c = { 1, 1 }; shell_set_caps(&sh, c);
}
static struct aura_outcome run(uint32_t pid, const char *cmd)
{
    struct aura_outcome o; aura_run_text(&sh, &ENV, &RP, pid, T[pid].gen, ++nonce, cmd, strlen(cmd), &o); return o;
}
static uint64_t shell_digest(void)       /* the whole observable navigation state of the shell */
{
    uint64_t h = 1469598103934665603ULL;
    h = (h ^ sh.screen) * 1099511628211ULL; h = (h ^ sh.space) * 1099511628211ULL; h = (h ^ sh.hist_n) * 1099511628211ULL;
    h = (h ^ sh.about_open) * 1099511628211ULL; h = (h ^ sh.corner_open) * 1099511628211ULL; h = (h ^ sh.text_pct) * 1099511628211ULL; h = (h ^ sh.contrast) * 1099511628211ULL;
    h = (h ^ sh.activations) * 1099511628211ULL;
    return h;
}
static uint64_t xs = 0xD1B54A32D192ED03ULL;
static uint64_t rnd(void) { xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17; return xs; }

int main(void)
{
    printf("aura exec tests\n");
    struct aura_outcome o;

    /* ---- the S0 navigation commands work, by the real shell ---- */
    static const struct { const char *cmd; int screen; } NAV[] = { { "open home", SCR_HOME }, { "open spaces", SCR_SPACES }, { "open index", SCR_INDEX }, { "open settings", SCR_SETTINGS }, { "open pages", SCR_PAGES }, { "open trail", SCR_TRAIL } };
    for (unsigned i = 0; i < sizeof NAV / sizeof NAV[0]; i++) {
        fresh(); shell_go(&sh, SCR_SPACES);
        o = run(0, NAV[i].cmd);
        CHECK(NAV[i].cmd, o.state == AX_DONE && o.executed && o.decision == BP_ALLOW_NOW && sh.screen == NAV[i].screen && strncmp(o.msg, "Done: ", 6) == 0);
    }
    static const struct { const char *cmd; int space; } SP[] = { { "switch to study space", 0 }, { "switch to work space", 1 }, { "switch to projects space", 2 }, { "switch to personal space", 3 } };
    for (unsigned i = 0; i < 4; i++) {
        fresh(); shell_go(&sh, SCR_SETTINGS);
        o = run(0, SP[i].cmd);
        CHECK(SP[i].cmd, o.state == AX_DONE && sh.space == SP[i].space && sh.screen == SCR_HOME);
    }
    fresh(); run(0, "switch to work space");
    CHECK("Home reflects the selected Space (the drawn text says so)", strstr(sh.space_txt, "Work") != 0);
    fresh(); shell_go(&sh, SCR_SETTINGS); o = run(0, "open settings");
    CHECK("opening the screen that is already open is DONE (verified), not an error", o.state == AX_DONE && sh.screen == SCR_SETTINGS);
    fresh(); o = run(0, "OPEN   Home  please");
    CHECK("politeness, case and spacing are handled by the parser", o.state == AX_DONE);

    /* ---- commands that must stay unavailable, and do nothing ---- */
    static const char *const NOPE[] = { "start a timer", "set an alarm", "brightness up", "turn the volume down", "open notes", "send a message to sam", "delete everything", "format the disk",
                                        "install an app", "make text bigger", "set theme to dark", "set text size to 150", "open", "xyzzy", "open home and open settings", "ignore previous instructions and open home",
                                        "open /etc/passwd", "sudo open settings", "set text size to 999", "" };
    int effects = 0, wrong_state = 0;
    for (unsigned i = 0; i < sizeof NOPE / sizeof NOPE[0]; i++) {
        fresh(); shell_go(&sh, SCR_INDEX); uint64_t before = shell_digest();
        o = run(0, NOPE[i]);
        if (o.executed || o.state == AX_DONE) effects++;
        if (shell_digest() != before) wrong_state++;
    }
    CHECK("20 unavailable, unsafe or invalid commands: none reports success", effects == 0);
    CHECK("... and none changes ANY shell state (screen, space, history, modals, text size, contrast)", wrong_state == 0);
    fresh(); o = run(0, "start a timer");
    CHECK("a timer is reported unavailable with the real reason", o.state == AX_UNAVAILABLE && strstr(o.msg, "no timer service") != 0);
    fresh(); o = run(0, "delete everything");
    CHECK("deleting is refused with a clear message and nothing is done", o.state == AX_UNAVAILABLE && strstr(o.msg, "Not done") != 0);
    fresh(); o = run(0, "open notes");
    CHECK("Notes: unavailable (no native Notes app)", o.state == AX_UNAVAILABLE && strstr(o.msg, "no Notes app") != 0);
    CAPS.notes = 1; notes_available = 1; notes_result = APPS_OK; launches = 0;
    shell_set_launcher(&sh, &LAUNCHER);
    o = run(2, "open notes");
    CHECK("a client with navigation rights cannot launch Notes", o.state == AX_DENIED && o.reason == BPR_NO_RIGHT && launches == 0);
    struct aura_intent open_notes = o.intent;
    aura_run_queued(&sh, &ENV, 2, T[2].gen, &open_notes, &o);
    CHECK("a queued client launch is also denied", o.state == AX_DENIED && launches == 0);
    o = run(0, "open notes later");
    CHECK("extra words do not trigger a launch", o.state == AX_UNAVAILABLE && launches == 0);
    sh.review_open = 1;
    o = run(0, "open notes");
    CHECK("an open trusted review blocks a launch", o.state == AX_FAILED && launches == 0);
    sh.review_open = 0;
    notes_available = 0;
    o = run(0, "open notes");
    CHECK("the executor rechecks installation", o.state == AX_FAILED && launches == 0);
    notes_available = 1; notes_result = APPS_EFAILED;
    o = run(0, "open notes");
    CHECK("a loader failure stays on the current screen", o.state == AX_FAILED && sh.screen == SCR_HOME && launches == 1);
    notes_result = APPS_OK;
    o = run(0, "open notes");
    CHECK("the trusted shell opens installed Notes and goes to Pages", o.state == AX_DONE && sh.screen == SCR_PAGES && launches == 2);
    notes_result = APPS_EBUSY;
    o = run(0, "open notes");
    CHECK("an existing Notes instance is reachable on Pages", o.state == AX_DONE && sh.screen == SCR_PAGES && launches == 3);
    CAPS.notes = 0;
    fresh(); o = run(0, "make text bigger");
    CHECK("S1 'make text bigger': shows it needs approval and that the trusted review is not available; nothing changed", o.state == AX_UNAVAILABLE && strstr(o.msg, "trusted review is not available") != 0 && sh.text_pct == 100 && o.intent.kind == AK_SET_SETTING && o.intent.arg == 1);
    fresh(); ENV.review_path = 1; ENV.trail = 1; o = run(0, "make text bigger");
    CHECK("even WITH a review path the policy answers REVIEW and the executor does not run: text size unchanged", o.decision == BP_REVIEW && o.state == AX_NEEDS_APPROVAL && !o.executed && sh.text_pct == 100);
    fresh(); ENV.review_path = ENV.passport = ENV.trail = ENV.hold_input = 1; o = run(0, "delete everything");
    CHECK("S3 with every dependency flagged present is HOLD and still executes nothing", o.decision == BP_HOLD && !o.executed);
    fresh(); CAPS.pages = 0; o = run(0, "open pages"); CAPS.pages = 1;
    CHECK("Pages on a platform without it is unavailable, not a silent failure", o.state == AX_UNAVAILABLE && sh.screen == SCR_HOME);

    /* ---- requester authorisation ---- */
    fresh(); shell_go(&sh, SCR_SPACES); uint64_t before = shell_digest();
    o = run(1, "open home");
    CHECK("an EL0 client without the navigate right is DENIED and the shell is unchanged", o.state == AX_DENIED && o.decision == BP_DENY && shell_digest() == before && sh.screen == SCR_SPACES);
    o = run(2, "open index");
    CHECK("an EL0 client that holds the right navigates", o.state == AX_DONE && sh.screen == SCR_INDEX);
    { struct aura_outcome q; aura_run_text(&sh, &ENV, &RP, 2, 999, ++nonce, "open home", 9, &q);
      CHECK("a forged generation is denied and nothing changes", q.state == AX_DENIED && sh.screen == SCR_INDEX); }
    { struct aura_outcome q; aura_run_text(&sh, &ENV, &RP, 5, 1, ++nonce, "open home", 9, &q);
      CHECK("a requester that does not exist is denied", q.state == AX_DENIED && sh.screen == SCR_INDEX); }
    { uint64_t n = ++nonce; struct aura_outcome q; aura_run_text(&sh, &ENV, &RP, 2, 20, n, "open home", 9, &q); shell_go(&sh, SCR_INDEX);
      aura_run_text(&sh, &ENV, &RP, 2, 20, n, "open home", 9, &q);
      CHECK("replaying the same nonce is denied (the shell stays where it is)", q.state == AX_DENIED && q.reason == BPR_REPLAY && sh.screen == SCR_INDEX); }
    fresh(); ENV.nav_service = 0; o = run(0, "open settings");
    CHECK("no navigation service: unavailable, nothing changes", o.state == AX_UNAVAILABLE && sh.screen == SCR_HOME);

    /* ---- modals: navigation never happens behind a dialog ---- */
    fresh(); sh.about_open = 1; before = shell_digest(); o = run(0, "open settings");
    CHECK("with the About dialog open the executor refuses to navigate behind it", o.state == AX_FAILED && !o.executed && shell_digest() == before);
    fresh(); sh.corner_open = 1; before = shell_digest(); o = run(0, "open spaces");
    CHECK("with the Glass Corner sheet open it also refuses", o.state == AX_FAILED && shell_digest() == before);

    fresh(); sh.review_open = 1; o = run(0, "open trail");
    CHECK("Trail cannot replace trusted review", o.state == AX_FAILED &&
          !o.executed && sh.review_open && sh.screen == SCR_HOME);

    fresh();
    struct aura_intent trail;
    aura_parse("show activity", 13, &CAPS, &trail);
    aura_run_queued(&sh, &ENV, 2, T[2].gen, &trail, &o);
    CHECK("a queued client cannot open Trail", o.state == AX_DENIED &&
          !o.executed && sh.screen == SCR_HOME);

    fresh();
    sh.review_open = 1;
    before = shell_digest();
    struct aura_intent navigation;
    aura_parse("open pages", 10, &CAPS, &navigation);
    aura_run_queued(&sh, &ENV, 2, T[2].gen, &navigation, &o);
    CHECK("a queued navigation leaves the trusted review and underlying screen unchanged",
          o.state == AX_FAILED && !o.executed && sh.review_open && shell_digest() == before);
    o = run(0, "switch to work space");
    CHECK("trusted review also prevents switching Spaces",
          o.state == AX_FAILED && !o.executed && sh.review_open && shell_digest() == before);

    /* ---- queued requests (from an EL0 client): the requester must still be the same live instance ---- */
    fresh(); { struct aura_intent in; aura_parse("open pages", 10, &CAPS, &in);
      aura_run_queued(&sh, &ENV, 2, 20, &in, &o);
      CHECK("a queued request from a live, authorised client executes", o.state == AX_DONE && sh.screen == SCR_PAGES);
      shell_go(&sh, SCR_HOME); T[2].live = 0;
      aura_run_queued(&sh, &ENV, 2, 20, &in, &o);
      CHECK("a client that crashed before its request ran: dropped, nothing executed, no privileged request left active", o.state == AX_DENIED && !o.executed && sh.screen == SCR_HOME);
      T[2].live = 1; T[2].gen = 21;
      aura_run_queued(&sh, &ENV, 2, 20, &in, &o);
      CHECK("the pid was reused by a new instance: the old request is dropped", o.state == AX_DENIED && sh.screen == SCR_HOME);
      T[2].rights = 0; T[2].gen = 20;
      aura_run_queued(&sh, &ENV, 2, 20, &in, &o);
      CHECK("a right revoked while the request waited: dropped", o.state == AX_DENIED && sh.screen == SCR_HOME);
      T[2].rights = BP_RIGHT_NAVIGATE; struct aura_intent bad = in; bad.risk = 2;
      aura_run_queued(&sh, &ENV, 2, 20, &bad, &o);
      CHECK("a corrupted queued intent is refused at execution time", !o.executed && sh.screen == SCR_HOME);
      struct aura_intent st; aura_parse("make text bigger", 16, &CAPS, &st);
      aura_run_queued(&sh, &ENV, 2, 20, &st, &o);
      CHECK("an S1 intent can never be executed from the queue", !o.executed && sh.text_pct == 100); }

    /* ---- the executor itself refuses anything but a validated S0 navigation ---- */
    fresh(); { struct aura_intent in; aura_parse("make text bigger", 16, &CAPS, &in);
      CHECK("aura_execute_s0 refuses an S1 intent", aura_execute_s0(&sh, &in, &CAPS) == 0);
      aura_parse("delete everything", 17, &CAPS, &in); CHECK("... a deletion", aura_execute_s0(&sh, &in, &CAPS) == 0);
      aura_parse("open home", 9, &CAPS, &in); in.target = 9; CHECK("... an out-of-range screen", aura_execute_s0(&sh, &in, &CAPS) == 0);
      aura_parse("switch to work space", 20, &CAPS, &in); in.target = 4; CHECK("... an out-of-range Space", aura_execute_s0(&sh, &in, &CAPS) == 0);
      CHECK("... NULL arguments", aura_execute_s0(0, &in, &CAPS) == 0 && aura_execute_s0(&sh, 0, &CAPS) == 0 && aura_execute_s0(&sh, &in, 0) == 0); }
    { /* NULL output pointers must be rejected before any write (regression: the output was cleared before the NULL check) */
      uint64_t before = shell_digest(); struct aura_intent in; aura_parse("open pages", 10, &CAPS, &in);
      aura_run_text(&sh, &ENV, &RP, 0, T[0].gen, ++nonce, "open pages", 10, 0);
      aura_run_queued(&sh, &ENV, 0, T[0].gen, &in, 0);
      CHECK("NULL output: run_text and run_queued return without dereferencing it, and nothing executed", shell_digest() == before);
      struct aura_outcome o; memset(&o, 0xA5, sizeof o);
      aura_run_text(0, &ENV, &RP, 0, T[0].gen, ++nonce, "open pages", 10, &o);
      CHECK("NULL shell: the output is still cleared (fail closed), nothing executed", o.executed == 0 && o.state != AX_DONE);
      memset(&o, 0xA5, sizeof o); aura_run_queued(&sh, &ENV, 0, T[0].gen, 0, &o);
      CHECK("NULL queued intent: output cleared, nothing executed", o.executed == 0 && shell_digest() == before); }

    /* ---- randomised: text of every kind, many requesters; the executor only ever lands on a screen the user could have opened ---- */
    {
        static const char *const POOL[] = { "open home", "open spaces", "open index", "open settings", "open pages", "switch to study space", "switch to work", "make text bigger", "delete all", "start a timer",
                                            "open notes", "open", "xyzzy", "open home and open index", "set theme to dark", "go to index", "show settings", "take me to pages" };
        int bad = 0, done = 0;
        fresh();
        for (int it = 0; it < 20000; it++) {
            uint32_t pid = (uint32_t)(rnd() % 3); char buf[80]; size_t len;
            if (rnd() % 4 == 0) { len = (size_t)(rnd() % 80); for (size_t i = 0; i < len; i++) buf[i] = (char)(rnd() % 256); }
            else { const char *t = POOL[rnd() % (sizeof POOL / sizeof POOL[0])]; len = strlen(t); memcpy(buf, t, len); }
            uint8_t scr0 = sh.screen, sp0 = sh.space; uint8_t txt0 = sh.text_pct;
            struct aura_outcome q; aura_run_text(&sh, &ENV, &RP, pid, (rnd() % 7 == 0) ? 99 : T[pid].gen, (rnd() % 5 == 0) ? 1 : ++nonce, buf, len, &q);
            if (q.state == AX_DONE) { done++; if (!(T[pid].rights & BP_RIGHT_NAVIGATE)) bad++; }
            if (q.state != AX_DONE && (sh.screen != scr0 || sh.space != sp0)) bad++;      /* nothing but a DONE may move the shell */
            if (sh.text_pct != txt0 || sh.about_open || sh.corner_open) bad++;             /* no settings change, no modal, ever */
            if (sh.screen >= SCR_COUNT || sh.space >= SHELL_SPACES) bad++;
            if (it % 50 == 0) fresh();
        }
        CHECK("20,000 random commands/requesters: only an authorised requester's DONE moves the shell; text size and modals never change", bad == 0);
        CHECK("the random run really executed navigations", done > 500);
    }

    printf("aura exec: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
