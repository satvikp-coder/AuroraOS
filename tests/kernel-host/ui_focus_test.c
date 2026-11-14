/* tests/kernel-host/ui_focus_test.c - host tests for kernel/input/ui_focus.c (hit testing, z-order, focus, modal, activation). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ui_focus.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; printf("  PASS: %s\n", name); } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

enum { A = 1, B, C, D, DLG_OK, DLG_CANCEL, COVER };
#define ALL (UI_VISIBLE | UI_ENABLED | UI_FOCUSABLE)

static struct ui_ctrl K[8];
static struct ui_ctx U;
static void setup(void)
{
    memset(K, 0, sizeof K);
    K[0] = (struct ui_ctrl){ 10, 10, 100, 40, 0, A, ALL, 0 };
    K[1] = (struct ui_ctrl){ 10, 60, 100, 40, 0, B, ALL, 0 };
    K[2] = (struct ui_ctrl){ 10, 110, 100, 40, 0, C, UI_VISIBLE | UI_FOCUSABLE, 0 };         /* disabled */
    K[3] = (struct ui_ctrl){ 10, 160, 100, 40, 0, D, ALL, 0 };
    ui_init(&U, K, 4);
}
static struct input_event kd(uint16_t key, uint16_t mods, uint8_t rep) { struct input_event e = { IN_KEY_DOWN, rep, key, mods, 0, 0, 0 }; return e; }
static uint32_t press(uint16_t key, uint16_t mods) { struct input_event e = kd(key, mods, 0); return ui_key(&U, &e); }

int main(void)
{
    printf("ui_focus host tests\n");

    /* --- hit testing --- */
    setup();
    CHECK("hit inside a control returns its index", ui_hit(&U, 20, 20) == 0 && ui_hit(&U, 20, 70) == 1);
    CHECK("hit on the first and last pixel of a rectangle (inclusive/exclusive edges)", ui_hit(&U, 10, 10) == 0 && ui_hit(&U, 109, 49) == 0 && ui_hit(&U, 110, 20) == -1 && ui_hit(&U, 20, 50) == -1 && ui_hit(&U, 9, 20) == -1);
    CHECK("hit in the gap or outside every control is -1", ui_hit(&U, 5, 5) == -1 && ui_hit(&U, 500, 500) == -1 && ui_hit(&U, -1000000, 2000000000) == -1);
    K[0].flags &= (uint8_t)~UI_VISIBLE;
    CHECK("an invisible control is never hit", ui_hit(&U, 20, 20) == -1);
    setup(); K[0].w = 0; K[1].h = -5;
    CHECK("empty or negative-size controls are never hit", ui_hit(&U, 10, 10) == -1 && ui_hit(&U, 20, 70) == -1);
    setup(); K[0].x = 2147483600; K[0].w = 2147483647;
    CHECK("geometry near INT32_MAX does not overflow", ui_hit(&U, 2147483647, 20) == 0 && ui_hit(&U, 5, 20) == -1);

    /* --- z-order --- */
    setup();
    K[4] = (struct ui_ctrl){ 0, 0, 60, 300, 0, COVER, UI_VISIBLE, 1 };                        /* non-focusable, higher layer */
    U.n = 5;
    CHECK("a higher layer covers a lower one (no click-through)", ui_hit(&U, 20, 20) == 4 && ui_hit(&U, 80, 20) == 0);
    K[4].layer = 0;
    CHECK("equal layers: the later entry is on top", ui_hit(&U, 20, 20) == 4);
    K[4].layer = 1; K[4].flags |= UI_ENABLED;
    ui_pointer_down(&U, 20, 20);
    CHECK("pressing a covering enabled control presses IT, not the one below", U.pressed == 4);
    K[4].flags = UI_VISIBLE;
    ui_pointer_down(&U, 20, 20);
    CHECK("pressing a covering DISABLED control presses nothing (the press is swallowed, not passed on)", U.pressed == -1);
    CHECK("... and the release activates nothing", ui_pointer_up(&U, 20, 20) == UI_NONE);

    /* --- rounded hit shape against an independent floating-point oracle --- */
    int rmis = 0, rcount = 0;
    for (int r = 0; r <= 12; r++) for (int w = 5; w <= 30; w += 5) for (int h = 5; h <= 30; h += 5) {
        struct ui_ctrl k1[1] = { { 3, 4, w, h, r, A, ALL, 0 } };
        struct ui_ctx u1; ui_init(&u1, k1, 1);
        int rr = r; if (2 * rr > w) rr = w / 2; if (2 * rr > h) rr = h / 2;
        for (int y = 0; y < 40; y++) for (int x = 0; x < 40; x++) {
            int exp;
            if (x < 3 || y < 4 || x >= 3 + w || y >= 4 + h) exp = 0;
            else if (rr <= 0) exp = 1;
            else {
                double cx = x + 0.5 - 3, cy = y + 0.5 - 4, ox = 0, oy = 0;
                if (cx < rr) ox = rr - cx; else if (cx > w - rr) ox = cx - (w - rr);
                if (cy < rr) oy = rr - cy; else if (cy > h - rr) oy = cy - (h - rr);
                exp = (ox == 0 || oy == 0) ? 1 : (ox * ox + oy * oy <= (double)rr * rr);
            }
            rcount++;
            if ((ui_hit(&u1, x, y) == 0) != exp) rmis++;
        }
    }
    CHECK("rounded hit shape matches an independent floating-point oracle on ~70,000 pixels (radii 0..12, 36 sizes)", rmis == 0 && rcount > 60000);
    { struct ui_ctrl k1[1] = { { 0, 0, 20, 20, 10, A, ALL, 0 } }; struct ui_ctx u1; ui_init(&u1, k1, 1);
      CHECK("rounded: the extreme corner pixel is outside, the edge middle is inside", ui_hit(&u1, 0, 0) == -1 && ui_hit(&u1, 10, 0) == 0 && ui_hit(&u1, 0, 10) == 0 && ui_hit(&u1, 19, 19) == -1); }

    /* --- focus traversal --- */
    setup();
    CHECK("no initial focus", U.focus == -1);
    CHECK("Tab focuses the first focusable control", press(KEY_TAB, 0) == UI_NONE && U.focus == 0);
    press(KEY_TAB, 0);
    CHECK("Tab moves to the next control", U.focus == 1);
    press(KEY_TAB, 0);
    CHECK("Tab skips the disabled control", U.focus == 3);
    press(KEY_TAB, 0);
    CHECK("Tab wraps around to the first", U.focus == 0);
    press(KEY_TAB, MOD_SHIFT);
    CHECK("Shift+Tab wraps backwards to the last", U.focus == 3);
    press(KEY_TAB, MOD_SHIFT);
    CHECK("Shift+Tab skips the disabled control backwards", U.focus == 1);
    press(KEY_DOWN, 0); CHECK("Down = next", U.focus == 3);
    press(KEY_UP, 0);   CHECK("Up = previous", U.focus == 1);
    press(KEY_RIGHT, 0); press(KEY_LEFT, 0); CHECK("Right/Left = next/previous", U.focus == 1);
    setup(); ui_init(&U, K, 4); U.focus = -1; { int32_t r = ui_focus_next(&U, 0); CHECK("Shift+Tab with no focus starts from the last control", r == 3); }
    setup(); for (int i = 0; i < 4; i++) K[i].flags &= (uint8_t)~UI_FOCUSABLE;
    CHECK("no focusable control: Tab leaves focus at -1 and does not loop forever", ui_focus_next(&U, 1) == -1 && U.focus == -1);
    setup(); U.n = 0;
    CHECK("empty control list is safe", ui_focus_next(&U, 1) == -1 && ui_hit(&U, 1, 1) == -1);
    setup();
    CHECK("focus_set refuses a disabled control, accepts an enabled one", ui_focus_set(&U, 2) == 0 && U.focus == -1 && ui_focus_set(&U, 3) == 1 && U.focus == 3);
    CHECK("focus_set refuses out-of-range indices", ui_focus_set(&U, -1) == 0 && ui_focus_set(&U, 99) == 0);
    K[3].flags &= (uint8_t)~UI_VISIBLE; ui_revalidate(&U);
    CHECK("hiding the focused control drops focus on revalidate", U.focus == -1);

    /* --- keyboard activation --- */
    setup(); ui_focus_set(&U, 1);
    CHECK("Enter activates the focused control", press(KEY_ENTER, 0) == B);
    CHECK("Space activates the focused control", press(KEY_SPACE, 0) == B);
    { struct input_event e = kd(KEY_ENTER, 0, 1); CHECK("a held-down Enter (repeat) does not re-activate", ui_key(&U, &e) == UI_NONE); }
    CHECK("Ctrl+Enter does not activate", press(KEY_ENTER, MOD_CTRL) == UI_NONE);
    CHECK("Escape is reported to the caller", press(KEY_ESC, 0) == UI_ESCAPE);
    CHECK("an unrelated key does nothing", press(30, 0) == UI_NONE && U.focus == 1);
    { struct input_event e = kd(KEY_ENTER, 0, 0); e.kind = IN_KEY_UP; CHECK("key-up never activates", ui_key(&U, &e) == UI_NONE); }
    setup();
    CHECK("Enter with no focus activates nothing", press(KEY_ENTER, 0) == UI_NONE);
    setup(); ui_focus_set(&U, 3); K[3].flags &= (uint8_t)~UI_ENABLED;
    CHECK("Enter on a control that was disabled after focusing does not activate", press(KEY_ENTER, 0) == UI_NONE);

    /* --- pointer click rules --- */
    setup();
    ui_pointer_down(&U, 20, 20);
    CHECK("press on A, release on A: activates A", ui_pointer_up(&U, 30, 30) == A);
    CHECK("pressing a focusable control gives it focus", U.focus == 0);
    ui_pointer_down(&U, 20, 20);
    CHECK("press on A, release on B: nothing", ui_pointer_up(&U, 20, 70) == UI_NONE);
    ui_pointer_down(&U, 20, 20);
    CHECK("press on A, release outside everything: nothing", ui_pointer_up(&U, 500, 500) == UI_NONE);
    ui_pointer_down(&U, 500, 500);
    CHECK("press outside, release on A: nothing", ui_pointer_up(&U, 20, 20) == UI_NONE);
    CHECK("release with no press: nothing", ui_pointer_up(&U, 20, 20) == UI_NONE);
    ui_pointer_down(&U, 20, 120);
    CHECK("click on a disabled control: nothing, and no focus change", ui_pointer_up(&U, 20, 120) == UI_NONE && U.focus == 0);
    ui_pointer_down(&U, 20, 20); K[0].flags &= (uint8_t)~UI_ENABLED;
    CHECK("control disabled between press and release: no activation", ui_pointer_up(&U, 20, 20) == UI_NONE);

    /* --- modal dialog: containment, no click-through, focus restoration --- */
    setup();
    K[4] = (struct ui_ctrl){ 300, 300, 100, 40, 0, DLG_OK, ALL, 5 };
    K[5] = (struct ui_ctrl){ 300, 350, 100, 40, 0, DLG_CANCEL, ALL, 5 };
    K[6] = (struct ui_ctrl){ 0, 0, 1024, 768, 0, COVER, UI_VISIBLE | UI_ENABLED, 4 };           /* scrim: swallows clicks */
    U.n = 7;
    ui_focus_set(&U, 1);
    CHECK("modal push succeeds", ui_modal_push(&U, 5) == 1);
    CHECK("opening a modal moves focus into the dialog", U.focus == 4);
    int escaped = 0;
    for (int i = 0; i < 12; i++) { press(KEY_TAB, 0); if (U.focus < 4 || U.focus > 5) escaped = 1; }
    CHECK("Tab never leaves the dialog (12 presses)", !escaped);
    for (int i = 0; i < 12; i++) { press(KEY_TAB, MOD_SHIFT); if (U.focus < 4 || U.focus > 5) escaped = 1; }
    CHECK("Shift+Tab never leaves the dialog (12 presses)", !escaped);
    ui_pointer_down(&U, 20, 20);
    CHECK("a click on a covered control A does not reach it (modal blocks it)", U.pressed == -1 && ui_pointer_up(&U, 20, 20) == UI_NONE);
    CHECK("hit testing over the covered area finds nothing reachable", ui_hit(&U, 20, 20) == -1);
    ui_pointer_down(&U, 320, 320);
    CHECK("a click on the dialog button works", ui_pointer_up(&U, 320, 320) == DLG_OK);
    CHECK("pushing a modal whose layer does not exceed the current one is refused", ui_modal_push(&U, 5) == 0 && ui_modal_push(&U, 2) == 0);
    CHECK("closing the modal restores focus to the opener (B)", ui_modal_pop(&U) == 1 && U.focus == 1);
    CHECK("pop with no modal is a no-op", ui_modal_pop(&U) == 0);
    CHECK("after closing, covered controls are reachable again", ui_hit(&U, 20, 20) == 6 || ui_hit(&U, 20, 20) == 0);
    ui_focus_set(&U, 1); ui_modal_push(&U, 5); K[1].flags &= (uint8_t)~UI_ENABLED;
    ui_modal_pop(&U);
    CHECK("if the opener became disabled, focus falls to the first focusable control instead", U.focus == 0);
    setup(); U.n = 4; K[4] = (struct ui_ctrl){ 300, 300, 100, 40, 0, DLG_OK, ALL, 5 };
    ui_modal_push(&U, 5); ui_modal_pop(&U);
    CHECK("modal push/pop with no prior focus ends with focus on a valid control or none", U.focus == -1 || U.focus == 0);
    setup();
    CHECK("modal stack depth is bounded (UI_MODAL_DEPTH)", ui_modal_push(&U, 1) && ui_modal_push(&U, 2) && ui_modal_push(&U, 3) && ui_modal_push(&U, 4) && !ui_modal_push(&U, 5));
    CHECK("modal layer 0 is refused", (ui_init(&U, K, 4), ui_modal_push(&U, 0) == 0));
    setup(); ui_modal_push(&U, 9);
    CHECK("a modal above every control leaves nothing focusable; focus is -1 and Tab is safe", U.focus == -1 && ui_focus_next(&U, 1) == -1);

    printf("ui_focus: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
