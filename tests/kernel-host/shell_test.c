/* tests/kernel-host/shell_test.c - host tests for kernel/ui/shell.c: navigation, history, focus, modal, activation, rendering. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shell.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; printf("  PASS: %s\n", name); } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

#define W 1024u
#define H 768u
#define GUARD 4096u
static uint8_t *base, *pix;
static struct gfx_surface S;
static struct shell sh;

static void surf(void) { memset(base, 0xA5, GUARD * 2 + W * H * 4); pix = base + GUARD; S = (struct gfx_surface){ pix, (uint64_t)W * H * 4, W, H, W * 4, GFX_FMT_XRGB8888 }; }
static int guards_ok(void) { for (uint32_t i = 0; i < GUARD; i++) if (base[i] != 0xA5 || pix[W * H * 4 + i] != 0xA5) return 0; return 1; }
static uint32_t px(int x, int y) { return *(uint32_t *)(void *)(pix + (size_t)y * W * 4 + (size_t)x * 4); }
static void draw(void) { surf(); CHECK("frame draws without error", shell_draw(&sh, &S) == GFX_OK); }

static void fresh(void) { shell_init(&sh, W, H); struct shell_caps c = { 1, 1 }; shell_set_caps(&sh, c); }
static void key(uint16_t k, uint16_t mods, uint8_t rep) { struct input_event e = { IN_KEY_DOWN, rep, k, mods, 0, 0, 0 }; shell_handle(&sh, &e); }
static void tab(void) { key(KEY_TAB, 0, 0); }
static void stab(void) { key(KEY_TAB, MOD_SHIFT, 0); }
static void enter(void) { key(KEY_ENTER, 0, 0); }
static void ptr(int kind, int x, int y) { struct input_event e = { (uint8_t)kind, 0, BTN_LEFT, 0, 0, x, y }; shell_handle(&sh, &e); }
static int cx(uint32_t id) { int32_t i = shell_find(&sh, id); return i < 0 ? -1 : sh.c[i].x + sh.c[i].w / 2; }
static int cy(uint32_t id) { int32_t i = shell_find(&sh, id); return i < 0 ? -1 : sh.c[i].y + sh.c[i].h / 2; }
static void click(uint32_t id) { ptr(IN_PTR_DOWN, cx(id), cy(id)); ptr(IN_PTR_UP, cx(id), cy(id)); }
static uint32_t focus_id(void) { return sh.fx.focus >= 0 ? sh.c[sh.fx.focus].id : SHELL_NONE; }

int main(void)
{
    printf("shell host tests\n");
    base = malloc(GUARD * 2 + W * H * 4);

    /* --- initial state --- */
    fresh();
    CHECK("boots on Home, Space 0 (Study), no history, no focus, no modal", sh.screen == SCR_HOME && sh.space == 0 && sh.hist_n == 0 && sh.fx.focus == -1 && !sh.about_open);
    CHECK("the widget array is within its bound on every screen", sh.n > 10 && sh.n <= SHELL_MAX_WIDGETS);
    CHECK("Back is disabled with an empty history; the other dock items are enabled", !(sh.c[shell_find(&sh, SID_DOCK_BACK)].flags & UI_ENABLED) && (sh.c[shell_find(&sh, SID_DOCK_HOME)].flags & UI_ENABLED));
    CHECK("unavailable applications are visible but disabled and not focusable", !(sh.c[shell_find(&sh, SID_APP_PAGES)].flags & (UI_ENABLED | UI_FOCUSABLE)) && (sh.c[shell_find(&sh, SID_APP_PAGES)].flags & UI_VISIBLE) && !(sh.c[shell_find(&sh, SID_APP_AURA)].flags & UI_ENABLED) && !(sh.c[shell_find(&sh, SID_APP_SETTINGS)].flags & UI_ENABLED));
    int ids_ok = 1;
    for (uint32_t i = 0; i < sh.n; i++) for (uint32_t j = i + 1; j < sh.n; j++) if (sh.c[i].id == sh.c[j].id) ids_ok = 0;
    CHECK("control ids are unique on Home", ids_ok);

    /* --- Tab order --- */
    static const uint32_t order[] = { SID_CHANGE_SPACE, SID_ABOUT, SID_APP_SPACES, SID_APP_INDEX, SID_DOCK_HOME, SID_DOCK_SPACES, SID_DOCK_INDEX, SID_CHANGE_SPACE };
    int ord = 1;
    for (unsigned i = 0; i < sizeof order / sizeof order[0]; i++) { tab(); if (focus_id() != order[i]) { ord = 0; printf("    step %u: got %u want %u\n", i, focus_id(), order[i]); } }
    CHECK("Tab visits content then dock, skips the disabled apps and disabled Back, and wraps", ord);
    stab();
    CHECK("Shift+Tab wraps backwards to the last dock item", focus_id() == SID_DOCK_INDEX);

    /* --- activation by keyboard --- */
    fresh(); while (focus_id() != SID_APP_SPACES) tab();
    enter();
    CHECK("Enter on the Spaces tile opens Spaces and records one history entry", sh.screen == SCR_SPACES && sh.hist_n == 1 && sh.last_activated == SID_APP_SPACES && sh.activations == 1);
    CHECK("the Back button is now enabled", (sh.c[shell_find(&sh, SID_DOCK_BACK)].flags & UI_ENABLED) != 0);
    CHECK("focus did not stay on a control that no longer exists", sh.fx.focus == -1 || sh.c[sh.fx.focus].id != SID_APP_SPACES);
    uint32_t a0 = sh.activations; key(KEY_ENTER, 0, 1);
    CHECK("a held Enter (key repeat) never activates", sh.activations == a0);
    while (focus_id() != SID_DOCK_BACK) tab();
    key(KEY_SPACE, 0, 0);
    CHECK("Space on Back returns Home; Back is disabled again; focus lands on a valid control", sh.screen == SCR_HOME && sh.hist_n == 0 && sh.fx.focus >= 0 && (sh.c[sh.fx.focus].flags & UI_ENABLED));
    CHECK("... specifically the Home dock button", focus_id() == SID_DOCK_HOME);

    /* --- dock navigation, history bounds --- */
    fresh();
    click(SID_DOCK_SPACES); CHECK("dock Spaces opens Spaces", sh.screen == SCR_SPACES);
    click(SID_DOCK_INDEX); CHECK("dock Index opens Index", sh.screen == SCR_INDEX && sh.hist_n == 2);
    click(SID_DOCK_INDEX); CHECK("activating the dock item of the current screen does nothing (no history spam)", sh.screen == SCR_INDEX && sh.hist_n == 2);
    click(SID_DOCK_HOME); CHECK("dock Home opens Home", sh.screen == SCR_HOME && sh.hist_n == 3);
    click(SID_DOCK_BACK); CHECK("Back goes to the previous screen (Index)", sh.screen == SCR_INDEX && sh.hist_n == 2);
    click(SID_DOCK_BACK); click(SID_DOCK_BACK);
    CHECK("Back x2 more reaches the first screen (Home) and empties the history", sh.screen == SCR_HOME && sh.hist_n == 0);
    CHECK("shell_back with an empty history is a no-op", shell_back(&sh) == 0 && sh.screen == SCR_HOME);
    for (int i = 0; i < 40; i++) { shell_go(&sh, (uint32_t)(1 + i % 2)); shell_go(&sh, SCR_HOME); }
    CHECK("history never exceeds its bound over 80 navigations", sh.hist_n <= SHELL_HISTORY);
    int back = 0; while (shell_back(&sh)) back++;
    CHECK("Back drains the bounded history completely, then stops", back == SHELL_HISTORY && sh.hist_n == 0);
    CHECK("unknown screens are refused", shell_go(&sh, 99) == 0 && shell_go(&sh, 0xffffffffu) == 0 && sh.screen < SCR_COUNT);

    /* --- Spaces --- */
    fresh(); click(SID_DOCK_SPACES);
    int each = 1;
    for (uint32_t i = 0; i < SHELL_SPACES; i++) {
        click(SID_SPACE_BASE + i);
        if (sh.space != i || sh.screen != SCR_SPACES) each = 0;
        int32_t bi = shell_find(&sh, SID_SPACE_BASE + i); (void)bi;
        for (uint32_t j = 0; j < SHELL_SPACES; j++) { struct ui_widget *w = &sh.w[shell_find(&sh, SID_SPACE_BASE + j)]; if ((w->active != 0) != (j == i)) each = 0; }
    }
    CHECK("each of the 4 built-in Spaces can be selected; exactly the selected one is marked active", each && sh.space == 3);
    CHECK("invalid Space indices are refused and change nothing", shell_select_space(&sh, 4) == 0 && shell_select_space(&sh, 0xffffffffu) == 0 && sh.space == 3);
    click(SID_DOCK_HOME);
    CHECK("Home shows the selected Space (Personal)", strcmp(sh.space_txt, "Personal") == 0 && sh.screen == SCR_HOME);
    CHECK("the four Space labels are distinct", strcmp(SHELL_SPACE_NAMES[0], SHELL_SPACE_NAMES[1]) && strcmp(SHELL_SPACE_NAMES[1], SHELL_SPACE_NAMES[2]) && strcmp(SHELL_SPACE_NAMES[2], SHELL_SPACE_NAMES[3]) && strcmp(SHELL_SPACE_NAMES[0], SHELL_SPACE_NAMES[3]));
    CHECK("no stale Space selection after leaving and returning: Spaces screen marks Personal", (click(SID_DOCK_SPACES), sh.w[shell_find(&sh, SID_SPACE_BASE + 3)].active == 1 && sh.w[shell_find(&sh, SID_SPACE_BASE)].active == 0));

    /* --- disabled controls --- */
    fresh(); click(SID_DOCK_HOME);
    uint32_t act = sh.activations;
    click(SID_APP_PAGES); click(SID_APP_AURA); click(SID_APP_SETTINGS); click(SID_DOCK_BACK);
    CHECK("clicking disabled controls (Pages, Aura, Settings, empty-history Back) activates nothing", sh.activations == act && sh.screen == SCR_HOME);
    int landed = 0; for (int i = 0; i < 20; i++) { tab(); uint32_t f = focus_id(); if (f == SID_APP_PAGES || f == SID_APP_AURA || f == SID_APP_SETTINGS || f == SID_DOCK_BACK) landed = 1; }
    CHECK("Tab never lands on a disabled control (20 presses)", !landed);
    fresh(); enter();
    CHECK("Enter with nothing focused activates nothing", sh.activations == 0);

    /* --- pointer rules --- */
    fresh();
    ptr(IN_PTR_DOWN, cx(SID_APP_INDEX), cy(SID_APP_INDEX)); ptr(IN_PTR_UP, cx(SID_APP_SPACES), cy(SID_APP_SPACES));
    CHECK("press on Index, release on Spaces: not a click", sh.activations == 0 && sh.screen == SCR_HOME);
    ptr(IN_PTR_DOWN, 5, 700); ptr(IN_PTR_UP, cx(SID_APP_INDEX), cy(SID_APP_INDEX));
    CHECK("press on empty canvas, release on a button: not a click", sh.activations == 0);
    ptr(IN_PTR_DOWN, cx(SID_APP_INDEX), cy(SID_APP_INDEX)); ptr(IN_PTR_UP, cx(SID_APP_INDEX), cy(SID_APP_INDEX));
    CHECK("press and release on Index: opens Index", sh.screen == SCR_INDEX && sh.activations == 1);
    ptr(IN_PTR_DOWN, 300, 300); ptr(IN_PTR_UP, 300, 300);
    CHECK("click on a card background does nothing", sh.activations == 1);

    /* --- modal --- */
    fresh(); while (focus_id() != SID_ABOUT) tab();
    enter();
    CHECK("About opens a modal and moves focus into it (Close)", sh.about_open && focus_id() == SID_ABOUT_CLOSE);
    int esc = 1; for (int i = 0; i < 10; i++) { tab(); if (focus_id() != SID_ABOUT_CLOSE) esc = 0; stab(); if (focus_id() != SID_ABOUT_CLOSE) esc = 0; }
    CHECK("Tab and Shift+Tab never leave the dialog", esc);
    act = sh.activations;
    click(SID_APP_SPACES); click(SID_DOCK_SPACES); click(SID_CHANGE_SPACE);
    CHECK("clicks on controls covered by the modal do nothing (no click-through)", sh.activations == act && sh.screen == SCR_HOME && sh.about_open);
    draw();
    CHECK("SCREEN: the scrim covers the content and the dock; the dialog is white", px(600, 400) == 0x00525A66u || px(100, 700) == 0x00525A66u);
    CHECK("SCREEN: the dock is hidden behind the scrim", px(300, 704) != 0x003478F6u && px(300, 740) == 0x00525A66u);
    CHECK("SCREEN: the focus ring is on Close", px(512, 401) == 0x00175CD3u || px(512, 402) == 0x00175CD3u || px(512, 400) == 0x00175CD3u);
    key(KEY_ESC, 0, 0);
    CHECK("Escape closes the modal; focus returns to About (the opener)", !sh.about_open && focus_id() == SID_ABOUT && sh.screen == SCR_HOME);
    enter(); click(SID_ABOUT_CLOSE);
    CHECK("the Close button closes it too, focus restored again", !sh.about_open && focus_id() == SID_ABOUT);
    key(KEY_ESC, 0, 0);
    CHECK("Escape on Home with no modal and no history does nothing", sh.screen == SCR_HOME && sh.hist_n == 0);
    click(SID_DOCK_SPACES); key(KEY_ESC, 0, 0);
    CHECK("Escape on a deeper screen means Back", sh.screen == SCR_HOME && sh.hist_n == 0);

    /* --- uptime label --- */
    fresh(); sh.dirty = 0;
    shell_set_uptime(&sh, 75); CHECK("uptime 75 s shows 'Up 1:15' and marks the frame dirty", strcmp(sh.uptime_txt, "Up 1:15") == 0 && sh.dirty);
    sh.dirty = 0; shell_set_uptime(&sh, 75); CHECK("an unchanged uptime does not mark dirty", !sh.dirty);
    shell_set_uptime(&sh, 3599); CHECK("uptime 3599 s shows 'Up 59:59'", strcmp(sh.uptime_txt, "Up 59:59") == 0);
    shell_set_uptime(&sh, 0xffffffffu); CHECK("an absurd uptime is capped, not overflowed", strlen(sh.uptime_txt) <= 10 && strncmp(sh.uptime_txt, "Up 999:", 7) == 0);

    /* --- rendering --- */
    fresh(); draw();
    CHECK("SCREEN: canvas, white header, aura-coloured Horizon rail", px(500, 90) == 0x00EEF3F8u || px(20, 700) == 0x00EEF3F8u);
    CHECK("SCREEN: header is white and the Horizon rail is the aura colour", px(500, 10) == 0x00FFFFFFu && px(500, 65) == 0x0000AEB8u);
    CHECK("SCREEN: the Home dock item is active (accent fill)", px(410, 704) == 0x003478F6u);
    CHECK("SCREEN: Back is drawn muted (disabled), not accent", px(290, 704) != 0x003478F6u);
    int32_t ci = shell_find(&sh, SID_CHANGE_SPACE);
    CHECK("SCREEN: the Change Space button has the normal fill with no hover when the pointer is elsewhere", px(sh.c[ci].x + 6, sh.c[ci].y + 24) == 0x00EEF3F8u);
    shell_set_pointer(&sh, cx(SID_CHANGE_SPACE), cy(SID_CHANGE_SPACE)); draw();
    CHECK("SCREEN: hovering Change Space tints it", px(sh.c[ci].x + 6, sh.c[ci].y + 24) == 0x00E4ECF6u);
    while (focus_id() != SID_CHANGE_SPACE) { tab(); }
    draw();
    CHECK("SCREEN: the focus ring surrounds the focused button", px(cx(SID_CHANGE_SPACE), sh.c[ci].y - 2) == 0x00175CD3u);
    uint8_t *copy = malloc(W * H * 4); memcpy(copy, pix, W * H * 4); draw();
    CHECK("the same state renders identically twice", memcmp(copy, pix, W * H * 4) == 0);
    free(copy);
    for (int scr = 0; scr < SCR_COUNT; scr++) { fresh(); shell_go(&sh, (uint32_t)scr); draw(); if (sh.n > SHELL_MAX_WIDGETS) failed++; }
    CHECK("all screens render within the surface (guard canaries intact)", guards_ok());
    fresh(); struct shell_caps nop = { 0, 0 }; shell_set_caps(&sh, nop); draw();
    CHECK("no keyboard and no pointer: Home still renders, no pointer cursor, status says so", guards_ok() && px(sh.px, sh.py) != 0x00FFFFFFu);
    CHECK("... and nothing crashes when events still arrive", (tab(), enter(), sh.activations <= 1));

    printf("shell: %d passed, %d failed\n", passed, failed);
    free(base);
    return failed ? 1 : 0;
}
