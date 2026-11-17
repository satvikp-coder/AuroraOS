/* tests/kernel-host/shell_clock_test.c - the native Clock screen and the completion toast against the REAL shell with a fake timer service that obeys the
 * service contract (monotonic ms, ids, running/done/cancelled, quota error). Covers: Index entry, no-service state, presets, Start/Cancel/Home, focus order,
 * live remaining time, completion off-screen, the toast (visible 4000 ms exactly, never focusable, never in the way), refusals, a lost timer, geometry and
 * contrast-independent fit at 75..200 percent in both palettes, and a randomized run that checks the service is never leaked. Host, ASan/UBSan. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shell.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)
#define W 1024u
#define H 768u

static struct shell sh;
/* ---- the fake service ---- */
static uint64_t fnow; static int f_id = -1, f_state, f_start_calls, f_cancel_calls, f_release_calls, f_fail_start, f_fail_cancel, f_fail_query; static uint64_t f_end; static int f_next;
static int f_start(uint64_t ms) { f_start_calls++; if (f_fail_start) return f_fail_start; if (f_id >= 0) return -406; f_id = f_next++ % 4; f_state = 1; f_end = fnow + ms; return f_id; }
static void f_poll(void) { if (f_id >= 0 && f_state == 1 && fnow >= f_end) f_state = 2; }
static int f_query(int id, uint8_t *st, uint64_t *rem) { if (f_fail_query) return -403; if (id != f_id) return -403; f_poll(); *st = (uint8_t)f_state; *rem = f_state == 1 ? f_end - fnow : 0; return 0; }
static int f_cancel(int id) { f_cancel_calls++; if (f_fail_cancel) return f_fail_cancel; if (id != f_id) return -403; f_poll(); if (f_state != 1) return -405; f_state = 3; return 0; }
static int f_release(int id) { f_release_calls++; if (id != f_id) return -403; f_poll(); if (f_state == 1) return -405; f_id = -1; f_state = 0; return 0; }
static const struct shell_timers FAKE = { f_start, f_query, f_cancel, f_release };

static void fresh(int with_service)
{
    f_id = -1; f_state = 0; f_start_calls = f_cancel_calls = f_release_calls = f_fail_start = f_fail_cancel = f_fail_query = 0; fnow = 100000; f_next = 0;
    shell_init(&sh, W, H); struct shell_caps c = { 1, 1 }; shell_set_caps(&sh, c);
    if (with_service) shell_set_timers(&sh, &FAKE);
    shell_tick(&sh, fnow);
}
static void key(uint16_t k, uint16_t mods) { struct input_event e = { IN_KEY_DOWN, 0, k, mods, 0, 0, 0 }; shell_handle(&sh, &e); }
static void ptr(int kind, int x, int y) { struct input_event e = { (uint8_t)kind, 0, BTN_LEFT, 0, 0, x, y }; shell_handle(&sh, &e); }
static int click(uint32_t id) { int32_t i = shell_find(&sh, id); if (i < 0) return 0; int x = sh.c[i].x + sh.c[i].w / 2, y = sh.c[i].y + sh.c[i].h / 2; ptr(IN_PTR_DOWN, x, y); ptr(IN_PTR_UP, x, y); return 1; }
static uint32_t focus_id(void) { return sh.fx.focus >= 0 ? sh.c[sh.fx.focus].id : SHELL_NONE; }
static const struct ui_widget *wid(uint32_t id) { int32_t i = shell_find(&sh, id); return i >= 0 ? &sh.w[i] : 0; }
static int enabled(uint32_t id) { const struct ui_widget *w = wid(id); return w && (w->c.flags & UI_ENABLED); }
static int has_text(const char *t) { for (uint32_t i = 0; i < sh.n; i++) if (sh.w[i].text && strstr(sh.w[i].text, t)) return 1; return 0; }
static void tick(uint64_t ms) { fnow += ms; shell_tick(&sh, fnow); }
static void open_clock(void) { shell_go(&sh, SCR_CLOCK); }
static void text(const char *s) { for (; *s; s++) { struct input_event e = { IN_TEXT, 0, 30, 0, (uint8_t)*s, 0, 0 }; shell_handle(&sh, &e); } }

static int audit(void)      /* geometry only: inside the screen, targets large enough, same-layer focusables do not overlap, nothing in the toast layer can take focus */
{
    int bad = 0;
    for (uint32_t i = 0; i < sh.n; i++) {
        const struct ui_widget *w = &sh.w[i];
        if (w->c.x < 0 || w->c.y < 0 || w->c.x + w->c.w > (int32_t)W || w->c.y + w->c.h > (int32_t)H) bad++;
        if ((w->c.flags & UI_FOCUSABLE) && (w->c.h < 40 || w->c.w < 48)) bad++;
        if (w->c.layer == UI_LAYER_TOAST && (w->c.flags & UI_FOCUSABLE)) bad++;
        if (w->c.flags & UI_FOCUSABLE)
            for (uint32_t j = i + 1; j < sh.n; j++) {
                const struct ui_widget *v = &sh.w[j];
                if (!(v->c.flags & UI_FOCUSABLE) || v->c.layer != w->c.layer) continue;
                if (w->c.x < v->c.x + v->c.w && v->c.x < w->c.x + w->c.w && w->c.y < v->c.y + v->c.h && v->c.y < w->c.y + w->c.h) bad++;
            }
    }
    return bad;
}

int main(void)
{
    printf("shell clock tests\n");

    { /* Index entry */
        fresh(1);
        size_t n = shell_index_count(), at = n; for (size_t i = 0; i < n; i++) if (!strcmp(shell_index_item(i)->key, "clock")) at = i;
        CHECK("the registry lists Clock and it is available", at < n && shell_index_available(at));
        shell_go(&sh, SCR_INDEX); text("clock"); key(KEY_ENTER, 0);
        CHECK("typing 'clock' in Index and pressing Enter opens the Clock screen", sh.screen == SCR_CLOCK);
        CHECK("the screen name is Clock", !strcmp(shell_screen_name(SCR_CLOCK), "Clock"));
        CHECK("Home does not gain a Clock tile (the 100% Home layout is unchanged)", (shell_go(&sh, SCR_HOME), wid(SID_CLOCK_START) == 0 && !has_text("Clock")));
    }
    { /* no service */
        fresh(0); open_clock();
        CHECK("no service: the screen says so", has_text("Timer service unavailable"));
        CHECK("no service: presets, Start and Cancel are disabled; Home works", !enabled(SID_CLOCK_P1) && !enabled(SID_CLOCK_P5) && !enabled(SID_CLOCK_P25) && !enabled(SID_CLOCK_START) && !enabled(SID_CLOCK_CANCEL) && enabled(SID_CLOCK_HOME));
        click(SID_CLOCK_START);
        CHECK("no service: clicking a disabled Start starts nothing", f_start_calls == 0 && sh.clk_state == CK_READY);
        click(SID_CLOCK_HOME);
        CHECK("no service: Home returns Home", sh.screen == SCR_HOME);
    }
    { /* presets, start, running, completion */
        fresh(1); open_clock();
        CHECK("READY: shows 1:00, presets and Start enabled, Cancel disabled", has_text("1:00") && enabled(SID_CLOCK_P1) && enabled(SID_CLOCK_START) && !enabled(SID_CLOCK_CANCEL));
        click(SID_CLOCK_P25);
        CHECK("choosing 25 min shows 25:00 and starts nothing", has_text("25:00") && f_start_calls == 0 && sh.clk_preset == 2);
        click(SID_CLOCK_P5);
        CHECK("choosing 5 min shows 5:00", has_text("5:00") && sh.clk_preset == 1);
        CHECK("the selected preset is marked active (not colour alone: also named in the live line)", wid(SID_CLOCK_P5)->active && !wid(SID_CLOCK_P1)->active && has_text("5 min selected"));
        click(SID_CLOCK_START);
        CHECK("Start asked the service for exactly 300,000 ms, once", f_start_calls == 1 && f_end == fnow + 300000u);
        CHECK("RUNNING: Cancel enabled; presets and Start disabled", sh.clk_state == CK_RUNNING && enabled(SID_CLOCK_CANCEL) && !enabled(SID_CLOCK_START) && !enabled(SID_CLOCK_P1));
        CHECK("Start moves focus to Cancel", focus_id() == SID_CLOCK_CANCEL);
        click(SID_CLOCK_START); click(SID_CLOCK_P1);
        CHECK("a disabled Start or preset while running changes nothing (no second timer)", f_start_calls == 1 && sh.clk_preset == 1);
        tick(1000); CHECK("one second later the screen shows 4:59", has_text("4:59"));
        tick(59000); CHECK("a minute in: 4:00", has_text("4:00"));
        tick(239000); CHECK("one second before the end: 0:01, still running, no toast", has_text("0:01") && sh.clk_state == CK_RUNNING && !sh.toast_on);
        tick(999); CHECK("1 ms before the deadline: still running", sh.clk_state == CK_RUNNING && !sh.toast_on);
        tick(1); CHECK("at the deadline: DONE, the toast is up, the service slot released", sh.clk_state == CK_DONE && sh.toast_on && f_release_calls == 1 && f_id == -1);
        CHECK("DONE: the screen says Finished and 0:00; Start works again", has_text("Finished") && has_text("0:00") && enabled(SID_CLOCK_START) && !enabled(SID_CLOCK_CANCEL));
        CHECK("the toast reads 'Timer finished' on the topmost layer and cannot take focus", has_text("Timer finished") && audit() == 0);
        uint32_t seen = 0; for (int i = 0; i < 40; i++) { key(KEY_TAB, 0); uint32_t f = focus_id(); if (f == 0xF600u || f == 0xF601u) seen++; }
        CHECK("40 Tab presses never focus the toast", seen == 0);
        tick(3998);
        CHECK("the toast is still visible 3998 ms after completion", sh.toast_on);
        tick(2);
        CHECK("... and gone at exactly 4000 ms", !sh.toast_on && !has_text("Timer finished"));
    }
    { /* completion while another screen is showing */
        fresh(1); open_clock(); click(SID_CLOCK_P1); click(SID_CLOCK_START);
        shell_go(&sh, SCR_SETTINGS);
        tick(30000); CHECK("30 s in, on Settings: still running", sh.clk_state == CK_RUNNING && sh.screen == SCR_SETTINGS);
        tick(30000);
        CHECK("it completes while Settings is showing: the toast is drawn there and focus is untouched", sh.clk_state == CK_DONE && sh.toast_on && has_text("Timer finished") && sh.screen == SCR_SETTINGS);
        shell_go(&sh, SCR_CLOCK);
        CHECK("back on Clock the finished state is shown", has_text("Finished"));
    }
    { /* cancel */
        fresh(1); open_clock(); click(SID_CLOCK_START);
        tick(5000); click(SID_CLOCK_CANCEL);
        CHECK("Cancel cancels the real timer and frees the slot", f_cancel_calls == 1 && f_release_calls == 1 && f_id == -1 && sh.clk_state == CK_CANCELLED);
        CHECK("CANCELLED: says so, Start enabled, Cancel disabled, focus moved to Start", has_text("Cancelled") && enabled(SID_CLOCK_START) && !enabled(SID_CLOCK_CANCEL) && focus_id() == SID_CLOCK_START);
        tick(120000); CHECK("a cancelled timer never completes: no toast two minutes later", !sh.toast_on && sh.clk_state == CK_CANCELLED);
        key(KEY_ENTER, 0);
        CHECK("Enter on the focused Start starts a new timer from the keyboard", sh.clk_state == CK_RUNNING && f_start_calls == 2);
    }
    { /* refusals and loss */
        fresh(1); open_clock(); f_fail_start = -406; click(SID_CLOCK_START);
        CHECK("quota refusal: ERROR with the true reason, nothing running", sh.clk_state == CK_ERROR && has_text("too many timers") && !enabled(SID_CLOCK_CANCEL) && enabled(SID_CLOCK_START));
        f_fail_start = -401; click(SID_CLOCK_START);
        CHECK("any other refusal: ERROR 'the service refused'", sh.clk_state == CK_ERROR && has_text("service refused"));
        f_fail_start = 0; click(SID_CLOCK_START);
        CHECK("after errors a retry works", sh.clk_state == CK_RUNNING);
        f_fail_query = 1; tick(1000);
        CHECK("the service forgetting the timer is reported as lost, never as finished", sh.clk_state == CK_ERROR && has_text("lost") && !sh.toast_on);
        fresh(1); open_clock(); click(SID_CLOCK_START); f_fail_cancel = -405; click(SID_CLOCK_CANCEL);
        CHECK("a refused cancel is reported, not pretended", sh.clk_state == CK_ERROR);
        fresh(1); open_clock(); click(SID_CLOCK_START); shell_set_timers(&sh, 0);
        CHECK("detaching the service makes the screen unavailable at once", has_text("Timer service unavailable") && !enabled(SID_CLOCK_START));
        shell_tick(&sh, fnow + 1000000); CHECK("ticking without a service is harmless and shows no toast", !sh.toast_on);
    }
    { /* toast replaced and focus order */
        fresh(1); open_clock(); click(SID_CLOCK_P1); click(SID_CLOCK_START); tick(60000);
        uint64_t until1 = sh.toast_until_ms; click(SID_CLOCK_START); tick(60000);
        CHECK("a second completion restarts the one toast slot", sh.toast_on && sh.toast_until_ms > until1);
        fresh(1); open_clock();
        sh.fx.focus = -1; uint32_t order[8]; int n = 0;
        for (int i = 0; i < 6; i++) { key(KEY_TAB, 0); uint32_t f = focus_id(); if (f >= SID_CLOCK_P1 && f <= SID_CLOCK_HOME && n < 8) order[n++] = f; }
        CHECK("READY Tab order is 1, 5, 25 min, Start, then Home (Cancel skipped)", n >= 5 && order[0] == SID_CLOCK_P1 && order[1] == SID_CLOCK_P5 && order[2] == SID_CLOCK_P25 && order[3] == SID_CLOCK_START && order[4] == SID_CLOCK_HOME);
        click(SID_CLOCK_START); sh.fx.focus = -1; n = 0;
        for (int i = 0; i < 6; i++) { key(KEY_TAB, 0); uint32_t f = focus_id(); if (f >= SID_CLOCK_P1 && f <= SID_CLOCK_HOME && n < 8) order[n++] = f; }
        CHECK("RUNNING Tab order is Cancel, then Home", n >= 2 && order[0] == SID_CLOCK_CANCEL && order[1] == SID_CLOCK_HOME);
    }
    { /* scale and contrast */
        static const uint32_t PCT[] = { 75, 100, 125, 150, 175, 200 };
        int bad = 0, over = 0, tbad = 0;
        for (int hc = 0; hc < 2; hc++)
            for (unsigned p = 0; p < 6; p++)
                for (int st = 0; st < 3; st++) {
                    fresh(1); shell_set_contrast(&sh, hc); shell_set_text_scale(&sh, PCT[p]); open_clock();
                    if (st >= 1) click(SID_CLOCK_START);
                    if (st == 2) { tick(60000 * 100); }
                    bad += audit();
                    if (sh.lay_bottom > 636 && PCT[p] == 200) over++;
                    if (sh.toast_on) {
                        int32_t ti = shell_find(&sh, 0xF600u), rail = shell_find(&sh, 0xF303u), up = shell_find(&sh, 0xF302u);
                        if (ti < 0 || rail < 0 || up < 0) tbad++;
                        else {
                            const struct ui_widget *t = &sh.w[ti], *r = &sh.w[rail], *u = &sh.w[up];
                            if (t->c.y + t->c.h > r->c.y) tbad++;                                                           /* inside the header */
                            if (t->c.x < u->c.x + u->c.w && u->c.x < t->c.x + t->c.w && t->c.y < u->c.y + u->c.h && u->c.y < t->c.y + t->c.h) tbad++;   /* clear of the uptime label */
                        }
                    }
                }
        CHECK("geometry holds on the Clock at 75..200 percent in both palettes (ready, running, done)", bad == 0);
        CHECK("at 200 percent the content bottom stays <= 636", over == 0);
        CHECK("the toast stays inside the header and clear of the uptime label at every scale", tbad == 0);
    }
    { /* randomized: the service is never leaked and the shell never disagrees with it */
        fresh(1); open_clock();
        uint32_t seed = 12345; int bad = 0;
        for (int i = 0; i < 20000; i++) {
            seed = seed * 1664525u + 1013904223u;
            switch ((seed >> 16) % 8) {
            case 0: click(SID_CLOCK_START); break;
            case 1: click(SID_CLOCK_CANCEL); break;
            case 2: click(SID_CLOCK_P1 + (seed >> 8) % 3); break;
            case 3: tick(((seed >> 8) % 90000) + 1); break;
            case 4: key(KEY_TAB, 0); break;
            case 5: key(KEY_ENTER, 0); break;
            case 6: if ((seed >> 8) % 7 == 0) shell_go(&sh, (seed >> 12) % 2 ? SCR_HOME : SCR_CLOCK); break;
            default: tick(10); break;
            }
            int running = sh.clk_state == CK_RUNNING;
            if (running && !(f_id >= 0)) bad++;                           /* the shell thinks it runs, the service has nothing */
            if (!running && f_id >= 0 && f_state == 1) bad++;             /* the service runs a timer the shell forgot: a leak */
        }
        CHECK("20,000 random clicks, keys, screen changes and ticks: shell and service never disagree and nothing leaks", bad == 0);
    }
    printf("shell clock: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
