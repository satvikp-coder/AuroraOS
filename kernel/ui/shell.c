/* kernel/ui/shell.c - native Aurora Glass shell (Tier 0 Clear): screens, navigation, layout, input dispatch. See kernel/include/shell.h. */
#include <shell.h>

const char *const SHELL_SPACE_NAMES[SHELL_SPACES] = { "Study", "Work", "Projects", "Personal" };

#define FOC (UI_VISIBLE | UI_ENABLED | UI_FOCUSABLE)
#define SOLID UI_VISIBLE   /* non-interactive surfaces: they absorb pointer events (no click-through) but never activate */
#define COL_HOVER 0x00E4ECF6u
#define COL_PRESSED 0x00CFDCF0u
#define COL_AURA 0x0000AEB8u
#define COL_DISABLED_FILL 0x00F3F6FAu

static const char *const NAMES[SCR_COUNT] = { "Home", "Spaces", "Index" };

const char *shell_screen_name(uint32_t screen) { return screen < SCR_COUNT ? NAMES[screen] : "?"; }

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
    k->kind = kind; k->text = text; k->fill = fill; k->ink = UI_COL_INK; k->border = UI_COL_MUTED;
    return k;
}

static struct ui_widget *label(struct shell *sh, int32_t x, int32_t y, int32_t w, int32_t h, const char *t, uint8_t size, uint32_t ink, uint8_t bold)
{
    struct ui_widget *k = add(sh, x, y, w, h, 0, UI_LAYER_CONTENT, UI_LABEL, t, UI_COL_SURFACE, 0xF000u + sh->n, SOLID);
    if (k) { k->font_size = size; k->ink = ink; k->bold = bold; }
    return k;
}

static void card(struct shell *sh, int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct ui_widget *k = add(sh, x, y, w, h, 16, UI_LAYER_CONTENT, UI_PANEL, 0, UI_COL_SURFACE, 0xF000u + sh->n, SOLID);
    if (k) { k->border_w = 1; k->border = 0x00C9D4E3u; }
}

static struct ui_widget *button(struct shell *sh, int32_t x, int32_t y, int32_t w, int32_t h, const char *t, uint32_t id, int enabled, uint8_t layer)
{
    struct ui_widget *k = add(sh, x, y, w, h, 12, layer, UI_BUTTON, t, UI_COL_CANVAS, id, enabled ? FOC : (UI_VISIBLE));
    if (k) { k->border_w = 1; k->border = enabled ? 0x00C9D4E3u : 0x00DCE3EDu; k->font_size = 16; }
    return k;
}

static void build_dock(struct shell *sh)
{
    struct ui_widget *p = add(sh, 262, 664, 500, 80, 28, UI_LAYER_DOCK, UI_PANEL, 0, UI_COL_SURFACE, 0xF100u, SOLID);
    if (p) { p->border_w = 1; p->border = 0x00C9D4E3u; }
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
    label(sh, 64, 112, 400, 24, "Current Space", 14, UI_COL_MUTED, 0);
    label(sh, 64, 142, 400, 40, sh->space_txt, 24, UI_COL_INK, 1);
    label(sh, 64, 190, 408, 24, "Kept in memory only. Not saved.", 14, UI_COL_MUTED, 0);
    button(sh, 64, 232, 200, 48, "Change Space", SID_CHANGE_SPACE, 1, UI_LAYER_CONTENT);

    card(sh, 40, 320, 456, 296);
    label(sh, 64, 336, 400, 24, "System", 14, UI_COL_MUTED, 0);
    label(sh, 64, 366, 408, 28, "Boot checks passed", 18, UI_COL_INK, 1);
    label(sh, 64, 404, 408, 24, "Display 1024x768, drawn by the kernel", 14, UI_COL_INK, 0);
    label(sh, 64, 432, 408, 24, sh->caps.keyboard ? "Keyboard: found" : "Keyboard: not found", 14, UI_COL_INK, 0);
    label(sh, 64, 460, 408, 24, sh->caps.pointer ? "Pointer: found" : "Pointer: not found", 14, UI_COL_INK, 0);
    label(sh, 64, 488, 408, 24, "Network, battery and radio: not supported", 14, UI_COL_MUTED, 0);
    button(sh, 64, 548, 200, 48, "About", SID_ABOUT, 1, UI_LAYER_CONTENT);

    card(sh, 520, 96, 464, 520);
    label(sh, 544, 112, 400, 24, "Applications", 14, UI_COL_MUTED, 0);
    button(sh, 544, 148, 416, 56, "Spaces", SID_APP_SPACES, 1, UI_LAYER_CONTENT);
    button(sh, 544, 216, 416, 56, "Index", SID_APP_INDEX, 1, UI_LAYER_CONTENT);
    button(sh, 544, 284, 416, 56, "Pages (not available yet)", SID_APP_PAGES, 0, UI_LAYER_CONTENT);
    button(sh, 544, 352, 416, 56, "Aura (not available yet)", SID_APP_AURA, 0, UI_LAYER_CONTENT);
    button(sh, 544, 420, 416, 56, "Settings (not available yet)", SID_APP_SETTINGS, 0, UI_LAYER_CONTENT);
    label(sh, 544, 496, 416, 24, "Only Spaces and Index exist so far.", 14, UI_COL_MUTED, 0);
}

static void build_spaces(struct shell *sh)
{
    card(sh, 40, 96, 944, 520);
    label(sh, 64, 112, 400, 40, "Spaces", 24, UI_COL_INK, 1);
    label(sh, 64, 156, 880, 24, "Choose a Space. The choice is shown on Home.", 14, UI_COL_MUTED, 0);
    for (uint32_t i = 0; i < SHELL_SPACES; i++) {
        struct ui_widget *b = button(sh, 64, 196 + (int32_t)i * 72, 400, 56, SHELL_SPACE_NAMES[i], SID_SPACE_BASE + i, 1, UI_LAYER_CONTENT);
        if (b && sh->space == i) b->active = 1;
    }
    label(sh, 64, 496, 880, 24, "Kept in memory only. Not saved. A Space does not isolate anything yet.", 14, UI_COL_MUTED, 0);
}

static void build_index(struct shell *sh)
{
    card(sh, 40, 96, 944, 520);
    label(sh, 64, 112, 400, 40, "Index", 24, UI_COL_INK, 1);
    label(sh, 64, 164, 880, 24, "Search is not available yet.", 16, UI_COL_INK, 0);
}

static void build_about(struct shell *sh)
{
    add(sh, 0, 0, (int32_t)sh->width, (int32_t)sh->height, 0, UI_LAYER_SCRIM, UI_PANEL, 0, UI_COL_SCRIM, 0xF200u, SOLID);
    struct ui_widget *d = add(sh, 262, 220, 500, 260, 20, UI_LAYER_MODAL, UI_PANEL, 0, UI_COL_SURFACE, 0xF201u, SOLID);
    if (d) { d->border_w = 2; d->border = UI_COL_FOCUS; }
    struct ui_widget *t = add(sh, 286, 240, 452, 36, 0, UI_LAYER_MODAL, UI_LABEL, "About AuroraOS", UI_COL_SURFACE, 0xF202u, SOLID);
    if (t) { t->font_size = 18; t->bold = 1; }
    struct ui_widget *l1 = add(sh, 286, 288, 452, 24, 0, UI_LAYER_MODAL, UI_LABEL, "Native Tier 0 Clear shell on QEMU.", UI_COL_SURFACE, 0xF203u, SOLID);
    struct ui_widget *l2 = add(sh, 286, 316, 452, 24, 0, UI_LAYER_MODAL, UI_LABEL, "Text is printable ASCII only.", UI_COL_SURFACE, 0xF204u, SOLID);
    struct ui_widget *l3 = add(sh, 286, 344, 452, 24, 0, UI_LAYER_MODAL, UI_LABEL, "Nothing here grants permissions.", UI_COL_SURFACE, 0xF205u, SOLID);
    if (l1) l1->font_size = 14;
    if (l2) l2->font_size = 14;
    if (l3) l3->font_size = 14;
    button(sh, 412, 404, 200, 48, "Close", SID_ABOUT_CLOSE, 1, UI_LAYER_MODAL);
}

/* Rebuild the whole widget array from the state. Focus is kept by stable id when that control still exists and can be focused. */
static void build(struct shell *sh)
{
    uint32_t keep = SHELL_NONE;
    if (sh->fx.focus >= 0 && (uint32_t)sh->fx.focus < sh->n) keep = sh->c[sh->fx.focus].id;
    sh->n = 0;
    struct ui_widget *h = add(sh, 0, 0, (int32_t)sh->width, 64, 0, UI_LAYER_CONTENT, UI_PANEL, 0, UI_COL_SURFACE, 0xF300u, SOLID);
    (void)h;
    struct ui_widget *t = add(sh, 24, 14, 300, 36, 0, UI_LAYER_CONTENT, UI_LABEL, "AuroraOS", UI_COL_SURFACE, 0xF301u, SOLID);
    if (t) { t->font_size = 24; t->bold = 1; }
    struct ui_widget *u = add(sh, 760, 20, 240, 28, 0, UI_LAYER_CONTENT, UI_LABEL, sh->uptime_txt, UI_COL_SURFACE, 0xF302u, SOLID);
    if (u) { u->font_size = 14; u->ink = UI_COL_MUTED; }
    add(sh, 0, 64, (int32_t)sh->width, 4, 0, UI_LAYER_CONTENT, UI_PANEL, 0, COL_AURA, 0xF303u, SOLID);   /* Aurora Horizon rail: boot checks passed */
    if (sh->screen == SCR_HOME) build_home(sh);
    else if (sh->screen == SCR_SPACES) build_spaces(sh);
    else build_index(sh);
    build_dock(sh);
    if (sh->about_open) build_about(sh);
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
    sh->screen = SCR_HOME; sh->space = 0;
    sh->px = (int32_t)(width / 2); sh->py = (int32_t)(height / 2);
    sh->last_activated = SHELL_NONE;
    scpy(sh->uptime_txt, 16, "Up 0:00");
    ui_init(&sh->fx, sh->c, 0);
    build(sh);
    sh->dirty = 1;
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
    build(sh); sh->dirty = 1;
    return 1;
}

int shell_back(struct shell *sh)
{
    if (!sh->hist_n) return 0;
    sh->screen = sh->hist[--sh->hist_n];
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

static void activate(struct shell *sh, uint32_t id)
{
    sh->last_activated = id; sh->activations++;
    switch (id) {
    case SID_DOCK_BACK: shell_back(sh); break;
    case SID_DOCK_HOME: shell_go(sh, SCR_HOME); break;
    case SID_DOCK_SPACES: case SID_CHANGE_SPACE: case SID_APP_SPACES: shell_go(sh, SCR_SPACES); break;
    case SID_DOCK_INDEX: case SID_APP_INDEX: shell_go(sh, SCR_INDEX); break;
    case SID_ABOUT: set_about(sh, 1); break;
    case SID_ABOUT_CLOSE: set_about(sh, 0); break;
    default:
        if (id >= SID_SPACE_BASE && id < SID_SPACE_BASE + SHELL_SPACES) shell_select_space(sh, id - SID_SPACE_BASE);
        break;                                                   /* unknown ids do nothing */
    }
}

int shell_handle(struct shell *sh, const struct input_event *e)
{
    uint32_t r = SHELL_NONE;
    switch (e->kind) {
    case IN_KEY_DOWN:
        r = ui_key(&sh->fx, e);
        if (r == UI_ESCAPE) { r = SHELL_NONE; if (sh->about_open) set_about(sh, 0); else shell_back(sh); }
        break;
    case IN_PTR_MOVE: shell_set_pointer(sh, e->x, e->y); return 1;
    case IN_PTR_DOWN: shell_set_pointer(sh, e->x, e->y); ui_pointer_down(&sh->fx, e->x, e->y); break;
    case IN_PTR_UP: shell_set_pointer(sh, e->x, e->y); r = ui_pointer_up(&sh->fx, e->x, e->y); break;
    default: return 0;
    }
    if (r != SHELL_NONE && r != UI_NONE) activate(sh, r);
    sh->dirty = 1;
    return 1;
}

int shell_draw(struct shell *sh, const struct gfx_surface *s)
{
    build(sh);                                                   /* deterministic: the frame is a pure function of the state */
    if (sh->caps.pointer) {
        int32_t hv = ui_hit(&sh->fx, sh->px, sh->py);
        if (hv >= 0 && sh->w[hv].kind == UI_BUTTON && (sh->w[hv].c.flags & UI_ENABLED) && !sh->w[hv].active) sh->w[hv].fill = COL_HOVER;
        if (sh->fx.pressed >= 0 && sh->fx.pressed < (int32_t)sh->n && !sh->w[sh->fx.pressed].active) sh->w[sh->fx.pressed].fill = COL_PRESSED;
    }
    int rc = gfx_clear(s, UI_COL_CANVAS);
    if (rc != GFX_OK) return rc;
    rc = ui_scene_draw(s, sh->w, sh->n, &sh->fx, sh->px, sh->py, sh->caps.pointer);
    if (rc == GFX_OK) sh->dirty = 0;
    return rc;
}
