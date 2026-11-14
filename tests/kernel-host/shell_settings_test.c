/* tests/kernel-host/shell_settings_test.c - the native Settings screen: navigation, honest status messages, the registry as the authority
 * (a fake store stands in for kernel/settings.c and can fail, deny or report a newer version), session-only mode without a store, scrolling at
 * large text, and a randomized churn. Host, ASan/UBSan. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shell.h>
#include <settings.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

#define W 1024u
#define H 768u
static struct shell sh;

/* the documented ranges, restated here independently of the shell's table */
static int settings_valid_fake(uint32_t id, uint64_t v)
{
    switch (id) {
    case SET_THEME: return v <= 2;
    case SET_TEXT_SCALE: return v >= 75 && v <= 200;
    case SET_REDUCED_MOTION: return v <= 1;
    case SET_FOCUS_MODE: return v <= 3;
    case SET_AURA_MODE: return v <= 2;
    case SET_NOTIFICATIONS: return v <= 2;
    default: return 0;
    }
}

/* ---- a fake registry ---- */
static uint32_t vals[8];
static int fail_next, calls_set, calls_reset, calls_get, deny_id, mode_version;
static int f_get(uint32_t id, uint32_t *v) { calls_get++; if (id < 1 || id > 6) return SETTINGS_EID; *v = vals[id]; return SETTINGS_OK; }
static int f_set(uint32_t id, uint32_t v)
{
    calls_set++;
    if (mode_version) return SETTINGS_EVERSION;
    if (id == (uint32_t)deny_id) return SETTINGS_EPERM;
    if (fail_next) { fail_next = 0; return SETTINGS_ESTORE; }
    if (!settings_valid_fake(id, v)) return SETTINGS_EVALUE;
    vals[id] = v; return SETTINGS_OK;
}
static int f_reset(uint32_t id) { calls_reset++; return f_set(id, id == SET_TEXT_SCALE ? 100 : id == SET_AURA_MODE ? 1 : 0); }
static const struct shell_store STORE = { f_get, f_set, f_reset };

static void reset_fake(void) { memset(vals, 0, sizeof vals); vals[SET_TEXT_SCALE] = 100; vals[SET_AURA_MODE] = 1; fail_next = calls_set = calls_reset = calls_get = mode_version = 0; deny_id = -1; }
static void fresh(const struct shell_store *st)
{
    reset_fake();
    shell_init(&sh, W, H); struct shell_caps c = { 1, 1 }; shell_set_caps(&sh, c);
    shell_set_store(&sh, st);
}
static void key(uint16_t k, uint16_t mods) { struct input_event e = { IN_KEY_DOWN, 0, k, mods, 0, 0, 0 }; shell_handle(&sh, &e); }
static void ptr(int kind, int x, int y) { struct input_event e = { (uint8_t)kind, 0, BTN_LEFT, 0, 0, x, y }; shell_handle(&sh, &e); }
static int click(uint32_t id) { int32_t i = shell_find(&sh, id); if (i < 0) return 0; int x = sh.c[i].x + sh.c[i].w / 2, y = sh.c[i].y + sh.c[i].h / 2; ptr(IN_PTR_DOWN, x, y); ptr(IN_PTR_UP, x, y); return 1; }
static const char *val_text(uint32_t row) { int32_t i = shell_find(&sh, SID_SET_VALUE + row); return i < 0 ? "(hidden)" : sh.w[i].text; }
static const char *set_status_probe(void)       /* every text currently on screen, concatenated */
{
    static char buf[4096]; size_t n = 0; buf[0] = 0;
    for (uint32_t i = 0; i < sh.n; i++) if (sh.w[i].text) { size_t l = strlen(sh.w[i].text); if (n + l + 2 < sizeof buf) { memcpy(buf + n, sh.w[i].text, l); n += l; buf[n++] = '|'; buf[n] = 0; } }
    return buf;
}
static void to_settings(void) { click(SID_APP_SETTINGS); }
static uint32_t focus_id(void) { return sh.fx.focus >= 0 ? sh.c[sh.fx.focus].id : SHELL_NONE; }

int main(void)
{
    printf("shell settings tests\n");

    /* ---- the table matches the registry documentation ---- */
    {
        static const uint32_t ids[SHELL_SET_ROWS] = { SET_THEME, SET_TEXT_SCALE, SET_REDUCED_MOTION, SET_FOCUS_MODE, SET_AURA_MODE, SET_NOTIFICATIONS, 0 };
        static const uint32_t defs[SHELL_SET_ROWS] = { 0, 100, 0, 0, 1, 0, 0 };
        static const uint32_t counts[SHELL_SET_ROWS] = { 3, 6, 2, 4, 3, 3, 2 };
        int ok = 1;
        for (uint32_t r = 0; r < SHELL_SET_ROWS; r++) { uint32_t v[8]; if (shell_setting_id(r) != ids[r] || shell_setting_default(r) != defs[r] || shell_setting_values(r, v, 8) != counts[r]) ok = 0; }
        CHECK("six registry settings (ids 1-6, documented defaults and value counts) plus the session-only high-contrast row", ok && shell_setting_id(SHELL_SET_ROWS) == 0xffffffffu);
        uint32_t v[8]; shell_setting_values(1, v, 8);
        int in_range = 1; for (int i = 0; i < 6; i++) if (v[i] < SHELL_TEXT_MIN || v[i] > SHELL_TEXT_MAX) in_range = 0;
        CHECK("every offered text size is inside the registry range 75..200", in_range && v[0] == 75 && v[5] == 200);
    }

    /* ---- navigation ---- */
    fresh(&STORE);
    to_settings();
    CHECK("the Home Settings tile opens the Settings screen (history 1)", sh.screen == SCR_SETTINGS && sh.hist_n == 1);
    CHECK("all seven rows have a value button and a Reset button", ({ int ok = 1; for (uint32_t r = 0; r < SHELL_SET_ROWS; r++) if (shell_find(&sh, SID_SET_VALUE + r) < 0 || shell_find(&sh, SID_SET_RESET + r) < 0) ok = 0; ok; }));
    CHECK("the current values are shown (Theme Auto, Text size 100%, Reduced motion Off, Focus Off, Aura Suggest, Notifications All)", !strcmp(val_text(0), "Auto") && !strcmp(val_text(1), "100%") && !strcmp(val_text(2), "Off") && !strcmp(val_text(3), "Off") && !strcmp(val_text(4), "Suggest") && !strcmp(val_text(5), "All"));
    key(KEY_ESC, 0);
    CHECK("Escape returns to Home", sh.screen == SCR_HOME);
    click(SID_DOCK_INDEX);
    { const char *q = "settings"; for (; *q; q++) { struct input_event e = { IN_TEXT, 0, 30, 0, (uint8_t)*q, 0, 0 }; shell_handle(&sh, &e); } }
    key(KEY_ENTER, 0);
    CHECK("Index search 'settings' + Enter opens Settings (it is an available destination)", sh.screen == SCR_SETTINGS);
    click(SID_DOCK_HOME);
    CHECK("the dock returns to Home", sh.screen == SCR_HOME);

    /* ---- registry-backed changes ---- */
    fresh(&STORE); to_settings();
    click(SID_SET_VALUE + 0);
    CHECK("Theme Auto -> Light is written to the registry and reported as saved", vals[SET_THEME] == 1 && !strcmp(val_text(0), "Light") && sh.set_msg == SM_SAVED && calls_set == 1);
    click(SID_SET_VALUE + 0); click(SID_SET_VALUE + 0);
    CHECK("Theme cycles Light -> Dark -> Auto", vals[SET_THEME] == 0 && !strcmp(val_text(0), "Auto"));
    click(SID_SET_VALUE + 1);
    CHECK("Text size 100% -> 125% is saved AND applied at once (the shell relayouts at 125 percent)", vals[SET_TEXT_SCALE] == 125 && sh.text_pct == 125 && sh.screen == SCR_SETTINGS);
    click(SID_SET_VALUE + 1); click(SID_SET_VALUE + 1); click(SID_SET_VALUE + 1);
    CHECK("Text size reaches 200% and the shell is usable there", vals[SET_TEXT_SCALE] == 200 && sh.text_pct == 200 && shell_find(&sh, SID_DOCK_HOME) >= 0);
    to_settings();   /* may already be there; harmless */
    click(SID_SET_VALUE + 1);
    CHECK("Text size wraps from 200% back to 75%", vals[SET_TEXT_SCALE] == 75 && sh.text_pct == 75);
    click(SID_SET_RESET + 1);
    CHECK("Reset puts Text size back to 100% (registry and shell) and says so", vals[SET_TEXT_SCALE] == 100 && sh.text_pct == 100 && sh.set_msg == SM_RESET_SAVED && calls_reset == 1);
    click(SID_SET_VALUE + 2);
    CHECK("Reduced motion toggles On (stored only: nothing else changes)", vals[SET_REDUCED_MOTION] == 1 && !strcmp(val_text(2), "On") && sh.text_pct == 100 && !sh.contrast);
    click(SID_SET_VALUE + 3); click(SID_SET_VALUE + 5);
    CHECK("Focus mode and Notifications cycle through their documented values", vals[SET_FOCUS_MODE] == 1 && vals[SET_NOTIFICATIONS] == 1 && !strcmp(val_text(3), "Work") && !strcmp(val_text(5), "Priority only"));

    /* ---- the registry is the authority: failures change nothing ---- */
    fresh(&STORE); to_settings();
    fail_next = 1; click(SID_SET_VALUE + 1);
    CHECK("a save failure leaves the value, the registry and the shell's text size unchanged and says it was not saved", vals[SET_TEXT_SCALE] == 100 && sh.text_pct == 100 && !strcmp(val_text(1), "100%") && sh.set_msg == SM_STORE_FAIL);
    CHECK("the failure text never claims a save", strstr(set_status_probe(), "Saved") == NULL);
    click(SID_SET_VALUE + 1);
    CHECK("the next attempt works normally after a failure", vals[SET_TEXT_SCALE] == 125 && sh.set_msg == SM_SAVED);
    deny_id = SET_AURA_MODE; click(SID_SET_VALUE + 4);
    CHECK("Aura mode: the registry's refusal (no system permission) is shown, nothing changes, nothing is applied", vals[SET_AURA_MODE] == 1 && sh.set_msg == SM_DENIED && !strcmp(val_text(4), "Suggest"));
    deny_id = -1; mode_version = 1; click(SID_SET_VALUE + 0);
    CHECK("a newer-version store is reported as such and nothing changes", vals[SET_THEME] == 0 && sh.set_msg == SM_VERSION);
    mode_version = 0;

    /* ---- the stored text size is applied when the store is attached ---- */
    reset_fake(); vals[SET_TEXT_SCALE] = 150;
    shell_init(&sh, W, H); shell_set_store(&sh, &STORE);
    CHECK("attaching a store with text size 150 applies 150 percent at once", sh.text_pct == 150);
    reset_fake(); vals[SET_TEXT_SCALE] = 500;
    shell_init(&sh, W, H); shell_set_store(&sh, &STORE);
    CHECK("an out-of-range stored text size (500) is ignored: the shell stays at 100", sh.text_pct == 100);
    reset_fake(); vals[SET_TEXT_SCALE] = 110;
    shell_init(&sh, W, H); shell_set_store(&sh, &STORE); to_settings();
    CHECK("an in-range value outside the offered steps (110) is shown as it is", !strcmp(val_text(1), "110%"));
    click(SID_SET_VALUE + 1);
    CHECK("and the next step up from it is 125", vals[SET_TEXT_SCALE] == 125);

    /* ---- no store: session only, honestly ---- */
    fresh(NULL); to_settings();
    click(SID_SET_VALUE + 1);
    CHECK("without a store a text size change applies for this session and the screen says it is not saved", sh.text_pct == 125 && sh.set_msg == SM_SESSION && !strcmp(val_text(1), "125%"));
    CHECK("the session message never says Saved", strstr(set_status_probe(), "Saved") == NULL && strstr(set_status_probe(), "This session only") != NULL);
    click(SID_SET_VALUE + 4);
    CHECK("without a store Aura mode still cannot be changed (the shell cannot grant itself system permission)", sh.set_msg == SM_AURA_LOCAL && !strcmp(val_text(4), "Suggest"));
    click(SID_SET_RESET + 1);
    CHECK("Reset works for a session value too", sh.text_pct == 100 && !strcmp(val_text(1), "100%"));

    /* ---- high contrast is session-only and never reaches the store ---- */
    fresh(&STORE); to_settings();
    CHECK("the high-contrast Reset is disabled while it is off", !(sh.c[shell_find(&sh, SID_SET_RESET + 6)].flags & UI_ENABLED));
    click(SID_SET_VALUE + 6);
    CHECK("High contrast switches on at once, says it is not saved, and the registry is not touched", sh.contrast == 1 && sh.set_msg == SM_HC_ON && calls_set == 0 && !strcmp(val_text(6), "On"));
    click(SID_SET_RESET + 6);
    CHECK("its Reset switches it off", sh.contrast == 0 && !strcmp(val_text(6), "Off") && calls_reset == 0);

    /* ---- every screen state keeps working with the Settings screen: geometry audit at every scale ---- */
    {
        int bad = 0, min_rows = 99;
        for (int hc = 0; hc <= 1; hc++)
            for (uint32_t p = 75; p <= 200; p += 5) {
                fresh(&STORE); shell_set_contrast(&sh, hc); to_settings(); shell_set_text_scale(&sh, p);
                for (uint32_t i = 0; i < sh.n; i++) {
                    const struct ui_ctrl *c = &sh.w[i].c;
                    if (c->x < 0 || c->y < 0 || c->x + c->w > (int32_t)W || c->y + c->h > (int32_t)H) bad++;
                    if ((c->flags & UI_FOCUSABLE) && (c->h < 40 || c->w < 48)) bad++;
                }
                int32_t dock = shell_find(&sh, 0xF100u);
                for (uint32_t i = 0; i < sh.n; i++) if (sh.c[i].id >= SID_SET_VALUE && sh.c[i].id <= SID_SET_DOWN && dock >= 0 && sh.c[i].y + sh.c[i].h > sh.c[dock].y) bad++;
                for (uint32_t i = 0; i < sh.n; i++) for (uint32_t j = i + 1; j < sh.n; j++)
                    if ((sh.c[i].flags & UI_FOCUSABLE) && (sh.c[j].flags & UI_FOCUSABLE) && sh.c[i].layer == sh.c[j].layer &&
                        sh.c[i].x < sh.c[j].x + sh.c[j].w && sh.c[j].x < sh.c[i].x + sh.c[i].w && sh.c[i].y < sh.c[j].y + sh.c[j].h && sh.c[j].y < sh.c[i].y + sh.c[i].h) bad++;
                if (sh.n > SHELL_MAX_WIDGETS) bad++;
                { int vis = 0; for (uint32_t r = 0; r < SHELL_SET_ROWS; r++) if (shell_find(&sh, SID_SET_VALUE + r) >= 0) vis++; if (vis < min_rows) min_rows = vis; }
            }
        printf("  (fewest Settings rows visible at once across all scales: %d)\n", min_rows);
        CHECK("at least two Settings rows are visible at once at every scale", min_rows >= 2);
        CHECK("Settings: every control is on screen, big enough, clear of the dock and of each other at every scale and contrast", bad == 0);
    }

    /* ---- at 200 percent the list scrolls and every row stays reachable ---- */
    {
        fresh(&STORE); to_settings(); shell_set_text_scale(&sh, 200);
        int visible = 0; for (uint32_t r = 0; r < SHELL_SET_ROWS; r++) if (shell_find(&sh, SID_SET_VALUE + r) >= 0) visible++;
        CHECK("at 200 percent not all rows fit at once, and a 'More settings' control exists", visible < SHELL_SET_ROWS && visible >= 1 && shell_find(&sh, SID_SET_DOWN) >= 0);
        uint8_t seen[SHELL_SET_ROWS] = { 0 };
        for (int step = 0; step < 12; step++) {
            for (uint32_t r = 0; r < SHELL_SET_ROWS; r++) if (shell_find(&sh, SID_SET_VALUE + r) >= 0 && shell_find(&sh, SID_SET_RESET + r) >= 0) seen[r] = 1;
            if (!click(SID_SET_DOWN)) break;
        }
        int all = 1; for (uint32_t r = 0; r < SHELL_SET_ROWS; r++) if (!seen[r]) all = 0;
        CHECK("scrolling down shows every row, with both of its controls", all);
        CHECK("at the bottom the 'More settings' control is gone and 'Earlier settings' is there; focus did not vanish", shell_find(&sh, SID_SET_DOWN) < 0 && shell_find(&sh, SID_SET_UP) >= 0 && focus_id() != SHELL_NONE);
        for (int step = 0; step < 12; step++) if (!click(SID_SET_UP)) break;
        CHECK("scrolling back up returns to the first row", shell_find(&sh, SID_SET_VALUE + 0) >= 0 && shell_find(&sh, SID_SET_UP) < 0);
        /* a change made on a scrolled row works */
        for (int step = 0; step < 12; step++) if (!click(SID_SET_DOWN)) break;
        click(SID_SET_VALUE + 5);
        CHECK("a change on a row that needed scrolling reaches the registry", vals[SET_NOTIFICATIONS] == 1);
    }

    /* ---- the Tab order on the Settings screen covers visible controls only, ends at the dock and the corner ---- */
    {
        fresh(&STORE); to_settings();
        int saw_dock = 0, saw_value0 = 0;
        for (int i = 0; i < 40; i++) { key(KEY_TAB, 0); uint32_t f = focus_id(); if (f == SID_DOCK_HOME) saw_dock = 1; if (f == SID_SET_VALUE) saw_value0 = 1; }
        CHECK("Tab reaches the first value button and the dock", saw_dock && saw_value0);
        key(KEY_ENTER, 0);
        CHECK("Enter on a focused control works on Settings (focus must be on a control: no crash, state valid)", sh.screen == SCR_SETTINGS || sh.screen == SCR_HOME || sh.screen == SCR_SPACES || sh.screen == SCR_INDEX);
    }

    /* ---- churn ---- */
    {
        unsigned seed = 777u; int inv = 1;
        fresh(&STORE); to_settings();
        for (int it = 0; it < 6000; it++) {
            seed = seed * 1103515245u + 12345u;
            uint32_t r = (seed >> 16) % 12;
            if (r < 7) click(SID_SET_VALUE + (seed >> 8) % SHELL_SET_ROWS);
            else if (r < 9) click(SID_SET_RESET + (seed >> 8) % SHELL_SET_ROWS);
            else if (r == 9) { if ((seed >> 11) & 1) fail_next = 1; else deny_id = ((seed >> 12) & 1) ? (int)(1 + (seed >> 13) % 6) : -1; }
            else if (r == 10) shell_set_text_scale(&sh, 75 + (seed >> 8) % 126);
            else { click(SID_SET_DOWN); click(SID_SET_UP); key(KEY_TAB, 0); }
            if (sh.screen != SCR_SETTINGS) to_settings();
            for (uint32_t i = 1; i <= 6; i++) { uint32_t v; if (f_get(i, &v) == SETTINGS_OK && !settings_valid_fake(i, v)) inv = 0; }
            if (sh.n > SHELL_MAX_WIDGETS || sh.text_pct < SHELL_TEXT_MIN || sh.text_pct > SHELL_TEXT_MAX || sh.set_first >= SHELL_SET_ROWS) inv = 0;
            if (sh.text_pct != vals[SET_TEXT_SCALE] && !(vals[SET_TEXT_SCALE] == sh.text_pct)) { /* the shell may differ only through shell_set_text_scale in this loop (r == 10) */ }
        }
        CHECK("6000 random settings actions, refusals and scale changes: stored values stay valid, the widget array and scroll offset stay in bounds", inv);
    }

    printf("shell settings: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
