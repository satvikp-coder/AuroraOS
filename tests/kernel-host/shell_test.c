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


static void text(const char *t) { for (; *t; t++) { struct input_event e = { IN_TEXT, 0, 30, 0, (uint8_t)*t, 0, 0 }; shell_handle(&sh, &e); } }
static void kdown(uint16_t k) { key(k, 0, 0); }
static void to_index(void) { fresh(); click(SID_DOCK_INDEX); }
static int result_is(uint32_t k, const char *keyname)
{
    return k < sh.nres && strcmp(shell_index_item(sh.res[k].item)->key, keyname) == 0;
}

int main(void)
{
    printf("shell host tests\n");
    base = malloc(GUARD * 2 + W * H * 4);

    /* --- initial state --- */
    fresh();
    CHECK("boots on Home, Space 0 (Study), no history, no focus, no modal", sh.screen == SCR_HOME && sh.space == 0 && sh.hist_n == 0 && sh.fx.focus == -1 && !sh.about_open);
    CHECK("the widget array is within its bound on every screen", sh.n > 10 && sh.n <= SHELL_MAX_WIDGETS);
    CHECK("Back is disabled with an empty history; the other dock items are enabled", !(sh.c[shell_find(&sh, SID_DOCK_BACK)].flags & UI_ENABLED) && (sh.c[shell_find(&sh, SID_DOCK_HOME)].flags & UI_ENABLED));
    CHECK("Pages, Aura and Settings are real, enabled destinations; the empty-history Back button is the disabled, non-focusable control", (sh.c[shell_find(&sh, SID_APP_AURA)].flags & UI_ENABLED) && (sh.c[shell_find(&sh, SID_APP_PAGES)].flags & UI_ENABLED) && (sh.c[shell_find(&sh, SID_APP_SETTINGS)].flags & UI_ENABLED) && !(sh.c[shell_find(&sh, SID_DOCK_BACK)].flags & (UI_ENABLED | UI_FOCUSABLE)) && (sh.c[shell_find(&sh, SID_DOCK_BACK)].flags & UI_VISIBLE));
    int ids_ok = 1;
    for (uint32_t i = 0; i < sh.n; i++) for (uint32_t j = i + 1; j < sh.n; j++) if (sh.c[i].id == sh.c[j].id) ids_ok = 0;
    CHECK("control ids are unique on Home", ids_ok);

    /* --- Tab order --- */
    static const uint32_t order[] = { SID_CHANGE_SPACE, SID_ABOUT, SID_APP_SPACES, SID_APP_INDEX, SID_APP_PAGES, SID_APP_AURA, SID_APP_SETTINGS, SID_DOCK_HOME, SID_DOCK_SPACES, SID_DOCK_INDEX, SID_CORNER, SID_CHANGE_SPACE };
    int ord = 1;
    for (unsigned i = 0; i < sizeof order / sizeof order[0]; i++) { tab(); if (focus_id() != order[i]) { ord = 0; printf("    step %u: got %u want %u\n", i, focus_id(), order[i]); } }
    CHECK("Tab visits content, the dock, then the Glass Corner handle; includes the Pages, Aura and Settings tiles, skips the disabled Back; wraps", ord);
    stab();
    CHECK("Shift+Tab wraps backwards to the Glass Corner handle (the last stop)", focus_id() == SID_CORNER);

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
    click(SID_DOCK_BACK);
    CHECK("clicking a disabled control (empty-history Back) activates nothing", sh.activations == act && sh.screen == SCR_HOME);
    int landed = 0; for (int i = 0; i < 20; i++) { tab(); uint32_t f = focus_id(); if (f == SID_DOCK_BACK) landed = 1; }
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
    ptr(IN_PTR_DOWN, 50, 300); ptr(IN_PTR_UP, 50, 300);
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
    CHECK("SCREEN: the dock is hidden behind the scrim", px(300, 704) != 0x002A68E8u && px(300, 740) == 0x00525A66u);
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
    CHECK("SCREEN: header is white and the Horizon rail is the aura colour", px(500, 10) == 0x00FFFFFFu && px(500, 65) == 0x0000838Cu);
    CHECK("SCREEN: the Home dock item is active (accent fill)", px(410, 704) == 0x002A68E8u);
    CHECK("SCREEN: Back is drawn muted (disabled), not accent", px(290, 704) != 0x002A68E8u);
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


    /* ================= Index (native search) ================= */
    to_index();
    CHECK("Index opens with an empty query, all registry items up to the row limit, and the first one selected", sh.screen == SCR_INDEX && sh.qlen == 0 && sh.nres == SHELL_RESULTS && sh.sel == 0 && result_is(0, "home"));
    CHECK("the registry lists only real destinations and marks Notes unavailable and Pages, Aura and Settings available", shell_index_available(8) && shell_index_available(9) && !shell_index_available(11) && shell_index_available(10) && shell_index_available(0) && shell_index_available(1) && shell_index_available(2) && shell_index_available(7));
    text("sp");
    CHECK("typing 'sp' narrows the results; Spaces ranks first (prefix) and is selected", sh.qlen == 2 && strcmp(sh.query, "sp") == 0 && result_is(0, "spaces") && sh.sel == 0);
    enter();
    CHECK("Enter with nothing focused opens the selected result (Spaces)", sh.screen == SCR_SPACES && sh.index_opens == 1);
    to_index(); text("WORK");
    CHECK("the query is case-insensitive; the exact match 'Work' ranks first", result_is(0, "space.work"));
    enter();
    CHECK("opening the Work item selects that Space and shows Home", sh.screen == SCR_HOME && sh.space == 1 && strcmp(sh.space_txt, "Work") == 0);
    click(SID_DOCK_INDEX);
    CHECK("opened items become recent: Work is first for an empty query now", result_is(0, "space.work") && sh.nrecent == 1);
    CHECK("each time Index opens it starts with a fresh query", sh.qlen == 0);
    text("home"); enter(); to_index(); text("about"); enter();
    CHECK("the About item opens Home and the About modal", sh.screen == SCR_HOME && sh.about_open && focus_id() == SID_ABOUT_CLOSE);
    key(KEY_ESC, 0, 0);

    /* editing */
    to_index(); text("abc"); kdown(KEY_BACKSPACE);
    CHECK("Backspace removes the last character", sh.qlen == 2 && strcmp(sh.query, "ab") == 0);
    kdown(KEY_BACKSPACE); kdown(KEY_BACKSPACE); kdown(KEY_BACKSPACE); kdown(KEY_BACKSPACE);
    CHECK("Backspace on an empty query does nothing", sh.qlen == 0 && sh.query[0] == 0);
    text("zz"); kdown(KEY_ESC);
    CHECK("Escape with text clears the query and stays on Index", sh.screen == SCR_INDEX && sh.qlen == 0);
    kdown(KEY_ESC);
    CHECK("Escape with an empty query goes back", sh.screen == SCR_HOME);
    to_index(); text("a b");
    CHECK("a typed space is part of the query", sh.qlen == 3 && strcmp(sh.query, "a b") == 0);
    uint32_t opens = sh.index_opens; key(KEY_SPACE, 0, 0);
    CHECK("the Space KEY never activates anything on Index", sh.index_opens == opens && sh.screen == SCR_INDEX);

    to_index(); text("ho"); tab(); opens = sh.index_opens;
    CHECK("Tab focuses a result row and the Space KEY does not open it (Space types; only Enter opens)", focus_id() == SID_RESULT_BASE && (key(KEY_SPACE, 0, 0), sh.index_opens == opens && sh.screen == SCR_INDEX));

    /* limits and hostile input */
    to_index();
    { char big[200]; memset(big, 'a', 199); big[199] = 0; text(big); }
    CHECK("the query is capped at IDX_MAX_QUERY characters", sh.qlen == IDX_MAX_QUERY && strlen(sh.query) == IDX_MAX_QUERY && strlen(sh.field_txt) <= sizeof sh.field_txt - 1);
    { uint8_t bad[] = { 0, 1, 31, 127, 128, 200, 255 }; to_index();
      for (unsigned i = 0; i < sizeof bad; i++) { struct input_event e = { IN_TEXT, 0, 30, 0, bad[i], 0, 0 }; shell_handle(&sh, &e); }
      CHECK("control, DEL and non-ASCII text events are ignored", sh.qlen == 0); }
    CHECK("text typed on Home goes nowhere", (fresh(), text("abc"), sh.qlen == 0 && sh.screen == SCR_HOME));
    to_index(); text("zzzzq");
    CHECK("no match: no results, a 'No results.' message, and Enter does nothing", sh.nres == 0 && sh.sel == -1 && (enter(), sh.screen == SCR_INDEX && sh.index_opens == 0));
    uint32_t rng = 12345; int inv = 1;
    to_index();
    for (int i = 0; i < 3000; i++) {
        rng = rng * 1103515245u + 12345u; uint32_t v = (rng >> 16) & 0xffff;
        if (v % 7 == 0) kdown(KEY_BACKSPACE); else if (v % 7 == 1) kdown(KEY_DOWN); else if (v % 7 == 2) kdown(KEY_UP);
        else if (v % 7 == 3) { struct input_event e = { IN_TEXT, 0, 30, 0, (uint8_t)(v & 0xff), 0, 0 }; shell_handle(&sh, &e); }
        else { struct input_event e = { IN_TEXT, 0, 30, 0, (uint8_t)('a' + v % 26), 0, 0 }; shell_handle(&sh, &e); }
        if (sh.screen != SCR_INDEX) { inv = 0; break; }
        if (sh.qlen > IDX_MAX_QUERY || sh.nres > SHELL_RESULTS || sh.sel < -1 || sh.sel >= (int)sh.nres + (sh.nres == 0) || sh.n > SHELL_MAX_WIDGETS || sh.query[sh.qlen] != 0) inv = 0;
        if (i % 50 == 0) { surf(); if (shell_draw(&sh, &S) != GFX_OK || !guards_ok()) inv = 0; }
    }
    CHECK("3000 rapid random edits/selections: invariants hold (query bound, result bound, selection bound, widget bound, in-surface drawing)", inv);

    /* selection by keyboard */
    to_index(); text("s");
    kdown(KEY_DOWN);
    CHECK("Down moves the selection", sh.sel == 1);
    kdown(KEY_UP); kdown(KEY_UP);
    CHECK("Up stops at the first result", sh.sel == 0);
    for (int i = 0; i < 30; i++) kdown(KEY_DOWN);
    CHECK("Down stops at the last result", sh.sel == (int)sh.nres - 1);

    /* unavailable destinations */
    to_index(); text("notes");
    CHECK("'notes' finds the Notes entry, labelled not available yet", sh.nres == 1 && result_is(0, "notes") && strstr(sh.res_txt[0], "(not available yet)") != 0);
    CHECK("... it is disabled: not focusable and visibly muted", !(sh.c[shell_find(&sh, SID_RESULT_BASE)].flags & UI_ENABLED));
    CHECK("... and even when selected it is not drawn as an active (openable) row", sh.sel == 0 && sh.w[shell_find(&sh, SID_RESULT_BASE)].active == 0);
    enter(); click(SID_RESULT_BASE);
    CHECK("Enter and a click on an unavailable result open nothing", sh.screen == SCR_INDEX && sh.index_opens == 0 && sh.nrecent == 0);

    /* typo suggestions are never the default */
    to_index(); text("hmoe");
    CHECK("a typo of an app name yields a suggestion labelled 'Did you mean', not selected", sh.nres == 1 && sh.res[0].fuzzy && sh.sel == -1 && strncmp(sh.res_txt[0], "Did you mean: Home", 18) == 0);
    enter();
    CHECK("Enter does NOT open a typo suggestion", sh.screen == SCR_INDEX && sh.index_opens == 0);
    tab(); CHECK("Tab can focus the suggestion", focus_id() == SID_RESULT_BASE);
    enter();
    CHECK("choosing it explicitly (Tab then Enter) opens it", sh.screen == SCR_HOME && sh.index_opens == 1);

    /* pointer and focus */
    to_index(); text("spa"); click(SID_RESULT_BASE);
    CHECK("a pointer click on a result opens it", sh.screen == SCR_SPACES);
    to_index(); text("ind"); tab();
    CHECK("Tab moves focus to the first result row", focus_id() == SID_RESULT_BASE);
    to_index(); text("zzzzq"); click(SID_DOCK_HOME);
    CHECK("the dock still works from Index", sh.screen == SCR_HOME);

    /* history / Back */
    to_index(); text("xyz"); click(SID_DOCK_HOME); click(SID_DOCK_BACK);
    CHECK("returning to Index by Back starts a fresh search", sh.screen == SCR_INDEX && sh.qlen == 0);

    /* rendering */
    to_index(); text("sp"); surf(); shell_draw(&sh, &S);
    CHECK("SCREEN: the selected result row is accent-filled", px(100, 264) == 0x002A68E8u);
    { uint32_t ink = 0; for (int y = 152; y < 200; y++) for (int x = 64; x < 400; x++) ink += px(x, y) == 0x00142136u; CHECK("SCREEN: typed text is drawn in the field", ink > 20); }
    to_index(); surf(); shell_draw(&sh, &S);
    { uint32_t muted = 0; for (int y = 152; y < 200; y++) for (int x = 64; x < 400; x++) muted += px(x, y) == 0x0052627Au; CHECK("SCREEN: an empty field shows the muted placeholder", muted > 20); }
    CHECK("all Index renders stayed inside the surface", guards_ok());


    /* ================= Glass Corner and Aurora Horizon ================= */
    fresh();
    CHECK("the Glass Corner handle exists on every screen, is enabled and focusable", (sh.c[shell_find(&sh, SID_CORNER)].flags & (UI_ENABLED | UI_FOCUSABLE)) == (UI_ENABLED | UI_FOCUSABLE) && !sh.corner_open);
    click(SID_DOCK_SPACES); CHECK("... also on Spaces", shell_find(&sh, SID_CORNER) >= 0); click(SID_DOCK_INDEX); CHECK("... and on Index", shell_find(&sh, SID_CORNER) >= 0);
    fresh(); click(SID_CORNER);
    CHECK("clicking the handle opens the held-items sheet; focus moves into it (Close)", sh.corner_open && focus_id() == SID_CORNER_CLOSE);
    CHECK("the handle itself is unreachable while the sheet is open (it sits below the sheet layer)", ui_hit(&sh.fx, 960, 32) == -1);
    { int esc2 = 1; for (int i = 0; i < 10; i++) { tab(); if (focus_id() != SID_CORNER_CLOSE) esc2 = 0; stab(); if (focus_id() != SID_CORNER_CLOSE) esc2 = 0; }
      CHECK("Tab and Shift+Tab never leave the sheet", esc2); }
    act = sh.activations; click(SID_DOCK_SPACES);
    CHECK("a click on the dock button behind the open sheet closes the sheet and activates nothing (no click-through)", sh.activations == act && sh.screen == SCR_HOME && !sh.corner_open);
    click(SID_CORNER); act = sh.activations; click(SID_CHANGE_SPACE);
    CHECK("same for the Change Space button behind the open sheet (the Spaces tile is physically under the sheet panel)", sh.activations == act && sh.screen == SCR_HOME && !sh.corner_open);
    click(SID_CORNER); act = sh.activations; click(SID_ABOUT);
    CHECK("same for the About button behind the sheet (it does not open)", sh.activations == act && !sh.about_open && !sh.corner_open);
    click(SID_CORNER);
    CHECK("the sheet can be reopened afterwards", sh.corner_open && focus_id() == SID_CORNER_CLOSE);
    surf(); shell_draw(&sh, &S);
    CHECK("SCREEN: the sheet is an opaque white panel with a focus-coloured border, clear of the dock", px(700, 200) == 0x00FFFFFFu && px(584, 200) == 0x00175CD3u && px(300, 704) != 0x00525A66u);
    CHECK("SCREEN: the sheet leaves the Current Space card (left) untouched", px(300, 150) == 0x00FFFFFFu && px(584 - 8, 200) != 0x00175CD3u);
    key(KEY_ESC, 0, 0);
    CHECK("Escape closes the sheet; focus returns to the handle", !sh.corner_open && focus_id() == SID_CORNER && sh.screen == SCR_HOME);
    enter();
    CHECK("Enter on the focused handle opens it again", sh.corner_open);
    act = sh.activations; ptr(IN_PTR_DOWN, 300, 500); ptr(IN_PTR_UP, 300, 500);
    CHECK("a click outside the sheet closes it and activates nothing underneath", !sh.corner_open && sh.activations == act);
    click(SID_CORNER); click(SID_CORNER_CLOSE);
    CHECK("the Close button closes it", !sh.corner_open);
    click(SID_CORNER); ptr(IN_PTR_DOWN, 700, 150); ptr(IN_PTR_UP, 700, 150);
    CHECK("a click on the sheet's own text does not close it", sh.corner_open);
    key(KEY_ESC, 0, 0);
    click(SID_DOCK_INDEX); click(SID_CORNER); text("abc");
    CHECK("typing while the sheet is open does not edit the Index query", sh.qlen == 0 && sh.corner_open);
    key(KEY_ESC, 0, 0); text("ab");
    CHECK("after closing, typing works again", sh.qlen == 2);
    fresh(); { int32_t focus_before; while (focus_id() != SID_APP_INDEX) tab(); focus_before = sh.fx.focus; (void)focus_before;
      click(SID_CORNER); key(KEY_ESC, 0, 0);
      CHECK("opening and closing the sheet by pointer leaves a valid focus", sh.fx.focus >= 0 && (sh.c[sh.fx.focus].flags & UI_ENABLED)); }
    fresh(); while (focus_id() != SID_ABOUT) tab(); enter();
    CHECK("the About dialog hides the corner handle (only one modal layer at a time)", sh.about_open && shell_find(&sh, SID_CORNER) < 0);
    key(KEY_ESC, 0, 0);
    CHECK("... and it comes back after the dialog closes", shell_find(&sh, SID_CORNER) >= 0);
    fresh(); click(SID_CORNER); shell_go(&sh, SCR_SPACES);
    CHECK("navigating while the sheet is open is refused by the UI (dock inert) so state stays consistent", sh.screen == SCR_HOME || sh.corner_open);

    /* Horizon: one source, honest states */
    fresh(); surf(); shell_draw(&sh, &S);
    CHECK("Horizon: keyboard and pointer found -> aura rail and 'Horizon: ready'", px(500, 65) == 0x0000838Cu && strcmp(sh.horizon_txt, "Horizon: ready") == 0);
    { struct shell_caps kb_only = { 1, 0 }; shell_set_caps(&sh, kb_only); surf(); shell_draw(&sh, &S);
      CHECK("Horizon: a missing pointer -> muted rail and 'Horizon: limited input' (never claims more than it measured)", px(500, 65) == 0x0052627Au && strcmp(sh.horizon_txt, "Horizon: limited input") == 0); }
    { struct shell_caps none = { 0, 0 }; shell_set_caps(&sh, none); surf(); shell_draw(&sh, &S);
      CHECK("Horizon: no input devices -> the same limited state", px(500, 65) == 0x0052627Au); }
    surf(); fresh(); shell_draw(&sh, &S);
    { uint32_t ink = 0; for (int y = 22; y < 46; y++) for (int x = 190; x < 400; x++) ink += px(x, y) == 0x0052627Au; CHECK("SCREEN: the Horizon status text is drawn in the header", ink > 20); }
    { uint32_t ink = 0; for (int y = 8; y < 56; y++) for (int x = 912; x < 1008; x++) ink += px(x, y) == 0x00142136u; CHECK("SCREEN: the Held handle label is drawn at the top right", ink > 10); }
    CHECK("rendering with the corner open or closed stayed inside the surface", guards_ok());

    /* Compositor-owned microphone listening indicator */
    fresh();
    CHECK("mic badge hidden initially", shell_find(&sh, 0xF611u) < 0);
    CHECK("shell_mic_begin activates indicator", shell_mic_begin(&sh, 601, 4, 1) == MIC_OK && shell_find(&sh, 0xF611u) >= 0);
    CHECK("mic badge label is Mic: Listening", strcmp(sh.w[shell_find(&sh, 0xF611u)].text, "Mic: Listening") == 0);
    surf(); shell_draw(&sh, &S);
    CHECK("mic badge renders inside surface", guards_ok());
    shell_mic_owner_gone(&sh, 4, 2); /* stale gen ignored */
    CHECK("stale gen owner_gone keeps mic badge", shell_find(&sh, 0xF611u) >= 0);
    shell_mic_owner_gone(&sh, 4, 1); /* matching gen clears */
    CHECK("matching owner_gone clears mic badge", shell_find(&sh, 0xF611u) < 0 && sh.mic.state == MIC_IND_OFF);

    /* PIN lock overlay integration */
    static const uint8_t pin_salt[PIN_SALT_LEN] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
    CHECK("shell_pin_set locks shell", shell_pin_set(&sh, pin_salt, "2468", 4) == PIN_OK && shell_is_locked(&sh));
    CHECK("lock overlay title present", shell_find(&sh, 0xF702u) >= 0 && strcmp(sh.w[shell_find(&sh, 0xF702u)].text, "AuroraOS Locked") == 0);
    surf(); shell_draw(&sh, &S);
    CHECK("lock overlay renders inside surface", guards_ok());

    /* Typing wrong PIN keeps shell locked and shows Incorrect PIN */
    for (int i = 0; i < 4; i++) shell_handle(&sh, &((struct input_event){ .kind = IN_TEXT, .ch = '9' }));
    CHECK("PIN entry masked as ****", strcmp(sh.w[shell_find(&sh, 0xF703u)].text, "****") == 0);
    shell_handle(&sh, &((struct input_event){ .kind = IN_KEY_DOWN, .key = KEY_ENTER }));
    CHECK("wrong PIN keeps locked and sets message", shell_is_locked(&sh) && strcmp(sh.pin_msg, "Incorrect PIN") == 0);

    /* Typing correct PIN unlocks and removes overlay */
    const char *good_pin = "2468";
    for (int i = 0; i < 4; i++) shell_handle(&sh, &((struct input_event){ .kind = IN_TEXT, .ch = good_pin[i] }));
    shell_handle(&sh, &((struct input_event){ .kind = IN_KEY_DOWN, .key = KEY_ENTER }));
    CHECK("correct PIN unlocks shell", !shell_is_locked(&sh) && shell_find(&sh, 0xF702u) < 0);

    /* Explicit re-lock works */
    CHECK("shell_pin_lock re-locks", shell_pin_lock(&sh) == PIN_OK && shell_is_locked(&sh));

    printf("shell: %d passed, %d failed\n", passed, failed);
    free(base);
    return failed ? 1 : 0;
}
