/* tests/kernel-host/shell_hold_test.c - the hold-to-confirm DEMO screen against the REAL shell (hold.c is the pure machine; this proves the wiring): entry from Index, a keyboard
 * hold and a pointer hold, 1499 ms vs 1500 ms, early release, key-repeat, pointer leaving, focus change, Escape, a modal, a release processed before the poll, one
 * confirmation per hold, a second hold needing a new press, time going backwards, the four-segment display, and geometry at 75..200 percent in both palettes.
 * The demo confirms nothing real: it is compositor-owned and there is no client path to it. Host, ASan/UBSan. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shell.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)
#define W 1024u
#define H 768u
#define T0 10000u

static struct shell sh;
static void fresh(void) { shell_init(&sh, W, H); struct shell_caps c = { 1, 1 }; shell_set_caps(&sh, c); shell_tick(&sh, T0 - 1000); }
static void kd(uint16_t k, int repeat) { struct input_event e = { IN_KEY_DOWN, (uint8_t)repeat, k, 0, 0, 0, 0 }; shell_handle(&sh, &e); }
static void ku(uint16_t k) { struct input_event e = { IN_KEY_UP, 0, k, 0, 0, 0, 0 }; shell_handle(&sh, &e); }
static void ptr(int kind, int x, int y) { struct input_event e = { (uint8_t)kind, 0, BTN_LEFT, 0, 0, x, y }; shell_handle(&sh, &e); }
static void text(const char *s) { for (; *s; s++) { struct input_event e = { IN_TEXT, 0, 30, 0, (uint8_t)*s, 0, 0 }; shell_handle(&sh, &e); } }
static void at(uint64_t ms) { shell_set_now_ms(&sh, ms); }                  /* the time of the input about to be handled (as the kernel loop does) */
static void tick(uint64_t ms) { shell_tick(&sh, ms); }
static uint32_t focus_id(void) { return sh.fx.focus >= 0 ? sh.c[sh.fx.focus].id : SHELL_NONE; }
static const struct ui_widget *wid(uint32_t id) { int32_t i = shell_find(&sh, id); return i >= 0 ? &sh.w[i] : 0; }
static int has_text(const char *t) { for (uint32_t i = 0; i < sh.n; i++) if (sh.w[i].text && strstr(sh.w[i].text, t)) return 1; return 0; }
static void btn_center(int *x, int *y) { int32_t i = shell_find(&sh, SID_HOLD_BTN); *x = sh.c[i].x + sh.c[i].w / 2; *y = sh.c[i].y + sh.c[i].h / 2; }
static void open_hold(void) { fresh(); shell_go(&sh, SCR_HOLD); }
static void focus_btn(void) { for (int i = 0; i < 12 && focus_id() != SID_HOLD_BTN; i++) kd(KEY_TAB, 0); }
static uint32_t confirms(void) { return sh.hd.confirmations; }
static int filled(uint32_t k) { const struct ui_widget *w = wid(0xF400u + k); return w && w->fill == UI_COL_INK; }

static int audit(void)
{
    int bad = 0;
    for (uint32_t i = 0; i < sh.n; i++) {
        const struct ui_widget *w = &sh.w[i];
        if (w->c.x < 0 || w->c.y < 0 || w->c.x + w->c.w > (int32_t)W || w->c.y + w->c.h > (int32_t)H) bad++;
        if ((w->c.flags & UI_FOCUSABLE) && (w->c.h < 40 || w->c.w < 48)) bad++;
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
    printf("shell hold-to-confirm demo tests\n");

    /* ---- entry and layout ---- */
    fresh();
    shell_go(&sh, SCR_INDEX); text("hold"); kd(KEY_ENTER, 0);
    CHECK("typing 'hold' in Index and pressing Enter opens the demo screen", sh.screen == SCR_HOLD);
    CHECK("the screen is named Hold", !strcmp(shell_screen_name(SCR_HOLD), "Hold"));
    CHECK("it shows the hold button, the Home button, four segments and the ready line", wid(SID_HOLD_BTN) && wid(SID_HOLD_HOME) && wid(0xF400u) && wid(0xF403u) && !wid(0xF404u) && has_text("Hold for 1.5 seconds to confirm."));
    CHECK("it says plainly that it is a demonstration that enables nothing", has_text("Demonstration only"));
    CHECK("no segment is filled at the start", !filled(0) && !filled(1) && !filled(2) && !filled(3));
    CHECK("both are real controls and the hold has not started", (wid(SID_HOLD_BTN)->c.flags & UI_FOCUSABLE) && sh.hd.state == HD_IDLE && confirms() == 0);

    /* ---- keyboard: 1499 ms does not confirm, 1500 ms confirms exactly once ---- */
    open_hold(); focus_btn();
    CHECK("Tab reaches the hold button", focus_id() == SID_HOLD_BTN);
    at(T0); kd(KEY_ENTER, 0);
    CHECK("a non-repeat Enter key-down starts the hold: 'Holding: 0 of 4'", sh.hd.state == HD_HOLDING && has_text("Holding: 0 of 4"));
    tick(T0 + 374);  CHECK("at 374 ms still 0 of 4 and no segment filled", has_text("Holding: 0 of 4") && !filled(0));
    tick(T0 + 375);  CHECK("at 375 ms 1 of 4: the first segment is filled", has_text("Holding: 1 of 4") && filled(0) && !filled(1));
    tick(T0 + 750);  CHECK("at 750 ms 2 of 4", has_text("Holding: 2 of 4") && filled(1) && !filled(2));
    tick(T0 + 1125); CHECK("at 1125 ms 3 of 4", has_text("Holding: 3 of 4") && filled(2) && !filled(3));
    tick(T0 + 1499); CHECK("at 1499 ms the action has NOT confirmed", confirms() == 0 && sh.hd.state == HD_HOLDING && !has_text("Confirmed."));
    tick(T0 + 1500); CHECK("at 1500 ms it confirms exactly once", confirms() == 1 && has_text("Confirmed.") && filled(3));
    tick(T0 + 1501); tick(T0 + 5000);
    CHECK("further polls cannot confirm again", confirms() == 1);
    at(T0 + 6000); ku(KEY_ENTER);
    CHECK("the key release after a confirmation does not confirm again and the result stays on screen", confirms() == 1 && has_text("Confirmed."));
    at(T0 + 6100); kd(KEY_ENTER, 0); tick(T0 + 7599);
    CHECK("a second hold needs a new press; 1499 ms into it nothing new is confirmed", confirms() == 1);
    tick(T0 + 7600); CHECK("... and it confirms once more at its own 1500 ms", confirms() == 2);
    ku(KEY_ENTER);

    /* ---- early release cancels ---- */
    open_hold(); focus_btn();
    at(T0); kd(KEY_ENTER, 0); at(T0 + 800); ku(KEY_ENTER); tick(T0 + 801);
    CHECK("early release cancels with the honest message", sh.hd.state == HD_CANCELLED && has_text("Released early. Nothing happened."));
    tick(T0 + 3000);
    CHECK("a cancelled hold never confirms later", confirms() == 0 && !filled(0));

    /* ---- key repeat neither starts nor extends ---- */
    open_hold(); focus_btn();
    at(T0); kd(KEY_ENTER, 1); tick(T0 + 10);
    CHECK("an auto-repeat key-down never starts a hold", sh.hd.state == HD_IDLE);
    open_hold(); focus_btn();
    at(T0); kd(KEY_ENTER, 0);
    for (int i = 1; i <= 20; i++) { at(T0 + (uint64_t)i * 70); kd(KEY_ENTER, 1); }
    tick(T0 + 1499); CHECK("20 repeats during a hold do not extend it: 1499 ms still unconfirmed", confirms() == 0);
    tick(T0 + 1500); CHECK("... and it confirms once, 1500 ms after the ORIGINAL press", confirms() == 1);
    open_hold(); focus_btn();
    at(T0); kd(KEY_ENTER, 0); at(T0 + 100); kd(KEY_SPACE, 0); tick(T0 + 1500);
    CHECK("a second Enter/Space press during a hold is ignored: it neither restarts nor cancels", confirms() == 1);

    /* ---- a release processed before the poll wins (fail closed) ---- */
    open_hold(); focus_btn();
    at(T0); kd(KEY_ENTER, 0); at(T0 + 1500); ku(KEY_ENTER); tick(T0 + 1500);
    CHECK("a release handled before the poll that would complete the hold cancels it", confirms() == 0 && sh.hd.state == HD_CANCELLED);

    /* ---- any other key, Tab and Escape cancel ---- */
    open_hold(); focus_btn();
    at(T0); kd(KEY_ENTER, 0); at(T0 + 200); kd(30 /* A */, 0); tick(T0 + 1500);
    CHECK("any other key cancels the hold", confirms() == 0 && sh.hd.state == HD_CANCELLED);
    open_hold(); focus_btn();
    at(T0); kd(KEY_ENTER, 0); at(T0 + 200); kd(KEY_TAB, 0); tick(T0 + 1500);
    CHECK("Tab (a focus change) cancels the hold", confirms() == 0 && sh.hd.state != HD_HOLDING && focus_id() != SID_HOLD_BTN);
    open_hold(); focus_btn();
    at(T0); kd(KEY_ENTER, 0); at(T0 + 200); kd(KEY_ESC, 0); tick(T0 + 1500);
    CHECK("Escape cancels the hold and leaves the screen: nothing survives", confirms() == 0 && sh.screen != SCR_HOLD && sh.hd.state == HD_IDLE);
    shell_go(&sh, SCR_HOLD);
    CHECK("re-opening the demo starts fresh", sh.hd.state == HD_IDLE && has_text("Hold for 1.5 seconds to confirm."));

    /* ---- an unfocused button is not held by Enter ---- */
    open_hold(); at(T0);
    CHECK("with focus elsewhere, Enter does not start a hold", focus_id() != SID_HOLD_BTN);
    kd(KEY_ENTER, 0);
    CHECK("... and no hold is running", sh.hd.state != HD_HOLDING);

    /* ---- pointer ---- */
    { int x, y;
      open_hold(); btn_center(&x, &y);
      at(T0); ptr(IN_PTR_MOVE, x, y); ptr(IN_PTR_DOWN, x, y);
      CHECK("a pointer press inside starts the hold", sh.hd.state == HD_HOLDING);
      tick(T0 + 1499); CHECK("pointer: 1499 ms does not confirm", confirms() == 0);
      tick(T0 + 1500); CHECK("pointer: 1500 ms confirms once", confirms() == 1 && has_text("Confirmed."));
      at(T0 + 3000); ptr(IN_PTR_UP, x, y); tick(T0 + 3001);
      CHECK("pointer: the release after the confirmation does nothing more", confirms() == 1);

      open_hold(); btn_center(&x, &y);
      at(T0); ptr(IN_PTR_DOWN, x, y); at(T0 + 500); ptr(IN_PTR_UP, x, y); tick(T0 + 2000);
      CHECK("pointer: early release cancels", confirms() == 0 && has_text("Released early. Nothing happened."));

      open_hold(); btn_center(&x, &y);
      at(T0); ptr(IN_PTR_DOWN, x, y); at(T0 + 300); ptr(IN_PTR_MOVE, 5, 5);
      CHECK("pointer: leaving the button cancels", sh.hd.state == HD_CANCELLED);
      at(T0 + 400); ptr(IN_PTR_MOVE, x, y); tick(T0 + 2000);
      CHECK("pointer: moving back in does NOT resume the hold", confirms() == 0 && sh.hd.state == HD_CANCELLED);

      open_hold();
      at(T0); ptr(IN_PTR_DOWN, 5, 5); tick(T0 + 2000);
      CHECK("a press outside the button starts nothing", confirms() == 0 && sh.hd.state != HD_HOLDING);
    }

    /* ---- a modal interrupts safely ---- */
    open_hold(); focus_btn();
    at(T0); kd(KEY_ENTER, 0); sh.about_open = 1; tick(T0 + 1500);
    CHECK("an opened modal cancels the hold, even if it was due to confirm at that moment", confirms() == 0 && sh.hd.state == HD_CANCELLED);
    open_hold(); focus_btn();
    at(T0); kd(KEY_ENTER, 0); sh.review_open = 1; tick(T0 + 1000); sh.review_open = 0; tick(T0 + 2000);
    CHECK("the trusted review opening cancels it and closing the review does not resume it", confirms() == 0 && sh.hd.state == HD_CANCELLED);

    /* ---- time going backwards is no time ---- */
    open_hold(); focus_btn();
    at(T0); kd(KEY_ENTER, 0); tick(T0 + 1000); tick(T0 + 200); tick(T0 + 1499);
    CHECK("a clock that goes backwards never helps a hold confirm early", confirms() == 0);

    /* ---- Enter on the button runs no shell action ---- */
    open_hold(); focus_btn(); at(T0); kd(KEY_ENTER, 0); ku(KEY_ENTER);
    CHECK("pressing the hold button leaves the screen and every other state alone", sh.screen == SCR_HOLD);

    /* ---- Home returns Home ---- */
    open_hold(); { int32_t i = shell_find(&sh, SID_HOLD_HOME); int x = sh.c[i].x + sh.c[i].w / 2, y = sh.c[i].y + sh.c[i].h / 2; ptr(IN_PTR_DOWN, x, y); ptr(IN_PTR_UP, x, y); }
    CHECK("Home returns Home", sh.screen == SCR_HOME);

    /* ---- geometry at every scale and both palettes ---- */
    { int bad = 0, n = 0;
      for (int hc = 0; hc <= 1; hc++)
          for (uint32_t p = 75; p <= 200; p += 5) {
              open_hold(); shell_set_contrast(&sh, hc); shell_set_text_scale(&sh, p);
              if (sh.n > SHELL_MAX_WIDGETS || !wid(SID_HOLD_BTN) || !wid(SID_HOLD_HOME) || !wid(0xF403u)) bad++;
              bad += audit(); n++;
          }
      CHECK("the demo fits the screen with large enough, non-overlapping controls at 75..200 percent in both palettes", bad == 0 && n > 0); }

    printf("shell hold: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
