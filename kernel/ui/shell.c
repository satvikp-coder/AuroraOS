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
struct pal { uint32_t canvas, surface, ink, muted, accent, active_ink, focus, border, border_dis, scrim, aura, hover, pressed; uint8_t bw, marker, rail; };
static const struct pal PAL_STD = { UI_COL_CANVAS, UI_COL_SURFACE, UI_COL_INK, UI_COL_MUTED, UI_COL_ACCENT, UI_COL_SURFACE, UI_COL_FOCUS, 0x00C9D4E3u, 0x00DCE3EDu, UI_COL_SCRIM, COL_AURA, COL_HOVER, COL_PRESSED, 1, 0, 4 };
static const struct pal PAL_HC = { 0x00FFFFFFu, 0x00FFFFFFu, 0x00000000u, 0x00333333u, 0x00101010u, 0x00FFFFFFu, 0x000000E0u, 0x00000000u, 0x00555555u, 0x00000000u, 0x00000000u, 0x00D9D9D9u, 0x00B3B3B3u, 2, 1, 6 };
#define PAL(sh) ((sh)->contrast ? &PAL_HC : &PAL_STD)

static const char *const NAMES[SCR_COUNT] = { "Home", "Spaces", "Index", "Settings" };

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
};
static const struct dest DEST[] = {
    { D_SCREEN, SCR_HOME }, { D_SCREEN, SCR_SPACES }, { D_SCREEN, SCR_INDEX },
    { D_SPACE, 0 }, { D_SPACE, 1 }, { D_SPACE, 2 }, { D_SPACE, 3 },
    { D_ABOUT, 0 }, { D_UNAVAILABLE, 0 }, { D_UNAVAILABLE, 0 }, { D_SCREEN, SCR_SETTINGS },
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
    button(sh, 544, 284, 416, 56, "Pages (not available yet)", SID_APP_PAGES, 0, UI_LAYER_CONTENT);
    button(sh, 544, 352, 416, 56, "Aura (not available yet)", SID_APP_AURA, 0, UI_LAYER_CONTENT);
    button(sh, 544, 420, 416, 56, "Settings", SID_APP_SETTINGS, 1, UI_LAYER_CONTENT);
    label(sh, 544, 496, 416, 24, "Pages and Aura are not available yet.", 14, PAL(sh)->muted, 0);
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
    static const char *const AT[5] = { "Spaces", "Index", "Pages (not available yet)", "Aura (not available yet)", "Settings" };
    static const uint32_t AI[5] = { SID_APP_SPACES, SID_APP_INDEX, SID_APP_PAGES, SID_APP_AURA, SID_APP_SETTINGS };
    for (int i = 0; i < 5; i++) y += sb(sh, rx + 24, y, bw, 56, AT[i], AI[i], i < 2 || i == 4, UI_LAYER_CONTENT) + 8;
    if (opt) y += sl(sh, rx + 16, y, iw, "Pages and Aura are not available yet.", 14, P->muted, 0);
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
    else build_index(sh);
    build_clients(sh);
    build_dock(sh);
    if (sh->about_open) build_about(sh);
    build_corner(sh);
    }
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
    build(sh); sh->dirty = 1;
    return 1;
}

int shell_back(struct shell *sh)
{
    if (!sh->hist_n) return 0;
    sh->screen = sh->hist[--sh->hist_n];
    if (sh->screen == SCR_INDEX) { sh->qlen = 0; index_refresh(sh); }
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
    if (open == sh->corner_open || sh->about_open) return;
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
    if (open == sh->about_open) return;
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
    if (sh->screen != SCR_INDEX || sh->about_open || sh->corner_open) return 0;
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
    if (index_input(sh, e)) { build(sh); sh->dirty = 1; return 1; }   /* rebuild: hit testing must see the layout the user sees */
    switch (e->kind) {
    case IN_KEY_DOWN:
        r = ui_key(&sh->fx, e);
        if (r == UI_ESCAPE) { r = SHELL_NONE; if (sh->about_open) set_about(sh, 0); else if (sh->corner_open) set_corner(sh, 0); else shell_back(sh); }
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
