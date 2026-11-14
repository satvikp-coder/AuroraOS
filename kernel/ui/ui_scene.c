/* kernel/ui/ui_scene.c - Tier 0 Clear scene drawing; see kernel/include/ui_scene.h. Deterministic, opaque, clipped, bounded. */
#include <gfx_font.h>
#include <ui_scene.h>

#define TEXT_MAX 256u            /* longer labels are cut here (the widget clips anyway); keeps work per widget bounded */
#define PAD 8
static int32_t clamp32_(int64_t v) { return v > 1000000000 ? 1000000000 : v < -1000000000 ? -1000000000 : (int32_t)v; }

static size_t slen(const char *s)
{
    size_t n = 0;
    if (!s) return 0;
    while (n < TEXT_MAX && s[n]) n++;
    return n;
}

static const struct gfx_font *font_of(const struct ui_widget *w)
{
    const struct gfx_font *f = gfx_font_find(w->bold ? 700 : 400, w->font_size ? w->font_size : 16);
    if (!f) f = gfx_font_find(400, 16);
    return f;
}

static int mag_of(const struct ui_widget *w) { return w->mag < 1 ? 1 : w->mag > 4 ? 4 : w->mag; }

int ui_text_metrics(uint8_t font_size, uint8_t bold, uint8_t mag, int32_t *advance, int32_t *line_height)
{
    const struct gfx_font *f = gfx_font_find(bold ? 700 : 400, font_size ? font_size : 16);
    if (!f || !advance || !line_height) return GFX_EARG;
    int m = mag < 1 ? 1 : mag > 4 ? 4 : mag;
    *advance = f->advance * m; *line_height = f->line_height * m;
    return GFX_OK;
}

static int draw_text(const struct gfx_surface *s, const struct ui_widget *w, struct gfx_rect inner)
{
    size_t n = slen(w->text);
    if (!n || inner.w <= 0 || inner.h <= 0) return GFX_OK;
    const struct gfx_font *f = font_of(w);
    if (!f) return GFX_OK;
    int mag = mag_of(w);
    if (w->wrap) {                                      /* wrapped block: lines centred (buttons) or left aligned; vertically centred; clipped to inner */
        int32_t avail = inner.w - 2 * PAD; if (avail < f->advance * mag) avail = f->advance * mag;
        struct gfx_text_line ln[16]; size_t total = 0;
        int rc = gfx_text_wrap(f, w->text, n, avail / mag, ln, 16, &total);
        if (rc != GFX_OK) return rc;
        size_t shown = total < 16 ? total : 16;
        int64_t lh = (int64_t)f->line_height * mag, y = inner.y + ((int64_t)inner.h - lh * (int64_t)shown) / 2;
        if (y < inner.y) y = inner.y;
        for (size_t i = 0; i < shown; i++) {
            int64_t tw = (int64_t)ln[i].cells * f->advance * mag;
            int64_t x = inner.x + PAD;
            if (w->kind == UI_BUTTON) x = inner.x + ((int64_t)inner.w - tw) / 2;
            if (x < inner.x) x = inner.x;
            rc = gfx_draw_text_mag(s, f, mag, clamp32_(x), clamp32_(y + (int64_t)i * lh), w->text + ln[i].start, ln[i].len, w->ink, &inner);
            if (rc != GFX_OK) return rc;
        }
        return GFX_OK;
    }
    int32_t tw = 0, th = 0;
    int rc = gfx_text_measure(f, w->text, n, &tw, &th);
    if (rc != GFX_OK) return rc;
    tw *= mag; th *= mag;
    int64_t x = inner.x + PAD, y = inner.y + ((int64_t)inner.h - th) / 2;
    if (w->kind == UI_BUTTON) x = inner.x + ((int64_t)inner.w - tw) / 2;
    if (x < inner.x) x = inner.x;                       /* too wide to centre: start at the left edge, clip on the right */
    if (y < inner.y) y = inner.y;
    return gfx_draw_text_mag(s, f, mag, (int32_t)x, (int32_t)y, w->text, n, w->ink, &inner);
}

static int32_t clamp32(int64_t v) { return v > 1000000000 ? 1000000000 : v < -1000000000 ? -1000000000 : (int32_t)v; }   /* kernel-owned geometry, but never overflow */

static int draw_widget(const struct gfx_surface *s, const struct ui_widget *w)
{
    struct gfx_rect r = { w->c.x, w->c.y, w->c.w, w->c.h };
    if (r.w <= 0 || r.h <= 0) return GFX_OK;
    uint32_t fill = (w->kind == UI_BUTTON && w->active) ? (w->active_fill ? w->active_fill : UI_COL_ACCENT) : w->fill;
    int rc = gfx_fill_round_rect(s, r, w->c.radius, fill);
    if (rc != GFX_OK) return rc;
    if (w->border_w) { rc = gfx_stroke_round_rect(s, r, w->c.radius, w->border_w, w->border); if (rc != GFX_OK) return rc; }
    struct gfx_rect inner = { clamp32((int64_t)r.x + w->border_w), clamp32((int64_t)r.y + w->border_w), clamp32((int64_t)r.w - 2 * (int64_t)w->border_w), clamp32((int64_t)r.h - 2 * (int64_t)w->border_w) };
    if (w->kind == UI_BUTTON && w->active) {
        struct ui_widget t = *w; t.ink = w->active_ink ? w->active_ink : UI_COL_SURFACE;
        if (w->marker && inner.w > 24 && inner.h > 16) {   /* a non-colour cue: a bar inside the left edge, in the text colour */
            rc = gfx_fill_rect(s, (struct gfx_rect){ inner.x + 6, inner.y + 6, 6, inner.h - 12 }, t.ink);
            if (rc != GFX_OK) return rc;
        }
        return draw_text(s, &t, inner);
    }
    if (w->kind == UI_BUTTON && !(w->c.flags & UI_ENABLED)) { struct ui_widget t = *w; t.ink = w->disabled_ink ? w->disabled_ink : UI_COL_MUTED; return draw_text(s, &t, inner); }   /* a disabled control is visibly muted */
    return draw_text(s, w, inner);
}

int ui_scene_draw(const struct gfx_surface *s, const struct ui_widget *w, uint32_t n, const struct ui_ctx *focus, int32_t px, int32_t py, int draw_pointer)
{
    return ui_scene_draw_ex(s, w, n, focus, px, py, draw_pointer, UI_COL_FOCUS);
}

int ui_scene_draw_ex(const struct gfx_surface *s, const struct ui_widget *w, uint32_t n, const struct ui_ctx *focus, int32_t px, int32_t py, int draw_pointer, uint32_t focus_color)
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
        struct gfx_rect r = { clamp32((int64_t)f->c.x - UI_FOCUS_RING), clamp32((int64_t)f->c.y - UI_FOCUS_RING), clamp32((int64_t)f->c.w + 2 * UI_FOCUS_RING), clamp32((int64_t)f->c.h + 2 * UI_FOCUS_RING) };
        if (f->c.w > 0 && f->c.h > 0) { rc = gfx_stroke_round_rect(s, r, clamp32((int64_t)f->c.radius + UI_FOCUS_RING), UI_FOCUS_RING, focus_color); if (rc != GFX_OK) return rc; }
    }
    if (draw_pointer) {
        struct gfx_rect o = { clamp32((int64_t)px - 6), clamp32((int64_t)py - 6), 12, 12 }, i = { clamp32((int64_t)px - 4), clamp32((int64_t)py - 4), 8, 8 };
        rc = gfx_fill_rect(s, o, UI_COL_INK); if (rc != GFX_OK) return rc;
        rc = gfx_fill_rect(s, i, UI_COL_SURFACE); if (rc != GFX_OK) return rc;
    }
    return GFX_OK;
}
