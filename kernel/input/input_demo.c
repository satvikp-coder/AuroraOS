/*
 * kernel/input/input_demo.c - TEST-BUILD ONLY (AURORA_INPUT_TEST=1): proves that real keyboard/pointer events reach the guest
 * and change the framebuffer. It is not the desktop. Every drawn change is a function of NORMALISED events only.
 *
 * Screen (1024x768):
 *   - a key panel at (40,40) 200x80: dark grey normally, blue while any text key is held, with the last text character drawn in it
 *   - a modifier strip at (40,140) of three 60x30 boxes: Shift, Ctrl, Alt (lit while held)
 *   - a button at (40,200) 200x80: red; turns green only after a complete click (press and release both inside it)
 *   - a 10x10 white cursor square at the pointer position
 * Serial lines ("input: ...") are for the test log; the QEMU screenshot is the proof of effect.
 */
#include <console.h>
#include <gfx.h>
#include <input.h>
#include <irq.h>
#include <timer.h>

#define BG 0x00102030u
#define PANEL_IDLE 0x00404048u
#define PANEL_ACTIVE 0x002050e0u
#define LIT 0x00f0c020u
#define DIM 0x00303038u
#define BTN_OFF 0x00c02020u
#define BTN_ON 0x0020c040u
#define CURSOR 0x00ffffffu

static const struct gfx_rect PANEL = { 40, 40, 200, 80 }, BTN = { 40, 200, 200, 80 };

static int inside(struct gfx_rect r, int32_t x, int32_t y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static void draw(const struct gfx_surface *s, const struct input_state *in, char last_ch, int text_down, int clicked)
{
    gfx_clear(s, BG);
    gfx_fill_rect(s, PANEL, text_down ? PANEL_ACTIVE : PANEL_IDLE);
    if (last_ch) {
        const struct gfx_font *f = gfx_font_find(400, 24);
        char str[2] = { last_ch, 0 };
        if (f) gfx_draw_text(s, f, PANEL.x + 20, PANEL.y + 20, str, 1, 0x00ffffffu, 0);
    }
    uint32_t bits[3] = { MOD_SHIFT, MOD_CTRL, MOD_ALT };
    for (int i = 0; i < 3; i++) {
        struct gfx_rect r = { 40 + i * 70, 140, 60, 30 };
        gfx_fill_rect(s, r, (in->mods & bits[i]) ? LIT : DIM);
    }
    gfx_fill_rect(s, BTN, clicked ? BTN_ON : BTN_OFF);
    struct gfx_rect c = { in->px - 5, in->py - 5, 10, 10 };
    gfx_fill_rect(s, c, CURSOR);
    gfx_present(0);
}

static void say(const char *a, uint64_t v) { console_puts("input: "); console_puts(a); console_dec(v); console_puts("\n"); }

void input_demo_run(const void *fdt, uint32_t seconds)
{
    static struct input_state in;
    const struct gfx_surface *s = gfx_get_surface();
    input_init(&in, s ? s->width : GFX_FB_WIDTH, s ? s->height : GFX_FB_HEIGHT);
    int n = vinput_init(fdt, &in);
    const struct vinput_info *vi = vinput_get_info();
    console_puts("input: devices="); console_dec(n < 0 ? 0 : (uint64_t)n);
    console_puts(" keyboards="); console_dec((uint64_t)vi->keyboards);
    console_puts(" pointers="); console_dec((uint64_t)vi->pointers);
    console_puts(" failed="); console_dec((uint64_t)vi->failed); console_puts("\n");
    for (int a = 0; a < 2; a++) {
        if (in.ax[a].valid) { console_puts(a ? "input: axis Y range " : "input: axis X range "); console_dec((uint64_t)in.ax[a].min); console_puts(".."); console_dec((uint64_t)in.ax[a].max); console_puts("\n"); }
    }
    char last = 0; int text_down = 0, clicked = 0, press_in = 0;
    if (s) draw(s, &in, last, text_down, clicked);
    console_puts("input: ready\n");

    timer_start_periodic(100);                                   /* wake from wfi 100 times a second: no busy loop */
    irq_cpu_enable();
    uint64_t t0 = timer_count(), limit = timer_freq() * seconds;
    while (timer_count() - t0 < limit) {
        vinput_poll(&in);
        struct input_event e;
        int dirty = 0;
        while (input_pop(&in, &e)) {
            dirty = 1;
            switch (e.kind) {
            case IN_KEY_DOWN: console_puts("input: KEY_DOWN code="); console_dec(e.key); console_puts(" mods="); console_dec(e.mods); console_puts(e.repeat ? " repeat\n" : "\n"); break;
            case IN_KEY_UP: console_puts("input: KEY_UP code="); console_dec(e.key); console_puts(" mods="); console_dec(e.mods); console_puts("\n"); break;
            case IN_TEXT: console_puts("input: TEXT ch="); { char c[2] = { (char)e.ch, 0 }; console_puts(c); } console_puts("\n"); last = (char)e.ch; text_down = 1; break;
            case IN_PTR_MOVE: console_puts("input: PTR_MOVE x="); console_dec((uint64_t)e.x); console_puts(" y="); console_dec((uint64_t)e.y); console_puts("\n"); break;
            case IN_PTR_DOWN: press_in = inside(BTN, e.x, e.y); say("PTR_DOWN inside=", (uint64_t)press_in); break;
            case IN_PTR_UP:
                say("PTR_UP inside=", (uint64_t)inside(BTN, e.x, e.y));
                if (press_in && inside(BTN, e.x, e.y)) { clicked = 1; console_puts("input: button activated\n"); }
                press_in = 0;
                break;
            }
            if (e.kind == IN_KEY_UP) {
                int any = 0;
                for (uint16_t k = 2; k < 58; k++) if (input_key_held(&in, k) && input_us_char(k, 0)) any = 1;
                text_down = any;
            }
        }
        if (dirty && s) draw(s, &in, last, text_down, clicked);
        __asm__ volatile ("wfi");
    }
    timer_stop();
    say("polled events=", vi->polled_events);
    say("ignored=", in.st.ignored);
    console_puts("input: demo done\n");
}
