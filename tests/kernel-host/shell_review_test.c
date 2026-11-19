/* tests/kernel-host/shell_review_test.c - the trusted-review MODAL against the real shell and the real review state machine: its exact content, default focus on
 * Cancel, Approve locked until armed, containment (nothing underneath receives input), Escape, the exact arming / approval / timeout behaviour on screen,
 * requester exit, single use, the secure colour used only by the modal, geometry at 75..200 percent in both palettes, and a randomized run that checks the
 * shell's own settings never change. Nothing here executes a change: S1 execution is disabled. Host, ASan/UBSan. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shell.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)
#define W 1024u
#define H 768u
#define SECURE 0x00FFB020u

static struct shell sh;
static const struct aura_caps CAPS = { 1, 1, 0, 0, 0 };
static uint64_t T;
static void fresh(void) { shell_init(&sh, W, H); struct shell_caps c = { 1, 1 }; shell_set_caps(&sh, c); T = 1000; shell_tick(&sh, T); }
static struct rv_request mk(const char *cmd, int32_t prior, int32_t next)
{
    struct rv_request q; memset(&q, 0, sizeof q);
    aura_parse(cmd, strlen(cmd), &CAPS, &q.intent);
    q.pid = 3; q.gen = 30; q.principal = 0x1003; q.nonce = 7; q.prior = prior; q.next = next;
    return q;
}
static int64_t open_rv(const char *cmd, int32_t pr, int32_t nx) { struct rv_request q = mk(cmd, pr, nx); return shell_review_open(&sh, T, &q, &CAPS, "Aura (test)"); }
static void key(uint16_t k, uint16_t mods) { struct input_event e = { IN_KEY_DOWN, 0, k, mods, 0, 0, 0 }; shell_handle(&sh, &e); }
static void text(const char *s) { for (; *s; s++) { struct input_event e = { IN_TEXT, 0, 30, 0, (uint8_t)*s, 0, 0 }; shell_handle(&sh, &e); } }
static void ptr(int kind, int x, int y) { struct input_event e = { (uint8_t)kind, 0, BTN_LEFT, 0, 0, x, y }; shell_handle(&sh, &e); }
static int click(uint32_t id) { int32_t i = shell_find(&sh, id); if (i < 0) return 0; int x = sh.c[i].x + sh.c[i].w / 2, y = sh.c[i].y + sh.c[i].h / 2; ptr(IN_PTR_DOWN, x, y); ptr(IN_PTR_UP, x, y); return 1; }
static void tick(uint64_t ms) { T += ms; shell_tick(&sh, T); }
static int attention(void) { return shell_review_attention(&sh, T, 0); }
static const struct ui_widget *wid(uint32_t id) { int32_t i = shell_find(&sh, id); return i >= 0 ? &sh.w[i] : 0; }
static int enabled(uint32_t id) { const struct ui_widget *w = wid(id); return w && (w->c.flags & UI_ENABLED); }
static int has_text(const char *t) { for (uint32_t i = 0; i < sh.n; i++) if (sh.w[i].text && strstr(sh.w[i].text, t)) return 1; return 0; }
static uint32_t focus_id(void) { return sh.fx.focus >= 0 ? sh.c[sh.fx.focus].id : SHELL_NONE; }
static int secure_pixels(void) { int n = 0; for (uint32_t i = 0; i < sh.n; i++) if (sh.w[i].fill == SECURE) n++; return n; }
static double lum(uint32_t c) { double v[3] = { (double)((c >> 16) & 255) / 255, (double)((c >> 8) & 255) / 255, (double)(c & 255) / 255 }; for (int i = 0; i < 3; i++) v[i] = v[i] <= 0.03928 ? v[i] / 12.92 : __builtin_pow((v[i] + 0.055) / 1.055, 2.4); return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2]; }
static double ratio(uint32_t a, uint32_t b) { double x = lum(a), y = lum(b); if (x < y) { double t = x; x = y; y = t; } return (x + 0.05) / (y + 0.05); }
static int audit(void)
{
    int bad = 0;
    for (uint32_t i = 0; i < sh.n; i++) {
        const struct ui_widget *w = &sh.w[i];
        if (w->c.x < 0 || w->c.y < 0 || w->c.x + w->c.w > (int32_t)W || w->c.y + w->c.h > (int32_t)H) bad++;
        if ((w->c.flags & UI_FOCUSABLE) && (w->c.h < 40 || w->c.w < 48)) bad++;
        if ((w->c.flags & UI_FOCUSABLE) && w->c.layer == UI_LAYER_MODAL)
            for (uint32_t j = i + 1; j < sh.n; j++) { const struct ui_widget *v = &sh.w[j]; if (!(v->c.flags & UI_FOCUSABLE) || v->c.layer != w->c.layer) continue;
                if (w->c.x < v->c.x + v->c.w && v->c.x < w->c.x + w->c.w && w->c.y < v->c.y + v->c.h && v->c.y < w->c.y + w->c.h) bad++; }
    }
    return bad;
}

int main(void)
{
    printf("shell review tests\n");

    { /* opening */
        fresh();
        CHECK("no review is open and the secure colour is not drawn anywhere before one opens", !sh.review_open && secure_pixels() == 0);
        shell_go(&sh, SCR_SPACES);
        int64_t t = open_rv("make text bigger", 100, 125);
        CHECK("open returns ticket 1 and shows the modal", t == 1 && sh.review_open && sh.rv.state == RV_OPEN);
        CHECK("a second open is BUSY", open_rv("set theme to dark", 0, 2) == RV_E_BUSY);
        CHECK("the dialog says AuroraOS secure review, who asked (from the kernel), the canonical action, the change and the risk",
              has_text("AuroraOS secure review") && has_text("Requested by: Aura (test)") && has_text("Make text larger") && has_text("Text size: 100% -> 125%") && has_text("Reversible. Recorded in Trail."));
        CHECK("the live line tells how to enable Approve", has_text("Press Ctrl+Alt+Delete to enable Approve."));
        CHECK("the secure colour appears (the strip and its label) only while the review is open", secure_pixels() >= 1 && secure_pixels() <= 2);
        CHECK("DEFAULT FOCUS IS CANCEL", focus_id() == SID_REVIEW_CANCEL);
        CHECK("Approve exists but is locked and says so", wid(SID_REVIEW_APPROVE) && !enabled(SID_REVIEW_APPROVE) && has_text("Approve (locked)"));
        fresh(); struct rv_request q = mk("make text bigger", 100, 125); shell_set_caps(&sh, (struct shell_caps){ 0, 1 });
        CHECK("with no keyboard a review cannot open (S1 stays unavailable)", shell_review_open(&sh, T, &q, &CAPS, "x") == RV_E_STATE && !sh.review_open);
        fresh(); sh.about_open = 1;
        CHECK("it refuses to open over another modal", shell_review_open(&sh, T, &q, &CAPS, "x") == RV_E_BUSY);
        fresh(); struct rv_request nav = mk("open home", 0, 0);
        CHECK("a non-S1 intent cannot be shown for review", shell_review_open(&sh, T, &nav, &CAPS, "x") == RV_E_INTENT && !sh.review_open);
        fresh(); struct rv_request th = mk("set theme to dark", 0, 2); shell_review_open(&sh, T, &th, &CAPS, "Aura");
        CHECK("a theme change states that nothing is drawn differently yet", has_text("Theme: Auto -> Dark") && has_text("Stored only: no dark theme."));
        fresh(); char longname[100]; memset(longname, 'A', 99); longname[99] = 0; longname[5] = 7;                          /* long, with a control character */
        { struct rv_request q2 = mk("make text bigger", 100, 125); shell_review_open(&sh, T, &q2, &CAPS, longname); }
        CHECK("an over-long or non-printable requester name is bounded and sanitised (the control character became a question mark)", strlen(sh.rv_name) < sizeof sh.rv_name && sh.rv_name[5] == 63 && sh.rv_name[4] == 65 && audit() == 0);
    }
    { /* containment: nothing underneath receives input */
        fresh(); shell_go(&sh, SCR_INDEX); open_rv("make text bigger", 100, 125);
        uint32_t acts = sh.activations; uint8_t qlen = sh.qlen; uint8_t scr = sh.screen;
        text("settings"); key(KEY_ENTER, 0); key(KEY_DOWN, 0);
        CHECK("typing and Enter on the Index screen behind the modal change nothing", sh.qlen == qlen && sh.screen == scr && sh.review_open == 0 ? 1 : (sh.qlen == qlen && sh.screen == scr));
        fresh(); shell_go(&sh, SCR_INDEX); open_rv("make text bigger", 100, 125); acts = sh.activations;
        ptr(IN_PTR_DOWN, 500, 700); ptr(IN_PTR_UP, 500, 700); ptr(IN_PTR_DOWN, 20, 20); ptr(IN_PTR_UP, 20, 20); ptr(IN_PTR_DOWN, 900, 300); ptr(IN_PTR_UP, 900, 300);
        CHECK("clicks on the dock, the header and the scrim activate nothing (a click on the scrim does not dismiss it)", sh.activations == acts && sh.review_open && sh.rv.state == RV_OPEN && sh.screen == SCR_INDEX);
        int bad = 0; for (int i = 0; i < 60; i++) { key(KEY_TAB, 0); uint32_t f = focus_id(); if (f != SID_REVIEW_CANCEL && f != SID_REVIEW_APPROVE) bad++; }
        CHECK("60 Tab presses never leave the modal (Cancel only, while Approve is locked)", bad == 0 && sh.review_open);
        CHECK("the modal screen was never changed behind it", sh.screen == SCR_INDEX && sh.text_pct == 100);
    }
    { /* Cancel, Enter on default focus, Escape */
        fresh(); int64_t t = open_rv("make text bigger", 100, 125);
        key(KEY_ENTER, 0);
        CHECK("Enter on the initial focus (Cancel) DECLINES and closes: it never approves", !sh.review_open && sh.rv.state == RV_DECLINED && !shell_review_take_approved(&sh, &(uint64_t){ 0 }));
        CHECK("... the ticket is retired: an approval with it is refused", rv_approve(&sh.rv, T, (uint64_t)t) != RV_OK);
        fresh(); t = open_rv("make text bigger", 100, 125); key(KEY_ESC, 0);
        CHECK("Escape declines and closes", !sh.review_open && sh.rv.state == RV_DECLINED);
        fresh(); open_rv("make text bigger", 100, 125); tick(700); attention(); key(KEY_ESC, 0);
        CHECK("Escape declines even while armed", !sh.review_open && sh.rv.state == RV_DECLINED && !sh.rv_approved_pending);
        fresh(); open_rv("make text bigger", 100, 125); click(SID_REVIEW_CANCEL);
        CHECK("clicking Cancel declines", !sh.review_open && sh.rv.state == RV_DECLINED);
        fresh(); open_rv("make text bigger", 100, 125); click(SID_REVIEW_APPROVE);
        CHECK("clicking the LOCKED Approve does nothing", sh.review_open && sh.rv.state == RV_OPEN && !sh.rv_approved_pending);
        fresh(); open_rv("make text bigger", 100, 125); sh.fx.focus = -1; key(KEY_ENTER, 0); key(KEY_SPACE, 0);
        CHECK("Enter/Space with nothing focused do not approve", sh.rv.state != RV_APPROVED && !sh.rv_approved_pending);
    }
    { /* arming and approval, with the exact timings on the screen */
        fresh(); int64_t t = open_rv("make text bigger", 100, 125);
        tick(599); CHECK("599 ms after opening the gesture does nothing", attention() == RV_E_EARLY && !enabled(SID_REVIEW_APPROVE) && has_text("Press Ctrl+Alt+Delete"));
        tick(1); CHECK("600 ms: the gesture arms", attention() == RV_OK && sh.rv.state == RV_ARMED);
        CHECK("armed: Approve is enabled, says so, and the live line says 10 seconds", enabled(SID_REVIEW_APPROVE) && has_text("Approve is enabled for 10 seconds.") && !has_text("Approve (locked)"));
        CHECK("ARMING DID NOT MOVE FOCUS: it is still on Cancel", focus_id() == SID_REVIEW_CANCEL);
        key(KEY_TAB, 0);
        CHECK("Tab reaches Approve", focus_id() == SID_REVIEW_APPROVE);
        tick(9998); CHECK("1 ms before the window ends the review is still armed", sh.rv.state == RV_ARMED);
        key(KEY_ENTER, 0);
        CHECK("Enter on Approve approves: the modal closes, the approval is pending, and NOTHING changed", !sh.review_open && sh.rv.state == RV_APPROVED && sh.rv_approved_pending && sh.text_pct == 100);
        uint64_t tk = 0;
        CHECK("the executor can take the approval once", shell_review_take_approved(&sh, &tk) == 1 && tk == (uint64_t)t && shell_review_take_approved(&sh, &tk) == 0);
        struct rv_request same = mk("make text bigger", 100, 125), other = mk("make text bigger", 100, 150);
        CHECK("verify with a CHANGED value is refused and kills the approval", shell_review_verify(&sh, T, tk, &other, &CAPS) == RV_E_CHANGED && shell_review_verify(&sh, T, tk, &same, &CAPS) == RV_E_STATE);
        fresh(); t = open_rv("make text bigger", 100, 125); tick(700); attention(); click(SID_REVIEW_APPROVE); tk = 0; shell_review_take_approved(&sh, &tk);
        CHECK("a pointer click on the enabled Approve also approves", sh.rv.state == RV_APPROVED && tk == (uint64_t)t);
        CHECK("verify with the unchanged request succeeds exactly once", shell_review_verify(&sh, T, tk, &same, &CAPS) == RV_OK && shell_review_verify(&sh, T, tk, &same, &CAPS) == RV_E_STATE);
        CHECK("the next review has a NEW ticket", open_rv("make text bigger", 100, 125) == t + 1);
        fresh(); open_rv("make text bigger", 100, 125); tick(700); attention(); tick(10000);
        CHECK("10 s after arming the window lapsed: Approve locked again and the line says to press the gesture again", sh.rv.state == RV_OPEN && !enabled(SID_REVIEW_APPROVE) && has_text("Approve timed out. Press Ctrl+Alt+Delete again."));
        CHECK("... the gesture re-arms", attention() == RV_OK && enabled(SID_REVIEW_APPROVE) && !has_text("timed out"));
        fresh(); open_rv("make text bigger", 100, 125); tick(59999); CHECK("one ms before 60 s the review is live", sh.review_open && sh.rv.state == RV_OPEN); tick(1);
        CHECK("at 60 s: expired, the modal stays to say so, Cancel becomes Close", sh.rv.state == RV_EXPIRED && sh.review_open && has_text("Review expired. Nothing changed.") && has_text("Close") && !enabled(SID_REVIEW_APPROVE));
        key(KEY_ENTER, 0);
        CHECK("Close closes it, nothing was approved", !sh.review_open && !sh.rv_approved_pending);
        fresh(); open_rv("make text bigger", 100, 125); tick(700); attention();
        CHECK("another process exiting does not touch the review", shell_review_requester_gone(&sh, 4, 30) == 0 && sh.rv.state == RV_ARMED);
        CHECK("the requester exiting kills it: the modal says so, Approve is locked, Close closes", shell_review_requester_gone(&sh, 3, 30) == 1 && has_text("Requester exited. Nothing changed.") && !enabled(SID_REVIEW_APPROVE) && has_text("Close"));
        key(KEY_ENTER, 0); CHECK("... and nothing was approved", !sh.review_open && !sh.rv_approved_pending);
    }
    { /* geometry at every scale and palette; the secure strip stays readable */
        static const uint32_t PCT[] = { 75, 100, 125, 150, 175, 200 };
        int bad = 0, shown = 0;
        for (int hc = 0; hc < 2; hc++) for (unsigned p = 0; p < 6; p++) for (int st = 0; st < 3; st++) {
            fresh(); shell_set_contrast(&sh, hc); shell_set_text_scale(&sh, PCT[p]); open_rv("set theme to dark", 0, 2);
            if (st >= 1) { tick(700); attention(); }
            if (st == 2) { tick(60000); }
            bad += audit(); shown++;
            int32_t d = shell_find(&sh, 0xF201u); if (d < 0 || sh.w[d].c.y + sh.w[d].c.h > (int32_t)H) bad++;                          /* the dialog fits the screen */
            int32_t sx = shell_find(&sh, 0xF211u); if (sx >= 0) { double r = ratio(sh.w[sx].ink, sh.w[sx].fill); if (r < (hc ? 7.0 : 4.5)) bad++; }   /* the fixed string is readable on its strip */
        }
        CHECK("the modal fits and stays readable at 75..200 percent in both palettes and every state", bad == 0 && shown == 36);
        fresh(); open_rv("make text bigger", 100, 125);
        CHECK("high contrast: the strip is black with white text (colour is not what proves authenticity)", ({ shell_set_contrast(&sh, 1); int32_t s = shell_find(&sh, 0xF210u); s >= 0 && sh.w[s].fill == 0 && sh.w[shell_find(&sh, 0xF211u)].ink == 0x00FFFFFFu; }));
    }
    { /* randomized: whatever happens, the shell's own settings are never touched and approvals only follow a real arming */
        uint32_t s = 4711; int bad = 0; fresh(); uint64_t lastt = 0;
        for (int it = 0; it < 20000 && !bad; it++) {
            s = s * 1664525u + 1013904223u;
            int was_open = sh.review_open; uint8_t tp = sh.text_pct, ct = sh.contrast, sp = sh.space, sc = sh.screen;
            switch ((s >> 12) % 9) {
            case 0: if (!sh.review_open) { if (open_rv("make text bigger", 100, 125) > 0) lastt = sh.rv.ticket; } break;
            case 1: attention(); break;
            case 2: click(SID_REVIEW_APPROVE); break;
            case 3: click(SID_REVIEW_CANCEL); break;
            case 4: tick(((s >> 20) % 3000) + 1); break;
            case 5: key(KEY_TAB, 0); break;
            case 6: key((s >> 24) & 1 ? KEY_ENTER : KEY_SPACE, 0); break;
            case 7: if ((s >> 25) % 11 == 0) key(KEY_ESC, 0); break;
            default: shell_review_requester_gone(&sh, (s >> 24) % 5, 30); break;
            }
            if (was_open && (sh.text_pct != tp || sh.contrast != ct || sh.space != sp || sh.screen != sc)) bad = 1;   /* while a review is open nothing underneath it, and no setting, may change */
            uint64_t tk; if (shell_review_take_approved(&sh, &tk)) { if (tk != lastt && tk != sh.rv.last_ticket) bad = 2; }
            if (sh.review_open) { uint32_t f = focus_id(); if (f != SID_REVIEW_CANCEL && f != SID_REVIEW_APPROVE && f != SHELL_NONE) bad = 3; }
            if (sh.review_open && sh.rv.state != RV_OPEN && sh.rv.state != RV_ARMED && sh.rv.state != RV_EXPIRED && sh.rv.state != RV_GONE) bad = 4;   /* a modal never lingers in a state that has no meaning */
        }
        if (bad) printf("    random run stopped: bad=%d\n", bad);
        CHECK("20,000 random gestures, clicks, keys, ticks and requester exits: no setting ever changes, focus never leaves the modal, the modal only shows meaningful states", bad == 0);
    }
    printf("shell review: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
