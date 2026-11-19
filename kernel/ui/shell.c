/* kernel/ui/shell.c - native Aurora Glass shell (Tier 0 Clear): screens, navigation, layout, input dispatch. See kernel/include/shell.h. */
#include <shell.h>
#include <settings.h>        /* result codes only (SETTINGS_*); the registry itself is behind struct shell_store */

const char *const SHELL_SPACE_NAMES[SHELL_SPACES] = { "Study", "Work", "Projects", "Personal" };

#define FOC (UI_VISIBLE | UI_ENABLED | UI_FOCUSABLE)
#define SOLID UI_VISIBLE   /* non-interactive surfaces: they absorb pointer events (no click-through) but never activate */
#define COL_HOVER 0x00E4ECF6u
#define COL_PRESSED 0x00CFDCF0u
#define COL_AURA 0x0000AEB8u
#define COL_DISABLED_FILL 0x00F3F6FAu

/* Colour roles. The standard palette is the Aurora Glass "Clear" light set; high contrast is opaque black on white with 2 px borders, a
 * non-colour marker on the active control, and a strong focus colour. Meaning is never carried by colour alone: the active control also
 * has a marker (high contrast) and the disabled controls say "not available yet" in their text. */
/* (accent is near-black, not 0: a zero widget override means "use the default") */
struct pal { uint32_t canvas, surface, ink, muted, accent, active_ink, focus, border, border_dis, scrim, aura, hover, pressed, secure, secure_ink; uint8_t bw, marker, rail; };
static const struct pal PAL_STD = { UI_COL_CANVAS, UI_COL_SURFACE, UI_COL_INK, UI_COL_MUTED, UI_COL_ACCENT, UI_COL_SURFACE, UI_COL_FOCUS, 0x00C9D4E3u, 0x00DCE3EDu, UI_COL_SCRIM, COL_AURA, COL_HOVER, COL_PRESSED, 0x00FFB020u, 0x00142136u, 1, 0, 4 };
static const struct pal PAL_HC = { 0x00FFFFFFu, 0x00FFFFFFu, 0x00000000u, 0x00333333u, 0x00101010u, 0x00FFFFFFu, 0x000000E0u, 0x00000000u, 0x00555555u, 0x00000000u, 0x00000000u, 0x00D9D9D9u, 0x00B3B3B3u, 0x00000000u, 0x00FFFFFFu, 2, 1, 6 };
#define PAL(sh) ((sh)->contrast ? &PAL_HC : &PAL_STD)

static const char *const NAMES[SCR_COUNT] = { "Home", "Spaces", "Index", "Settings", "Pages", "Aura", "Clock", "Trail" };

const char *shell_screen_name(uint32_t screen) { return screen < SCR_COUNT ? NAMES[screen] : "?"; }

/* ---- Index registry: only real destinations. The model (index_model.c) ranks them; DEST[] says what opening one does. ---- */
enum { D_SCREEN, D_SPACE, D_ABOUT, D_UNAVAILABLE };
struct dest { uint8_t kind, arg; };
static const struct idx_item ITEMS[] = {
    { "home", "Home", { "desk", "start", 0 }, 1 },
    { "spaces", "Spaces", { "workspace", "context", 0 }, 1 },
    { "index", "Index", { "search", "find", "launcher" }, 1 },
    { "space.study", "Study", { "learn", "school", 0 }, 0 },
    { "space.work", "Work", { "job", 0, 0 }, 0 },
    { "space.projects", "Projects", { "build", 0, 0 }, 0 },
    { "space.personal", "Personal", { "me", 0, 0 }, 0 },
    { "about", "About AuroraOS", { "version", "info", 0 }, 0 },
    { "pages", "Pages", { "multitasking", "windows", 0 }, 1 },
    { "aura", "Aura", { "assistant", "ask", 0 }, 1 },
    { "settings", "Settings", { "preferences", "options", 0 }, 1 },
    { "notes", "Notes", { "memo", "write", 0 }, 0 },
    { "clock", "Clock", { "timer", "countdown", "uptime" }, 1 },
    { "trail", "Trail", { "audit", "history", "log" }, 1 },           /* listed, honestly unavailable: no native Notes app exists */
};
static const struct dest DEST[] = {
    { D_SCREEN, SCR_HOME }, { D_SCREEN, SCR_SPACES }, { D_SCREEN, SCR_INDEX },
    { D_SPACE, 0 }, { D_SPACE, 1 }, { D_SPACE, 2 }, { D_SPACE, 3 },
    { D_ABOUT, 0 }, { D_SCREEN, SCR_PAGES }, { D_SCREEN, SCR_AURA }, { D_SCREEN, SCR_SETTINGS }, { D_UNAVAILABLE, 0 }, { D_SCREEN, SCR_CLOCK }, { D_SCREEN, SCR_TRAIL },
};
#define NITEMS (sizeof ITEMS / sizeof ITEMS[0])
_Static_assert(sizeof ITEMS / sizeof ITEMS[0] == sizeof DEST / sizeof DEST[0], "registry tables must match");

size_t shell_index_count(void) { return NITEMS; }
const struct idx_item *shell_index_item(size_t i) { return i < NITEMS ? &ITEMS[i] : 0; }
int shell_index_available(size_t i) { return i < NITEMS && DEST[i].kind != D_UNAVAILABLE; }

static void index_refresh(struct shell *sh)
{
    sh->query[sh->qlen] = 0;
    sh->nres = (uint8_t)idx_search(ITEMS, NITEMS, sh->query, sh->recent, sh->nrecent, sh->res, SHELL_RESULTS);
    sh->sel = (int8_t)idx_default_active(sh->res, sh->nres);      /* never a typo suggestion: Enter must not open one by itself */
}

static void scpy(char *d, uint32_t cap, const char *s)
{
    uint32_t i = 0;
    for (; i + 1 < cap && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

static struct ui_widget *add(struct shell *sh, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint8_t layer, uint8_t kind,
                             const char *text, uint32_t fill, uint32_t id, uint8_t flags)
{
    if (sh->n >= SHELL_MAX_WIDGETS) return 0;                   /* a layout that outgrows the array is a bug: refuse, never overrun */
    struct ui_widget *k = &sh->w[sh->n++];
    uint8_t *z = (uint8_t *)k;
    for (uint32_t i = 0; i < sizeof *k; i++) z[i] = 0;
    k->c = (struct ui_ctrl){ x, y, w, h, r, id, flags, layer };
    k->kind = kind; k->text = text; k->fill = fill; k->ink = PAL(sh)->ink; k->border = PAL(sh)->muted; k->active_fill = PAL(sh)->accent; k->active_ink = PAL(sh)->active_ink; k->disabled_ink = PAL(sh)->muted; k->marker = PAL(sh)->marker;
    return k;
}

static struct ui_widget *label(struct shell *sh, int32_t x, int32_t y, int32_t w, int32_t h, const char *t, uint8_t size, uint32_t ink, uint8_t bold)
{
    struct ui_widget *k = add(sh, x, y, w, h, 0, UI_LAYER_CONTENT, UI_LABEL, t, PAL(sh)->surface, 0xF000u + sh->n, SOLID);
    if (k) { k->font_size = size; k->ink = ink; k->bold = bold; }
    return k;
}

static void card(struct shell *sh, int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct ui_widget *k = add(sh, x, y, w, h, 16, UI_LAYER_CONTENT, UI_PANEL, 0, PAL(sh)->surface, 0xF000u + sh->n, SOLID);
    if (k) { k->border_w = PAL(sh)->bw; k->border = PAL(sh)->border; }
}

static struct ui_widget *button(struct shell *sh, int32_t x, int32_t y, int32_t w, int32_t h, const char *t, uint32_t id, int enabled, uint8_t layer)
{
    struct ui_widget *k = add(sh, x, y, w, h, 12, layer, UI_BUTTON, t, PAL(sh)->canvas, id, enabled ? FOC : (UI_VISIBLE));
    if (k) { k->border_w = enabled ? PAL(sh)->bw : 1; k->border = enabled ? PAL(sh)->border : PAL(sh)->border_dis; k->font_size = 16; }
    return k;
}

static void build_dock(struct shell *sh)
{
    struct ui_widget *p = add(sh, 262, 664, 500, 80, 28, UI_LAYER_DOCK, UI_PANEL, 0, PAL(sh)->surface, 0xF100u, SOLID);
    if (p) { p->border_w = PAL(sh)->bw; p->border = PAL(sh)->border; }
    static const char *const T[4] = { "Back", "Home", "Spaces", "Index" };
    static const uint32_t ID[4] = { SID_DOCK_BACK, SID_DOCK_HOME, SID_DOCK_SPACES, SID_DOCK_INDEX };
    for (int i = 0; i < 4; i++) {
        int enabled = i != 0 || sh->hist_n > 0;                 /* Back is disabled when there is nowhere to go back to */
        struct ui_widget *b = button(sh, 280 + i * 118, 680, 112, 48, T[i], ID[i], enabled, UI_LAYER_DOCK);
        if (!b) continue;
        b->c.radius = 20;
        if ((i == 1 && sh->screen == SCR_HOME) || (i == 2 && sh->screen == SCR_SPACES) || (i == 3 && sh->screen == SCR_INDEX)) b->active = 1;   /* the active indicator */
    }
}

static void build_home(struct shell *sh)
{
    scpy(sh->space_txt, 32, SHELL_SPACE_NAMES[sh->space]);
    card(sh, 40, 96, 456, 200);
    label(sh, 64, 112, 400, 24, "Current Space", 14, PAL(sh)->muted, 0);
    label(sh, 64, 142, 400, 40, sh->space_txt, 24, PAL(sh)->ink, 1);
    label(sh, 64, 190, 408, 24, "Kept in memory only. Not saved.", 14, PAL(sh)->muted, 0);
    button(sh, 64, 232, 200, 48, "Change Space", SID_CHANGE_SPACE, 1, UI_LAYER_CONTENT);

    card(sh, 40, 320, 456, 296);
    label(sh, 64, 336, 400, 24, "System", 14, PAL(sh)->muted, 0);
    label(sh, 64, 366, 408, 28, "Boot checks passed", 18, PAL(sh)->ink, 1);
    label(sh, 64, 404, 408, 24, "Display 1024x768, drawn by the kernel", 14, PAL(sh)->ink, 0);
    label(sh, 64, 432, 408, 24, sh->caps.keyboard ? "Keyboard: found" : "Keyboard: not found", 14, PAL(sh)->ink, 0);
    label(sh, 64, 460, 408, 24, sh->caps.pointer ? "Pointer: found" : "Pointer: not found", 14, PAL(sh)->ink, 0);
    label(sh, 64, 488, 408, 24, "Network, battery and radio: not supported", 14, PAL(sh)->muted, 0);
    button(sh, 64, 548, 200, 48, "About", SID_ABOUT, 1, UI_LAYER_CONTENT);

    card(sh, 520, 96, 464, 520);
    label(sh, 544, 112, 400, 24, "Applications", 14, PAL(sh)->muted, 0);
    button(sh, 544, 148, 416, 56, "Spaces", SID_APP_SPACES, 1, UI_LAYER_CONTENT);
    button(sh, 544, 216, 416, 56, "Index", SID_APP_INDEX, 1, UI_LAYER_CONTENT);
    button(sh, 544, 284, 416, 56, "Pages", SID_APP_PAGES, 1, UI_LAYER_CONTENT);
    button(sh, 544, 352, 416, 56, "Aura", SID_APP_AURA, 1, UI_LAYER_CONTENT);
    button(sh, 544, 420, 416, 56, "Settings", SID_APP_SETTINGS, 1, UI_LAYER_CONTENT);
    label(sh, 544, 496, 416, 24, "Aura runs typed quick commands. No model.", 14, PAL(sh)->muted, 0);
}

static void build_spaces(struct shell *sh)
{
    card(sh, 40, 96, 944, 520);
    label(sh, 64, 112, 400, 40, "Spaces", 24, PAL(sh)->ink, 1);
    label(sh, 64, 156, 880, 24, "Choose a Space. The choice is shown on Home.", 14, PAL(sh)->muted, 0);
    for (uint32_t i = 0; i < SHELL_SPACES; i++) {
        struct ui_widget *b = button(sh, 64, 196 + (int32_t)i * 72, 400, 56, SHELL_SPACE_NAMES[i], SID_SPACE_BASE + i, 1, UI_LAYER_CONTENT);
        if (b && sh->space == i) b->active = 1;
    }
    label(sh, 64, 496, 880, 24, "Kept in memory only. Not saved. A Space does not isolate anything yet.", 14, PAL(sh)->muted, 0);
}

static void build_index(struct shell *sh)
{
    card(sh, 40, 96, 944, 520);
    label(sh, 64, 108, 400, 40, "Index", 24, PAL(sh)->ink, 1);
    {   /* the search field: typed text plus a caret at the end (the only editing supported is typing at the end and Backspace) */
        uint32_t n = 0;
        if (sh->qlen) { for (uint32_t i = 0; i < sh->qlen && n + 2 < sizeof sh->field_txt; i++) sh->field_txt[n++] = sh->query[i]; sh->field_txt[n++] = '_'; }
        else { const char *ph = "Type to search"; while (*ph && n + 1 < sizeof sh->field_txt) sh->field_txt[n++] = *ph++; }
        sh->field_txt[n] = 0;
        struct ui_widget *f = add(sh, 64, 152, 880, 48, 12, UI_LAYER_CONTENT, UI_LABEL, sh->field_txt, PAL(sh)->canvas, 0xF400u, UI_VISIBLE);
        if (f) { f->border_w = 2; f->border = PAL(sh)->focus; f->font_size = 18; f->ink = sh->qlen ? PAL(sh)->ink : PAL(sh)->muted; }
    }
    label(sh, 64, 208, 880, 24, "Up/Down choose, Enter open, Esc clears then goes back.", 14, PAL(sh)->muted, 0);
    if (!sh->nres) { label(sh, 64, 248, 880, 28, sh->qlen ? "No results." : "Nothing to show.", 16, PAL(sh)->ink, 0); return; }
    for (uint32_t k = 0; k < sh->nres; k++) {
        const struct idx_item *it = &ITEMS[sh->res[k].item];
        int avail = DEST[sh->res[k].item].kind != D_UNAVAILABLE;
        char *t = sh->res_txt[k]; uint32_t n = 0;
        const char *pre = sh->res[k].fuzzy ? "Did you mean: " : "";
        for (; *pre && n + 1 < sizeof sh->res_txt[k]; pre++) t[n++] = *pre;
        for (const char *l = it->label; *l && n + 1 < sizeof sh->res_txt[k]; l++) t[n++] = *l;
        if (!avail) for (const char *u = " (not available yet)"; *u && n + 1 < sizeof sh->res_txt[k]; u++) t[n++] = *u;
        t[n] = 0;
        struct ui_widget *b = button(sh, 64, 244 + (int32_t)k * 44, 880, 40, t, SID_RESULT_BASE + k, avail, UI_LAYER_CONTENT);
        if (b && (int)k == sh->sel && avail) b->active = 1;      /* an unavailable row is never drawn as selected/openable */
    }
}

static void build_about(struct shell *sh)
{
    add(sh, 0, 0, (int32_t)sh->width, (int32_t)sh->height, 0, UI_LAYER_SCRIM, UI_PANEL, 0, PAL(sh)->scrim, 0xF200u, SOLID);
    struct ui_widget *d = add(sh, 262, 220, 500, 260, 20, UI_LAYER_MODAL, UI_PANEL, 0, PAL(sh)->surface, 0xF201u, SOLID);
    if (d) { d->border_w = 2; d->border = PAL(sh)->focus; }
    struct ui_widget *t = add(sh, 286, 240, 452, 36, 0, UI_LAYER_MODAL, UI_LABEL, "About AuroraOS", PAL(sh)->surface, 0xF202u, SOLID);
    if (t) { t->font_size = 18; t->bold = 1; }
    struct ui_widget *l1 = add(sh, 286, 288, 452, 24, 0, UI_LAYER_MODAL, UI_LABEL, "Native Tier 0 Clear shell on QEMU.", PAL(sh)->surface, 0xF203u, SOLID);
    struct ui_widget *l2 = add(sh, 286, 316, 452, 24, 0, UI_LAYER_MODAL, UI_LABEL, "Text is printable ASCII only.", PAL(sh)->surface, 0xF204u, SOLID);
    struct ui_widget *l3 = add(sh, 286, 344, 452, 24, 0, UI_LAYER_MODAL, UI_LABEL, "Nothing here grants permissions.", PAL(sh)->surface, 0xF205u, SOLID);
    if (l1) l1->font_size = 14;
    if (l2) l2->font_size = 14;
    if (l3) l3->font_size = 14;
    button(sh, 412, 404, 200, 48, "Close", SID_ABOUT_CLOSE, 1, UI_LAYER_MODAL);
}

/* Glass Corner: a small handle at the top right (layer CORNER, last in Tab order) that opens a "held items" sheet. There is no
 * notification source in the OS yet, so the sheet shows an honest empty state; nothing is fabricated. The sheet is modal for input
 * (focus contained, covered controls inert) but sits clear of the dock and the content, and a click outside it closes it. */
static void build_corner(struct shell *sh)
{
    if (!sh->about_open) {
        struct ui_widget *g = button(sh, 912, 8, 96, 48, "Held", SID_CORNER, 1, UI_LAYER_CORNER);   /* below the sheet's layer: unreachable while it is open */
        if (g) g->c.radius = 18;
    }
    if (!sh->corner_open) return;
    struct ui_widget *p = add(sh, 584, 76, 424, 232, 16, UI_LAYER_SHEET, UI_PANEL, 0, PAL(sh)->surface, 0xF500u, SOLID);
    if (p) { p->border_w = 2; p->border = PAL(sh)->focus; }
    struct ui_widget *t = add(sh, 608, 92, 376, 32, 0, UI_LAYER_SHEET, UI_LABEL, "Held items", PAL(sh)->surface, 0xF501u, SOLID);
    if (t) { t->font_size = 18; t->bold = 1; }
    struct ui_widget *a = add(sh, 608, 132, 376, 28, 0, UI_LAYER_SHEET, UI_LABEL, "Nothing is held right now.", PAL(sh)->surface, 0xF502u, SOLID);
    if (a) a->font_size = 16;
    struct ui_widget *b = add(sh, 608, 164, 376, 24, 0, UI_LAYER_SHEET, UI_LABEL, "No notification source exists yet.", PAL(sh)->surface, 0xF503u, SOLID);
    if (b) { b->font_size = 14; b->ink = PAL(sh)->muted; }
    button(sh, 608, 236, 160, 48, "Close", SID_CORNER_CLOSE, 1, UI_LAYER_SHEET);
}


/* ============================ adaptive layout (text scale other than 100 percent) ============================
 * The 100 percent layout above is the reference and is tested pixel for pixel. Any other scale uses this flow layout: every size comes from
 * a font role, text wraps to the width it has, buttons grow to hold their wrapped label (never below the 48 px reference touch size),
 * and at the largest scales optional lines are left out rather than clipped. Nothing is ever placed outside the content region. */
struct fnt { uint8_t size, mag; };

static struct fnt pick_font(uint32_t base, uint32_t pct)
{
    static const struct fnt L[] = { { 12, 1 }, { 14, 1 }, { 16, 1 }, { 18, 1 }, { 24, 1 }, { 14, 2 }, { 16, 2 }, { 18, 2 }, { 24, 2 } };
    uint32_t want = (base * pct + 50) / 100, best = 0, bd = 0xffffffffu;
    for (uint32_t i = 0; i < sizeof L / sizeof L[0]; i++) {
        uint32_t eff = (uint32_t)L[i].size * L[i].mag, d = eff > want ? eff - want : want - eff;
        if (d < bd || (d == bd && eff < (uint32_t)L[best].size * L[best].mag)) { bd = d; best = i; }
    }
    return L[best];
}

void shell_font_for(uint32_t base, uint32_t pct, uint8_t *size, uint8_t *mag)
{
    struct fnt f = pick_font(base, pct);
    if (size) *size = f.size;
    if (mag) *mag = f.mag;
}

static int32_t lines_for(struct fnt f, int bold, const char *t, int32_t inner_w)
{
    const struct gfx_font *g = gfx_font_find(bold ? 700 : 400, f.size);
    if (!g || !t) return 1;
    size_t n = 0; while (t[n] && n < 256) n++;
    int32_t avail = inner_w - 16, cell = g->advance * f.mag;
    if (avail < cell) avail = cell;
    size_t total = 0;
    if (gfx_text_wrap(g, t, n, avail / f.mag, 0, 0, &total) != GFX_OK || total < 1) return 1;
    return total > 16 ? 16 : (int32_t)total;
}

static int32_t lh_of(struct fnt f, int bold) { int32_t a = 0, l = 0; ui_text_metrics(f.size, (uint8_t)bold, f.mag, &a, &l); return l; }
static int32_t adv_of(struct fnt f, int bold) { int32_t a = 0, l = 0; ui_text_metrics(f.size, (uint8_t)bold, f.mag, &a, &l); return a; }
static void track(struct shell *sh, int32_t bottom) { if (bottom > sh->lay_bottom) sh->lay_bottom = bottom; }
static size_t slen_(const char *t) { size_t n = 0; while (t && t[n] && n < 256) n++; return n; }

/* A wrapped label. Returns its height. */
static int32_t sl(struct shell *sh, int32_t x, int32_t y, int32_t w, const char *t, uint32_t base, uint32_t ink, int bold)
{
    struct fnt f = pick_font(base, sh->text_pct);
    int32_t hh = lines_for(f, bold, t, w) * lh_of(f, bold) + 4;
    struct ui_widget *k = label(sh, x, y, w, hh, t, f.size, ink, (uint8_t)bold);
    if (k) { k->mag = f.mag; k->wrap = 1; }
    track(sh, y + hh);
    return hh;
}

/* A wrapped button: at least minh high and tall enough for its wrapped label. Returns its height. */
static int32_t sb(struct shell *sh, int32_t x, int32_t y, int32_t w, int32_t minh, const char *t, uint32_t id, int enabled, uint8_t layer)
{
    struct fnt f = pick_font(16, sh->text_pct);
    int32_t bw = enabled ? PAL(sh)->bw : 1;
    int32_t hh = lines_for(f, 0, t, w - 2 * bw) * lh_of(f, 0) + 16;
    if (hh < minh) hh = minh;
    struct ui_widget *k = button(sh, x, y, w, hh, t, id, enabled, layer);
    if (k) { k->font_size = f.size; k->mag = f.mag; k->wrap = 1; }
    track(sh, y + hh);
    return hh;
}

struct geo { int32_t W, H, hdr_h, rail_h, top, bottom, dock_y, dock_h, dock_x, dock_w, btn_w, btn_h; };

static struct geo make_geo(const struct shell *sh)
{
    struct geo g;
    uint32_t pct = sh->text_pct;
    struct fnt tf = pick_font(24, pct), bf = pick_font(16, pct), hf = pick_font(14, pct);
    g.W = (int32_t)sh->width; g.H = (int32_t)sh->height;
    g.btn_h = lh_of(bf, 0) + 16; if (g.btn_h < 48) g.btn_h = 48;
    g.btn_w = 6 * adv_of(bf, 0) + 32; if (g.btn_w < 112) g.btn_w = 112;      /* "Spaces" is the longest dock label */
    g.dock_h = g.btn_h + 32;
    g.dock_w = 4 * g.btn_w + 3 * 6 + 36;
    if (g.dock_w > g.W - 16) g.dock_w = g.W - 16;
    g.dock_x = (g.W - g.dock_w) / 2; g.dock_y = g.H - 24 - g.dock_h;
    int32_t hb = 4 * adv_of(bf, 0) + 32; if (hb < 96) hb = 96;                 /* the Held handle */
    int32_t row1 = lh_of(tf, 1) + 16; if (row1 < 64) row1 = 64;
    int32_t hh2 = g.btn_h + 16; if (row1 < hh2) row1 = hh2;
    int32_t title_w = 8 * adv_of(tf, 1) + 16;
    int32_t hz_w = (int32_t)slen_("Horizon: limited input") * adv_of(hf, 0) + 16, up_w = 9 * adv_of(hf, 0) + 16;
    int32_t room = (g.W - 16 - hb - 12 - up_w) - (24 + title_w + 16);
    g.hdr_h = room >= hz_w ? row1 : row1 + lh_of(hf, 0) + 8;
    g.rail_h = PAL(sh)->rail;
    g.top = g.hdr_h + g.rail_h + 16;
    g.bottom = g.dock_y - 16;
    return g;
}

static void header_scaled(struct shell *sh, const struct geo *g)
{
    const struct pal *P = PAL(sh);
    struct fnt tf = pick_font(24, sh->text_pct), bf = pick_font(16, sh->text_pct), hf = pick_font(14, sh->text_pct);
    add(sh, 0, 0, g->W, g->hdr_h, 0, UI_LAYER_CONTENT, UI_PANEL, 0, P->surface, 0xF300u, SOLID);
    int32_t row1 = lh_of(tf, 1) + 16; if (row1 < 64) row1 = 64;
    if (row1 < g->btn_h + 16) row1 = g->btn_h + 16;
    int32_t title_w = 8 * adv_of(tf, 1) + 16;
    struct ui_widget *t = add(sh, 24, (row1 - lh_of(tf, 1)) / 2, title_w, lh_of(tf, 1), 0, UI_LAYER_CONTENT, UI_LABEL, "AuroraOS", P->surface, 0xF301u, SOLID);
    if (t) { t->font_size = tf.size; t->mag = tf.mag; t->bold = 1; t->ink = P->ink; }
    int32_t hb = 4 * adv_of(bf, 0) + 32; if (hb < 96) hb = 96;
    int32_t up_w = 9 * adv_of(hf, 0) + 16, up_x = g->W - 16 - hb - 12 - up_w, lhh = lh_of(hf, 0);
    struct ui_widget *u = add(sh, up_x, (row1 - lhh) / 2, up_w, lhh, 0, UI_LAYER_CONTENT, UI_LABEL, sh->uptime_txt, P->surface, 0xF302u, SOLID);
    if (u) { u->font_size = hf.size; u->mag = hf.mag; u->ink = P->muted; }
    int32_t hz_w = (int32_t)slen_(sh->horizon_txt) * adv_of(hf, 0) + 16;
    int32_t hx = 24 + title_w + 16, hy = (row1 - lhh) / 2;
    if (g->hdr_h > row1) { hx = 24; hy = row1; }                                  /* no room beside the title: the status text gets its own row */
    struct ui_widget *hz = add(sh, hx, hy, hz_w, lhh, 0, UI_LAYER_CONTENT, UI_LABEL, sh->horizon_txt, P->surface, 0xF304u, SOLID);
    if (hz) { hz->font_size = hf.size; hz->mag = hf.mag; hz->ink = P->muted; }
    int full = sh->caps.keyboard && sh->caps.pointer;
    add(sh, 0, g->hdr_h, g->W, g->rail_h, 0, UI_LAYER_CONTENT, UI_PANEL, 0, full ? P->aura : P->muted, 0xF303u, SOLID);
}

static void dock_scaled(struct shell *sh, const struct geo *g)
{
    struct ui_widget *p = add(sh, g->dock_x, g->dock_y, g->dock_w, g->dock_h, 28, UI_LAYER_DOCK, UI_PANEL, 0, PAL(sh)->surface, 0xF100u, SOLID);
    if (p) { p->border_w = PAL(sh)->bw; p->border = PAL(sh)->border; }
    static const char *const T[4] = { "Back", "Home", "Spaces", "Index" };
    static const uint32_t ID[4] = { SID_DOCK_BACK, SID_DOCK_HOME, SID_DOCK_SPACES, SID_DOCK_INDEX };
    int32_t bw = (g->dock_w - 36 - 18) / 4;
    for (int i = 0; i < 4; i++) {
        int enabled = i != 0 || sh->hist_n > 0;
        struct ui_widget *b = button(sh, g->dock_x + 18 + i * (bw + 6), g->dock_y + 16, bw, g->btn_h, T[i], ID[i], enabled, UI_LAYER_DOCK);
        if (!b) continue;
        struct fnt f = pick_font(16, sh->text_pct);
        b->font_size = f.size; b->mag = f.mag; b->wrap = 1; b->c.radius = 20;
        if ((i == 1 && sh->screen == SCR_HOME) || (i == 2 && sh->screen == SCR_SPACES) || (i == 3 && sh->screen == SCR_INDEX)) b->active = 1;
    }
}

static struct ui_widget *card_s(struct shell *sh, int32_t x, int32_t y, int32_t w)
{
    struct ui_widget *k = add(sh, x, y, w, 8, 16, UI_LAYER_CONTENT, UI_PANEL, 0, PAL(sh)->surface, 0xF000u + sh->n, SOLID);
    if (k) { k->border_w = PAL(sh)->bw; k->border = PAL(sh)->border; }
    return k;
}
static void card_end(struct shell *sh, struct ui_widget *k, int32_t y_end)
{
    if (k) k->c.h = y_end + 16 - k->c.y;
    track(sh, y_end + 16);
}

static void home_scaled(struct shell *sh, const struct geo *g, int opt)
{
    const struct pal *P = PAL(sh);
    scpy(sh->space_txt, 32, SHELL_SPACE_NAMES[sh->space]);
    int32_t cw = (g->W - 80 - 24) / 2, lx = 40, rx = 40 + cw + 24, iw = cw - 32, bw = cw - 48, y = g->top;
    struct ui_widget *ca = card_s(sh, lx, y, cw);
    y += 12;
    y += sl(sh, lx + 16, y, iw, "Current Space", 14, P->muted, 0) + 2;
    y += sl(sh, lx + 16, y, iw, sh->space_txt, 24, P->ink, 1) + 2;
    y += sl(sh, lx + 16, y, iw, "Kept in memory only. Not saved.", 14, P->muted, 0) + 8;
    y += sb(sh, lx + 24, y, bw, 48, "Change Space", SID_CHANGE_SPACE, 1, UI_LAYER_CONTENT);
    card_end(sh, ca, y);
    y += 16 + 16;
    struct ui_widget *cb = card_s(sh, lx, y, cw);
    y += 12;
    if (opt) y += sl(sh, lx + 16, y, iw, "System", 14, P->muted, 0) + 2;     /* the caption is the first thing dropped at the largest scales */
    y += sl(sh, lx + 16, y, iw, "Boot checks passed", 18, P->ink, 1) + 2;
    if (opt) y += sl(sh, lx + 16, y, iw, "Display 1024x768, drawn by the kernel", 14, P->ink, 0);
    y += sl(sh, lx + 16, y, iw, sh->caps.keyboard ? "Keyboard: found" : "Keyboard: not found", 14, P->ink, 0);
    y += sl(sh, lx + 16, y, iw, sh->caps.pointer ? "Pointer: found" : "Pointer: not found", 14, P->ink, 0);
    if (opt) y += sl(sh, lx + 16, y, iw, "Network, battery and radio: not supported", 14, P->muted, 0);
    y += 8;
    y += sb(sh, lx + 24, y, bw, 48, "About", SID_ABOUT, 1, UI_LAYER_CONTENT);
    card_end(sh, cb, y);

    y = g->top;
    struct ui_widget *cc = card_s(sh, rx, y, cw);
    y += 12;
    y += sl(sh, rx + 16, y, iw, "Applications", 14, P->muted, 0) + 6;
    static const char *const AT[5] = { "Spaces", "Index", "Pages", "Aura", "Settings" };
    static const uint32_t AI[5] = { SID_APP_SPACES, SID_APP_INDEX, SID_APP_PAGES, SID_APP_AURA, SID_APP_SETTINGS };
    for (int i = 0; i < 5; i++) y += sb(sh, rx + 24, y, bw, 56, AT[i], AI[i], i < 5, UI_LAYER_CONTENT) + 8;
    if (opt) y += sl(sh, rx + 16, y, iw, "Aura runs typed quick commands. No model.", 14, P->muted, 0);
    card_end(sh, cc, y);
}

static void spaces_scaled(struct shell *sh, const struct geo *g, int opt)
{
    const struct pal *P = PAL(sh);
    int32_t y = g->top, w = g->W - 80;
    struct ui_widget *c = card_s(sh, 40, y, w);
    y += 12;
    y += sl(sh, 56, y, w - 32, "Spaces", 24, P->ink, 1) + 2;
    if (opt) y += sl(sh, 56, y, w - 32, "Choose a Space. The choice is shown on Home.", 14, P->muted, 0) + 4;
    for (uint32_t i = 0; i < SHELL_SPACES; i++) {
        int32_t bh = sb(sh, 64, y, w - 48 > 400 ? 400 : w - 48, 56, SHELL_SPACE_NAMES[i], SID_SPACE_BASE + i, 1, UI_LAYER_CONTENT);
        if (sh->space == i && sh->n) sh->w[sh->n - 1].active = 1;
        y += bh + 8;
    }
    y += sl(sh, 56, y, w - 32, "Kept in memory only. Not saved. A Space does not isolate anything yet.", 14, P->muted, 0);
    card_end(sh, c, y);
}

static void index_scaled(struct shell *sh, const struct geo *g, int opt)
{
    const struct pal *P = PAL(sh);
    int32_t y = g->top, w = g->W - 80;
    struct ui_widget *c = card_s(sh, 40, y, w);
    y += 12;
    y += sl(sh, 56, y, w - 32, "Index", 24, P->ink, 1) + 4;
    {
        uint32_t n = 0;
        if (sh->qlen) { for (uint32_t i = 0; i < sh->qlen && n + 2 < sizeof sh->field_txt; i++) sh->field_txt[n++] = sh->query[i]; sh->field_txt[n++] = '_'; }
        else { const char *ph = "Type to search"; while (*ph && n + 1 < sizeof sh->field_txt) sh->field_txt[n++] = *ph++; }
        sh->field_txt[n] = 0;
        struct fnt f = pick_font(18, sh->text_pct);
        int32_t fh = lh_of(f, 0) + 16; if (fh < 48) fh = 48;
        struct ui_widget *fl = add(sh, 56, y, w - 32, fh, 12, UI_LAYER_CONTENT, UI_LABEL, sh->field_txt, P->canvas, 0xF400u, UI_VISIBLE);
        if (fl) { fl->border_w = 2; fl->border = P->focus; fl->font_size = f.size; fl->mag = f.mag; fl->ink = sh->qlen ? P->ink : P->muted; }
        track(sh, y + fh);
        y += fh + 6;
    }
    if (opt) y += sl(sh, 56, y, w - 32, "Up/Down choose, Enter open, Esc clears then goes back.", 14, P->muted, 0) + 2;
    if (!sh->nres) { y += sl(sh, 56, y, w - 32, sh->qlen ? "No results." : "Nothing to show.", 16, P->ink, 0); card_end(sh, c, y); return; }
    struct fnt rf = pick_font(16, sh->text_pct);
    int32_t rh = lh_of(rf, 0) + 16; if (rh < 40) rh = 40;
    int32_t foot = lh_of(pick_font(14, sh->text_pct), 0) + 8;
    int32_t room = g->bottom - 16 - y - foot, rows = room / (rh + 4);
    if (rows < 1) rows = 1;
    if (rows > (int32_t)sh->nres) rows = sh->nres;
    int32_t first = 0;
    if (sh->nres > rows) { first = sh->sel >= rows ? sh->sel - rows + 1 : 0; if (first > sh->nres - rows) first = sh->nres - rows; }
    for (int32_t k = first; k < first + rows; k++) {
        const struct idx_item *it = &ITEMS[sh->res[k].item];
        int avail = DEST[sh->res[k].item].kind != D_UNAVAILABLE;
        char *t = sh->res_txt[k]; uint32_t n = 0;
        const char *pre = sh->res[k].fuzzy ? "Did you mean: " : "";
        for (; *pre && n + 1 < sizeof sh->res_txt[k]; pre++) t[n++] = *pre;
        for (const char *l = it->label; *l && n + 1 < sizeof sh->res_txt[k]; l++) t[n++] = *l;
        if (!avail) for (const char *u = " (not available yet)"; *u && n + 1 < sizeof sh->res_txt[k]; u++) t[n++] = *u;
        t[n] = 0;
        int32_t bh = sb(sh, 56, y, w - 32, rh, t, SID_RESULT_BASE + (uint32_t)k, avail, UI_LAYER_CONTENT);
        if (sh->n && k == sh->sel && avail) sh->w[sh->n - 1].active = 1;
        y += bh + 4;
    }
    if (sh->nres > rows) {                                                         /* the list is longer than the room: say which part is shown */
        static char note[40];
        uint32_t n = 0; const char *a = "Showing "; while (*a) note[n++] = *a++;
        uint32_t v = (uint32_t)first + 1; note[n++] = (char)('0' + v); note[n++] = '-'; v = (uint32_t)(first + rows); note[n++] = (char)('0' + v);
        a = " of "; while (*a) note[n++] = *a++;
        note[n++] = (char)('0' + sh->nres); note[n] = 0;
        y += sl(sh, 56, y, w - 32, note, 14, P->muted, 0);
    }
    card_end(sh, c, y);
}

static void about_scaled(struct shell *sh, const struct geo *g)
{
    const struct pal *P = PAL(sh);
    add(sh, 0, 0, g->W, g->H, 0, UI_LAYER_SCRIM, UI_PANEL, 0, P->scrim, 0xF200u, SOLID);
    int32_t w = 500 * (int32_t)sh->text_pct / 100; if (w < 500) w = 500; if (w > g->W - 48) w = g->W - 48;
    int32_t x = (g->W - w) / 2, y = g->hdr_h + 24;
    struct ui_widget *d = add(sh, x, y, w, 100, 20, UI_LAYER_MODAL, UI_PANEL, 0, P->surface, 0xF201u, SOLID);
    if (d) { d->border_w = 2; d->border = P->focus; }
    int32_t iy = y + 16, iw = w - 32;
    struct fnt f18 = pick_font(18, sh->text_pct), f14 = pick_font(14, sh->text_pct);
    static const char *const L[3] = { "Native Tier 0 Clear shell on QEMU.", "Text is printable ASCII only.", "Nothing here grants permissions." };
    int32_t th = lines_for(f18, 1, "About AuroraOS", iw) * lh_of(f18, 1) + 4;
    struct ui_widget *t = add(sh, x + 16, iy, iw, th, 0, UI_LAYER_MODAL, UI_LABEL, "About AuroraOS", P->surface, 0xF202u, SOLID);
    if (t) { t->font_size = f18.size; t->mag = f18.mag; t->bold = 1; t->wrap = 1; t->ink = P->ink; }
    iy += th + 4;
    for (int i = 0; i < 3; i++) {
        int32_t lh = lines_for(f14, 0, L[i], iw) * lh_of(f14, 0) + 4;
        struct ui_widget *l = add(sh, x + 16, iy, iw, lh, 0, UI_LAYER_MODAL, UI_LABEL, L[i], P->surface, 0xF203u + (uint32_t)i, SOLID);
        if (l) { l->font_size = f14.size; l->mag = f14.mag; l->wrap = 1; l->ink = P->ink; }
        iy += lh + 2;
    }
    iy += 10;
    int32_t bw = 200 * (int32_t)sh->text_pct / 100; if (bw < 200) bw = 200; if (bw > iw) bw = iw;
    int32_t bh = sb(sh, x + (w - bw) / 2, iy, bw, 48, "Close", SID_ABOUT_CLOSE, 1, UI_LAYER_MODAL);
    if (d) d->c.h = iy + bh + 16 - y;
}

static void corner_scaled(struct shell *sh, const struct geo *g)
{
    const struct pal *P = PAL(sh);
    struct fnt bf = pick_font(16, sh->text_pct), f18 = pick_font(18, sh->text_pct), f16 = bf, f14 = pick_font(14, sh->text_pct);
    int32_t row1 = g->hdr_h;
    {   /* recompute the first header row exactly as header_scaled does */
        struct fnt tf = pick_font(24, sh->text_pct);
        row1 = lh_of(tf, 1) + 16; if (row1 < 64) row1 = 64;
        if (row1 < g->btn_h + 16) row1 = g->btn_h + 16;
    }
    int32_t hb = 4 * adv_of(bf, 0) + 32; if (hb < 96) hb = 96;
    if (!sh->about_open) {
        struct ui_widget *h = button(sh, g->W - 16 - hb, (row1 - g->btn_h) / 2, hb, g->btn_h, "Held", SID_CORNER, 1, UI_LAYER_CORNER);   /* below the sheet's layer: unreachable while it is open */
        if (h) { h->c.radius = 18; h->font_size = bf.size; h->mag = bf.mag; }
    }
    if (!sh->corner_open) return;
    int32_t w = 424 * (int32_t)sh->text_pct / 100; if (w < 424) w = 424; if (w > g->W - 32) w = g->W - 32;
    int32_t x = g->W - 16 - w, y = g->hdr_h + g->rail_h + 8, iw = w - 32, iy = y + 16;
    struct ui_widget *p = add(sh, x, y, w, 100, 16, UI_LAYER_SHEET, UI_PANEL, 0, P->surface, 0xF500u, SOLID);
    if (p) { p->border_w = 2; p->border = P->focus; }
    int32_t th = lines_for(f18, 1, "Held items", iw) * lh_of(f18, 1) + 4;
    struct ui_widget *t = add(sh, x + 16, iy, iw, th, 0, UI_LAYER_SHEET, UI_LABEL, "Held items", P->surface, 0xF501u, SOLID);
    if (t) { t->font_size = f18.size; t->mag = f18.mag; t->bold = 1; t->wrap = 1; t->ink = P->ink; }
    iy += th + 4;
    int32_t ah = lines_for(f16, 0, "Nothing is held right now.", iw) * lh_of(f16, 0) + 4;
    struct ui_widget *a = add(sh, x + 16, iy, iw, ah, 0, UI_LAYER_SHEET, UI_LABEL, "Nothing is held right now.", P->surface, 0xF502u, SOLID);
    if (a) { a->font_size = f16.size; a->mag = f16.mag; a->wrap = 1; a->ink = P->ink; }
    iy += ah + 2;
    int32_t bh2 = lines_for(f14, 0, "No notification source exists yet.", iw) * lh_of(f14, 0) + 4;
    struct ui_widget *b = add(sh, x + 16, iy, iw, bh2, 0, UI_LAYER_SHEET, UI_LABEL, "No notification source exists yet.", P->surface, 0xF503u, SOLID);
    if (b) { b->font_size = f14.size; b->mag = f14.mag; b->wrap = 1; b->ink = P->muted; }
    iy += bh2 + 12;
    int32_t cw = 160 * (int32_t)sh->text_pct / 100; if (cw < 160) cw = 160; if (cw > iw) cw = iw;
    int32_t ch = sb(sh, x + 24, iy, cw, 48, "Close", SID_CORNER_CLOSE, 1, UI_LAYER_SHEET);
    if (p) p->c.h = iy + ch + 16 - y;
}

static void build(struct shell *sh);

/* ============================ Settings (native screen over the typed registry) ============================ */
enum { K_WORKS = 0, K_STORED, K_SESSION };
struct srow { uint8_t id, kind, nvals; uint16_t vals[6]; const char *const *vnames; const char *name, *note; uint16_t def; };
static const char *const V_THEME[] = { "Auto", "Light", "Dark" };
static const char *const V_SCALE[] = { "75%", "100%", "125%", "150%", "175%", "200%" };
static const char *const V_ONOFF[] = { "Off", "On" };
static const char *const V_FOCUS[] = { "Off", "Work", "Rest", "Sleep" };
static const char *const V_AURA[] = { "Off", "Suggest", "Act with review" };
static const char *const V_NOTIF[] = { "All", "Priority only", "None" };
static const struct srow SROWS[SHELL_SET_ROWS] = {
    { SET_THEME, K_STORED, 3, { 0, 1, 2 }, V_THEME, "Theme", "stored only, no dark theme yet", 0 },
    { SET_TEXT_SCALE, K_WORKS, 6, { 75, 100, 125, 150, 175, 200 }, V_SCALE, "Text size", "works now", 100 },
    { SET_REDUCED_MOTION, K_STORED, 2, { 0, 1 }, V_ONOFF, "Reduced motion", "stored only, nothing animates", 0 },
    { SET_FOCUS_MODE, K_STORED, 4, { 0, 1, 2, 3 }, V_FOCUS, "Focus mode", "stored only", 0 },
    { SET_AURA_MODE, K_STORED, 3, { 0, 1, 2 }, V_AURA, "Aura mode", "stored only, Aura does not exist, needs system permission", 1 },
    { SET_NOTIFICATIONS, K_STORED, 3, { 0, 1, 2 }, V_NOTIF, "Notifications", "stored only, no notification source", 0 },
    { 0, K_SESSION, 2, { 0, 1 }, V_ONOFF, "High contrast", "works now, this session only", 0 },
};

uint32_t shell_setting_id(uint32_t row) { return row < SHELL_SET_ROWS ? SROWS[row].id : 0xffffffffu; }
uint32_t shell_setting_default(uint32_t row) { return row < SHELL_SET_ROWS ? SROWS[row].def : 0; }
uint32_t shell_setting_values(uint32_t row, uint32_t *vals, uint32_t max)
{
    if (row >= SHELL_SET_ROWS) return 0;
    for (uint32_t i = 0; i < SROWS[row].nvals && i < max && vals; i++) vals[i] = SROWS[row].vals[i];
    return SROWS[row].nvals;
}

static uint32_t set_current(struct shell *sh, uint32_t row)
{
    uint32_t v = 0;
    if (SROWS[row].id && sh->store && sh->store->get && sh->store->get(SROWS[row].id, &v) == SETTINGS_OK) return v;
    return sh->sess[row];
}

static void put_num(char *d, uint32_t cap, uint32_t v, const char *suffix)
{
    char t[12]; int k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 11);
    uint32_t n = 0; while (k && n + 1 < cap) d[n++] = t[--k];
    while (*suffix && n + 1 < cap) d[n++] = *suffix++;
    d[n] = 0;
}

static void set_texts(struct shell *sh)
{
    for (uint32_t r = 0; r < SHELL_SET_ROWS; r++) {
        uint32_t v = set_current(sh, r), i;
        for (i = 0; i < SROWS[r].nvals && SROWS[r].vals[i] != v; i++) {}
        if (i < SROWS[r].nvals) scpy(sh->set_val[r], sizeof sh->set_val[r], SROWS[r].vnames[i]);
        else put_num(sh->set_val[r], sizeof sh->set_val[r], v, SROWS[r].id == SET_TEXT_SCALE ? "%" : "");   /* a stored value outside the offered steps is shown as it is */
        uint32_t n = 0;
        for (const char *a = SROWS[r].name; *a && n + 1 < sizeof sh->set_lab[r]; a++) sh->set_lab[r][n++] = *a;
        for (const char *a = " - "; *a && n + 1 < sizeof sh->set_lab[r]; a++) sh->set_lab[r][n++] = *a;
        const char *note = sh->text_pct > 125 ? (SROWS[r].kind == K_WORKS ? "works now" : SROWS[r].kind == K_SESSION ? "session only" : "stored only") : SROWS[r].note;   /* short at large text so more rows fit */
        for (const char *a = note; *a && n + 1 < sizeof sh->set_lab[r]; a++) sh->set_lab[r][n++] = *a;
        sh->set_lab[r][n] = 0;
    }
}

static const char *set_status_text(const struct shell *sh)
{
    switch (sh->set_msg) {
    case SM_SAVED: return "Saved to disk.";
    case SM_RESET_SAVED: return "Reset and saved to disk.";
    case SM_SESSION: return "This session only. Nothing is saved.";
    case SM_STORE_FAIL: return "Not saved. Value unchanged.";
    case SM_DENIED: return "Not permitted: no permission.";
    case SM_VERSION: return "Not saved: newer stored version.";
    case SM_HC_ON: return "High contrast on. Not saved.";
    case SM_HC_OFF: return "High contrast off.";
    case SM_AURA_LOCAL: return "Not permitted: needs system permission.";
    default: return "Enter or click to change.";
    }
}

/* One user action on Settings row r: new value `nv` (or the default when reset). The registry is the authority; the shell only reports its answer. */
static void set_change(struct shell *sh, uint32_t r, int reset)
{
    if (r >= SHELL_SET_ROWS) return;
    const struct srow *w = &SROWS[r];
    uint32_t cur = set_current(sh, r), nv = w->def, i;
    if (!reset) {
        for (i = 0; i < w->nvals && w->vals[i] != cur; i++) {}
        if (i < w->nvals) nv = w->vals[(i + 1) % w->nvals];
        else { nv = w->vals[0]; for (i = 0; i < w->nvals; i++) if (w->vals[i] > cur) { nv = w->vals[i]; break; } }
    }
    sh->set_row = (uint8_t)r; sh->set_seq++;
    if (!w->id) {                                                   /* high contrast: a runtime preference, never stored */
        sh->sess[r] = nv; shell_set_contrast(sh, (int)nv);
        sh->set_msg = nv ? SM_HC_ON : SM_HC_OFF;
        return;
    }
    if (sh->store && sh->store->set) {
        int rc = reset ? (sh->store->reset ? sh->store->reset(w->id) : SETTINGS_EARG) : sh->store->set(w->id, nv);
        if (rc == SETTINGS_OK) { sh->set_msg = reset ? SM_RESET_SAVED : SM_SAVED; if (w->id == SET_TEXT_SCALE) shell_set_text_scale(sh, nv); }
        else sh->set_msg = rc == SETTINGS_EPERM ? SM_DENIED : rc == SETTINGS_EVERSION ? SM_VERSION : SM_STORE_FAIL;   /* nothing changed, nothing applied */
        return;
    }
    if (w->id == SET_AURA_MODE) { sh->set_msg = SM_AURA_LOCAL; return; }   /* system policy: the shell cannot grant itself this, with or without a store */
    sh->sess[r] = nv; sh->set_msg = SM_SESSION;
    if (w->id == SET_TEXT_SCALE) shell_set_text_scale(sh, nv);
}

void shell_set_store(struct shell *sh, const struct shell_store *store)
{
    sh->store = store;
    uint32_t v = 0;
    if (store && store->get && store->get(SET_TEXT_SCALE, &v) == SETTINGS_OK && v >= SHELL_TEXT_MIN && v <= SHELL_TEXT_MAX) shell_set_text_scale(sh, v);
    build(sh); sh->dirty = 1;
}

static void settings_flow(struct shell *sh, const struct geo *g, int opt)
{
    const struct pal *P = PAL(sh);
    set_texts(sh);
    int32_t y = g->top, w = g->W - 80, iw = w - 32;
    struct ui_widget *c = card_s(sh, 40, y, w);
    y += 12;
    y += sl(sh, 56, y, iw, "Settings", 24, P->ink, 1) + 2;
    int32_t foot = 0;
    foot += lines_for(pick_font(14, sh->text_pct), 0, set_status_text(sh), iw) * lh_of(pick_font(14, sh->text_pct), 0) + 4;
    if (opt) foot += lines_for(pick_font(14, sh->text_pct), 0, "Brightness, sound, Wi-Fi and battery: not supported, there are no drivers.", iw) * lh_of(pick_font(14, sh->text_pct), 0) + 4;
    int32_t rowh[SHELL_SET_ROWS], total = 0;
    int32_t lw = iw * 38 / 100, vw = iw * 38 / 100, rw = iw * 20 / 100;
    struct fnt bf = pick_font(16, sh->text_pct);
    for (uint32_t r = 0; r < SHELL_SET_ROWS; r++) {
        int32_t lh = lines_for(pick_font(14, sh->text_pct), 0, sh->set_lab[r], lw) * lh_of(pick_font(14, sh->text_pct), 0) + 4;
        int32_t bh = lines_for(bf, 0, sh->set_val[r], vw - 2) * lh_of(bf, 0) + 16; if (bh < 48) bh = 48;
        rowh[r] = (lh > bh ? lh : bh) + 8;
        total += rowh[r];
    }
    int32_t room = g->bottom - 16 - y - foot;
    int need_scroll = total > room;
    if (need_scroll) room -= 2 * (48 + 8);
    uint32_t first = sh->set_first, last;
    if (first >= SHELL_SET_ROWS) first = SHELL_SET_ROWS - 1;
    if (!need_scroll) first = 0;
    int32_t used = 0;
    for (last = first; last < SHELL_SET_ROWS && (used + rowh[last] <= room || last == first); last++) used += rowh[last];
    if (need_scroll && last == SHELL_SET_ROWS) {                      /* at the end: show as many rows as fit, counted back from the last */
        used = 0; first = SHELL_SET_ROWS;
        while (first > 0 && used + rowh[first - 1] <= room) { first--; used += rowh[first]; }
        if (first == SHELL_SET_ROWS) first = SHELL_SET_ROWS - 1;
    }
    sh->set_first = (uint8_t)first;
    if (need_scroll && first > 0) y += sb(sh, 56, y, iw, 48, "Earlier settings", SID_SET_UP, 1, UI_LAYER_CONTENT) + 8;
    for (uint32_t r = first; r < last; r++) {
        int32_t ry = y;
            int32_t lh = sl(sh, 56, ry, lw, sh->set_lab[r], 14, P->muted, 0);
            int32_t bh = sb(sh, 56 + lw + 8, ry, vw, 48, sh->set_val[r], SID_SET_VALUE + r, 1, UI_LAYER_CONTENT);
            sb(sh, 56 + lw + 8 + vw + 8, ry, rw, 48, "Reset", SID_SET_RESET + r, SROWS[r].id != 0 || sh->sess[r] != 0, UI_LAYER_CONTENT);
            y = ry + (lh > bh ? lh : bh) + 8;
    }
    if (need_scroll && last < SHELL_SET_ROWS) y += sb(sh, 56, y, iw, 48, "More settings", SID_SET_DOWN, 1, UI_LAYER_CONTENT) + 8;
    y += sl(sh, 56, y, iw, set_status_text(sh), 14, sh->set_msg == SM_NONE || sh->set_msg == SM_SAVED || sh->set_msg == SM_RESET_SAVED ? P->muted : P->ink, sh->set_msg >= SM_STORE_FAIL && sh->set_msg != SM_HC_ON && sh->set_msg != SM_HC_OFF);
    if (opt) y += sl(sh, 56, y, iw, "Brightness, sound, Wi-Fi and battery: not supported, there are no drivers.", 14, P->muted, 0);
    card_end(sh, c, y);
}


/* ---- Pages: a live, read-only view of the real process table (procsnap). Nothing is simulated; there is no Kill/Pause/Resume because no secure
 * service for them exists. Text comes only from the snapshot (names are sanitised there). ---- */
static void pcat(char *d, uint32_t cap, const char *t) { uint32_t n = 0; while (n + 1 < cap && d[n]) n++; for (uint32_t i = 0; n + 1 < cap && t[i]; i++) d[n++] = t[i]; d[n] = 0; }
static void pnum(char *d, uint32_t cap, uint64_t v) { char t[24]; int k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 24); char o[25]; int m = 0; while (k) o[m++] = t[--k]; o[m] = 0; pcat(d, cap, o); }
static void ppad(char *d, uint32_t cap, const char *t, uint32_t w) { uint32_t b = 0; while (d[b]) b++; pcat(d, cap, t); uint32_t n = 0; while (d[n]) n++; while (n - b < w && n + 1 < cap) { d[n++] = ' '; d[n] = 0; } }

static void pages_texts(struct shell *sh)
{
    int compact = sh->text_pct > 125;
    for (uint32_t i = 0; i < PROC_MAX; i++) sh->pg_txt[i][0] = 0;
    sh->pg_head[0] = 0;
    if (!compact && sh->ps.n) pcat(sh->pg_head, sizeof sh->pg_head, "pid name            state    ticks switches yields preempt pages");
    for (uint32_t i = 0; i < sh->ps.n && i < PROC_MAX; i++) {
        const struct procsnap_entry *e = &sh->ps.e[i];
        char *d = sh->pg_txt[i]; const uint32_t cap = sizeof sh->pg_txt[i];
        if (compact) {
            pcat(d, cap, "p"); pnum(d, cap, e->pid); pcat(d, cap, " "); pcat(d, cap, e->name); pcat(d, cap, " "); pcat(d, cap, procsnap_state_name(e->state));
            pcat(d, cap, " "); pnum(d, cap, e->ticks); pcat(d, cap, "t "); pnum(d, cap, e->pages); pcat(d, cap, "pg");
        } else {
            char t[24]; t[0] = 0; pnum(t, sizeof t, e->pid); ppad(d, cap, t, 4);
            ppad(d, cap, e->name, 16); ppad(d, cap, procsnap_state_name(e->state), 9);
            t[0] = 0; pnum(t, sizeof t, e->ticks); ppad(d, cap, t, 6);
            t[0] = 0; pnum(t, sizeof t, e->switches); ppad(d, cap, t, 9);
            t[0] = 0; pnum(t, sizeof t, e->yields); ppad(d, cap, t, 7);
            t[0] = 0; pnum(t, sizeof t, e->preemptions); ppad(d, cap, t, 8);
            pnum(d, cap, e->pages);
            if (e->state == PSS_EXITED) { pcat(d, cap, "  exit "); pnum(d, cap, (uint64_t)(e->exit_code < 0 ? -(int64_t)e->exit_code : e->exit_code)); }
        }
    }
    sh->pg_sum[0] = 0;
    if (!sh->pages_src) pcat(sh->pg_sum, sizeof sh->pg_sum, "Process list unavailable.");
    else if (!sh->ps.n) pcat(sh->pg_sum, sizeof sh->pg_sum, "No processes are running.");
    else { pnum(sh->pg_sum, sizeof sh->pg_sum, sh->ps.n); pcat(sh->pg_sum, sizeof sh->pg_sum, sh->ps.n == 1 ? " process" : " processes"); }
}

static void pages_flow(struct shell *sh, const struct geo *g, int opt)
{
    const struct pal *P = PAL(sh);
    pages_texts(sh);
    int32_t y = g->top, w = g->W - 80, iw = w - 32;
    struct ui_widget *c = card_s(sh, 40, y, w);
    y += 12;
    y += sl(sh, 56, y, iw, "Pages", 24, P->ink, 1) + 2;
    if (opt) y += sl(sh, 56, y, iw, "Live processes. Read-only: nothing here can stop or change them.", 14, P->muted, 0) + 4;
    if (sh->pg_head[0]) y += sl(sh, 56, y, iw, sh->pg_head, 14, P->muted, 0);
    for (uint32_t i = 0; i < sh->ps.n && i < PROC_MAX; i++)
        y += sl(sh, 56, y, iw, sh->pg_txt[i], 14, P->ink, sh->ps.e[i].state == PSS_FAULTED);
    y += sl(sh, 56, y, iw, sh->pg_sum, 14, P->muted, 0) + 12;
    y += sb(sh, 56, y, 200 < iw ? 200 : iw, 48, "Home", SID_PAGES_HOME, 1, UI_LAYER_CONTENT) + 8;
    card_end(sh, c, y);
}

int shell_pages_refresh(struct shell *sh)
{
    struct procsnap t;
    t.n = 0;
    if (sh->pages_src) sh->pages_src(&t);
    uint64_t d = procsnap_digest(&t);
    if (sh->ps_valid && d == sh->ps_digest) return 0;
    sh->ps = t; sh->ps_digest = d; sh->ps_valid = 1;
    if (sh->screen == SCR_PAGES) { build(sh); sh->dirty = 1; }
    return 1;
}

void shell_set_pages(struct shell *sh, uint32_t (*src)(struct procsnap *out))
{
    sh->pages_src = src; sh->ps_valid = 0;
    shell_pages_refresh(sh);
}

/* ---- Aura: type a quick command, see what it would do, run it through the Action Broker, read the real result. Deterministic; no language model. ---- */
static const struct aura_caps AURA_PREVIEW_CAPS = { 1, 0, 0, 0, 0 };         /* only for the preview; the broker applies the platform's real capabilities */

static void aura_refresh(struct shell *sh)
{
    sh->aura_in[sh->aura_len] = 0;
    struct aura_caps pc = AURA_PREVIEW_CAPS; pc.timer = sh->timers != 0;          /* the preview offers timer commands only when the Clock has a service */
    aura_parse(sh->aura_in, sh->aura_len, &pc, &sh->aura_pv);
}

static void aura_submit(struct shell *sh)
{
    if (!sh->aura_len) return;
    sh->aura_runs++;
    uint8_t st = 0; char m[64]; m[0] = 0;
    int ok = sh->aura_run ? sh->aura_run(sh, sh->aura_in, sh->aura_len, &st, m, sizeof m) : 0;
    if (!ok) { scpy(sh->aura_msg, sizeof sh->aura_msg, "Aura is not connected. Nothing changed."); sh->aura_state = 6; }
    else { scpy(sh->aura_msg, sizeof sh->aura_msg, m[0] ? m : "Nothing changed."); sh->aura_state = st ? st : 6; }
    if (ok && st == 1) { sh->aura_len = 0; sh->aura_in[0] = 0; aura_refresh(sh); }      /* a command that was done is consumed; one that was refused stays so it can be corrected */
    build(sh); sh->dirty = 1;
}

void shell_set_aura(struct shell *sh, int (*run)(struct shell *sh, const char *text, size_t len, uint8_t *state, char *msg, size_t cap))
{
    sh->aura_run = run;
}

static void aura_texts(struct shell *sh)
{
    uint32_t n = 0;
    if (sh->aura_len) { for (uint32_t i = 0; i < sh->aura_len && n + 2 < sizeof sh->aura_field; i++) sh->aura_field[n++] = sh->aura_in[i]; sh->aura_field[n++] = '_'; }
    else { const char *ph = "Type a command, e.g. open settings"; while (*ph && n + 1 < sizeof sh->aura_field) sh->aura_field[n++] = *ph++; }
    sh->aura_field[n] = 0;
    sh->aura_prev[0] = 0;
    if (sh->aura_len) {
        const char *how = "";
        switch (sh->aura_pv.status) {
        case AS_ACTIONABLE: how = " - runs when you press Enter"; break;
        case AS_NEEDS_APPROVAL: how = " - needs approval, not available yet"; break;
        case AS_UNSUPPORTED: how = " - not available"; break;
        case AS_CLARIFY: how = " - unclear"; break;
        default: how = " - will not run"; break;
        }
        pcat(sh->aura_prev, sizeof sh->aura_prev, "Preview: "); pcat(sh->aura_prev, sizeof sh->aura_prev, sh->aura_pv.preview[0] ? sh->aura_pv.preview : "not understood"); pcat(sh->aura_prev, sizeof sh->aura_prev, how);
    } else pcat(sh->aura_prev, sizeof sh->aura_prev, "Preview: nothing typed yet");
    sh->aura_res[0] = 0;
    pcat(sh->aura_res, sizeof sh->aura_res, "Result: ");
    pcat(sh->aura_res, sizeof sh->aura_res, sh->aura_state ? sh->aura_msg : "nothing run yet");
}

static void aura_flow(struct shell *sh, const struct geo *g, int opt)
{
    const struct pal *P = PAL(sh);
    aura_texts(sh);
    int32_t y = g->top, w = g->W - 80, iw = w - 32;
    struct ui_widget *c = card_s(sh, 40, y, w);
    y += 12;
    y += sl(sh, 56, y, iw, "Aura", 24, P->ink, 1) + 2;
    if (opt) y += sl(sh, 56, y, iw, "Quick commands. Nothing runs until you press Enter. No language model.", 14, P->muted, 0) + 4;
    {
        struct fnt f = pick_font(18, sh->text_pct);
        int32_t fh = lh_of(f, 0) * lines_for(f, 0, sh->aura_field, iw - 24) + 16; if (fh < 48) fh = 48;
        struct ui_widget *fl = add(sh, 56, y, iw, fh, 12, UI_LAYER_CONTENT, UI_LABEL, sh->aura_field, P->canvas, 0xF410u, UI_VISIBLE);
        if (fl) { fl->border_w = 2; fl->border = P->focus; fl->font_size = f.size; fl->mag = f.mag; fl->ink = sh->aura_len ? P->ink : P->muted; }
        track(sh, y + fh);
        y += fh + 6;
    }
    y += sl(sh, 56, y, iw, sh->aura_prev, 14, P->muted, 0) + 4;
    {
        int32_t bw = iw / 2 - 4, h1 = sb(sh, 56, y, bw, 48, "Run", SID_AURA_RUN, sh->aura_len > 0, UI_LAYER_CONTENT);
        int32_t h2 = sb(sh, 56 + bw + 8, y, bw, 48, "Clear", SID_AURA_CLEAR, sh->aura_len > 0 || sh->aura_state, UI_LAYER_CONTENT);
        y += (h1 > h2 ? h1 : h2) + 8;
    }
    y += sl(sh, 56, y, iw, sh->aura_res, 16, P->ink, sh->aura_state != 0) + 4;
    if (opt) y += sl(sh, 56, y, iw, "Try: open settings, open pages, switch to work space.", 14, P->muted, 0) + 4;
    y += sb(sh, 56, y, 200 < iw ? 200 : iw, 48, "Home", SID_AURA_HOME, 1, UI_LAYER_CONTENT) + 8;
    card_end(sh, c, y);
}

/* Typed text goes to the command field; Enter runs it unless a control has focus (then that control handles Enter); Escape clears a non-empty command. */
static int aura_input(struct shell *sh, const struct input_event *e)
{
    if (sh->screen != SCR_AURA || sh->about_open || sh->corner_open || sh->review_open) return 0;
    if (e->kind == IN_TEXT) {
        sh->fx.focus = -1;
        if (e->ch >= 0x20 && e->ch <= 0x7e && sh->aura_len < AURA_CMD_MAX) { sh->aura_in[sh->aura_len++] = (char)e->ch; sh->aura_msg[0] = 0; sh->aura_state = 0; aura_refresh(sh); }
        return 1;
    }
    if (e->kind != IN_KEY_DOWN) return 0;
    switch (e->key) {
    case KEY_BACKSPACE: sh->fx.focus = -1; if (sh->aura_len) { sh->aura_len--; sh->aura_msg[0] = 0; sh->aura_state = 0; aura_refresh(sh); } return 1;
    case KEY_SPACE: return 1;
    case KEY_ENTER:
        if (sh->fx.focus >= 0) return 0;
        if (!e->repeat) aura_submit(sh);
        return 1;
    case KEY_ESC:
        if (sh->aura_len) { sh->aura_len = 0; sh->aura_msg[0] = 0; sh->aura_state = 0; aura_refresh(sh); return 1; }
        return 0;
    default: return 0;
    }
}

/* ---- Clock / Timer: a real countdown on the timer service. Everything shown is measured: uptime and the remaining time come from the monotonic counter. ---- */
static const uint32_t CLK_PRESET_S[3] = { 60, 300, 1500 };
static const char *const CLK_PRESET_NAME[3] = { "1 min", "5 min", "25 min" };

static void put_ms(char *d, uint32_t cap, uint32_t s)             /* m:ss (minutes may exceed 59) */
{
    char t[16]; uint32_t n = 0, m = s / 60; char dig[8]; int k = 0;
    do { dig[k++] = (char)('0' + m % 10); m /= 10; } while (m && k < 8);
    while (k) t[n++] = dig[--k];
    t[n++] = ':'; t[n++] = (char)('0' + (s % 60) / 10); t[n++] = (char)('0' + s % 10); t[n] = 0;
    scpy(d, cap, t);
}
static void put_hms(char *d, uint32_t cap, uint32_t s)            /* "Up h:mm:ss" */
{
    char t[24]; uint32_t n = 0, h = s / 3600, m = (s / 60) % 60; char dig[8]; int k = 0;
    t[n++] = 'U'; t[n++] = 'p'; t[n++] = ' ';
    do { dig[k++] = (char)('0' + h % 10); h /= 10; } while (h && k < 8);
    while (k) t[n++] = dig[--k];
    t[n++] = ':'; t[n++] = (char)('0' + m / 10); t[n++] = (char)('0' + m % 10); t[n++] = ':'; t[n++] = (char)('0' + (s % 60) / 10); t[n++] = (char)('0' + s % 10); t[n] = 0;
    scpy(d, cap, t);
}
static uint8_t clk_eff(const struct shell *sh) { return sh->timers ? sh->clk_state : CK_UNAVAILABLE; }
static void clk_update_live(struct shell *sh)
{
    char t[64]; const char *src = "";
    switch (clk_eff(sh)) {
    case CK_UNAVAILABLE: src = "Timer service unavailable. Nothing can be started."; break;
    case CK_RUNNING: src = "Running. Cancel stops it."; break;
    case CK_DONE: src = "Finished. Choose a time to run another."; break;
    case CK_CANCELLED: src = "Cancelled. Nothing is running."; break;
    case CK_ERROR: src = sh->clk_err == 1 ? "Could not start: too many timers." : sh->clk_err == 3 ? "The timer was lost. Nothing is running." : "Could not start: the service refused."; break;
    default: scpy(t, sizeof t, "Ready. "); { uint32_t n = 0; while (t[n]) n++; scpy(t + n, (uint32_t)sizeof t - n, CLK_PRESET_NAME[sh->clk_preset % 3]); } { uint32_t n = 0; while (t[n]) n++; scpy(t + n, (uint32_t)sizeof t - n, " selected. Press Start."); } src = t; break;
    }
    uint32_t i = 0; while (src[i] && src[i] == sh->clk_live[i]) i++;
    if (src[i] || sh->clk_live[i]) { scpy(sh->clk_live, sizeof sh->clk_live, src); sh->clk_seq++; }
    uint8_t e = clk_eff(sh);
    put_ms(sh->clk_val, sizeof sh->clk_val, e == CK_RUNNING ? sh->clk_rem_s : e == CK_DONE ? 0 : CLK_PRESET_S[sh->clk_preset % 3]);
    put_hms(sh->clk_up, sizeof sh->clk_up, sh->uptime_s);
}

static void clock_flow(struct shell *sh, const struct geo *g, int opt)
{
    const struct pal *P = PAL(sh);
    clk_update_live(sh);
    uint8_t e = clk_eff(sh);
    int can_start = (e == CK_READY || e == CK_DONE || e == CK_CANCELLED || e == CK_ERROR), can_cancel = (e == CK_RUNNING);
    int32_t y = g->top, w = g->W - 80, iw = w - 32;
    struct ui_widget *c = card_s(sh, 40, y, w);
    y += 12;
    y += sl(sh, 56, y, iw, "Clock", 24, P->ink, 1) + 2;
    if (opt) y += sl(sh, 56, y, iw, "Time since boot. No wall clock exists.", 14, P->muted, 0) + 4;
    y += sl(sh, 56, y, iw, sh->clk_up, 18, P->ink, 1) + 4;
    if (opt) y += sl(sh, 56, y, iw, "Timer", 14, P->muted, 0) + 2;
    y += sl(sh, 56, y, iw, sh->clk_val, 24, P->ink, 1) + 2;
    y += sl(sh, 56, y, iw, sh->clk_live, 16, P->ink, 0) + 8;
    int32_t gap = 8, bw = (iw - 2 * gap) / 3, rowh = 0;
    for (int k = 0; k < 3; k++) {
        int32_t h = sb(sh, 56 + k * (bw + gap), y, bw, 48, CLK_PRESET_NAME[k], SID_CLOCK_P1 + (uint32_t)k, can_start, UI_LAYER_CONTENT);
        if (sh->n && k == sh->clk_preset && sh->w[sh->n - 1].c.id == SID_CLOCK_P1 + (uint32_t)k) sh->w[sh->n - 1].active = 1;   /* the selected length: accent fill and, in high contrast, a marker */
        if (h > rowh) rowh = h;
    }
    y += rowh + 8;
    int32_t bw2 = (iw - gap) / 2, h1 = sb(sh, 56, y, bw2, 48, "Start", SID_CLOCK_START, can_start, UI_LAYER_CONTENT);
    int32_t h2 = sb(sh, 56 + bw2 + gap, y, bw2, 48, "Cancel", SID_CLOCK_CANCEL, can_cancel, UI_LAYER_CONTENT);
    y += (h1 > h2 ? h1 : h2) + 8;
    if (opt) y += sl(sh, 56, y, iw, "Stops if AuroraOS restarts. No sound.", 14, P->muted, 0) + 4;
    y += sb(sh, 56, y, 200 < iw ? 200 : iw, 48, "Home", SID_CLOCK_HOME, 1, UI_LAYER_CONTENT) + 8;
    card_end(sh, c, y);
}

void shell_set_timers(struct shell *sh, const struct shell_timers *timers)
{
    sh->timers = timers; sh->clk_state = CK_READY; sh->clk_id = -1; sh->clk_rem_s = 0;
    clk_update_live(sh);
    if (sh->screen == SCR_CLOCK) { build(sh); sh->dirty = 1; }
}

static void clk_start(struct shell *sh, uint32_t preset)
{
    if (clk_eff(sh) == CK_RUNNING || !sh->timers) return;
    sh->clk_preset = (uint8_t)(preset % 3);
    int id = sh->timers->start((uint64_t)CLK_PRESET_S[sh->clk_preset] * 1000u);
    if (id < 0) { sh->clk_state = CK_ERROR; sh->clk_err = (id == -406) ? 1 : 2; sh->clk_id = -1; }
    else { sh->clk_state = CK_RUNNING; sh->clk_id = id; sh->clk_rem_s = sh->clk_dur_s = CLK_PRESET_S[sh->clk_preset]; sh->clk_err = 0; }
    clk_update_live(sh);
}

int shell_clock_start(struct shell *sh, uint32_t seconds)
{
    if (!sh->timers || seconds < 1 || seconds > 86400u || clk_eff(sh) == CK_RUNNING) return 0;
    int id = sh->timers->start((uint64_t)seconds * 1000u);
    if (id < 0) { sh->clk_state = CK_ERROR; sh->clk_err = (id == -406) ? 1 : 2; sh->clk_id = -1; clk_update_live(sh); build(sh); sh->dirty = 1; return 0; }
    uint8_t st = 0; uint64_t rem = 0;
    if (sh->timers->query(id, &st, &rem) != 0 || st != 1 || rem > (uint64_t)seconds * 1000u) {       /* the service does not hold what we asked for: undo and say so */
        (void)sh->timers->cancel(id); (void)sh->timers->release(id);
        sh->clk_state = CK_ERROR; sh->clk_err = 2; sh->clk_id = -1; clk_update_live(sh); build(sh); sh->dirty = 1;
        return 0;
    }
    sh->clk_state = CK_RUNNING; sh->clk_id = id; sh->clk_rem_s = sh->clk_dur_s = seconds; sh->clk_err = 0;
    clk_update_live(sh); build(sh); sh->dirty = 1;
    return 1;
}

static void clk_cancel(struct shell *sh)
{
    if (clk_eff(sh) != CK_RUNNING) return;
    int rc = sh->timers->cancel(sh->clk_id);
    if (rc == 0) { (void)sh->timers->release(sh->clk_id); sh->clk_state = CK_CANCELLED; sh->clk_id = -1; sh->clk_rem_s = 0; }
    else { sh->clk_state = CK_ERROR; sh->clk_err = 3; sh->clk_id = -1; }                  /* it finished first, or the service lost it: say what is true */
    clk_update_live(sh);
}

int shell_clock_cancel(struct shell *sh)
{
    if (clk_eff(sh) != CK_RUNNING) return 0;
    clk_cancel(sh);
    build(sh); sh->dirty = 1;
    return sh->clk_state == CK_CANCELLED;
}

/* ---- Trusted review: the compositor-owned modal. Colour alone proves nothing (software can draw amber): authenticity rests on the fixed string, the scrim over everything, the
 * kernel-supplied requester name, and the chord, which only the kernel input path delivers here. The secure colour is used ONLY by this function. ---- */
static int rv_live(const struct shell *sh) { return sh->rv.state == RV_OPEN || sh->rv.state == RV_ARMED; }
static int rv_ended(const struct shell *sh) { return sh->rv.state == RV_EXPIRED || sh->rv.state == RV_GONE; }
static void rv_pct(char *d, uint32_t cap, int32_t v) { pnum(d, cap, (uint64_t)(v < 0 ? 0 : v)); pcat(d, cap, "%"); }
static void rv_val(char *d, uint32_t cap, uint8_t target, int32_t v)
{
    if (target == 2) rv_pct(d, cap, v);
    else pcat(d, cap, v == 0 ? "Auto" : v == 1 ? "Light" : v == 2 ? "Dark" : "unknown");
}
static void rv_texts(struct shell *sh)
{
    sh->rv_l_req[0] = sh->rv_l_chg[0] = sh->rv_l_live[0] = 0;
    pcat(sh->rv_l_req, sizeof sh->rv_l_req, "Requested by: "); pcat(sh->rv_l_req, sizeof sh->rv_l_req, sh->rv_name[0] ? sh->rv_name : "unknown");
    uint8_t tg = sh->rv_req.intent.target;
    pcat(sh->rv_l_chg, sizeof sh->rv_l_chg, tg == 2 ? "Text size: " : tg == 1 ? "Theme: " : "Setting: ");
    rv_val(sh->rv_l_chg, sizeof sh->rv_l_chg, tg, sh->rv_req.prior); pcat(sh->rv_l_chg, sizeof sh->rv_l_chg, " -> "); rv_val(sh->rv_l_chg, sizeof sh->rv_l_chg, tg, sh->rv_req.next);
    const char *l = sh->rv.state == RV_ARMED ? "Approve is enabled for 10 seconds." : sh->rv.state == RV_EXPIRED ? "Review expired. Nothing changed."
        : sh->rv.state == RV_GONE ? "Requester exited. Nothing changed." : sh->rv_lapsed ? "Approve timed out. Press Ctrl+Alt+Delete again." : "Press Ctrl+Alt+Delete to enable Approve.";
    pcat(sh->rv_l_live, sizeof sh->rv_l_live, l);
}
static int32_t ml(struct shell *sh, int32_t x, int32_t y, int32_t w, const char *t, uint32_t base, uint32_t ink, int bold, uint32_t id)
{
    struct fnt f = pick_font(base, sh->text_pct);
    int32_t hh = lines_for(f, bold, t, w) * lh_of(f, bold) + 4;
    struct ui_widget *k = add(sh, x, y, w, hh, 0, UI_LAYER_MODAL, UI_LABEL, t, PAL(sh)->surface, id, SOLID);
    if (k) { k->font_size = f.size; k->mag = f.mag; k->bold = (uint8_t)bold; k->wrap = 1; k->ink = ink; }
    return hh;
}
static void review_flow(struct shell *sh, const struct geo *g)
{
    const struct pal *P = PAL(sh);
    rv_texts(sh);
    add(sh, 0, 0, g->W, g->H, 0, UI_LAYER_SCRIM, UI_PANEL, 0, P->scrim, 0xF200u, SOLID);
    int32_t w = 640 * (int32_t)sh->text_pct / 100; if (w < 640) w = 640; if (w > g->W - 48) w = g->W - 48;
    int32_t x = (g->W - w) / 2, y = g->top, iw = w - 32;
    struct ui_widget *d = add(sh, x, y, w, 100, 20, UI_LAYER_MODAL, UI_PANEL, 0, P->surface, 0xF201u, SOLID);
    if (d) { d->border_w = 2; d->border = P->focus; }
    int32_t iy = y + 16;
    struct fnt f16 = pick_font(16, sh->text_pct);
    int32_t sh_h = lh_of(f16, 1) + 16; if (sh_h < 40) sh_h = 40;
    struct ui_widget *strip = add(sh, x + 16, iy, iw, sh_h, 8, UI_LAYER_MODAL, UI_PANEL, 0, P->secure, 0xF210u, SOLID);      /* the secure strip: fixed text, the only user of the secure colour */
    if (strip) { strip->border_w = 2; strip->border = P->ink; }
    struct ui_widget *st = add(sh, x + 18, iy + (sh_h - lh_of(f16, 1)) / 2, iw - 4, lh_of(f16, 1), 0, UI_LAYER_MODAL, UI_LABEL, "AuroraOS secure review", P->secure, 0xF211u, SOLID);
    if (st) { st->font_size = f16.size; st->mag = f16.mag; st->bold = 1; st->ink = P->secure_ink; }
    iy += sh_h + 8;
    iy += ml(sh, x + 16, iy, iw, sh->rv_l_req, 16, P->ink, 0, 0xF212u) + 2;
    iy += ml(sh, x + 16, iy, iw, sh->rv_req.intent.preview, 18, P->ink, 1, 0xF213u) + 2;
    iy += ml(sh, x + 16, iy, iw, sh->rv_l_chg, 16, P->ink, 0, 0xF214u) + 2;
    iy += ml(sh, x + 16, iy, iw, sh->rv_req.intent.target == 1 ? "Reversible. Recorded in Trail. Stored only: no dark theme." : "Reversible. Recorded in Trail.", 14, P->muted, 0, 0xF215u) + 2;
    iy += ml(sh, x + 16, iy, iw, sh->rv_l_live, 14, P->ink, 1, 0xF216u) + 8;
    int32_t gap = 8, bw = (iw - gap) / 2, armed = sh->rv.state == RV_ARMED;
    int32_t h1 = sb(sh, x + 16, iy, bw, 48, rv_ended(sh) ? "Close" : "Cancel", SID_REVIEW_CANCEL, 1, UI_LAYER_MODAL);          /* added BEFORE Approve: default focus is Cancel */
    int32_t h2 = sb(sh, x + 16 + bw + gap, iy, bw, 48, armed ? "Approve" : "Approve (locked)", SID_REVIEW_APPROVE, armed, UI_LAYER_MODAL);
    iy += (h1 > h2 ? h1 : h2);
    if (d) d->c.h = iy + 16 - y;
}
static void review_close(struct shell *sh)
{
    if (!sh->review_open) return;
    ui_modal_pop(&sh->fx);
    uint32_t keep = sh->fx.focus >= 0 ? sh->c[sh->fx.focus].id : SHELL_NONE;
    sh->review_open = 0; sh->rv_seq++; build(sh);
    if (keep != SHELL_NONE) { int32_t i = shell_find(sh, keep); if (i >= 0) ui_focus_set(&sh->fx, i); }
    sh->dirty = 1;
}
static void review_decline(struct shell *sh)               /* Cancel, Close and Escape: a live review is declined (its ticket retired), an ended one is simply closed */
{
    if (!sh->review_open) return;
    if (rv_live(sh)) rv_decline(&sh->rv, sh->rv.ticket);
    review_close(sh);
}
void shell_set_now_ms(struct shell *sh, uint64_t now_ms) { if (now_ms > sh->now_ms) sh->now_ms = now_ms; }
int64_t shell_review_open(struct shell *sh, uint64_t now_ms, const struct rv_request *q, const struct aura_caps *caps, const char *requester_name)
{
    if (!sh || !q || !caps) return RV_E_ARG;
    if (!sh->caps.keyboard) return RV_E_STATE;                                       /* a review needs a keyboard: with none, S1 stays unavailable */
    if (sh->review_open || sh->about_open || sh->corner_open) return RV_E_BUSY;
    shell_set_now_ms(sh, now_ms);
    int64_t t = rv_open(&sh->rv, now_ms, q, caps);
    if (t <= 0) return t;
    sh->rv_req = *q; sh->rv_lapsed = 0; sh->rv_approved_pending = 0; sh->rv_prev = RV_OPEN;
    { uint32_t n = 0; if (requester_name) for (; requester_name[n] && n + 1 < sizeof sh->rv_name; n++) sh->rv_name[n] = (requester_name[n] >= 0x20 && requester_name[n] <= 0x7e) ? requester_name[n] : '?'; sh->rv_name[n] = 0; }
    sh->review_open = 1; sh->rv_seq++;
    build(sh); ui_modal_push(&sh->fx, UI_LAYER_MODAL);                              /* focus lands on the first control of the modal: Cancel */
    sh->dirty = 1;
    return t;
}
int shell_review_attention(struct shell *sh, uint64_t now_ms, int repeat)
{
    if (!sh) return RV_E_ARG;
    shell_set_now_ms(sh, now_ms);
    int rc = rv_attention(&sh->rv, now_ms, repeat);
    sh->rv_prev = sh->rv.state;                                                      /* so a later ARMED -> OPEN is recognised as a lapse */
    if (sh->review_open) { if (rc == RV_OK) sh->rv_lapsed = 0; sh->rv_seq++; build(sh); sh->dirty = 1; }
    return rc;
}
int shell_review_requester_gone(struct shell *sh, uint32_t pid, uint64_t gen)
{
    if (!sh) return 0;
    int dead = rv_requester_gone(&sh->rv, pid, gen);
    if (dead && sh->review_open) { sh->rv_seq++; build(sh); sh->dirty = 1; }
    return dead;
}
int shell_review_take_approved(struct shell *sh, uint64_t *ticket)
{
    if (!sh || !ticket || !sh->rv_approved_pending) return 0;
    sh->rv_approved_pending = 0; *ticket = sh->rv_approved_ticket;
    return 1;
}
int shell_review_verify(struct shell *sh, uint64_t now_ms, uint64_t ticket, const struct rv_request *now_q, const struct aura_caps *caps)
{
    if (!sh) return RV_E_ARG;
    return rv_verify(&sh->rv, now_ms, ticket, now_q, caps);
}
static void review_tick(struct shell *sh, uint64_t now_ms)
{
    int st = rv_tick(&sh->rv, now_ms);
    int changed = 0;
    if (sh->rv_prev == RV_ARMED && st == RV_OPEN) { sh->rv_lapsed = 1; changed = 1; }       /* the 10 s window ended without an answer */
    if (st != sh->rv_prev) changed = 1;
    sh->rv_prev = (uint8_t)st;
    if (changed && sh->review_open) { sh->rv_seq++; build(sh); sh->dirty = 1; }
}

/* ---- Trail: a read-only view of the audit ledger. Everything shown is a constant or a number from the kernel's copy; no user text, no wall-clock time. ---- */
static const char *const TV_PERM[] = { "nothing", "navigation", "settings proposals", "file writes", "file deletes", "installs", "sending", "timers" };
static const char *const TV_WHY[] = { "none", "bad request", "unknown requester", "stale requester", "repeated request", "busy", "no permission", "not a command", "unsupported",
                                      "navigation unavailable", "review unavailable", "passport unavailable", "trail unavailable", "hold unavailable", "timer unavailable" };
static const char *const TV_KIND[] = { "none", "open", "space", "setting", "unsupported", "delete", "clarify", "unknown", "refused", "timer" };

static uint64_t tv_digest_of(const struct shell_trail_view *v)
{
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p = (const uint8_t *)v;
    for (size_t i = 0; i < sizeof *v; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}
static void tr_page(const struct shell *sh, uint32_t *per, uint32_t *total)
{
    uint32_t n = sh->tv.n > SHELL_TV_RECS ? SHELL_TV_RECS : sh->tv.n;
    *per = sh->text_pct > 150 ? 2 : sh->text_pct > 100 ? 4 : 6;
    *total = n;
}
static int tr_streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void tr_perm_text(char *d, uint32_t cap, uint8_t perm) { pcat(d, cap, perm < 8 ? TV_PERM[perm] : "an unknown permission"); }

static void tr_texts(struct shell *sh)
{
    const struct shell_trail_view *v = &sh->tv;
    uint32_t per, total; tr_page(sh, &per, &total);
    for (int i = 0; i < 10; i++) sh->tr_rows[i][0] = 0;
    sh->tr_head[0] = sh->tr_sum[0] = sh->tr_perm[0] = sh->tr_range[0] = 0;
    for (int i = 0; i < 8; i++) sh->tr_gr[i][0] = 0;
    const char *h = !sh->trail_src ? "Trail: not available." : v->health == TV_RECORDING ? "Trail: recording to disk." : v->health == TV_NODISK ? "Trail: off. No disk, nothing is saved."
        : v->health == TV_READONLY ? "Trail: read-only disk. Changes refused." : v->health == TV_UNREADABLE ? "Trail: file unreadable. Changes refused."
        : v->health == TV_ROLLBACK ? "Trail: rollback found. Changes refused." : v->health == TV_HELD ? "Trail: held. Another record file needs attention. Changes refused." : "Trail: not available.";
    pcat(sh->tr_head, sizeof sh->tr_head, h);
    if (!tr_streq(sh->tr_live, sh->tr_head)) { scpy(sh->tr_live, sizeof sh->tr_live, sh->tr_head); sh->tr_seq++; }                  /* the health line is spoken (logged) when it changes */
    uint32_t changes = 0, denials = 0;
    for (uint32_t i = 0; i < total; i++) { if (v->rec[i].kind == 12 || v->rec[i].kind == 13) changes++; if (v->rec[i].state == 2) denials++; }
    pcat(sh->tr_sum, sizeof sh->tr_sum, "Changes: "); pnum(sh->tr_sum, sizeof sh->tr_sum, changes); pcat(sh->tr_sum, sizeof sh->tr_sum, ". Denials: "); pnum(sh->tr_sum, sizeof sh->tr_sum, denials); pcat(sh->tr_sum, sizeof sh->tr_sum, ".");
    if (!total) { pcat(sh->tr_rows[0], sizeof sh->tr_rows[0], "No records yet."); }
    uint32_t first = sh->tr_first; if (first >= total) first = total ? total - 1 : 0;
    uint32_t last = first + per < total ? first + per : total;
    for (uint32_t k = first, row = 0; k < last && row < 10; k++, row++) {                       /* newest first: k = 0 is the newest record */
        const struct shell_trail_rec *r = &v->rec[total - 1 - k];
        char *d = sh->tr_rows[row]; const uint32_t cap = sizeof sh->tr_rows[row];
        pcat(d, cap, "#"); pnum(d, cap, r->seq); pcat(d, cap, " ");
        if (r->kind == 12) { pcat(d, cap, "Granted: principal "); pnum(d, cap, r->principal); pcat(d, cap, " may use "); tr_perm_text(d, cap, r->target); pcat(d, cap, "."); }
        else if (r->kind == 13) { pcat(d, cap, "Revoked: principal "); pnum(d, cap, r->principal); pcat(d, cap, " may not use "); tr_perm_text(d, cap, r->target); pcat(d, cap, "."); }
        else if (r->kind == 14) pcat(d, cap, "System started.");
        else if (r->kind >= 1 && r->kind <= 9) {
            if (r->state == 2) { pcat(d, cap, "Denied: "); pcat(d, cap, TV_KIND[r->kind]); pcat(d, cap, " request from principal "); pnum(d, cap, r->principal); pcat(d, cap, ". "); pcat(d, cap, r->reason < 15 ? TV_WHY[r->reason] : "unknown reason"); pcat(d, cap, "."); }
            else { pcat(d, cap, "Request: "); pcat(d, cap, TV_KIND[r->kind]); pcat(d, cap, r->state == 1 ? ". Done." : ". Not done."); }
        } else { pcat(d, cap, "Unknown record kind "); pnum(d, cap, r->kind); pcat(d, cap, "."); }
    }
    if (total > per) {
        pcat(sh->tr_range, sizeof sh->tr_range, "Showing "); pnum(sh->tr_range, sizeof sh->tr_range, first + 1); pcat(sh->tr_range, sizeof sh->tr_range, "-"); pnum(sh->tr_range, sizeof sh->tr_range, last);
        pcat(sh->tr_range, sizeof sh->tr_range, " of "); pnum(sh->tr_range, sizeof sh->tr_range, total); pcat(sh->tr_range, sizeof sh->tr_range, ".");
    }
    if (v->ngrants) { pcat(sh->tr_perm, sizeof sh->tr_perm, "Saved permissions: "); pnum(sh->tr_perm, sizeof sh->tr_perm, v->ngrants > SHELL_TV_GRANTS ? SHELL_TV_GRANTS : v->ngrants); }
    else pcat(sh->tr_perm, sizeof sh->tr_perm, "Saved permissions: none.");
    for (uint32_t i = 0; i < v->ngrants && i < 8 && i < SHELL_TV_GRANTS; i++) {
        char *d = sh->tr_gr[i]; const uint32_t cap = sizeof sh->tr_gr[i];
        pcat(d, cap, "Principal "); pnum(d, cap, v->grant[i].principal); pcat(d, cap, " may use "); tr_perm_text(d, cap, v->grant[i].perm); pcat(d, cap, ".");
    }
}

static void trail_flow(struct shell *sh, const struct geo *g, int opt)
{
    const struct pal *P = PAL(sh);
    tr_texts(sh);
    uint32_t per, total; tr_page(sh, &per, &total);
    uint32_t first = sh->tr_first; if (first >= total) first = total ? total - 1 : 0;
    uint32_t last = first + per < total ? first + per : total;
    int32_t y = g->top, w = g->W - 80, iw = w - 32;
    struct ui_widget *c = card_s(sh, 40, y, w);
    y += 12;
    y += sl(sh, 56, y, iw, "Trail", 24, P->ink, 1) + 2;
    if (opt) y += sl(sh, 56, y, iw, "Read-only. Checksums catch accidents, not tampering.", 14, P->muted, 0) + 2;
    y += sl(sh, 56, y, iw, sh->tr_head, 16, P->ink, 1) + 2;
    if (opt) y += sl(sh, 56, y, iw, sh->tr_sum, 14, P->ink, 0) + 4;                 /* the counts are a convenience: dropped before anything essential when space is short */
    uint32_t rows = total ? last - first : 1;
    for (uint32_t i = 0; i < rows && i < 10; i++) y += sl(sh, 56, y, iw, sh->tr_rows[i], 14, P->ink, 0);
    if (sh->tr_range[0]) y += sl(sh, 56, y, iw, sh->tr_range, 14, P->muted, 0);
    y += 4;
    y += sl(sh, 56, y, iw, sh->tr_perm, 14, P->ink, 0);
    if (opt && sh->text_pct <= 125) for (uint32_t i = 0; i < 2 && i < sh->tv.ngrants; i++) y += sl(sh, 56, y, iw, sh->tr_gr[i], 14, P->muted, 0);
    y += 8;
    if (total > per) {                                                               /* a window over the records: two scroll buttons, never any other control */
        int32_t gap = 8, bw = (iw - gap) / 2;
        int32_t h1 = sb(sh, 56, y, bw, 48, "Show newer", SID_TRAIL_NEWER, first > 0, UI_LAYER_CONTENT);
        int32_t h2 = sb(sh, 56 + bw + gap, y, bw, 48, "Show older", SID_TRAIL_OLDER, last < total, UI_LAYER_CONTENT);
        y += (h1 > h2 ? h1 : h2) + 8;
    }
    y += sb(sh, 56, y, 200 < iw ? 200 : iw, 48, "Home", SID_TRAIL_HOME, 1, UI_LAYER_CONTENT) + 8;
    card_end(sh, c, y);
}

int shell_trail_refresh(struct shell *sh)
{
    struct shell_trail_view t;
    uint8_t *z = (uint8_t *)&t; for (size_t i = 0; i < sizeof t; i++) z[i] = 0;
    if (sh->trail_src) { if (sh->trail_src(&t) < 0) { z = (uint8_t *)&t; for (size_t i = 0; i < sizeof t; i++) z[i] = 0; } }      /* a failing source shows "not available" */
    if (t.n > SHELL_TV_RECS) t.n = SHELL_TV_RECS;
    if (t.ngrants > SHELL_TV_GRANTS) t.ngrants = SHELL_TV_GRANTS;
    uint64_t d = tv_digest_of(&t);
    if (sh->tv_valid && d == sh->tv_digest) return 0;
    sh->tv = t; sh->tv_digest = d; sh->tv_valid = 1;
    uint32_t per, total; tr_page(sh, &per, &total);
    if (sh->tr_first >= total) sh->tr_first = 0;                                       /* records may have changed under the window: start from the newest */
    tr_texts(sh);                                                                      /* also updates the spoken health line */
    if (sh->screen == SCR_TRAIL) { build(sh); sh->dirty = 1; }
    return 1;
}
void shell_set_trail(struct shell *sh, int (*src)(struct shell_trail_view *out))
{
    sh->trail_src = src; sh->tv_valid = 0; sh->tr_first = 0;
    shell_trail_refresh(sh);
}

/* The completion toast: a compositor-owned notice in the header (the Horizon status slot). It never takes focus and cannot be clicked; it is drawn on the
 * topmost layer but only inside the header, so it cannot cover the dock, the content, a sheet or a dialog. */
static void toast_add(struct shell *sh)
{
    if (!sh->toast_on) return;
    struct geo g = make_geo(sh);
    struct fnt tf = pick_font(24, sh->text_pct), bf = pick_font(16, sh->text_pct), hf = pick_font(14, sh->text_pct);
    int32_t row1 = lh_of(tf, 1) + 16; if (row1 < 64) row1 = 64;
    if (row1 < g.btn_h + 16) row1 = g.btn_h + 16;
    int32_t title_w = 8 * adv_of(tf, 1) + 16, hb = 4 * adv_of(bf, 0) + 32; if (hb < 96) hb = 96;
    int32_t up_w = 9 * adv_of(hf, 0) + 16, up_x = g.W - 16 - hb - 12 - up_w;
    int32_t w = 14 * adv_of(bf, 1) + 32, h = lh_of(bf, 1) + 24; if (h < 48) h = 48;
    int32_t x = 24 + title_w + 16, y = (row1 - h) / 2;
    if (x + w > up_x - 8) x = up_x - 8 - w;
    if (x < 8) x = 8;
    if (y < 0) y = 0;
    struct ui_widget *p = add(sh, x, y, w, h, 12, UI_LAYER_TOAST, UI_PANEL, 0, PAL(sh)->ink, 0xF600u, SOLID);
    if (p) { p->border_w = 2; p->border = PAL(sh)->surface; }
    struct ui_widget *t = add(sh, x + 2, y + (h - lh_of(bf, 1)) / 2, w - 4, lh_of(bf, 1), 0, UI_LAYER_TOAST, UI_LABEL, "Timer finished", PAL(sh)->ink, 0xF601u, SOLID);
    if (t) { t->font_size = bf.size; t->mag = bf.mag; t->bold = 1; t->ink = PAL(sh)->surface; }
}

int shell_tick(struct shell *sh, uint64_t now_ms)
{
    int changed = 0;
    if (now_ms > sh->now_ms) sh->now_ms = now_ms;
    review_tick(sh, now_ms);
    if (sh->toast_on && now_ms >= sh->toast_until_ms) { sh->toast_on = 0; changed = 1; }
    if (sh->timers && sh->clk_state == CK_RUNNING) {
        uint8_t st = 0; uint64_t rem = 0;
        int rc = sh->timers->query(sh->clk_id, &st, &rem);
        if (rc != 0 || st == 3) { sh->clk_state = CK_ERROR; sh->clk_err = 3; sh->clk_id = -1; sh->clk_rem_s = 0; changed = 1; }
        else if (st == 2) {
            (void)sh->timers->release(sh->clk_id);
            sh->clk_state = CK_DONE; sh->clk_id = -1; sh->clk_rem_s = 0;
            sh->toast_on = 1; sh->toast_until_ms = now_ms + SHELL_TOAST_MS;
            changed = 1;
        } else {
            uint64_t s = (rem + 999u) / 1000u; if (s > 86400u) s = 86400u;
            if ((uint32_t)s != sh->clk_rem_s) { sh->clk_rem_s = (uint32_t)s; if (sh->screen == SCR_CLOCK) changed = 1; }
        }
        if (changed) clk_update_live(sh);
    }
    if (changed) { build(sh); sh->dirty = 1; }
    return changed;
}

static void build_clients(struct shell *sh);

static void build_scaled(struct shell *sh)
{
    struct geo g = make_geo(sh);
    scpy(sh->horizon_txt, sizeof sh->horizon_txt, (sh->caps.keyboard && sh->caps.pointer) ? "Horizon: ready" : "Horizon: limited input");
    header_scaled(sh, &g);
    uint32_t n0 = sh->n;
    for (int opt = 1; opt >= 0; opt--) {                                           /* first try with the optional lines; drop them when they do not fit */
        sh->n = n0; sh->lay_bottom = 0;
        if (sh->screen == SCR_HOME) home_scaled(sh, &g, opt);
        else if (sh->screen == SCR_SPACES) spaces_scaled(sh, &g, opt);
        else if (sh->screen == SCR_SETTINGS) settings_flow(sh, &g, opt);
        else if (sh->screen == SCR_PAGES) pages_flow(sh, &g, opt);
        else if (sh->screen == SCR_AURA) aura_flow(sh, &g, opt);
        else if (sh->screen == SCR_CLOCK) clock_flow(sh, &g, opt);
        else if (sh->screen == SCR_TRAIL) trail_flow(sh, &g, opt);
        else index_scaled(sh, &g, opt);
        if (sh->lay_bottom <= g.bottom) break;
    }
    build_clients(sh);
    dock_scaled(sh, &g);
    if (sh->about_open) about_scaled(sh, &g);
    corner_scaled(sh, &g);
}

/* EL0 client surfaces: compositor-owned chrome above each placed surface (name from the kernel, a focus colour only the compositor uses), a body, and the
 * client's COMMITTED widgets as inert labels and panels. Clients cannot name a layer or a pixel: everything is in the content band, inside the
 * rectangle the compositor allotted, and the validated geometry keeps every widget inside the body. */
static void build_clients(struct shell *sh)
{
    if (!sh->clients) return;
    const struct pal *P = PAL(sh);
    for (int slot = 0; slot < UIP_MAX_SURFACES; slot++) {
        const struct uip_surface *c = uip_surface(sh->clients, slot);
        if (!c || !c->placed) continue;
        int32_t w = UIP_SIZE_W[c->size_class], h = UIP_SIZE_H[c->size_class];
        int focused = sh->clients->focus == slot;
        const char *nm = sh->client_name ? sh->client_name(c->owner_pid) : 0;
        struct ui_widget *ch = add(sh, c->sx, c->sy - UIP_CHROME_H, w, UIP_CHROME_H, 0, UI_LAYER_CONTENT, UI_LABEL, nm ? nm : "app", focused ? P->focus : P->muted, 0xF600u + (uint32_t)slot, SOLID);
        if (ch) { ch->font_size = 14; ch->bold = 1; ch->ink = 0x00FFFFFFu; }
        struct ui_widget *body = add(sh, c->sx, c->sy, w, h, 0, UI_LAYER_CONTENT, UI_PANEL, 0, P->surface, 0xF620u + (uint32_t)slot, SOLID);
        if (body) { body->border_w = 2; body->border = focused ? P->focus : P->border; }
        for (int k = 0; k < UIP_WIDGETS; k++) {
            const struct uip_widget *g = &c->vis[k];
            if (!g->has_rect) continue;                                           /* text without a rectangle has no place to go */
            struct ui_widget *wd = add(sh, c->sx + g->x, c->sy + g->y, g->w, g->h, 0, UI_LAYER_CONTENT, g->has_text ? UI_LABEL : UI_PANEL, g->has_text ? g->text : 0,
                                       g->role == 2 ? P->canvas : P->surface, 0xF640u + (uint32_t)(slot * UIP_WIDGETS + k), SOLID);
            if (!wd) return;
            wd->font_size = 14; wd->ink = (g->style & 2) ? P->muted : P->ink; wd->bold = (g->style & 1) ? 1 : 0;
            if (g->role == 1 || g->role == 3) { wd->border_w = 1; wd->border = P->border; }
        }
    }
}

void shell_set_clients(struct shell *sh, const struct uip_state *st, const char *(*name_of)(uint8_t pid))
{
    sh->clients = st; sh->client_name = name_of;
    sh->dirty = 1;
}

/* Rebuild the whole widget array from the state. Focus is kept by stable id when that control still exists and can be focused. */
static void build(struct shell *sh)
{
    uint32_t keep = SHELL_NONE;
    if (sh->fx.focus >= 0 && (uint32_t)sh->fx.focus < sh->n) keep = sh->c[sh->fx.focus].id;
    sh->n = 0;
    if (sh->text_pct != 100) build_scaled(sh);
    else {
    struct ui_widget *h = add(sh, 0, 0, (int32_t)sh->width, 64, 0, UI_LAYER_CONTENT, UI_PANEL, 0, PAL(sh)->surface, 0xF300u, SOLID);
    (void)h;
    struct ui_widget *t = add(sh, 24, 14, 300, 36, 0, UI_LAYER_CONTENT, UI_LABEL, "AuroraOS", PAL(sh)->surface, 0xF301u, SOLID);
    if (t) { t->font_size = 24; t->bold = 1; }
    struct ui_widget *hz = add(sh, 190, 22, 440, 24, 0, UI_LAYER_CONTENT, UI_LABEL, sh->horizon_txt, PAL(sh)->surface, 0xF304u, SOLID);
    if (hz) { hz->font_size = 14; hz->ink = PAL(sh)->muted; }
    struct ui_widget *u = add(sh, 780, 22, 112, 24, 0, UI_LAYER_CONTENT, UI_LABEL, sh->uptime_txt, PAL(sh)->surface, 0xF302u, SOLID);
    if (u) { u->font_size = 14; u->ink = PAL(sh)->muted; }
    /* Aurora Horizon: a thin status rail with ONE source. Aura colour = the boot checks passed and both a keyboard and a pointer were found;
     * muted = the shell is running with limited input. It never animates and never shows anything it did not measure. */
    int full = sh->caps.keyboard && sh->caps.pointer;
    scpy(sh->horizon_txt, sizeof sh->horizon_txt, full ? "Horizon: ready" : "Horizon: limited input");
    add(sh, 0, 64, (int32_t)sh->width, 4, 0, UI_LAYER_CONTENT, UI_PANEL, 0, full ? PAL(sh)->aura : PAL(sh)->muted, 0xF303u, SOLID);
    if (sh->screen == SCR_HOME) build_home(sh);
    else if (sh->screen == SCR_SPACES) build_spaces(sh);
    else if (sh->screen == SCR_SETTINGS) { struct geo g = make_geo(sh); sh->lay_bottom = 0; uint32_t n0 = sh->n; settings_flow(sh, &g, 1); if (sh->lay_bottom > g.bottom) { sh->n = n0; sh->lay_bottom = 0; settings_flow(sh, &g, 0); } }
    else if (sh->screen == SCR_PAGES) { struct geo g = make_geo(sh); sh->lay_bottom = 0; uint32_t n0 = sh->n; pages_flow(sh, &g, 1); if (sh->lay_bottom > g.bottom) { sh->n = n0; sh->lay_bottom = 0; pages_flow(sh, &g, 0); } }
    else if (sh->screen == SCR_AURA) { struct geo g = make_geo(sh); sh->lay_bottom = 0; uint32_t n0 = sh->n; aura_flow(sh, &g, 1); if (sh->lay_bottom > g.bottom) { sh->n = n0; sh->lay_bottom = 0; aura_flow(sh, &g, 0); } }
    else if (sh->screen == SCR_CLOCK) { struct geo g = make_geo(sh); sh->lay_bottom = 0; uint32_t n0 = sh->n; clock_flow(sh, &g, 1); if (sh->lay_bottom > g.bottom) { sh->n = n0; sh->lay_bottom = 0; clock_flow(sh, &g, 0); } }
    else if (sh->screen == SCR_TRAIL) { struct geo g = make_geo(sh); sh->lay_bottom = 0; uint32_t n0 = sh->n; trail_flow(sh, &g, 1); if (sh->lay_bottom > g.bottom) { sh->n = n0; sh->lay_bottom = 0; trail_flow(sh, &g, 0); } }
    else build_index(sh);
    build_clients(sh);
    build_dock(sh);
    if (sh->about_open) build_about(sh);
    build_corner(sh);
    }
    if (sh->review_open) { struct geo g = make_geo(sh); review_flow(sh, &g); }                  /* above everything the shell and every client can draw */
    toast_add(sh);
    for (uint32_t i = 0; i < sh->n; i++) sh->c[i] = sh->w[i].c;
    sh->fx.c = sh->c; sh->fx.n = sh->n;
    sh->fx.focus = -1;
    if (keep != SHELL_NONE) { int32_t i = shell_find(sh, keep); if (i >= 0) ui_focus_set(&sh->fx, i); }
    sh->fx.pressed = sh->fx.pressed < (int32_t)sh->n ? sh->fx.pressed : -1;
}

int32_t shell_find(const struct shell *sh, uint32_t id)
{
    for (uint32_t i = 0; i < sh->n; i++) if (sh->c[i].id == id) return (int32_t)i;
    return -1;
}

void shell_init(struct shell *sh, uint32_t width, uint32_t height)
{
    uint8_t *z = (uint8_t *)sh;
    for (uint32_t i = 0; i < sizeof *sh; i++) z[i] = 0;
    sh->width = width; sh->height = height;
    sh->screen = SCR_HOME; sh->space = 0; sh->text_pct = 100;
    for (uint32_t r = 0; r < SHELL_SET_ROWS; r++) sh->sess[r] = SROWS[r].def;
    sh->px = (int32_t)(width / 2); sh->py = (int32_t)(height / 2);
    sh->last_activated = SHELL_NONE;
    scpy(sh->uptime_txt, 16, "Up 0:00");
    ui_init(&sh->fx, sh->c, 0);
    build(sh);
    sh->dirty = 1;
}

int shell_set_text_scale(struct shell *sh, uint32_t pct)
{
    if (pct < SHELL_TEXT_MIN || pct > SHELL_TEXT_MAX) return 0;                    /* out of the documented range: refuse, change nothing */
    if (pct == sh->text_pct) return 1;
    sh->text_pct = (uint8_t)pct;
    build(sh); sh->dirty = 1;                                                      /* focus is kept by control id; modal state is untouched */
    return 1;
}

int shell_set_contrast(struct shell *sh, int on)
{
    sh->contrast = on ? 1 : 0;
    build(sh); sh->dirty = 1;
    return 1;
}

void shell_set_caps(struct shell *sh, struct shell_caps caps)
{
    if (caps.keyboard != sh->caps.keyboard || caps.pointer != sh->caps.pointer) { sh->caps = caps; build(sh); sh->dirty = 1; }
}

void shell_set_uptime(struct shell *sh, uint32_t seconds)
{
    char t[16]; uint32_t m = seconds / 60, s = seconds % 60;
    if (m > 999) m = 999;
    uint32_t n = 0; t[n++] = 'U'; t[n++] = 'p'; t[n++] = ' ';
    char dig[4]; int k = 0; do { dig[k++] = (char)('0' + m % 10); m /= 10; } while (m && k < 4);
    while (k) t[n++] = dig[--k];
    t[n++] = ':'; t[n++] = (char)('0' + s / 10); t[n++] = (char)('0' + s % 10); t[n] = 0;
    sh->uptime_s = seconds;
    uint32_t i = 0; while (t[i] && t[i] == sh->uptime_txt[i]) i++;
    if (t[i] || sh->uptime_txt[i]) { scpy(sh->uptime_txt, 16, t); sh->dirty = 1; }
}

void shell_set_pointer(struct shell *sh, int32_t x, int32_t y) { if (x != sh->px || y != sh->py) { sh->px = x; sh->py = y; sh->dirty = 1; } }

int shell_go(struct shell *sh, uint32_t screen)
{
    if (screen >= SCR_COUNT || screen == sh->screen) return 0;
    if (sh->hist_n == SHELL_HISTORY) { for (int i = 1; i < SHELL_HISTORY; i++) sh->hist[i - 1] = sh->hist[i]; sh->hist_n--; }   /* bounded: drop the oldest */
    sh->hist[sh->hist_n++] = sh->screen;
    sh->screen = (uint8_t)screen;
    if (screen == SCR_INDEX) { sh->qlen = 0; index_refresh(sh); }      /* a fresh search each time Index opens */
    if (screen == SCR_PAGES) { sh->ps_valid = 0; shell_pages_refresh(sh); }   /* always show the table as it is now */
    if (screen == SCR_TRAIL) { sh->tr_first = 0; sh->tv_valid = 0; shell_trail_refresh(sh); }   /* the newest records, as they are now */
    build(sh); sh->dirty = 1;
    return 1;
}

int shell_back(struct shell *sh)
{
    if (!sh->hist_n) return 0;
    sh->screen = sh->hist[--sh->hist_n];
    if (sh->screen == SCR_INDEX) { sh->qlen = 0; index_refresh(sh); }
    if (sh->screen == SCR_PAGES) { sh->ps_valid = 0; shell_pages_refresh(sh); }
    build(sh);
    if (sh->fx.focus < 0) { int32_t hm = shell_find(sh, SID_DOCK_HOME); if (!(hm >= 0 && ui_focus_set(&sh->fx, hm))) ui_focus_next(&sh->fx, 1); }   /* the focused control may not exist here (e.g. Back after the last step) */
    sh->dirty = 1;
    return 1;
}

int shell_select_space(struct shell *sh, uint32_t index)
{
    if (index >= SHELL_SPACES) return 0;
    sh->space = (uint8_t)index;
    build(sh); sh->dirty = 1;
    return 1;
}

static void set_corner(struct shell *sh, int open)
{
    if (open == sh->corner_open || sh->about_open || sh->review_open) return;
    if (open) { sh->corner_open = 1; build(sh); ui_modal_push(&sh->fx, UI_LAYER_SHEET); }
    else {
        ui_modal_pop(&sh->fx);
        sh->corner_open = 0; build(sh);
        int32_t i = shell_find(sh, SID_CORNER);                  /* the sheet can only be opened from the handle, so focus returns to it */
        if (i >= 0) ui_focus_set(&sh->fx, i);
    }
    sh->dirty = 1;
}

static void set_about(struct shell *sh, int open)
{
    if (open == sh->about_open || sh->review_open) return;
    if (open) { sh->about_open = 1; build(sh); ui_modal_push(&sh->fx, UI_LAYER_MODAL); }
    else {
        ui_modal_pop(&sh->fx);                                   /* focus returns to the control that opened it, if it still exists */
        uint32_t keep = sh->fx.focus >= 0 ? sh->c[sh->fx.focus].id : SHELL_NONE;
        sh->about_open = 0; build(sh);
        if (keep != SHELL_NONE) { int32_t i = shell_find(sh, keep); if (i >= 0) ui_focus_set(&sh->fx, i); }
    }
    sh->dirty = 1;
}

static void push_recent(struct shell *sh, const char *key)
{
    uint32_t j = 0;
    for (uint32_t i = 0; i < sh->nrecent; i++) {                 /* drop an existing copy of the key, keep the rest in order */
        const char *x = sh->recent[i], *y = key; uint32_t q = 0;
        while (x[q] && x[q] == y[q]) q++;
        if (!(x[q] == 0 && y[q] == 0)) sh->recent[j++] = sh->recent[i];
    }
    if (j >= SHELL_RECENT) j = SHELL_RECENT - 1;
    for (uint32_t i = j; i > 0; i--) sh->recent[i] = sh->recent[i - 1];
    sh->recent[0] = key;
    sh->nrecent = (uint8_t)(j + 1);
}

/* Open result row k. Only an available destination opens; unavailable items do nothing. A typo suggestion opens only when chosen
 * explicitly (a click or Tab+Enter on it): the default selection never points at one. Returns 1 if something opened. */
static int index_open(struct shell *sh, uint32_t k)
{
    if (k >= sh->nres) return 0;
    uint32_t item = sh->res[k].item;
    if (item >= NITEMS) return 0;
    const struct dest *d = &DEST[item];
    if (d->kind == D_UNAVAILABLE) return 0;
    push_recent(sh, ITEMS[item].key);
    sh->index_opens++;
    switch (d->kind) {
    case D_SCREEN: shell_go(sh, d->arg); break;
    case D_SPACE: shell_select_space(sh, d->arg); shell_go(sh, SCR_HOME); break;
    case D_ABOUT: shell_go(sh, SCR_HOME); set_about(sh, 1); break;
    default: return 0;
    }
    return 1;
}

static void activate(struct shell *sh, uint32_t id)
{
    sh->last_activated = id; sh->activations++;
    switch (id) {
    case SID_DOCK_BACK: shell_back(sh); break;
    case SID_DOCK_HOME: shell_go(sh, SCR_HOME); break;
    case SID_DOCK_SPACES: case SID_CHANGE_SPACE: case SID_APP_SPACES: shell_go(sh, SCR_SPACES); break;
    case SID_DOCK_INDEX: case SID_APP_INDEX: shell_go(sh, SCR_INDEX); break;
    case SID_APP_SETTINGS: shell_go(sh, SCR_SETTINGS); break;
    case SID_APP_PAGES: shell_go(sh, SCR_PAGES); break;
    case SID_APP_AURA: shell_go(sh, SCR_AURA); break;
    case SID_AURA_HOME: shell_go(sh, SCR_HOME); break;
    case SID_AURA_RUN: aura_submit(sh); break;
    case SID_AURA_CLEAR: sh->aura_len = 0; sh->aura_in[0] = 0; sh->aura_msg[0] = 0; sh->aura_state = 0; aura_refresh(sh); build(sh); break;
    case SID_PAGES_HOME: shell_go(sh, SCR_HOME); break;
    case SID_CLOCK_P1: case SID_CLOCK_P5: case SID_CLOCK_P25:
        if (clk_eff(sh) != CK_RUNNING) { sh->clk_preset = (uint8_t)(id - SID_CLOCK_P1); if (sh->clk_state != CK_READY && sh->timers) sh->clk_state = CK_READY; clk_update_live(sh); build(sh); }
        break;
    case SID_CLOCK_START: clk_start(sh, sh->clk_preset); build(sh); { int32_t k = shell_find(sh, clk_eff(sh) == CK_RUNNING ? SID_CLOCK_CANCEL : SID_CLOCK_START); if (k >= 0) ui_focus_set(&sh->fx, k); } break;   /* Start moves focus to Cancel */
    case SID_CLOCK_CANCEL: clk_cancel(sh); build(sh); { int32_t k = shell_find(sh, SID_CLOCK_START); if (k >= 0) ui_focus_set(&sh->fx, k); } break;
    case SID_CLOCK_HOME: shell_go(sh, SCR_HOME); break;
    case SID_REVIEW_CANCEL: review_decline(sh); break;
    case SID_REVIEW_APPROVE: {
        if (!sh->review_open) break;
        int rc = rv_approve(&sh->rv, sh->now_ms, sh->rv.ticket);
        if (rc == RV_OK) { sh->rv_approved_pending = 1; sh->rv_approved_ticket = sh->rv.ticket; review_close(sh); }         /* approved: nothing executes here, the executor must still verify */
        else { if (rc == RV_E_LAPSED) sh->rv_lapsed = 1; sh->rv_prev = sh->rv.state; sh->rv_seq++; build(sh); }
        break; }
    case SID_TRAIL_HOME: shell_go(sh, SCR_HOME); break;
    case SID_TRAIL_NEWER: { uint32_t per, total; tr_page(sh, &per, &total); sh->tr_first = (uint8_t)(sh->tr_first > per ? sh->tr_first - per : 0); build(sh); if (sh->fx.focus < 0) { int32_t k = shell_find(sh, SID_TRAIL_OLDER); if (k >= 0) ui_focus_set(&sh->fx, k); } break; }
    case SID_TRAIL_OLDER: { uint32_t per, total; tr_page(sh, &per, &total); if (sh->tr_first + per < total) sh->tr_first = (uint8_t)(sh->tr_first + per); build(sh); if (sh->fx.focus < 0) { int32_t k = shell_find(sh, SID_TRAIL_NEWER); if (k >= 0) ui_focus_set(&sh->fx, k); } break; }
    case SID_SET_UP: if (sh->set_first) sh->set_first--; build(sh); if (sh->fx.focus < 0) { int32_t k = shell_find(sh, SID_SET_DOWN); if (k < 0) k = shell_find(sh, SID_SET_UP); if (k >= 0) ui_focus_set(&sh->fx, k); } break;
    case SID_SET_DOWN: if (sh->set_first + 1 < SHELL_SET_ROWS) sh->set_first++; build(sh); if (sh->fx.focus < 0) { int32_t k = shell_find(sh, SID_SET_UP); if (k >= 0) ui_focus_set(&sh->fx, k); } break;
    case SID_ABOUT: set_about(sh, 1); break;
    case SID_ABOUT_CLOSE: set_about(sh, 0); break;
    case SID_CORNER: set_corner(sh, 1); break;
    case SID_CORNER_CLOSE: set_corner(sh, 0); break;
    default:
        if (id >= SID_SET_VALUE && id < SID_SET_VALUE + SHELL_SET_ROWS) set_change(sh, id - SID_SET_VALUE, 0);
        else if (id >= SID_SET_RESET && id < SID_SET_RESET + SHELL_SET_ROWS) set_change(sh, id - SID_SET_RESET, 1);
        else if (id >= SID_SPACE_BASE && id < SID_SPACE_BASE + SHELL_SPACES) shell_select_space(sh, id - SID_SPACE_BASE);
        else if (id >= SID_RESULT_BASE && id < SID_RESULT_BASE + SHELL_RESULTS) index_open(sh, id - SID_RESULT_BASE);
        break;                                                   /* unknown ids do nothing */
    }
}

/* Index input: typed text goes to the search field, Backspace edits, Up/Down choose, Enter opens the choice when no control has focus,
 * Escape clears a non-empty query. Returns 1 when the event was consumed here. Printable ASCII only; the field never exceeds IDX_MAX_QUERY. */
static int index_input(struct shell *sh, const struct input_event *e)
{
    if (sh->screen != SCR_INDEX || sh->about_open || sh->corner_open || sh->review_open) return 0;
    if (e->kind == IN_TEXT) {
        sh->fx.focus = -1;                                       /* typing means the search field has the focus, not a button */
        if (e->ch >= 0x20 && e->ch <= 0x7e && sh->qlen < IDX_MAX_QUERY) { sh->query[sh->qlen++] = (char)e->ch; index_refresh(sh); }
        return 1;
    }
    if (e->kind != IN_KEY_DOWN) return 0;
    switch (e->key) {
    case KEY_BACKSPACE: sh->fx.focus = -1; if (sh->qlen) { sh->qlen--; index_refresh(sh); } return 1;
    case KEY_DOWN: sh->fx.focus = -1; if (sh->nres) sh->sel = (int8_t)(sh->sel < 0 ? 0 : (sh->sel + 1 < sh->nres ? sh->sel + 1 : sh->nres - 1)); return 1;
    case KEY_UP: sh->fx.focus = -1; if (sh->nres) sh->sel = (int8_t)(sh->sel > 0 ? sh->sel - 1 : 0); return 1;
    case KEY_SPACE: return 1;                                    /* the matching IN_TEXT event types the space; Space never activates a control here */
    case KEY_ENTER:
        if (sh->fx.focus >= 0) return 0;                         /* a focused control (a result row or a dock button) handles Enter itself */
        if (!e->repeat && sh->sel >= 0) index_open(sh, (uint32_t)sh->sel);
        return 1;
    case KEY_ESC:
        if (sh->qlen) { sh->qlen = 0; index_refresh(sh); return 1; }
        return 0;                                                /* empty query: Escape goes back */
    default: return 0;
    }
}

int shell_handle(struct shell *sh, const struct input_event *e)
{
    uint32_t r = SHELL_NONE;
    if (index_input(sh, e) || aura_input(sh, e)) { build(sh); sh->dirty = 1; return 1; }   /* rebuild: hit testing must see the layout the user sees */
    switch (e->kind) {
    case IN_KEY_DOWN:
        r = ui_key(&sh->fx, e);
        if (r == UI_ESCAPE) { r = SHELL_NONE; if (sh->review_open) review_decline(sh); else if (sh->about_open) set_about(sh, 0); else if (sh->corner_open) set_corner(sh, 0); else shell_back(sh); }
        break;
    case IN_PTR_MOVE: shell_set_pointer(sh, e->x, e->y); return 1;
    case IN_PTR_DOWN: shell_set_pointer(sh, e->x, e->y); ui_pointer_down(&sh->fx, e->x, e->y); break;
    case IN_PTR_UP:
        shell_set_pointer(sh, e->x, e->y);
        if (sh->corner_open && ui_hit(&sh->fx, e->x, e->y) < 0) { set_corner(sh, 0); sh->fx.pressed = -1; break; }   /* click outside the sheet: close it, nothing underneath is activated */
        r = ui_pointer_up(&sh->fx, e->x, e->y);
        break;
    default: return 0;
    }
    if (r != SHELL_NONE && r != UI_NONE) activate(sh, r);
    build(sh);
    sh->dirty = 1;
    return 1;
}

int shell_draw(struct shell *sh, const struct gfx_surface *s)
{
    build(sh);                                                   /* deterministic: the frame is a pure function of the state */
    if (sh->caps.pointer) {
        int32_t hv = ui_hit(&sh->fx, sh->px, sh->py);
        if (hv >= 0 && sh->w[hv].kind == UI_BUTTON && (sh->w[hv].c.flags & UI_ENABLED) && !sh->w[hv].active) sh->w[hv].fill = PAL(sh)->hover;
        if (sh->fx.pressed >= 0 && sh->fx.pressed < (int32_t)sh->n && !sh->w[sh->fx.pressed].active) sh->w[sh->fx.pressed].fill = PAL(sh)->pressed;
    }
    int rc = gfx_clear(s, PAL(sh)->canvas);
    if (rc != GFX_OK) return rc;
    rc = ui_scene_draw_ex(s, sh->w, sh->n, &sh->fx, sh->px, sh->py, sh->caps.pointer, PAL(sh)->focus);
    if (rc == GFX_OK) sh->dirty = 0;
    return rc;
}
