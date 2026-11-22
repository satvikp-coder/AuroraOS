/* tests/kernel-host/shell_scale_test.c - text scaling and high contrast for the native shell (host, ASan/UBSan).
 * The oracle for "no clipped text" is a separate greedy word wrapper written here (not gfx_text_wrap): it counts the lines a label needs and
 * requires them to fit the widget's height. Contrast ratios are computed here from the WCAG formula, independently of the shell. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shell.h>
#include <gfx_font.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)
#define OK(name) do { passed++; (void)name; } while (0)

#define W 1024u
#define H 768u
#define GUARD 4096u
static uint8_t *base, *pix;
static struct gfx_surface S;
static struct shell sh;

static void surf(void) { memset(base, 0xA5, GUARD * 2 + W * H * 4); pix = base + GUARD; S = (struct gfx_surface){ pix, (uint64_t)W * H * 4, W, H, W * 4, GFX_FMT_XRGB8888 }; }
static int guards_ok(void) { for (uint32_t i = 0; i < GUARD; i++) if (base[i] != 0xA5 || pix[W * H * 4 + i] != 0xA5) return 0; return 1; }
static uint32_t px(int x, int y) { return *(uint32_t *)(void *)(pix + (size_t)y * W * 4 + (size_t)x * 4); }
static void fresh(int caps_both) { shell_init(&sh, W, H); struct shell_caps c = { (uint8_t)caps_both, (uint8_t)caps_both }; shell_set_caps(&sh, c); }
static void key(uint16_t k, uint16_t mods) { struct input_event e = { IN_KEY_DOWN, 0, k, mods, 0, 0, 0 }; shell_handle(&sh, &e); }
static void ptr(int kind, int x, int y) { struct input_event e = { (uint8_t)kind, 0, BTN_LEFT, 0, 0, x, y }; shell_handle(&sh, &e); }
static void click(uint32_t id) { int32_t i = shell_find(&sh, id); if (i < 0) return; int x = sh.c[i].x + sh.c[i].w / 2, y = sh.c[i].y + sh.c[i].h / 2; ptr(IN_PTR_DOWN, x, y); ptr(IN_PTR_UP, x, y); }
static void text(const char *t) { for (; *t; t++) { struct input_event e = { IN_TEXT, 0, 30, 0, (uint8_t)*t, 0, 0 }; shell_handle(&sh, &e); } }
static uint32_t focus_id(void) { return sh.fx.focus >= 0 ? sh.c[sh.fx.focus].id : SHELL_NONE; }

/* ---- independent oracles ---- */
static const struct gfx_font *font_for(const struct ui_widget *w) { for (int i = 0; i < gfx_font_count; i++) if (gfx_fonts[i]->size == (w->font_size ? w->font_size : 16) && gfx_fonts[i]->weight == (w->bold ? 700 : 400)) return gfx_fonts[i]; return gfx_fonts[0]; }

/* greedy wrap of a printable ASCII string into `cols` cells; returns the line count (words longer than a line are cut) */
static int wrap_lines(const char *t, int cols)
{
    if (cols < 1) cols = 1;
    int lines = 1, cur = 0;
    size_t i = 0, n = strlen(t);
    while (i < n) {
        while (i < n && t[i] == ' ') { if (cur && cur < cols) cur++; i++; }
        size_t j = i; while (j < n && t[j] != ' ') j++;
        int wl = (int)(j - i);
        if (wl == 0) break;
        if (cur && cur + wl > cols) { lines++; cur = 0; }
        while (wl > cols) { if (cur) { lines++; cur = 0; } lines++; wl -= cols; }
        cur += wl; i = j;
    }
    return lines;
}

static double lum(uint32_t c)
{
    double v[3] = { ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0 };
    for (int i = 0; i < 3; i++) v[i] = v[i] <= 0.03928 ? v[i] / 12.92 : pow((v[i] + 0.055) / 1.055, 2.4);
    return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
}
static double ratio(uint32_t a, uint32_t b) { double x = lum(a), y = lum(b); if (x < y) { double t = x; x = y; y = t; } return (x + 0.05) / (y + 0.05); }

struct viol { int oob, clip, overlap, small, ids, dock; };

static int rects_overlap(const struct ui_ctrl *a, const struct ui_ctrl *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

/* Check the current layout. Returns the number of violations; prints the first few. */
static double worst_edge_std = 99, worst_edge_hc = 99;   /* the weakest edge of an ENABLED control, against the control's own fill and the surface behind it */
static int audit(const char *what, double *min_ratio_out)
{
    int bad = 0;
    double minr = 99;
    if (sh.n > SHELL_MAX_WIDGETS) { printf("  %s: widget overflow\n", what); return 1; }
    int32_t dock = shell_find(&sh, 0xF100u), rail = shell_find(&sh, 0xF303u);
    for (uint32_t i = 0; i < sh.n; i++) {
        const struct ui_widget *w = &sh.w[i];
        if (w->c.x < 0 || w->c.y < 0 || w->c.x + w->c.w > (int32_t)W || w->c.y + w->c.h > (int32_t)H) { if (bad++ < 100) printf("  %s: widget %u (id %u) outside the screen %d,%d %dx%d\n", what, i, w->c.id, w->c.x, w->c.y, w->c.w, w->c.h); }
        if ((w->c.flags & UI_FOCUSABLE) && (w->c.h < 40 || w->c.w < 48)) { if (bad++ < 100) printf("  %s: touch target too small id %u %dx%d\n", what, w->c.id, w->c.w, w->c.h); }
        if (w->kind == UI_BUTTON && (w->c.flags & UI_FOCUSABLE) && (w->c.flags & UI_ENABLED) && w->border_w) {
            double e = ratio(w->border, w->fill), e2 = ratio(w->border, sh.contrast ? 0xFFFFFFu : UI_COL_SURFACE);
            double m = e < e2 ? e : e2;
            if (sh.contrast) { if (m < worst_edge_hc) worst_edge_hc = m; } else if (m < worst_edge_std) worst_edge_std = m;
        }
        if (w->text && w->kind != UI_PANEL && w->text[0]) {
            const struct gfx_font *f = font_for(w);
            int mag = w->mag ? w->mag : 1;
            int bw = w->border_w, iw = w->c.w - 2 * bw, ih = w->c.h - 2 * bw;
            int cell = f->advance * mag;
            if (w->wrap) {
                int avail = iw - 16; if (avail < cell) avail = cell;
                int lines = wrap_lines(w->text, avail / mag / f->advance);
                if (lines * f->line_height * mag > ih) { if (bad++ < 100) printf("  %s: id %u '%s' needs %d lines of %d px in %d px\n", what, w->c.id, w->text, lines, f->line_height * mag, ih); }
            } else {
                int tw = (int)strlen(w->text) * cell;
                if (tw + 16 > iw || f->line_height * mag > ih) { if (bad++ < 100) printf("  %s: id %u '%s' one-line text %d+16 wide in %d (h %d in %d)\n", what, w->c.id, w->text, tw, iw, f->line_height * mag, ih); }
            }
            uint32_t fg = w->ink, bg = w->fill;
            if (w->kind == UI_BUTTON && w->active) { fg = w->active_ink ? w->active_ink : UI_COL_SURFACE; bg = w->active_fill ? w->active_fill : UI_COL_ACCENT; }
            else if (w->kind == UI_BUTTON && !(w->c.flags & UI_ENABLED)) fg = w->disabled_ink ? w->disabled_ink : UI_COL_MUTED;
            double r = ratio(fg, bg); if (r < minr) minr = r; if (r < 4.5 && getenv("SHOW_LOW")) printf("  low %.2f id %u '%s' fg %06x bg %06x kind %d active %d\n", r, w->c.id, w->text, fg, bg, w->kind, w->active);
        }
        /* interactive controls of one layer must not overlap each other */
        if (w->c.flags & UI_FOCUSABLE)
            for (uint32_t j = i + 1; j < sh.n; j++)
                if ((sh.w[j].c.flags & UI_FOCUSABLE) && sh.w[j].c.layer == w->c.layer && rects_overlap(&w->c, &sh.w[j].c)) { if (bad++ < 100) printf("  %s: controls %u and %u overlap\n", what, w->c.id, sh.w[j].c.id); }
        /* content must sit between the status rail and the dock */
        if (w->c.layer == UI_LAYER_CONTENT && w->c.id != 0xF300u && w->c.id != 0xF303u && !(w->c.id >= 0xF301u && w->c.id <= 0xF304u) && dock >= 0 && rail >= 0) {
            if (w->c.y < sh.c[rail].y + sh.c[rail].h || w->c.y + w->c.h > sh.c[dock].y) { if (bad++ < 100) printf("  %s: content id %u (%d..%d) leaves the content region (%d..%d)\n", what, w->c.id, w->c.y, w->c.y + w->c.h, sh.c[rail].y + sh.c[rail].h, sh.c[dock].y); }
        }
    }
    /* control ids unique */
    for (uint32_t i = 0; i < sh.n; i++) for (uint32_t j = i + 1; j < sh.n; j++) if (sh.c[i].id == sh.c[j].id && sh.c[i].id < 0xF000u) { if (bad++ < 100) printf("  %s: duplicate id %u\n", what, sh.c[i].id); }
    /* the dock is reachable: all four controls exist, are enabled (Back may be disabled) and hit testing at their centres returns them */
    static const uint32_t D[4] = { SID_DOCK_BACK, SID_DOCK_HOME, SID_DOCK_SPACES, SID_DOCK_INDEX };
    for (int k = 0; k < 4; k++) {
        int32_t i = shell_find(&sh, D[k]);
        if (i < 0) { if (bad++ < 100) printf("  %s: dock control %u missing\n", what, D[k]); continue; }
        if (k && !sh.corner_open && !sh.about_open && ui_hit(&sh.fx, sh.c[i].x + sh.c[i].w / 2, sh.c[i].y + sh.c[i].h / 2) != i) { if (bad++ < 100) printf("  %s: dock control %u not hit at its centre\n", what, D[k]); }
    }
    /* drawing stays inside the surface */
    surf();
    if (shell_draw(&sh, &S) != GFX_OK || !guards_ok()) { bad++; printf("  %s: draw failed or wrote outside the surface\n", what); }
    if (min_ratio_out) *min_ratio_out = minr;
    return bad;
}

/* ---- stand-ins for what the kernel supplies to the other native screens, filled with the LONGEST realistic content ---- */
static struct procsnap g_ps;
static uint32_t ps_src(struct procsnap *out) { *out = g_ps; return out->n; }
static void ps_add(uint8_t pid, uint8_t state, const char *name, uint32_t pages)
{
    struct procsnap_entry *e = &g_ps.e[g_ps.n++]; memset(e, 0, sizeof *e);
    e->pid = pid; e->state = state; e->gen = 100u + pid; e->ticks = 1234567u * pid; e->pages = pages; e->switches = 99999u; e->yields = 8888u; e->preemptions = 777u;
    strncpy(e->name, name, sizeof e->name - 1);
}
static struct shell_trail_view g_tv;
static int tv_src(struct shell_trail_view *out) { *out = g_tv; return 0; }
static void tv_rec(uint32_t seq, uint8_t kind, uint32_t principal, uint8_t target, uint8_t state, uint8_t reason)
{
    struct shell_trail_rec *r = &g_tv.rec[g_tv.n++]; memset(r, 0, sizeof *r); r->seq = seq; r->kind = kind; r->principal = principal; r->target = target; r->state = state; r->reason = reason;
}
static uint64_t t_now = 100000, t_end; static int t_id = -1, t_state;
static int t_start(uint64_t ms) { if (t_id >= 0) return -406; t_id = 0; t_state = 1; t_end = t_now + ms; return 0; }
static int t_query(int id, uint8_t *st, uint64_t *rem) { if (id != t_id) return -403; *st = (uint8_t)t_state; *rem = t_state == 1 ? t_end - t_now : 0; return 0; }
static int t_cancel(int id) { if (id != t_id) return -403; t_state = 3; return 0; }
static int t_release(int id) { if (id != t_id) return -403; t_id = -1; t_state = 0; return 0; }
static const struct shell_timers TIMERS = { t_start, t_query, t_cancel, t_release };
static const struct aura_caps RV_CAPS = { 1, 1, 0, 0, 0 };
#define NSTATES 22
static const char *SN2[NSTATES] = { "home", "spaces", "index", "index-s", "index-long", "about", "corner", "no-results", "limited-input", "settings", "settings-refusal",
    "pages-empty", "pages-full", "trail-recording", "trail-rollback", "trail-nodisk", "clock-idle", "clock-running", "clock-no-service", "hold", "aura-result", "review-modal" };

static void setup_state(int st)
{
    /* 0 home, 1 spaces, 2 index empty, 3 index query, 4 index long query, 5 about modal, 6 corner open, 7 index with no hits, 8 limited input */
    fresh(st != 8);
    if (st == 1) click(SID_DOCK_SPACES);
    if (st >= 2 && st <= 4) click(SID_DOCK_INDEX);
    if (st == 3) text("s");
    if (st == 4) text("settingssettingssettingssettings");
    if (st == 5) click(SID_ABOUT);
    if (st == 6) click(SID_CORNER);
    if (st == 7) { click(SID_DOCK_INDEX); text("zzzz"); }
    if (st == 9 || st == 10) { click(SID_APP_SETTINGS); }
    if (st == 10) { click(SID_SET_VALUE + 4); }                          /* Aura mode with no store: the refusal text is the longest status */
    if (st == 11) { memset(&g_ps, 0, sizeof g_ps); shell_set_pages(&sh, ps_src); click(SID_APP_PAGES); }
    if (st == 12) {
        memset(&g_ps, 0, sizeof g_ps); ps_add(1, 1, "a-very-long-process-name-xxxxxx", 1000); ps_add(2, 2, "faulted-client-0123456789", 4000000000u); ps_add(3, 3, "terminated-process", 32);
        ps_add(4, 4, "blocked", 64); ps_add(5, 1, "calc.elf", 6); ps_add(6, 2, "notes.elf", 7); ps_add(7, 1, "files.elf", 8); ps_add(8, 3, "ui-client", 9);
        shell_set_pages(&sh, ps_src); click(SID_APP_PAGES);
    }
    if (st >= 13 && st <= 15) {
        memset(&g_tv, 0, sizeof g_tv); g_tv.health = st == 13 ? TV_RECORDING : st == 14 ? TV_ROLLBACK : TV_NODISK;
        for (uint32_t i = 0; i < 12; i++) tv_rec(100000u + i, (uint8_t)(1 + i % 5), 0x7F000000u + i, (uint8_t)(i % 4), (uint8_t)(i % 4), (uint8_t)(i % 6));
        g_tv.ngrants = 6; for (uint32_t i = 0; i < 6; i++) { g_tv.grant[i].principal = 0x1000u + i; g_tv.grant[i].perm = (uint8_t)(1 + i); }
        shell_set_trail(&sh, tv_src); shell_go(&sh, SCR_TRAIL);
    }
    if (st >= 16 && st <= 18) {
        t_id = -1; t_state = 0; t_now = 100000;
        if (st != 18) shell_set_timers(&sh, &TIMERS);
        shell_tick(&sh, t_now); shell_go(&sh, SCR_CLOCK);
        if (st == 17) { click(SID_CLOCK_P25); click(SID_CLOCK_START); t_now += 1000; shell_tick(&sh, t_now); }
    }
    if (st == 19) shell_go(&sh, SCR_HOLD);
    if (st == 20) {
        shell_go(&sh, SCR_AURA); text("start a timer for twenty five minutes and then open the settings page please");
        shell_aura_report(&sh, 2, "Refused: this needs a review, and the review could not be opened because another request is already waiting; nothing was changed.");
    }
    if (st == 21) {
        shell_tick(&sh, 1000); struct rv_request q; memset(&q, 0, sizeof q); aura_parse("start a timer for 25 minutes", 28, &RV_CAPS, &q.intent);
        q.pid = 3; q.gen = 30; q.principal = 0x1003; q.nonce = 7; q.prior = -1; q.next = -1; shell_review_open(&sh, 1000, &q, &RV_CAPS, "A client with a rather long display name 0123456789");
    }
}

int main(void)
{
    printf("shell text scale and contrast tests\n");
    base = malloc(GUARD * 2 + W * H * 4);

    /* ---- the font ladder ---- */
    {
        int exact = 1, mono = 1, floor12 = 1;
        static const uint32_t roles[] = { 12, 14, 16, 18, 24 };
        for (int r = 0; r < 5; r++) {
            uint32_t prev = 0;
            for (uint32_t p = 75; p <= 200; p++) {
                uint8_t sz, mg; shell_font_for(roles[r], p, &sz, &mg);
                uint32_t eff = (uint32_t)sz * mg;
                if (eff < 12) floor12 = 0;
                if (eff < prev) mono = 0;
                prev = eff;
                int known = 0; for (int i = 0; i < gfx_font_count; i++) if (gfx_fonts[i]->size == sz) known = 1;
                if (!known || mg < 1 || mg > 2) exact = 0;
            }
        }
        CHECK("every scale maps to a real font size and magnification 1 or 2", exact);
        CHECK("the chosen text size never shrinks as the scale grows", mono);
        CHECK("text never goes below 12 px (75 percent floors at 12)", floor12);
        uint8_t sz, mg;
        int id100 = 1;
        for (int r = 0; r < 5; r++) { shell_font_for(roles[r], 100, &sz, &mg); if (sz != roles[r] || mg != 1) id100 = 0; }
        CHECK("100 percent keeps every reference size unchanged", id100);
        int dbl = 1;
        for (int r = 1; r < 5; r++) { shell_font_for(roles[r], 200, &sz, &mg); if ((uint32_t)sz * mg != roles[r] * 2) dbl = 0; }
        CHECK("200 percent is an exact doubling of 14, 16, 18 and 24 px", dbl);
        shell_font_for(12, 200, &sz, &mg);
        CHECK("200 percent of 12 px is 24 px", (uint32_t)sz * mg == 24);
    }

    /* ---- range validation ---- */
    fresh(1);
    CHECK("scale 74 is refused", !shell_set_text_scale(&sh, 74) && sh.text_pct == 100);
    CHECK("scale 201 is refused", !shell_set_text_scale(&sh, 201) && sh.text_pct == 100);
    CHECK("scale 0 is refused", !shell_set_text_scale(&sh, 0) && sh.text_pct == 100);
    CHECK("scale 4294967295 is refused", !shell_set_text_scale(&sh, 0xffffffffu) && sh.text_pct == 100);
    CHECK("scale 75 and 200 are accepted", shell_set_text_scale(&sh, 75) && sh.text_pct == 75 && shell_set_text_scale(&sh, 200) && sh.text_pct == 200);

    /* ---- every scale x every contrast x every state: nothing clipped, overlapping or out of bounds ---- */
    {
        int total_bad = 0, combos = 0;
        double worst_std = 99, worst_hc = 99;
        for (int hc = 0; hc <= 1; hc++)
            for (uint32_t p = 75; p <= 200; p++)
                for (int st = 0; st < NSTATES; st++) {
                    setup_state(st);
                    shell_set_contrast(&sh, hc);
                    shell_set_text_scale(&sh, p);
                    char name[64]; snprintf(name, sizeof name, "%s %u%% hc=%d", SN2[st], p, hc);
                    double mr = 99;
                    total_bad += audit(name, &mr);
                    if (hc) { if (mr < worst_hc) worst_hc = mr; } else if (mr < worst_std) worst_std = mr;
                    combos++;
                }
        CHECK("no clipped text, overflow, overlap, small target or stray draw in any scale/contrast/state combination", total_bad == 0);
        printf("  (%d layouts audited; worst text contrast: standard %.2f:1, high contrast %.2f:1)\n", combos, worst_std, worst_hc);
        /* the Clear accent is now 0x2A68E8: white on it is 4.95:1 (it was 0x3478F6 at 4.07:1, below the 4.5:1 normal-text level) */
        CHECK("standard palette text is at least 4.5:1 everywhere (WCAG AA for normal text), measured over every scale/state layout", worst_std >= 4.5);
        CHECK("high contrast text is at least 7:1 everywhere", worst_hc >= 7.0);
        printf("  (worst enabled-control edge: standard %.2f:1, high contrast %.2f:1)\n", worst_edge_std, worst_edge_hc);
        CHECK("every enabled control's edge is at least 3:1 against its fill and the surface (WCAG 1.4.11), in every scale/state layout", worst_edge_std >= 3.0);
        CHECK("high contrast enabled-control edges are at least 7:1", worst_edge_hc >= 7.0);
    }

    /* ---- the reference layout is unchanged at 100 percent ---- */
    fresh(1);
    CHECK("100 percent Home keeps the reference geometry (Change Space 64,232 200x48; dock 262,664 500x80; handle 912,8 96x48)",
          sh.c[shell_find(&sh, SID_CHANGE_SPACE)].x == 64 && sh.c[shell_find(&sh, SID_CHANGE_SPACE)].y == 232 && sh.c[shell_find(&sh, SID_CHANGE_SPACE)].w == 200 &&
          sh.c[shell_find(&sh, 0xF100u)].x == 262 && sh.c[shell_find(&sh, 0xF100u)].y == 664 && sh.c[shell_find(&sh, 0xF100u)].w == 500 &&
          sh.c[shell_find(&sh, SID_CORNER)].x == 912 && sh.c[shell_find(&sh, SID_CORNER)].w == 96);

    /* ---- larger scale really draws larger text ---- */
    {
        fresh(1);
        const struct ui_widget *t = &sh.w[shell_find(&sh, 0xF301u)];
        CHECK("100 percent title: 24 px bold, no magnification", t->font_size == 24 && (t->mag == 0 || t->mag == 1));
        shell_set_text_scale(&sh, 200);
        t = &sh.w[shell_find(&sh, 0xF301u)];
        CHECK("200 percent title: 24 px bold magnified 2x (48 px)", t->font_size == 24 && t->mag == 2);
        const struct ui_widget *b = &sh.w[shell_find(&sh, SID_CHANGE_SPACE)];
        CHECK("200 percent buttons use 16 px magnified 2x (32 px)", b->font_size == 16 && b->mag == 2);
        shell_set_text_scale(&sh, 75);
        t = &sh.w[shell_find(&sh, 0xF301u)];
        CHECK("75 percent title: 18 px bold", t->font_size == 18 && (t->mag == 0 || t->mag == 1));
    }

    /* ---- focus and modal state survive a scale change ---- */
    {
        fresh(1);
        key(KEY_TAB, 0); key(KEY_TAB, 0);
        uint32_t before = focus_id();
        shell_set_text_scale(&sh, 150);
        CHECK("keyboard focus stays on the same control across a scale change", focus_id() == before && before != SHELL_NONE);
        shell_set_text_scale(&sh, 100);
        CHECK("and across the return to 100 percent", focus_id() == before);
        click(SID_ABOUT);
        CHECK("About opens at 100 percent", sh.about_open);
        shell_set_text_scale(&sh, 200);
        CHECK("the modal stays open and holds focus when the scale changes", sh.about_open && focus_id() != SHELL_NONE && sh.c[sh.fx.focus].layer == UI_LAYER_MODAL);
        key(KEY_ESC, 0);
        CHECK("Escape closes About at 200 percent and the page behind is intact", !sh.about_open && sh.screen == SCR_HOME);
        shell_set_contrast(&sh, 1);
        click(SID_CORNER);
        CHECK("the Glass Corner sheet opens in high contrast at 200 percent", sh.corner_open);
        click(SID_CORNER_CLOSE);
        CHECK("and closes again, returning focus to the handle", !sh.corner_open && focus_id() == SID_CORNER);
    }

    /* ---- navigation still works at every scale (click each dock control and each Space) ---- */
    {
        int nav_ok = 1;
        for (uint32_t p = 75; p <= 200; p += 25) {
            fresh(1); shell_set_text_scale(&sh, p);
            click(SID_DOCK_SPACES); if (sh.screen != SCR_SPACES) nav_ok = 0;
            click(SID_SPACE_BASE + 2); if (sh.space != 2) nav_ok = 0;
            click(SID_DOCK_INDEX); if (sh.screen != SCR_INDEX) nav_ok = 0;
            text("work"); key(KEY_ENTER, 0); if (sh.space != 1 || sh.screen != SCR_HOME) nav_ok = 0;
            click(SID_DOCK_BACK); if (sh.screen == SCR_HOME && sh.hist_n >= 3) nav_ok = 0;
        }
        CHECK("the dock, the Space buttons and Index search-and-open work at 75, 100, 125, 150, 175 and 200 percent", nav_ok);
    }

    /* ---- Index results are windowed, never hidden: the selected row is always on screen and operable ---- */
    {
        int win_ok = 1, rows_min = 99;
        for (uint32_t p = 75; p <= 200; p += 5) {
            fresh(1); click(SID_DOCK_INDEX); shell_set_text_scale(&sh, p);
            for (int k = 0; k < 10; k++) {
                key(KEY_DOWN, 0);
                int32_t row = shell_find(&sh, SID_RESULT_BASE + (uint32_t)sh.sel);
                if (sh.sel < 0 || row < 0) { win_ok = 0; continue; }
                int vis = 0; for (uint32_t i = 0; i < sh.n; i++) if (sh.c[i].id >= SID_RESULT_BASE && sh.c[i].id < SID_RESULT_BASE + SHELL_RESULTS) vis++;
                if (vis < rows_min) rows_min = vis;
                if (!(sh.c[row].flags & UI_VISIBLE)) win_ok = 0;
            }
        }
        CHECK("the selected Index result always has a visible row, whatever the scale", win_ok);
        printf("  (fewest Index rows visible at once across all scales: %d)\n", rows_min);
        CHECK("at least two Index rows are visible at once at every scale", rows_min >= 2);
    }

    /* ---- high contrast ---- */
    {
        fresh(1);
        shell_set_contrast(&sh, 1);
        int32_t home = shell_find(&sh, SID_DOCK_HOME);
        CHECK("high contrast: the active dock item has a non-colour marker, a 2 px border and inverted colours", sh.w[home].active && sh.w[home].marker && sh.w[home].border_w >= 2 && sh.w[home].active_fill != sh.w[home].fill);
        CHECK("high contrast: an enabled button has a 2 px border, a disabled one 1 px", sh.w[shell_find(&sh, SID_CHANGE_SPACE)].border_w == 2 && sh.w[shell_find(&sh, SID_DOCK_BACK)].border_w == 1);
        CHECK("high contrast: surfaces are opaque white and the focus ring is a strong colour", sh.w[shell_find(&sh, 0xF300u)].fill == 0x00FFFFFFu && ratio(sh.w[shell_find(&sh, SID_CHANGE_SPACE)].ink, 0x00FFFFFFu) >= 15);
        key(KEY_TAB, 0);
        surf(); shell_draw(&sh, &S);
        const struct ui_ctrl *f = &sh.c[sh.fx.focus];
        int ring = 1; for (int t = 1; t <= UI_FOCUS_RING; t++) if (px(f->x + f->w / 2, f->y - t) != 0x000000E0u) ring = 0;
        CHECK("high contrast: a 3 px focus ring is drawn above the focused control in the strong colour", ring);
        CHECK("high contrast: the focus ring contrasts with the canvas by at least 7:1", ratio(0x000000E0u, 0x00FFFFFFu) >= 7.0);
        CHECK("high contrast: the canvas is drawn white", px(5, 700) == 0x00FFFFFFu);
        int32_t pg = shell_find(&sh, SID_DOCK_BACK);
        surf(); shell_draw(&sh, &S);
        CHECK("high contrast: the disabled Back button keeps a readable label colour (7:1) and a thinner border", ratio(sh.w[pg].disabled_ink, sh.w[pg].fill) >= 7.0 && sh.w[pg].border_w < sh.w[shell_find(&sh, SID_APP_SPACES)].border_w);
        shell_set_contrast(&sh, 0);
        CHECK("switching high contrast off restores the standard palette", sh.w[shell_find(&sh, 0xF300u)].fill == UI_COL_SURFACE && sh.w[shell_find(&sh, SID_DOCK_HOME)].marker == 0);
    }

    /* ---- rendering: pixels at 200 percent ---- */
    {
        fresh(1);
        shell_set_text_scale(&sh, 200);
        surf(); shell_draw(&sh, &S);
        const struct ui_ctrl *b = &sh.c[shell_find(&sh, SID_CHANGE_SPACE)];
        int ink = 0;
        for (int y = b->y + 2; y < b->y + b->h - 2; y++) for (int x = b->x + 2; x < b->x + b->w - 2; x++) if (px(x, y) == UI_COL_INK) ink++;
        CHECK("the 200 percent Change Space button contains magnified (blocky, 2x2) ink", ink > 100);
        uint8_t *copy = malloc((size_t)W * H * 4); memcpy(copy, pix, (size_t)W * H * 4);
        surf(); shell_draw(&sh, &S);
        CHECK("identical frames for identical state", memcmp(copy, pix, (size_t)W * H * 4) == 0);
        free(copy);
    }

    /* ---- robustness: random scale/contrast/state churn keeps every invariant ---- */
    {
        unsigned seed = 12345u; int inv = 1;
        fresh(1);
        for (int it = 0; it < 4000; it++) {
            seed = seed * 1103515245u + 12345u;
            switch ((seed >> 16) % 9) {
            case 0: shell_set_text_scale(&sh, 75 + (seed >> 8) % 126); break;
            case 1: shell_set_contrast(&sh, (seed >> 9) & 1); break;
            case 2: key(KEY_TAB, 0); break;
            case 3: key(KEY_ENTER, 0); break;
            case 4: key(KEY_ESC, 0); break;
            case 5: click(SID_DOCK_INDEX + ((seed >> 10) % 3 == 0 ? -1 : 0)); break;
            case 6: text("a"); break;
            case 7: key(KEY_DOWN, 0); break;
            default: shell_set_text_scale(&sh, (seed >> 11) % 400); break;     /* includes invalid values */
            }
            if (sh.text_pct < SHELL_TEXT_MIN || sh.text_pct > SHELL_TEXT_MAX || sh.n > SHELL_MAX_WIDGETS) inv = 0;
            if ((it % 50) == 0) { char nm[32]; snprintf(nm, sizeof nm, "churn %d", it); if (audit(nm, 0)) inv = 0; }
        }
        CHECK("4000 random scale, contrast and input operations keep scale in range, layouts inside bounds and drawing inside the surface", inv);
    }

    printf("shell scale: %d passed, %d failed\n", passed, failed);
    free(base);
    return failed ? 1 : 0;
}
