/*
 * kernel/ui/shell_run.c - the kernel side of the native shell: draws Home as soon as the display is up, then (after the boot self-tests)
 * runs the interactive loop: poll input, feed the shell, redraw only when something changed, refresh the uptime label once a second.
 * The loop sleeps in `wfi` between 100 Hz timer ticks, so an idle shell does not spin the CPU. Serial lines "shell: ..." describe state
 * changes for the QEMU tests; the framebuffer is the proof of what the user sees.
 */
#include <console.h>
#include <gfx.h>
#include <input.h>
#include <irq.h>
#include <shell.h>
#include <timer.h>

static struct shell SH;
static int up;

static void present(void)
{
    const struct gfx_surface *s = gfx_get_surface();
    if (s && shell_draw(&SH, s) == GFX_OK) gfx_present(0);
}

void shell_boot_draw(void)
{
    const struct gfx_surface *s = gfx_get_surface();
    if (!s) return;
    shell_init(&SH, s->width, s->height);
    up = 1;
    present();
    console_puts("shell: home drawn\n");
}

static void log_state(uint32_t *screen, uint32_t *space, int32_t *focus, uint32_t *modal, uint32_t *opens, uint32_t *qsig)
{
    if (SH.screen != *screen) { *screen = SH.screen; console_puts("shell: screen="); console_puts(shell_screen_name(SH.screen)); console_puts(" history="); console_dec(SH.hist_n); console_puts("\n"); }
    if (SH.space != *space) { *space = SH.space; console_puts("shell: space="); console_puts(SHELL_SPACE_NAMES[SH.space]); console_puts("\n"); }
    int32_t f = SH.fx.focus >= 0 ? (int32_t)SH.c[SH.fx.focus].id : -1;
    if (f != *focus) { *focus = f; console_puts("shell: focus="); if (f >= 0) console_dec((uint64_t)f); else console_puts("none"); console_puts("\n"); }
    if (SH.screen == SCR_INDEX) {                                /* a signature of the search state, so only real changes are logged */
        uint32_t sig = SH.qlen * 131u + (uint32_t)(SH.sel + 2) * 17u + SH.nres;
        for (uint32_t i = 0; i < SH.qlen; i++) sig = sig * 31u + (uint8_t)SH.query[i];
        if (sig != *qsig) {
            *qsig = sig;
            console_puts("shell: query=["); console_puts(SH.query); console_puts("] results="); console_dec(SH.nres);
            console_puts(" sel="); if (SH.sel >= 0) console_puts(shell_index_item(SH.res[SH.sel].item)->key); else console_puts("none"); console_puts("\n");
        }
    } else *qsig = 0xffffffffu;
    if (SH.index_opens != *opens) { *opens = SH.index_opens; console_puts("shell: opened="); console_puts(SH.recent[0]); console_puts("\n"); }
    if (SH.about_open != *modal) { *modal = SH.about_open; console_puts(*modal ? "shell: modal open\n" : "shell: modal closed\n"); }
}

void shell_loop(struct input_state *in)
{
    if (!up) return;
    const struct vinput_info *vi = vinput_get_info();
    struct shell_caps caps = { vi->keyboards != 0, vi->pointers != 0 };
    shell_set_caps(&SH, caps);
    present();
    console_puts("shell: ready keyboards="); console_dec((uint64_t)vi->keyboards);
    console_puts(" pointers="); console_dec((uint64_t)vi->pointers); console_puts("\n");
    timer_start_periodic(100);
    irq_cpu_enable();
    uint64_t t0 = timer_count(), hz = timer_freq();
    uint32_t screen = SH.screen, space = SH.space, modal = SH.about_open; int32_t focus = -2;
    uint32_t last_act = SH.activations, opens = SH.index_opens, qsig = 0xffffffffu;
    for (;;) {
        vinput_poll(in);
        struct input_event e;
        while (input_pop(in, &e)) shell_handle(&SH, &e);
        shell_set_uptime(&SH, (uint32_t)((timer_count() - t0) / hz));
        log_state(&screen, &space, &focus, &modal, &opens, &qsig);
        if (SH.activations != last_act) { last_act = SH.activations; console_puts("shell: activate "); console_dec(SH.last_activated); console_puts("\n"); }
        if (SH.dirty) present();
        __asm__ volatile ("wfi");
    }
}
