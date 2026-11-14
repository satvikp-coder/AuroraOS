/*
 * kernel/ui/ui_demo.c - TEST-BUILD ONLY (AURORA_UI_DEMO=N): a small native Tier 0 Clear screen driven by real keyboard/pointer
 * events, to prove input -> focus/hit testing -> scene -> framebuffer end to end. It is a demonstration, NOT the desktop shell.
 *
 *   header "AuroraOS"       status label "Active: <name>"          [Help]   (Tab order: Home, Spaces, Index, Help, then Close in the dialog)
 *   card
 *                 dock:  [Home] [Spaces] [Index]
 *   Help opens a modal dialog (scrim + dialog + [Close]); Escape or Close closes it and restores focus.
 * Every control does something real: dock buttons change the active item and the status text; Help opens the dialog; Close closes it.
 * Serial lines "ui: ..." are for the test log; the screenshot is the proof of effect. Printable ASCII only.
 */
#include <console.h>
#include <input.h>
#include <irq.h>
#include <timer.h>
#include <ui_scene.h>

enum { ID_HELP = 1, ID_HOME, ID_SPACES, ID_INDEX, ID_CLOSE, ID_NONE_ = 99 };
enum { W_HEADER, W_STATUS, W_CARD, W_DOCK, W_HOME, W_SPACES, W_INDEX, W_HELP, W_SCRIM, W_DIALOG, W_DTITLE, W_CLOSE, W_COUNT };

static struct ui_widget WD[W_COUNT];
static struct ui_ctrl CT[W_COUNT];
static struct ui_ctx FX;
static char status[32] = "Active: Home";

static void set(int i, int x, int y, int w, int h, int r, uint8_t layer, uint8_t kind, const char *t, uint32_t fill, uint32_t id, uint8_t flags)
{
    struct ui_widget k = { { x, y, w, h, r, id, flags, layer }, kind, 0, t, fill, UI_COL_MUTED, UI_COL_INK, 0, 0 };
    WD[i] = k;
}

static void sync_ctrls(void) { for (int i = 0; i < W_COUNT; i++) CT[i] = WD[i].c; FX.c = CT; ui_revalidate(&FX); }

static void build(void)
{
    const uint8_t FOC = UI_VISIBLE | UI_ENABLED | UI_FOCUSABLE, SOLID = UI_VISIBLE | UI_ENABLED;
    set(W_HEADER, 0, 0, 1024, 56, 0, UI_LAYER_CONTENT, UI_LABEL, "AuroraOS  Tier 0 Clear", UI_COL_SURFACE, 90, SOLID);
    set(W_STATUS, 40, 100, 400, 48, 12, UI_LAYER_CONTENT, UI_LABEL, status, UI_COL_SURFACE, 91, SOLID);
    set(W_CARD, 40, 170, 944, 420, 16, UI_LAYER_CONTENT, UI_PANEL, 0, UI_COL_SURFACE, 92, SOLID);
    set(W_HELP, 800, 100, 184, 48, 12, UI_LAYER_CONTENT, UI_BUTTON, "Help", UI_COL_SURFACE, ID_HELP, FOC);
    set(W_DOCK, 312, 680, 400, 64, 24, UI_LAYER_DOCK, UI_PANEL, 0, UI_COL_SURFACE, 93, SOLID);
    set(W_HOME, 332, 690, 112, 44, 14, UI_LAYER_DOCK, UI_BUTTON, "Home", UI_COL_CANVAS, ID_HOME, FOC);
    set(W_SPACES, 456, 690, 112, 44, 14, UI_LAYER_DOCK, UI_BUTTON, "Spaces", UI_COL_CANVAS, ID_SPACES, FOC);
    set(W_INDEX, 580, 690, 112, 44, 14, UI_LAYER_DOCK, UI_BUTTON, "Index", UI_COL_CANVAS, ID_INDEX, FOC);
    set(W_SCRIM, 0, 0, 1024, 768, 0, UI_LAYER_SCRIM, UI_PANEL, 0, UI_COL_SCRIM, 94, SOLID);
    set(W_DIALOG, 312, 250, 400, 220, 20, UI_LAYER_MODAL, UI_PANEL, 0, UI_COL_SURFACE, 95, SOLID);
    set(W_DTITLE, 332, 270, 360, 40, 0, UI_LAYER_MODAL, UI_LABEL, "Help: Tab moves, Enter acts", UI_COL_SURFACE, 96, SOLID);
    set(W_CLOSE, 412, 400, 200, 48, 14, UI_LAYER_MODAL, UI_BUTTON, "Close", UI_COL_CANVAS, ID_CLOSE, FOC);
    WD[W_HOME].active = 1;
    WD[W_SCRIM].c.flags = WD[W_DIALOG].c.flags = WD[W_DTITLE].c.flags = WD[W_CLOSE].c.flags = 0;   /* modal parts hidden until opened */
    FX.n = W_COUNT; sync_ctrls();
}

static void show_modal(int on)
{
    uint8_t f = on ? UI_VISIBLE : 0;
    WD[W_SCRIM].c.flags = WD[W_DIALOG].c.flags = WD[W_DTITLE].c.flags = f | (on ? UI_ENABLED : 0);
    WD[W_CLOSE].c.flags = on ? (UI_VISIBLE | UI_ENABLED | UI_FOCUSABLE) : 0;
    sync_ctrls();
}

static void open_modal(void)
{
    show_modal(1);
    if (ui_modal_push(&FX, UI_LAYER_MODAL)) console_puts("ui: modal open\n");
}

static void close_modal(void)
{
    if (ui_modal_pop(&FX)) { show_modal(0); console_puts("ui: modal closed\n"); }
}

static void activate(uint32_t id)
{
    console_puts("ui: activate "); console_dec(id); console_puts("\n");
    const char *names[] = { 0, 0, "Home", "Spaces", "Index" };
    switch (id) {
    case ID_HELP: open_modal(); break;
    case ID_CLOSE: close_modal(); break;
    case ID_HOME: case ID_SPACES: case ID_INDEX: {
        WD[W_HOME].active = id == ID_HOME; WD[W_SPACES].active = id == ID_SPACES; WD[W_INDEX].active = id == ID_INDEX;
        const char *p = "Active: "; int n = 0;
        for (; p[n]; n++) status[n] = p[n];
        for (const char *q = names[id]; *q && n < 30; q++) status[n++] = *q;
        status[n] = 0;
        break;
    }
    default: break;
    }
}

void ui_demo_run(const void *fdt, uint32_t seconds)
{
    static struct input_state in;
    const struct gfx_surface *s = gfx_get_surface();
    input_init(&in, s ? s->width : GFX_FB_WIDTH, s ? s->height : GFX_FB_HEIGHT);
    int n = vinput_init(fdt, &in);
    const struct vinput_info *vi = vinput_get_info();
    console_puts("ui: devices="); console_dec(n < 0 ? 0 : (uint64_t)n);
    console_puts(" keyboards="); console_dec((uint64_t)vi->keyboards);
    console_puts(" pointers="); console_dec((uint64_t)vi->pointers); console_puts("\n");
    ui_init(&FX, CT, W_COUNT);
    build();
    if (s) { gfx_clear(s, UI_COL_CANVAS); ui_scene_draw(s, WD, W_COUNT, &FX, in.px, in.py, vi->pointers != 0); gfx_present(0); }
    console_puts("ui: ready\n");

    timer_start_periodic(100);
    irq_cpu_enable();
    uint64_t t0 = timer_count(), limit = timer_freq() * seconds;
    int32_t last_focus = -2;
    while (timer_count() - t0 < limit) {
        vinput_poll(&in);
        struct input_event e;
        int dirty = 0;
        while (input_pop(&in, &e)) {
            dirty = 1;
            uint32_t r = UI_NONE;
            if (e.kind == IN_KEY_DOWN) {
                r = ui_key(&FX, &e);
                if (r == UI_ESCAPE) { if (FX.modal_depth) close_modal(); r = UI_NONE; }
            } else if (e.kind == IN_PTR_DOWN) ui_pointer_down(&FX, e.x, e.y);
            else if (e.kind == IN_PTR_UP) r = ui_pointer_up(&FX, e.x, e.y);
            if (r != UI_NONE) activate(r);
        }
        if (FX.focus != last_focus) {
            last_focus = FX.focus;
            console_puts("ui: focus "); if (FX.focus >= 0) console_dec(CT[FX.focus].id); else console_puts("none"); console_puts("\n");
        }
        if (dirty && s) { sync_ctrls(); gfx_clear(s, UI_COL_CANVAS); ui_scene_draw(s, WD, W_COUNT, &FX, in.px, in.py, vi->pointers != 0); gfx_present(0); }
        __asm__ volatile ("wfi");
    }
    timer_stop();
    console_puts("ui: demo done\n");
}
