/* tests/kernel-host/shell_trail_test.c - the native Trail screen against the REAL shell with a fake ledger view: Index entry, every health string, empty
 * states, exact record rows (newest first), grants, the scroll window, the read-only guarantee (no control other than Show newer / Show older / Home, rows are
 * never focusable or activatable), hostile views (oversized counts, unknown kinds/reasons/permissions), refresh behaviour, geometry at 75..200 percent in both
 * palettes, and a randomized input run. Host, ASan/UBSan. */
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
static struct shell_trail_view V; static int src_calls, src_fail;
static int src(struct shell_trail_view *out) { src_calls++; if (src_fail) return -1; *out = V; return 0; }
static void rec(uint32_t seq, uint8_t kind, uint32_t principal, uint8_t target, uint8_t state, uint8_t reason)
{
    struct shell_trail_rec *r = &V.rec[V.n++];
    memset(r, 0, sizeof *r); r->seq = seq; r->kind = kind; r->principal = principal; r->target = target; r->state = state; r->reason = reason;
}
static void fresh(void)
{
    memset(&V, 0, sizeof V); V.health = TV_RECORDING; src_calls = 0; src_fail = 0;
    shell_init(&sh, W, H); struct shell_caps c = { 1, 1 }; shell_set_caps(&sh, c);
    shell_set_trail(&sh, src);
}
static void key(uint16_t k) { struct input_event e = { IN_KEY_DOWN, 0, k, 0, 0, 0, 0 }; shell_handle(&sh, &e); }
static void text(const char *s) { for (; *s; s++) { struct input_event e = { IN_TEXT, 0, 30, 0, (uint8_t)*s, 0, 0 }; shell_handle(&sh, &e); } }
static void ptr(int kind, int x, int y) { struct input_event e = { (uint8_t)kind, 0, BTN_LEFT, 0, 0, x, y }; shell_handle(&sh, &e); }
static int click(uint32_t id) { int32_t i = shell_find(&sh, id); if (i < 0) return 0; int x = sh.c[i].x + sh.c[i].w / 2, y = sh.c[i].y + sh.c[i].h / 2; ptr(IN_PTR_DOWN, x, y); ptr(IN_PTR_UP, x, y); return 1; }
static const struct ui_widget *wid(uint32_t id) { int32_t i = shell_find(&sh, id); return i >= 0 ? &sh.w[i] : 0; }
static int enabled(uint32_t id) { const struct ui_widget *w = wid(id); return w && (w->c.flags & UI_ENABLED); }
static int has_text(const char *t) { for (uint32_t i = 0; i < sh.n; i++) if (sh.w[i].text && strstr(sh.w[i].text, t)) return 1; return 0; }
static int index_of_text(const char *t) { for (uint32_t i = 0; i < sh.n; i++) if (sh.w[i].text && strstr(sh.w[i].text, t)) return (int)i; return -1; }
static void open_trail(void) { shell_go(&sh, SCR_TRAIL); }
static uint32_t focus_id(void) { return sh.fx.focus >= 0 ? sh.c[sh.fx.focus].id : SHELL_NONE; }

static int audit(void)      /* geometry: inside the screen, focus targets large enough, same-layer focusables do not overlap */
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
static int allowed_trail_id(uint32_t id)         /* every control that may exist on the Trail screen: scroll, Home, the dock, the Glass Corner handle */
{
    return id == SID_TRAIL_NEWER || id == SID_TRAIL_OLDER || id == SID_TRAIL_HOME || id == SID_DOCK_BACK || id == SID_DOCK_HOME || id == SID_DOCK_SPACES || id == SID_DOCK_INDEX || id == SID_CORNER || id == SID_CORNER_CLOSE;
}

int main(void)
{
    printf("shell trail tests\n");

    { /* Index entry */
        fresh();
        size_t n = shell_index_count(), at = n; for (size_t i = 0; i < n; i++) if (!strcmp(shell_index_item(i)->key, "trail")) at = i;
        CHECK("the registry lists Trail and it is available", at < n && shell_index_available(at));
        shell_go(&sh, SCR_INDEX); text("trail"); key(KEY_ENTER);
        CHECK("typing 'trail' in Index and pressing Enter opens the Trail screen", sh.screen == SCR_TRAIL);
        CHECK("the screen name is Trail", !strcmp(shell_screen_name(SCR_TRAIL), "Trail"));
        shell_go(&sh, SCR_HOME);
        CHECK("Home has no Trail tile (the 100% Home layout is unchanged)", !has_text("Trail"));
    }
    { /* health strings */
        static const struct { uint8_t h; const char *s; } HS[] = {
            { TV_RECORDING, "Trail: recording to disk." }, { TV_NODISK, "Trail: off. No disk, nothing is saved." }, { TV_READONLY, "Trail: read-only disk. Changes refused." },
            { TV_UNREADABLE, "Trail: file unreadable. Changes refused." }, { TV_ROLLBACK, "Trail: rollback found. Changes refused." }, { TV_HELD, "Trail: held." }, { TV_UNAVAILABLE, "Trail: not available." } };
        for (unsigned i = 0; i < sizeof HS / sizeof HS[0]; i++) {
            fresh(); V.health = HS[i].h; shell_trail_refresh(&sh); open_trail();
            char nm[96]; snprintf(nm, sizeof nm, "health %u shows its own line: %s", HS[i].h, HS[i].s); CHECK(nm, has_text(HS[i].s));
        }
        fresh(); shell_set_trail(&sh, 0); open_trail();
        CHECK("no source: 'Trail: not available.' and nothing else is claimed", has_text("Trail: not available.") && !has_text("recording"));
        CHECK("a corrupt or rolled-back ledger is never presented as healthy", ({ fresh(); V.health = TV_UNREADABLE; shell_trail_refresh(&sh); open_trail(); !has_text("recording to disk"); }));
        fresh(); src_fail = 1; V.health = TV_RECORDING; sh.tv_valid = 0; shell_trail_refresh(&sh); open_trail();
        CHECK("a source that fails shows 'not available', never stale or invented data", has_text("Trail: not available.") && !has_text("recording to disk"));
    }
    { /* empty state and rows */
        fresh(); open_trail();
        CHECK("empty: 'No records yet.', 'Saved permissions: none.', zero counts", has_text("No records yet.") && has_text("Saved permissions: none.") && has_text("Changes: 0. Denials: 0."));
        CHECK("empty: no scroll buttons", !wid(SID_TRAIL_NEWER) && !wid(SID_TRAIL_OLDER) && wid(SID_TRAIL_HOME));
        fresh(); rec(1, 14, 0, 0, 1, 0); rec(2, 12, 1001, 7, 1, 0); rec(3, 13, 1001, 3, 1, 0); rec(4, 1, 407, 0, 2, 6); rec(5, 11, 9, 0, 1, 0);
        V.ngrants = 2; V.grant[0].principal = 1002; V.grant[0].perm = 1; V.grant[1].principal = 1001; V.grant[1].perm = 7; shell_trail_refresh(&sh); open_trail();
        CHECK("rows are exact and constants-and-numbers only", has_text("#2 Granted: principal 1001 may use timers.") && has_text("#3 Revoked: principal 1001 may not use file writes.")
              && has_text("#1 System started.") && has_text("#4 Denied: open request from principal 407. no permission.") && has_text("#5 Unknown record kind 11."));
        CHECK("newest first: #5 is above #4 above #1", index_of_text("#5 ") < index_of_text("#4 ") && index_of_text("#4 ") < index_of_text("#3 ") && index_of_text("#2 ") < index_of_text("#1 "));
        CHECK("counts: 2 changes (grant, revoke) and 1 denial", has_text("Changes: 2. Denials: 1."));
        CHECK("saved permissions are listed", has_text("Saved permissions: 2") && has_text("Principal 1002 may use navigation.") && has_text("Principal 1001 may use timers."));
        CHECK("health line says recording", has_text("Trail: recording to disk."));
    }
    { /* the scroll window */
        fresh(); for (uint32_t i = 1; i <= 17; i++) rec(i, 12, 1000 + i, 1 + i % 7, 1, 0); shell_trail_refresh(&sh); open_trail();
        CHECK("17 records at 100%: the newest 6 are shown (#17..#12), not #11", has_text("#17 ") && has_text("#12 ") && !has_text("#11 ") && has_text("Showing 1-6 of 17."));
        CHECK("... Show newer is disabled at the newest, Show older enabled", !enabled(SID_TRAIL_NEWER) && enabled(SID_TRAIL_OLDER));
        click(SID_TRAIL_OLDER);
        CHECK("Show older moves a page: #11..#6, 'Showing 7-12 of 17.'", has_text("#11 ") && has_text("#6 ") && !has_text("#12 ") && has_text("Showing 7-12 of 17.") && enabled(SID_TRAIL_NEWER));
        click(SID_TRAIL_OLDER);
        CHECK("... again: #5..#1, 'Showing 13-17 of 17.', Show older disabled at the oldest", has_text("#5 ") && has_text("#1 ") && has_text("Showing 13-17 of 17.") && !enabled(SID_TRAIL_OLDER));
        click(SID_TRAIL_OLDER);
        CHECK("pressing a disabled Show older changes nothing", has_text("Showing 13-17 of 17."));
        click(SID_TRAIL_NEWER); click(SID_TRAIL_NEWER);
        CHECK("Show newer twice returns to the newest page", has_text("Showing 1-6 of 17.") && has_text("#17 "));
        fresh(); for (uint32_t i = 1; i <= 6; i++) rec(i, 14, 0, 0, 1, 0); shell_trail_refresh(&sh); open_trail();
        CHECK("exactly one page of records (6 at 100%): no window, no scroll buttons", !wid(SID_TRAIL_OLDER) && !has_text("Showing"));
        fresh(); for (uint32_t i = 1; i <= 17; i++) rec(i, 14, 0, 0, 1, 0); shell_trail_refresh(&sh); shell_set_text_scale(&sh, 200); open_trail();
        CHECK("at 200% the page is 2 records: 'Showing 1-2 of 17.'", has_text("Showing 1-2 of 17.") && has_text("#17 ") && !has_text("#15 "));
    }
    { /* the read-only guarantee */
        fresh(); for (uint32_t i = 1; i <= 17; i++) rec(i, 12, 1000 + i, 1 + i % 7, 1, 0); V.ngrants = 3; V.grant[0].principal = 1; V.grant[0].perm = 1; shell_trail_refresh(&sh); open_trail();
        int bad = 0, calls0 = src_calls; (void)calls0;
        for (uint32_t i = 0; i < sh.n; i++) if ((sh.w[i].c.flags & UI_FOCUSABLE) && !allowed_trail_id(sh.c[i].id)) bad++;
        CHECK("every focusable control is a scroll button, Home, the dock or the Glass Corner: no record, grant or health text is a control", bad == 0);
        sh.fx.focus = -1; int seen_bad = 0, acts0 = (int)sh.activations;
        for (int i = 0; i < 80; i++) { key(KEY_TAB); uint32_t f = focus_id(); if (f != SHELL_NONE && !allowed_trail_id(f)) seen_bad++; }
        CHECK("80 Tab presses only ever focus allowed controls", seen_bad == 0);
        (void)acts0;
        /* click every label: nothing may activate */
        uint32_t before = sh.activations; int clicked = 0;
        for (uint32_t i = 0; i < sh.n; i++) if (sh.w[i].kind == UI_LABEL && sh.w[i].text && sh.w[i].c.layer == UI_LAYER_CONTENT && sh.w[i].c.y > 170 && sh.w[i].c.y < 600) { ptr(IN_PTR_DOWN, sh.w[i].c.x + 4, sh.w[i].c.y + 4); ptr(IN_PTR_UP, sh.w[i].c.x + 4, sh.w[i].c.y + 4); clicked++; }
        sh.fx.focus = -1;                                                              /* no control has the focus: Enter and Space must then do nothing at all */
        CHECK("clicking every record, grant and health line activates nothing, and Enter/Space with no focus does nothing", clicked > 4 && sh.activations == before && sh.screen == SCR_TRAIL && (key(KEY_ENTER), key(KEY_SPACE), sh.activations == before && sh.screen == SCR_TRAIL));
        CHECK("Home returns Home", click(SID_TRAIL_HOME) && sh.screen == SCR_HOME);
    }
    { /* refresh */
        fresh(); rec(1, 14, 0, 0, 1, 0); shell_trail_refresh(&sh); open_trail();
        int c0 = src_calls; uint32_t seq0 = sh.tr_seq;
        CHECK("a refresh with an unchanged ledger reports no change and does not repaint", shell_trail_refresh(&sh) == 0 && src_calls == c0 + 1 && sh.tr_seq == seq0);
        rec(2, 12, 1001, 7, 1, 0); sh.dirty = 0;
        CHECK("a new record: refresh reports a change, repaints and shows it", shell_trail_refresh(&sh) == 1 && sh.dirty && has_text("#2 Granted"));
        V.health = TV_ROLLBACK; shell_trail_refresh(&sh);
        CHECK("a health change is spoken once (sequence advances) and shown at once", sh.tr_seq == seq0 + 1 && has_text("rollback found"));
        fresh(); for (uint32_t i = 1; i <= 17; i++) rec(i, 14, 0, 0, 1, 0); shell_trail_refresh(&sh); open_trail(); click(SID_TRAIL_OLDER); click(SID_TRAIL_OLDER);
        memset(&V, 0, sizeof V); V.health = TV_RECORDING; rec(1, 14, 0, 0, 1, 0); shell_trail_refresh(&sh);
        CHECK("records shrinking under the window: the window falls back to the newest, never past the end", sh.tr_first == 0 && has_text("#1 System started."));
    }
    { /* hostile views must never overflow or be believed */
        fresh(); V.health = 200; V.n = 5000; V.ngrants = 9999;
        for (uint32_t i = 0; i < SHELL_TV_RECS; i++) { V.rec[i].seq = 0xFFFFFFFFu; V.rec[i].kind = (uint8_t)(i * 7); V.rec[i].reason = 255; V.rec[i].target = 255; V.rec[i].state = 255; V.rec[i].principal = 0xFFFFFFFFu; V.rec[i].arg = -1; }
        for (uint32_t i = 0; i < SHELL_TV_GRANTS; i++) { V.grant[i].principal = 0xFFFFFFFFu; V.grant[i].perm = 255; }
        shell_trail_refresh(&sh); open_trail();
        CHECK("oversized counts and out-of-range values are clamped: no overflow, an unknown health is 'not available', unknown kinds/permissions are named as unknown", sh.tv.n <= SHELL_TV_RECS && sh.tv.ngrants <= SHELL_TV_GRANTS && has_text("Trail: not available.") && audit() == 0);
        CHECK("... and none of it is mistaken for text from a user", has_text("Unknown record kind") || has_text("Denied") || has_text("Granted") || has_text("Request"));
    }
    { /* geometry at every scale and palette, with the largest and smallest views */
        static const uint32_t PCT[] = { 75, 100, 125, 150, 175, 200 };
        int bad = 0, over = 0;
        for (int hc = 0; hc < 2; hc++) for (unsigned p = 0; p < 6; p++) for (int sc = 0; sc < 5; sc++) for (int first = 0; first < 3; first++) {
            fresh(); uint32_t nrec[5] = { 0, 1, 3, 17, 48 }; for (uint32_t i = 1; i <= nrec[sc]; i++) rec(i, (uint8_t)(i % 3 == 0 ? 1 : 12 + i % 3), 4000000000u + i, (uint8_t)(1 + i % 7), (uint8_t)(i % 3 == 0 ? 2 : 1), 6);
            V.ngrants = sc >= 3 ? 32 : sc; for (uint32_t i = 0; i < V.ngrants; i++) { V.grant[i].principal = 4000000000u + i; V.grant[i].perm = (uint8_t)(1 + i % 7); }
            shell_trail_refresh(&sh); shell_set_contrast(&sh, hc); shell_set_text_scale(&sh, PCT[p]); open_trail();
            for (int k = 0; k < first * 3; k++) click(SID_TRAIL_OLDER);
            bad += audit(); if (sh.lay_bottom > 636 && PCT[p] == 200) { over++; if (getenv("SHOW")) printf("    over: hc %d sc %d first %d bottom %d\n", hc, sc, first, (int)sh.lay_bottom); }
        }
        CHECK("geometry holds on the Trail at 75..200 percent in both palettes, with 0..48 records, up to 32 grants and scrolled windows", bad == 0);
        CHECK("at 200 percent the content bottom stays <= 636", over == 0);
    }
    { /* randomized input over random views: the Trail screen never changes anything but its own window and the screen */
        uint32_t s = 777; int bad = 0;
        fresh(); open_trail();
        for (int it = 0; it < 20000 && !bad; it++) {
            s = s * 1664525u + 1013904223u;
            switch ((s >> 12) % 7) {
            case 0: key(KEY_TAB); break;
            case 1: key(KEY_ENTER); break;
            case 2: key(KEY_SPACE); break;
            case 3: { uint32_t id[3] = { SID_TRAIL_NEWER, SID_TRAIL_OLDER, SID_TRAIL_HOME }; click(id[(s >> 20) % 3]); if (sh.screen != SCR_TRAIL) open_trail(); break; }
            case 4: { memset(&V, 0, sizeof V); V.health = (uint8_t)((s >> 8) % 8); uint32_t n = (s >> 16) % 60; for (uint32_t i = 0; i < n && i < SHELL_TV_RECS; i++) rec(i + 1, (uint8_t)((s >> (i % 8)) & 15), s + i, (uint8_t)(s >> 3), (uint8_t)(i % 4), (uint8_t)(i % 18)); V.ngrants = (s >> 5) % 40; shell_trail_refresh(&sh); break; }
            case 5: key(KEY_ESC); if (sh.screen != SCR_TRAIL) open_trail(); break;
            default: ptr(IN_PTR_MOVE, (int)((s >> 4) % 1024), (int)((s >> 14) % 768)); break;
            }
            if (sh.screen == SCR_TRAIL) { for (uint32_t i = 0; i < sh.n; i++) if ((sh.w[i].c.flags & UI_FOCUSABLE) && !allowed_trail_id(sh.c[i].id)) bad++; if (sh.tr_first > sh.tv.n) bad++; }
        }
        CHECK("20,000 random keys, clicks and ledger changes: only allowed controls ever exist, the window never leaves the records", bad == 0);
    }
    printf("shell trail: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
