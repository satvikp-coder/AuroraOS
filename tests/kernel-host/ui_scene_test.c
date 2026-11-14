/* tests/kernel-host/ui_scene_test.c - host tests for kernel/ui/ui_scene.c: layering, clipping, focus ring, modal cover, bounds. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ui_scene.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; printf("  PASS: %s\n", name); } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

#define W 320u
#define H 240u
#define GUARD 512u
static uint8_t *base, *pix;
static struct gfx_surface S;
static void fresh(uint32_t bg)
{
    memset(base, 0xA5, GUARD * 2 + W * H * 4);
    pix = base + GUARD;
    S = (struct gfx_surface){ pix, (uint64_t)W * H * 4, W, H, W * 4, GFX_FMT_XRGB8888 };
    gfx_clear(&S, bg);
}
static int guards_ok(void) { for (uint32_t i = 0; i < GUARD; i++) if (base[i] != 0xA5 || pix[W * H * 4 + i] != 0xA5) return 0; return 1; }
static uint32_t px(int x, int y) { return *(uint32_t *)(void *)(pix + (size_t)y * W * 4 + (size_t)x * 4); }
static uint32_t count(uint32_t c, int x0, int y0, int x1, int y1) { uint32_t n = 0; for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) n += px(x, y) == c; return n; }

#define VIS (UI_VISIBLE | UI_ENABLED | UI_FOCUSABLE)
static struct ui_widget mk(int x, int y, int w, int h, int r, uint8_t layer, uint8_t kind, const char *t, uint32_t fill)
{
    struct ui_widget k; memset(&k, 0, sizeof k);
    k.c = (struct ui_ctrl){ x, y, w, h, r, (uint32_t)(x + y + 1), VIS, layer };
    k.kind = kind; k.text = t; k.fill = fill; k.ink = UI_COL_INK; k.border = 0x00000000; return k;
}

int main(void)
{
    printf("ui_scene host tests\n");
    base = malloc(GUARD * 2 + W * H * 4);
    struct ui_widget wd[16];
    struct ui_ctrl ct[16];
    struct ui_ctx fx;

    fresh(UI_COL_CANVAS);
    wd[0] = mk(20, 20, 100, 60, 0, 0, UI_PANEL, 0, 0x00FF0000);
    wd[1] = mk(60, 40, 100, 60, 0, 0, UI_PANEL, 0, 0x0000FF00);
    CHECK("scene draws OK", ui_scene_draw(&S, wd, 2, 0, 0, 0, 0) == GFX_OK);
    CHECK("same layer: the later widget covers the earlier one in the overlap", px(70, 50) == 0x0000FF00 && px(30, 30) == 0x00FF0000 && px(150, 90) == 0x0000FF00);
    CHECK("outside the widgets the canvas is untouched", px(5, 5) == UI_COL_CANVAS && px(200, 200) == UI_COL_CANVAS);

    fresh(UI_COL_CANVAS);
    wd[0] = mk(20, 20, 100, 60, 0, 3, UI_PANEL, 0, 0x00FF0000);          /* listed first but on a higher layer */
    wd[1] = mk(60, 40, 100, 60, 0, 0, UI_PANEL, 0, 0x0000FF00);
    ui_scene_draw(&S, wd, 2, 0, 0, 0, 0);
    CHECK("a higher layer covers a lower one regardless of array order", px(70, 50) == 0x00FF0000 && px(150, 90) == 0x0000FF00);

    /* window B above A, then A above B after the layers swap (focus change) */
    fresh(UI_COL_CANVAS);
    wd[0] = mk(10, 10, 120, 100, 12, 0, UI_PANEL, 0, UI_COL_SURFACE); wd[0].border_w = 2; wd[0].border = 0x00000001;
    wd[1] = mk(70, 50, 120, 100, 12, 0, UI_PANEL, 0, 0x00DDEEFF); wd[1].border_w = 2; wd[1].border = 0x00000002;
    ui_scene_draw(&S, wd, 2, 0, 0, 0, 0);
    CHECK("window B above A: B's fill and border win in the overlap", px(100, 80) == 0x00DDEEFF && px(70 + 40, 50) == 0x00000002);
    wd[0].c.layer = 1;
    fresh(UI_COL_CANVAS); ui_scene_draw(&S, wd, 2, 0, 0, 0, 0);
    CHECK("raising A makes A win the same overlap", px(100, 80) == UI_COL_SURFACE);
    CHECK("rounded corner: the very corner pixel of a radius-12 panel is not painted", px(10, 10) == UI_COL_CANVAS && px(30, 10) == 0x00000001);

    /* invisible widgets */
    fresh(UI_COL_CANVAS); wd[0].c.flags &= (uint8_t)~UI_VISIBLE; ui_scene_draw(&S, wd, 2, 0, 0, 0, 0);
    CHECK("an invisible widget draws nothing", px(20, 20) == UI_COL_CANVAS);

    /* text stays inside its widget */
    fresh(UI_COL_CANVAS);
    wd[0] = mk(40, 40, 60, 24, 0, 0, UI_BUTTON, "A very long label that cannot fit", UI_COL_SURFACE);
    ui_scene_draw(&S, wd, 1, 0, 0, 0, 0);
    uint32_t outside = 0;
    for (int y = 0; y < (int)H; y++) for (int x = 0; x < (int)W; x++) if ((x < 40 || x >= 100 || y < 40 || y >= 64) && px(x, y) != UI_COL_CANVAS) outside++;
    CHECK("an over-long label is clipped: no pixel outside the widget changes", outside == 0 && count(UI_COL_INK, 40, 40, 100, 64) > 20);
    fresh(UI_COL_CANVAS);
    wd[0] = mk(40, 40, 60, 24, 0, 0, UI_BUTTON, "", UI_COL_SURFACE);
    ui_scene_draw(&S, wd, 1, 0, 0, 0, 0);
    CHECK("an empty label draws only the fill", count(UI_COL_INK, 0, 0, W, H) == 0 && px(50, 50) == UI_COL_SURFACE);
    wd[0].text = 0; fresh(UI_COL_CANVAS);
    CHECK("a NULL label is safe", ui_scene_draw(&S, wd, 1, 0, 0, 0, 0) == GFX_OK);
    char longtxt[2000]; memset(longtxt, 'W', sizeof longtxt - 1); longtxt[sizeof longtxt - 1] = 0;
    wd[0].text = longtxt; fresh(UI_COL_CANVAS);
    CHECK("a 2000-character label is bounded and safe", ui_scene_draw(&S, wd, 1, 0, 0, 0, 0) == GFX_OK && guards_ok());

    /* button text centred: the text bounding box is centred within 1 cell */
    fresh(UI_COL_CANVAS);
    wd[0] = mk(40, 40, 120, 40, 0, 0, UI_BUTTON, "OK", UI_COL_SURFACE);
    ui_scene_draw(&S, wd, 1, 0, 0, 0, 0);
    int minx = 999, maxx = -1; for (int y = 40; y < 80; y++) for (int x = 40; x < 160; x++) if (px(x, y) == UI_COL_INK) { if (x < minx) minx = x; if (x > maxx) maxx = x; }
    CHECK("button text is horizontally centred (+-8 px)", maxx > 0 && abs((minx + maxx) / 2 - 100) <= 8);

    /* active button */
    fresh(UI_COL_CANVAS);
    wd[0] = mk(40, 40, 120, 40, 8, 0, UI_BUTTON, "Home", UI_COL_SURFACE); wd[0].active = 1;
    ui_scene_draw(&S, wd, 1, 0, 0, 0, 0);
    CHECK("an active button uses the accent fill and light text", px(45, 60) == UI_COL_ACCENT && count(UI_COL_SURFACE, 40, 40, 160, 80) > 20);

    /* focus ring: drawn outside, on top, only for the focused widget */
    fresh(UI_COL_CANVAS);
    wd[0] = mk(40, 40, 100, 40, 8, 0, UI_BUTTON, "One", UI_COL_SURFACE);
    wd[1] = mk(40, 100, 100, 40, 8, 0, UI_BUTTON, "Two", UI_COL_SURFACE);
    for (int i = 0; i < 2; i++) ct[i] = wd[i].c;
    ui_init(&fx, ct, 2); ui_focus_set(&fx, 1);
    ui_scene_draw(&S, wd, 2, &fx, 0, 0, 0);
    CHECK("focus ring surrounds the focused widget (top, bottom, left, right midpoints)", px(90, 100 - 1) == UI_COL_FOCUS && px(90, 140 + 1) == UI_COL_FOCUS && px(40 - 1, 120) == UI_COL_FOCUS && px(140 + 1, 120) == UI_COL_FOCUS);
    CHECK("the ring is exactly UI_FOCUS_RING thick outside, and the control interior is unchanged", px(90, 100 - UI_FOCUS_RING) == UI_COL_FOCUS && px(90, 100 - UI_FOCUS_RING - 1) == UI_COL_CANVAS && px(45, 120) == UI_COL_SURFACE);
    CHECK("the unfocused widget has no ring", px(90, 40 - 1) == UI_COL_CANVAS && count(UI_COL_FOCUS, 0, 0, W, 95) == 0);

    /* modal: scrim + dialog over a button; the covered button's pixels are gone */
    fresh(UI_COL_CANVAS);
    wd[0] = mk(40, 40, 100, 40, 8, UI_LAYER_CONTENT, UI_BUTTON, "Covered", 0x00112233);
    wd[1] = mk(0, 0, W, H, 0, UI_LAYER_SCRIM, UI_PANEL, 0, UI_COL_SCRIM);
    wd[2] = mk(100, 60, 140, 100, 16, UI_LAYER_MODAL, UI_PANEL, 0, UI_COL_SURFACE);
    ui_scene_draw(&S, wd, 3, 0, 0, 0, 0);
    CHECK("scrim hides content outside the dialog; dialog is on top of the scrim", px(45, 60) == UI_COL_SCRIM && px(170, 110) == UI_COL_SURFACE && count(0x00112233, 0, 0, W, H) == 0);

    /* partly off-screen and huge-but-sane geometry */
    fresh(UI_COL_CANVAS);
    wd[0] = mk(-50, -30, 100, 100, 16, 0, UI_BUTTON, "Offscreen", 0x00123456);
    wd[1] = mk(280, 200, 100, 100, 16, 0, UI_BUTTON, "Offscreen", 0x00123457);
    wd[2] = mk(-100000, -100000, 5000000, 5000000, 16, 0, UI_PANEL, 0, 0x00123458);
    CHECK("partly and wholly off-screen widgets are clipped without writing out of bounds", ui_scene_draw(&S, wd, 3, 0, 0, 0, 0) == GFX_OK && guards_ok());
    CHECK("the on-screen part of the off-screen widget is drawn", px(5, 5) == 0x00123458 && px(W - 1, H - 1) == 0x00123458);

    /* many controls */
    fresh(UI_COL_CANVAS);
    struct ui_widget *many = calloc(300, sizeof *many);
    for (int i = 0; i < 300; i++) many[i] = mk((i % 20) * 16, (i / 20) * 16, 14, 14, 3, (uint8_t)(i % 7), UI_BUTTON, "x", 0x00AA0000 + (uint32_t)i);
    CHECK("300 widgets across 7 layers draw cleanly", ui_scene_draw(&S, many, 300, 0, 0, 0, 0) == GFX_OK && guards_ok());
    free(many);

    /* pointer */
    fresh(UI_COL_CANVAS);
    ui_scene_draw(&S, wd, 0, 0, 100, 100, 1);
    CHECK("pointer: dark outline, light centre", px(94, 100) == UI_COL_INK && px(100, 100) == UI_COL_SURFACE && px(106, 100) == UI_COL_CANVAS);
    fresh(UI_COL_CANVAS);
    CHECK("pointer at and beyond the screen corner is clipped safely", ui_scene_draw(&S, wd, 0, 0, -1, -1, 1) == GFX_OK && ui_scene_draw(&S, wd, 0, 0, (int32_t)W + 100, (int32_t)H + 100, 1) == GFX_OK && ui_scene_draw(&S, wd, 0, 0, 2147483640, -2147483640, 1) == GFX_OK && guards_ok());
    CHECK("NULL widgets with n > 0 is refused; NULL surface is refused", ui_scene_draw(&S, 0, 3, 0, 0, 0, 0) == GFX_EARG && ui_scene_draw(0, wd, 1, 0, 0, 0, 0) != GFX_OK);

    /* determinism */
    fresh(UI_COL_CANVAS); wd[0] = mk(10, 10, 100, 40, 8, 0, UI_BUTTON, "Det", UI_COL_SURFACE); ui_scene_draw(&S, wd, 1, 0, 50, 50, 1);
    uint8_t *copy = malloc(W * H * 4); memcpy(copy, pix, W * H * 4);
    fresh(UI_COL_CANVAS); ui_scene_draw(&S, wd, 1, 0, 50, 50, 1);
    CHECK("drawing the same scene twice produces identical frames", memcmp(copy, pix, W * H * 4) == 0);
    free(copy);

    printf("ui_scene: %d passed, %d failed\n", passed, failed);
    free(base);
    return failed ? 1 : 0;
}
