/* kernel/input/ui_focus.c - see kernel/include/ui_focus.h for the rules. Pure logic, host-tested. */
#include <ui_focus.h>

static uint8_t min_layer(const struct ui_ctx *u) { return u->modal_depth ? u->modal_layer[u->modal_depth - 1] : 0; }
static int reachable(const struct ui_ctx *u, int32_t i) { return i >= 0 && (uint32_t)i < u->n && (u->c[i].flags & UI_VISIBLE) && u->c[i].layer >= min_layer(u); }
static int can_focus(const struct ui_ctx *u, int32_t i)
{
    return reachable(u, i) && (u->c[i].flags & UI_ENABLED) && (u->c[i].flags & UI_FOCUSABLE);
}

void ui_init(struct ui_ctx *u, struct ui_ctrl *c, uint32_t n)
{
    u->c = c; u->n = n; u->focus = -1; u->pressed = -1; u->modal_depth = 0;
    for (int i = 0; i < UI_MODAL_DEPTH; i++) { u->modal_layer[i] = 0; u->restore[i] = -1; }
}

/* Pixel-centre test in doubled coordinates (same integer rule as the G3 renderer): no square roots, no overflow in 64 bits. */
static int inside(const struct ui_ctrl *k, int32_t x, int32_t y)
{
    if (k->w <= 0 || k->h <= 0) return 0;
    int64_t px = (int64_t)x, py = (int64_t)y;
    if (px < k->x || py < k->y || px >= (int64_t)k->x + k->w || py >= (int64_t)k->y + k->h) return 0;
    int64_t r = k->radius;
    if (r <= 0) return 1;
    if (2 * r > k->w) r = k->w / 2;
    if (2 * r > k->h) r = k->h / 2;
    if (r <= 0) return 1;
    int64_t cx = 2 * px + 1 - 2 * (int64_t)k->x, cy = 2 * py + 1 - 2 * (int64_t)k->y;      /* pixel centre relative to the rect origin, x2 */
    int64_t W = 2 * (int64_t)k->w, H = 2 * (int64_t)k->h, R = 2 * r;
    int64_t dx = 0, dy = 0;
    if (cx < R) dx = R - cx; else if (cx > W - R) dx = cx - (W - R);
    if (cy < R) dy = R - cy; else if (cy > H - R) dy = cy - (H - R);
    if (!dx || !dy) return 1;                                                                /* not in a corner square */
    return dx * dx + dy * dy <= R * R;
}

int32_t ui_hit(const struct ui_ctx *u, int32_t x, int32_t y)
{
    int32_t best = -1;
    for (uint32_t i = 0; i < u->n; i++) {
        if (!reachable(u, (int32_t)i) || !inside(&u->c[i], x, y)) continue;
        if (best < 0 || u->c[i].layer >= u->c[best].layer) best = (int32_t)i;      /* >= : later entry wins ties */
    }
    return best;
}

int32_t ui_focus_set(struct ui_ctx *u, int32_t index)
{
    if (!can_focus(u, index)) return 0;
    u->focus = index;
    return 1;
}

int32_t ui_focus_next(struct ui_ctx *u, int forward)
{
    if (!u->n) { u->focus = -1; return -1; }
    int64_t start = u->focus >= 0 ? u->focus : (forward ? -1 : (int64_t)u->n);
    for (uint32_t step = 1; step <= u->n; step++) {
        int64_t i = forward ? (start + (int64_t)step) % (int64_t)u->n : ((start - (int64_t)step) % (int64_t)u->n + (int64_t)u->n) % (int64_t)u->n;
        if (can_focus(u, (int32_t)i)) { u->focus = (int32_t)i; return u->focus; }
    }
    u->focus = -1;
    return -1;
}

uint32_t ui_key(struct ui_ctx *u, const struct input_event *e)
{
    if (e->kind != IN_KEY_DOWN) return UI_NONE;
    switch (e->key) {
    case KEY_TAB: ui_focus_next(u, !(e->mods & MOD_SHIFT)); return UI_NONE;
    case KEY_DOWN: case KEY_RIGHT: ui_focus_next(u, 1); return UI_NONE;
    case KEY_UP: case KEY_LEFT: ui_focus_next(u, 0); return UI_NONE;
    case KEY_ESC: return UI_ESCAPE;
    case KEY_ENTER: case KEY_SPACE:
        if (e->repeat) return UI_NONE;                    /* holding Enter must not re-fire an action */
        if (e->mods & (MOD_CTRL | MOD_ALT)) return UI_NONE;
        if (can_focus(u, u->focus)) return u->c[u->focus].id;
        return UI_NONE;
    default: return UI_NONE;
    }
}

void ui_pointer_down(struct ui_ctx *u, int32_t x, int32_t y)
{
    int32_t h = ui_hit(u, x, y);
    u->pressed = (h >= 0 && (u->c[h].flags & UI_ENABLED)) ? h : -1;     /* a disabled or covering control swallows the press */
    if (u->pressed >= 0 && can_focus(u, u->pressed)) u->focus = u->pressed;
}

uint32_t ui_pointer_up(struct ui_ctx *u, int32_t x, int32_t y)
{
    int32_t p = u->pressed;
    u->pressed = -1;
    if (p < 0 || !reachable(u, p) || !(u->c[p].flags & UI_ENABLED)) return UI_NONE;
    return ui_hit(u, x, y) == p ? u->c[p].id : UI_NONE;
}

int ui_modal_push(struct ui_ctx *u, uint8_t layer)
{
    if (u->modal_depth >= UI_MODAL_DEPTH) return 0;
    if (u->modal_depth && layer <= u->modal_layer[u->modal_depth - 1]) return 0;
    if (!u->modal_depth && layer == 0) return 0;
    u->restore[u->modal_depth] = u->focus;
    u->modal_layer[u->modal_depth++] = layer;
    u->pressed = -1;
    if (!can_focus(u, u->focus)) ui_focus_next(u, 1);       /* focus must live inside the modal; the opener's control is now unreachable */
    return 1;
}

int ui_modal_pop(struct ui_ctx *u)
{
    if (!u->modal_depth) return 0;
    int32_t back = u->restore[--u->modal_depth];
    u->pressed = -1;
    u->focus = -1;
    if (can_focus(u, back)) u->focus = back; else ui_focus_next(u, 1);
    return 1;
}

void ui_revalidate(struct ui_ctx *u)
{
    if (u->focus >= 0 && !can_focus(u, u->focus)) u->focus = -1;
    if (u->pressed >= 0 && !reachable(u, u->pressed)) u->pressed = -1;
}
