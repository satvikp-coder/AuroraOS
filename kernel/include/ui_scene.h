/*
 * kernel/include/ui_scene.h - Tier 0 "Clear" scene drawing (issue #7, stage G6): opaque widgets drawn in layer order with the
 * existing G2-G4 renderer. No blur, no alpha, no GPU. The scene is a flat array of widgets; the SAME array (the `c` member of each
 * widget, via ui_scene_ctrls) feeds ui_focus.c, so what is drawn on top is exactly what hit testing and focus see.
 *
 * Ownership: the kernel owns the scene, the focus context and the framebuffer. EL0 programs get no pointer to any of them.
 * Layer order (Aurora Glass): 0 content, 1 dock, 2 glass corner, 3 sheets/menus, 4 modal scrim, 5 modal dialog, 6 toasts.
 */
#ifndef AURORA_UI_SCENE_H
#define AURORA_UI_SCENE_H
#include <gfx.h>
#include <ui_focus.h>

#define UI_LAYER_CONTENT 0
#define UI_LAYER_DOCK 1
#define UI_LAYER_CORNER 2
#define UI_LAYER_SHEET 3
#define UI_LAYER_SCRIM 4
#define UI_LAYER_MODAL 5
#define UI_LAYER_TOAST 6

/* Aurora Glass "Clear" light tokens (DESIGN.md): 0x00RRGGBB. */
#define UI_COL_CANVAS 0x00EEF3F8u
#define UI_COL_SURFACE 0x00FFFFFFu
#define UI_COL_INK 0x00142136u
#define UI_COL_MUTED 0x0052627Au
#define UI_COL_ACCENT 0x002A68E8u
#define UI_COL_FOCUS 0x00175CD3u
#define UI_COL_SCRIM 0x00525A66u        /* opaque stand-in for the dimming scrim (Tier 0 has no alpha) */
#define UI_FOCUS_RING 3                 /* ring thickness in pixels, drawn OUTSIDE the control */

enum ui_kind { UI_PANEL = 0, UI_BUTTON, UI_LABEL };

struct ui_widget {
    struct ui_ctrl c;
    uint8_t kind;
    uint8_t active;                     /* button: selected/active state (drawn with the accent fill) */
    const char *text;                   /* NUL-terminated ASCII, may be NULL/empty; clipped to the widget, never overflows */
    uint32_t fill, border, ink;         /* 0x00RRGGBB; border thickness 0 = none via border_w */
    uint8_t border_w;
    uint8_t font_size;                  /* 12/14/16/18/24; 0 = 16 */
    uint8_t bold;                       /* 1 = bold weight */
    uint8_t mag;                        /* integer glyph magnification 1..4 (0 = 1): the font_size glyphs drawn mag times larger */
    uint8_t wrap;                       /* 1 = word-wrap inside the widget (lines are centred as a block for buttons); 0 = one line */
    uint8_t marker;                     /* 1 = an active button also shows a non-colour marker (a bar inside its left edge) */
    uint32_t active_fill, active_ink, disabled_ink;   /* theme overrides; 0 = the Clear defaults (accent / surface / muted) */
};

/* Draw every visible widget in (layer, index) order, then the focus ring of the focused widget, then the pointer.
 * `focus` may be NULL. Everything is clipped to the surface. Returns GFX_OK or the first renderer error. */
int ui_scene_draw(const struct gfx_surface *s, const struct ui_widget *w, uint32_t n, const struct ui_ctx *focus, int32_t pointer_x, int32_t pointer_y, int draw_pointer);
/* As ui_scene_draw, with an explicit focus-ring colour (high-contrast themes). */
int ui_scene_draw_ex(const struct gfx_surface *s, const struct ui_widget *w, uint32_t n, const struct ui_ctx *focus, int32_t pointer_x, int32_t pointer_y, int draw_pointer, uint32_t focus_color);
/* Pixel size of the font a widget uses (after magnification): line height, advance. Shared with the shell layout. */
int ui_text_metrics(uint8_t font_size, uint8_t bold, uint8_t mag, int32_t *advance, int32_t *line_height);

#endif
