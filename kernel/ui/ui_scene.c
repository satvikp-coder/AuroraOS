/* kernel/ui/ui_scene.c - Tier 0 Clear scene drawing; see kernel/include/ui_scene.h. Deterministic, opaque, clipped, bounded. */
#include <gfx_font.h>
#include <ui_scene.h>

#define TEXT_MAX 256u            /* longer labels are cut here (the widget clips anyway); keeps work per widget bounded */
#define PAD 8

static size_t slen(const char *s)
{
    size_t n = 0;
    if (!s) return 0;
    while (n < TEXT_MAX && s[n]) n++;
    return n;
}

static int draw_text(const struct gfx_surface *s, const struct ui_widget *w, struct gfx_rect inner)
{
    size_t n = slen(w->text);
    if (!n || inner.w <= 0 || inner.h <= 0) return GFX_OK;
    const struct gfx_font *f = gfx_font_find(400, w->font_size ? w->font_size : 16);
    if (!f) f = gfx_font_find(400, 16);
    if (!f) return GFX_OK;
    int32_t tw = 0, th = 0;
    int rc = gfx_text_measure(f, w->text, n, &tw, &th);
    if (rc != GFX_OK) return rc;
    int64_t x = inner.x + PAD, y = inner.y + ((int64_t)inner.h - th) / 2;
    if (w->kind == UI_BUTTON) x = inner.x + ((int64_t)inner.w - tw) / 2;
    if (x < inner.x) x = inner.x;                       /* too wide to centre: start at the left edge, clip on the right */
    if (y < inner.y) y = inner.y;
    return gfx_draw_text(s, f, (int32_t)x, (int32_t)y, w->text, n, w->ink, &inner);
}

static int draw_widget(const struct gfx_surface *s, const struct ui_widget *w)
{
    struct gfx_rect r = { w->c.x, w->c.y, w->c.w, w->c.h };
    if (r.w <= 0 || r.h <= 0) return GFX_OK;
    uint32_t fill = (w->kind == UI_BUTTON && w->active) ? UI_COL_ACCENT : w->fill;
    int rc = gfx_fill_round_rect(s, r, w->c.radius, fill);
    if (rc != GFX_OK) return rc;
    if (w->border_w) { rc = gfx_stroke_round_rect(s, r, w->c.radius, w->border_w, w->border); if (rc != GFX_OK) return rc; }
    struct gfx_rect inner = { r.x + w->border_w, r.y + w->border_w, r.w - 2 * (int32_t)w->border_w, r.h - 2 * (int32_t)w->border_w };
    if (w->kind == UI_BUTTON && w->active) { struct ui_widget t = *w; t.ink = UI_COL_SURFACE; return draw_text(s, &t, inner); }
    return draw_text(s, w, inner);
}

int ui_scene_draw(const struct gfx_surface *s, const struct ui_widget *w, uint32_t n, const struct ui_ctx *focus, int32_t px, int32_t py, int draw_pointer)
{
    int rc = gfx_surface_check(s);
    if (rc != GFX_OK) return rc;
    if (!w && n) return GFX_EARG;
    for (int layer = 0; layer <= 255; layer++) {                  /* stable: ascending layer, then array order (256 * n; n is small) */
        for (uint32_t i = 0; i < n; i++) {
            if (w[i].c.layer != layer || !(w[i].c.flags & UI_VISIBLE)) continue;
            rc = draw_widget(s, &w[i]);
            if (rc != GFX_OK) return rc;
        }
    }
    if (focus && focus->focus >= 0 && (uint32_t)focus->focus < n) {
        const struct ui_widget *f = &w[focus->focus];
        struct gfx_rect r = { f->c.x - UI_FOCUS_RING, f->c.y - UI_FOCUS_RING, f->c.w + 2 * UI_FOCUS_RING, f->c.h + 2 * UI_FOCUS_RING };
        if (f->c.w > 0 && f->c.h > 0) { rc = gfx_stroke_round_rect(s, r, f->c.radius + UI_FOCUS_RING, UI_FOCUS_RING, UI_COL_FOCUS); if (rc != GFX_OK) return rc; }
    }
    if (draw_pointer) {
        struct gfx_rect o = { px - 6, py - 6, 12, 12 }, i = { px - 4, py - 4, 8, 8 };
        rc = gfx_fill_rect(s, o, UI_COL_INK); if (rc != GFX_OK) return rc;
        rc = gfx_fill_rect(s, i, UI_COL_SURFACE); if (rc != GFX_OK) return rc;
    }
    return GFX_OK;
}
