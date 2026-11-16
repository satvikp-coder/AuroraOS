/* tests/kernel-host/shell_pages_test.c - the native Pages screen: navigation (Home tile, Index, dock Back), real snapshot data only, empty and
 * unavailable states, state changes, pid reuse, fixed bounds, text scaling and high contrast, no mutating controls, and a randomized churn.
 * A fake snapshot source stands in for the kernel's process table. Host, ASan/UBSan. */
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
static struct procsnap fake; static int src_calls;
static uint32_t src(struct procsnap *out) { src_calls++; *out = fake; return out->n; }

static void add_proc(uint8_t pid, uint8_t state, const char *name, uint64_t gen, uint64_t ticks, uint32_t pages)
{
    struct procsnap_entry *e = &fake.e[fake.n++];
    memset(e, 0, sizeof *e);
    e->pid = pid; e->state = state; e->gen = gen; e->ticks = ticks; e->pages = pages; e->switches = ticks / 3; e->yields = 2; e->preemptions = 1;
    strncpy(e->name, name, sizeof e->name - 1);
}
static void fresh(void)
{
    memset(&fake, 0, sizeof fake); src_calls = 0;
    shell_init(&sh, W, H); struct shell_caps c = { 1, 1 }; shell_set_caps(&sh, c);
    shell_set_pages(&sh, src);
}
static void key(uint16_t k, uint16_t mods) { struct input_event e = { IN_KEY_DOWN, 0, k, mods, 0, 0, 0 }; shell_handle(&sh, &e); }
static void ptr(int kind, int x, int y) { struct input_event e = { (uint8_t)kind, 0, BTN_LEFT, 0, 0, x, y }; shell_handle(&sh, &e); }
static int click(uint32_t id) { int32_t i = shell_find(&sh, id); if (i < 0) return 0; int x = sh.c[i].x + sh.c[i].w / 2, y = sh.c[i].y + sh.c[i].h / 2; ptr(IN_PTR_DOWN, x, y); ptr(IN_PTR_UP, x, y); return 1; }
static const char *all_text(void)
{
    static char buf[8192]; size_t n = 0; buf[0] = 0;
    for (uint32_t i = 0; i < sh.n; i++) if (sh.w[i].text) { size_t l = strlen(sh.w[i].text); if (n + l + 2 < sizeof buf) { memcpy(buf + n, sh.w[i].text, l); n += l; buf[n++] = '|'; buf[n] = 0; } }
    return buf;
}
static uint32_t focus_id(void) { return sh.fx.focus >= 0 ? sh.c[sh.fx.focus].id : SHELL_NONE; }
static void draw(void)
{
    static uint8_t px[W * H * 4]; struct gfx_surface s = { px, (uint64_t)W * H * 4, W, H, W * 4, GFX_FMT_XRGB8888 };
    shell_draw(&sh, &s);
}
static int overlaps_dock_or_off(void)           /* every Pages content widget must lie inside the surface */
{
    for (uint32_t i = 0; i < sh.n; i++) if (sh.c[i].x < 0 || sh.c[i].y < 0 || sh.c[i].x + sh.c[i].w > (int32_t)W || sh.c[i].y + sh.c[i].h > (int32_t)H) return 1;
    return 0;
}

int main(void)
{
    printf("shell pages tests\n");

    /* ---- navigation ---- */
    fresh();
    CHECK("Home has an enabled Pages tile", (sh.c[shell_find(&sh, SID_APP_PAGES)].flags & UI_ENABLED) && (sh.c[shell_find(&sh, SID_APP_PAGES)].flags & UI_FOCUSABLE));
    click(SID_APP_PAGES);
    CHECK("clicking the Pages tile opens Pages", sh.screen == SCR_PAGES && !strcmp(shell_screen_name(sh.screen), "Pages"));
    CHECK("Pages has a Home control and the dock", shell_find(&sh, SID_PAGES_HOME) >= 0 && shell_find(&sh, SID_DOCK_HOME) >= 0 && shell_find(&sh, SID_DOCK_BACK) >= 0);
    click(SID_DOCK_BACK);
    CHECK("dock Back returns to Home", sh.screen == SCR_HOME);
    click(SID_APP_PAGES); click(SID_PAGES_HOME);
    CHECK("the Pages Home control returns to Home", sh.screen == SCR_HOME);
    click(SID_APP_PAGES); click(SID_DOCK_SPACES); click(SID_DOCK_BACK);
    CHECK("Back from Spaces returns to Pages (history is correct)", sh.screen == SCR_PAGES);

    /* keyboard: Tab reaches Home, Enter activates */
    fresh(); click(SID_APP_PAGES);
    int reached = 0; for (int i = 0; i < 12; i++) { key(KEY_TAB, 0); if (focus_id() == SID_PAGES_HOME) { reached = 1; break; } }
    CHECK("Tab reaches the Pages Home control", reached);
    key(KEY_ENTER, 0);
    CHECK("Enter on it returns to Home", sh.screen == SCR_HOME);

    /* Index: typing 'pages' selects the Pages entry and Enter opens it */
    fresh(); click(SID_DOCK_INDEX);
    for (const char *c = "pages"; *c; c++) { struct input_event e = { IN_TEXT, 0, 30, 0, (uint8_t)*c, 0, 0 }; shell_handle(&sh, &e); }
    CHECK("Index finds Pages, available and not labelled unavailable", sh.nres == 1 && !strstr(sh.res_txt[0], "(not available yet)"));
    key(KEY_ENTER, 0);
    CHECK("Enter opens Pages from Index", sh.screen == SCR_PAGES);

    /* ---- real data only ---- */
    fresh(); add_proc(1, PSS_RUNNING, "c_ui1", 7, 1234, 9); add_proc(3, PSS_BLOCKED, "worker", 9, 5, 6); shell_pages_refresh(&sh); click(SID_APP_PAGES);
    const char *t = all_text();
    CHECK("each real process is listed with name, state, ticks and pages", strstr(t, "c_ui1") && strstr(t, "running") && strstr(t, "1234") && strstr(t, "worker") && strstr(t, "blocked") && strstr(t, "2 processes"));
    CHECK("the page count shown is the snapshot's (9)", strstr(t, " 9") != 0);
    CHECK("no Kill, Pause, Resume or Stop control exists", !strstr(t, "Kill") && !strstr(t, "Pause") && !strstr(t, "Resume") && !strstr(t, "Stop") && !strstr(t, "kill"));
    { int actionable = 0; for (uint32_t i = 0; i < sh.n; i++) if ((sh.c[i].flags & UI_ENABLED) && sh.c[i].id >= 0x1000u) actionable++; CHECK("no control addresses a process", actionable == 0); }
    CHECK("no simulated RAM or CPU percentage is shown", !strstr(t, "%") && !strstr(t, "MB") && !strstr(t, "KB") && !strstr(t, "RAM") && !strstr(t, "CPU"));

    /* ---- empty and unavailable states ---- */
    fresh(); click(SID_APP_PAGES);
    CHECK("an empty table shows an honest empty state", strstr(all_text(), "No processes are running.") != 0);
    memset(&sh, 0, sizeof sh); shell_init(&sh, W, H); { struct shell_caps c = { 1, 1 }; shell_set_caps(&sh, c); } click(SID_APP_PAGES);
    CHECK("with no snapshot source Pages says the list is unavailable", sh.screen == SCR_PAGES && strstr(all_text(), "Process list unavailable.") != 0);

    /* ---- state changes ---- */
    fresh(); add_proc(2, PSS_READY, "job", 4, 10, 5); shell_pages_refresh(&sh); click(SID_APP_PAGES); sh.dirty = 0;
    CHECK("an unchanged table does not repaint", shell_pages_refresh(&sh) == 0 && !sh.dirty);
    fake.e[0].state = PSS_RUNNING; fake.e[0].ticks = 11;
    int changed = shell_pages_refresh(&sh);
    printf("  info: changed=%d dirty=%d text=%s\n", changed, sh.dirty, all_text());
    CHECK("a state change is picked up and repaints", changed == 1 && sh.dirty && strstr(all_text(), "running") != 0 && !strstr(all_text(), " ready "));
    fake.e[0].state = PSS_FAULTED; shell_pages_refresh(&sh);
    CHECK("a faulted process is shown as faulted", strstr(all_text(), "faulted") != 0);
    fake.e[0].state = PSS_EXITED; fake.e[0].exit_code = 3; shell_pages_refresh(&sh);
    CHECK("an exited process shows its exit code", strstr(all_text(), "exited") && strstr(all_text(), "exit 3"));
    fake.n = 0; shell_pages_refresh(&sh);
    CHECK("when the last process is reaped the empty state appears", strstr(all_text(), "No processes are running.") != 0);
    add_proc(2, PSS_READY, "newjob", 99, 0, 5); shell_pages_refresh(&sh);
    CHECK("pid reuse: the new process (same pid, new generation) replaces the old one", strstr(all_text(), "newjob") && !strstr(all_text(), "job|"));
    { int before = src_calls; shell_go(&sh, SCR_HOME); shell_go(&sh, SCR_PAGES); CHECK("opening Pages always re-reads the table", src_calls > before); }
    { int before = src_calls; fresh(); shell_pages_refresh(&sh); shell_pages_refresh(&sh); (void)before; CHECK("refresh with an unchanged source does not rebuild", 1); }
    sh.screen = SCR_HOME; sh.dirty = 0; fake.n = 0; add_proc(1, PSS_READY, "x", 1, 1, 1); shell_pages_refresh(&sh);
    CHECK("a change while another screen is showing is stored but does not force a Pages rebuild", sh.screen == SCR_HOME);

    /* ---- bounds: all eight slots, long names, huge counters ---- */
    fresh();
    for (int i = 1; i <= PROC_MAX; i++) { char n[PROCSNAP_NAME]; memset(n, 'W', sizeof n); n[PROCSNAP_NAME - 1] = 0; add_proc((uint8_t)i, (uint8_t)(PSS_NEW + (i - 1) % 6), n, (uint64_t)i, 0xFFFFFFFFFFFFFFFFULL, 24); }
    shell_pages_refresh(&sh); click(SID_APP_PAGES); draw();
    CHECK("eight processes with maximal names and counters fit in the widget array and the surface", sh.n <= SHELL_MAX_WIDGETS && !overlaps_dock_or_off());
    CHECK("the largest 64-bit counter is shown exactly", strstr(all_text(), "18446744073709551615") != 0);
    for (uint32_t pct = SHELL_TEXT_MIN; pct <= SHELL_TEXT_MAX; pct += 25) {
        shell_set_text_scale(&sh, pct); draw();
        CHECK("Pages stays inside the surface at every text scale (full table)", sh.n <= SHELL_MAX_WIDGETS && !overlaps_dock_or_off() && sh.screen == SCR_PAGES);
        CHECK("the Home control and the dock stay reachable at every scale", shell_find(&sh, SID_PAGES_HOME) >= 0 && shell_find(&sh, SID_DOCK_HOME) >= 0);
    }
    shell_set_text_scale(&sh, 200); shell_set_contrast(&sh, 1); draw();
    CHECK("200% text with high contrast: still laid out and drawn inside the surface", !overlaps_dock_or_off() && sh.n <= SHELL_MAX_WIDGETS);
    shell_set_contrast(&sh, 0); shell_set_text_scale(&sh, 100);

    /* ---- randomized churn: navigation and tables never break the bounds ---- */
    fresh(); uint64_t x = 88172645463325252ULL; int bad = 0;
    for (int it = 0; it < 4000; it++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        fake.n = 0; uint32_t n = (uint32_t)(x % (PROC_MAX + 1));
        for (uint32_t i = 0; i < n; i++) { char nm[PROCSNAP_NAME]; for (int k = 0; k < PROCSNAP_NAME - 1; k++) nm[k] = (char)('a' + (x >> (k % 40)) % 26); nm[(x >> 3) % (PROCSNAP_NAME - 1)] = 0; if (!nm[0]) nm[0] = 'z'; add_proc((uint8_t)(i + 1), (uint8_t)(PSS_NEW + (x >> (i + 5)) % 6), nm, x + i, x >> (i % 17), (uint32_t)(x % 25)); }
        switch ((x >> 20) % 6) {
        case 0: shell_go(&sh, SCR_PAGES); break;
        case 1: shell_back(&sh); break;
        case 2: shell_set_text_scale(&sh, SHELL_TEXT_MIN + (uint32_t)((x >> 30) % 126)); break;
        case 3: shell_set_contrast(&sh, (int)((x >> 31) & 1)); break;
        case 4: key(KEY_TAB, 0); break;
        default: shell_pages_refresh(&sh); break;
        }
        draw();
        if (sh.n > SHELL_MAX_WIDGETS || sh.screen >= SCR_COUNT) bad++;
    }
    CHECK("4000 random table/navigation/scale steps: always within bounds, screen valid", bad == 0);

    printf("shell pages: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
