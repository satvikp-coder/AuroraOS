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
#include <fs.h>
#include <launch.h>
#include <proc.h>
#include <settings.h>
#include <shell.h>
#include <timer.h>

/* The shell's own identity for the settings registry. Kernel code (this file, at boot) grants it appearance and attention rights, and NOT the
 * system right: the registry, not the UI, decides, so the Aura-mode setting stays refused. No process can ever hold this principal. */
#define PRINCIPAL_SHELL 0x7F000003u
static int store_get(uint32_t id, uint32_t *v) { return settings_get(PRINCIPAL_SHELL, id, v); }
static int store_set(uint32_t id, uint32_t v) { return settings_set(PRINCIPAL_SHELL, id, v); }
static int store_reset(uint32_t id) { return settings_reset(PRINCIPAL_SHELL, id); }
static const struct shell_store STORE = { store_get, store_set, store_reset };

#define SHELL_SLICE_TICKS 6        /* scheduler ticks (1 ms each while it runs) handed to EL0 processes per shell iteration */
#define SHELL_QUANTUM_TICKS 2      /* round-robin quantum among EL0 processes inside a slice */

#ifdef AURORA_SHELL_CLIENTS
/* Test builds: start EL0 clients from the installer-owned "shclient.elf" before the shell loop, selected by a bit mask:
 * 1 two CPU-bound spinners, 2 a yielder, 4 a process blocked on IPC, 8 a process that exits after a while, 16 one that faults after a while,
 * 32 a process that exits and is replaced repeatedly (the loop below recreates it). The mode travels in x0 (kernel-chosen). */
static const struct { const char *app; uint32_t principal; uint64_t mode; uint32_t bit; } CLIENTS[] = {
    { "c_spin1", 301, 1, 1 }, { "c_spin2", 302, 1, 1 }, { "c_yield", 303, 2, 2 }, { "c_block", 304, 3, 4 }, { "c_exit", 305, 4, 8 }, { "c_fault", 306, 5, 16 },
};
static void launch_clients(void)
{
    if (!fs_mounted()) { console_puts("shell: clients: no filesystem, none started\n"); return; }
    for (unsigned i = 0; i < sizeof CLIENTS / sizeof CLIENTS[0]; i++) {
        if (!(AURORA_SHELL_CLIENTS & CLIENTS[i].bit)) continue;
        int pid = 0, detail = 0;
        int r = launch_register(CLIENTS[i].app, "shclient.elf", CLIENTS[i].principal, CLIENTS[i].mode);
        if (r == LAUNCH_OK) r = launch_app(CLIENTS[i].app, &pid, &detail);
        console_puts("shell: client "); console_puts(CLIENTS[i].app);
        if (r == LAUNCH_OK) { console_puts(" pid="); console_dec((uint64_t)pid); console_puts("\n"); }
        else { console_puts(" failed: "); console_puts(launch_strerror(r)); console_puts("\n"); }
    }
}
#endif

#if defined(AURORA_SHELL_CLIENTS) && (AURORA_SHELL_CLIENTS & 32)
/* Test builds: a short-lived client (mode 6, exits after about 20 ticks) is started again every time the previous one is gone, 25 times in total:
 * repeated process creation and destruction while the shell stays interactive. Failures to start (no slot, no memory) are counted, not fatal. */
static void churn_step(void)
{
    static int pid, left = 25, started, failed;
    if (!left) return;
    struct proc *p = pid ? proc_get(pid) : 0;
    if (p && p->state != PROC_UNUSED) return;                    /* the previous one still exists (running, or waiting to be reaped) */
    int np = 0, detail = 0;
    static int registered;
    int r = LAUNCH_OK;
    if (!registered) { r = launch_register("c_churn", "shclient.elf", 307, 6); if (r == LAUNCH_OK) registered = 1; }    /* once: the registry refuses a second entry of the same name */
    if (r == LAUNCH_OK) r = launch_app("c_churn", &np, &detail);
    left--;
    if (r == LAUNCH_OK) { pid = np; started++; } else { pid = 0; failed++; }
    if (!left) { console_puts("shell: churn done started="); console_dec((uint64_t)started); console_puts(" failed="); console_dec((uint64_t)failed); console_puts("\n"); }
}
#endif

/* Free the slot of every process that has finished or faulted (kernel code only: there is no syscall for this). */
static void reap(void)
{
    for (int pid = 1; pid <= PROC_MAX; pid++) {
        struct proc *p = proc_get(pid);
        if (!p || (p->state != PROC_TERMINATED && p->state != PROC_FAULTED)) continue;
        console_puts("shell: reaped pid="); console_dec((uint64_t)pid);
        console_puts(p->state == PROC_FAULTED ? " faulted\n" : " exited code=");
        if (p->state == PROC_TERMINATED) { console_dec((uint64_t)(uint32_t)p->exit_code); console_puts("\n"); }
        proc_destroy(pid);
    }
}

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
#ifdef AURORA_SHELL_TEXT_PCT
    shell_set_text_scale(&SH, AURORA_SHELL_TEXT_PCT);          /* test builds only: the preference store is not wired to the shell yet */
#endif
#ifdef AURORA_SHELL_CONTRAST
    shell_set_contrast(&SH, AURORA_SHELL_CONTRAST);
#endif
    up = 1;
    present();
    console_puts("shell: appearance text="); console_dec(SH.text_pct); console_puts(" contrast="); console_dec(SH.contrast); console_puts("\n");
    console_puts("shell: home drawn\n");
}

static void log_state(uint32_t *screen, uint32_t *space, int32_t *focus, uint32_t *modal, uint32_t *opens, uint32_t *qsig, uint32_t *corner)
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
    if (SH.corner_open != *corner) { *corner = SH.corner_open; console_puts(*corner ? "shell: corner open\n" : "shell: corner closed\n"); }
    if (SH.about_open != *modal) { *modal = SH.about_open; console_puts(*modal ? "shell: modal open\n" : "shell: modal closed\n"); }
}

#ifdef AURORA_SHELL_LAYOUT_LOG
/* Test builds: print the geometry of every focusable control whenever it changes, so tests/qemu can click real controls. */
static void log_layout(uint32_t *sig)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < SH.n; i++) {
        if (!(SH.c[i].flags & UI_FOCUSABLE)) continue;
        h = (h ^ SH.c[i].id) * 16777619u; h = (h ^ (uint32_t)SH.c[i].x) * 16777619u; h = (h ^ (uint32_t)SH.c[i].y) * 16777619u;
        h = (h ^ (uint32_t)SH.c[i].w) * 16777619u; h = (h ^ (uint32_t)SH.c[i].h) * 16777619u;
    }
    if (h == *sig) return;
    *sig = h;
    console_puts("shell: layout begin\n");
    for (uint32_t i = 0; i < SH.n; i++) {
        if (!(SH.c[i].flags & UI_FOCUSABLE)) continue;
        console_puts("shell: ctrl "); console_dec(SH.c[i].id); console_puts(" "); console_dec((uint64_t)SH.c[i].x); console_puts(" "); console_dec((uint64_t)SH.c[i].y);
        console_puts(" "); console_dec((uint64_t)SH.c[i].w); console_puts(" "); console_dec((uint64_t)SH.c[i].h); console_puts("\n");
    }
    console_puts("shell: layout end\n");
}
#endif

void shell_loop(struct input_state *in)
{
    if (!up) return;
    const struct vinput_info *vi = vinput_get_info();
    struct shell_caps caps = { vi->keyboards != 0, vi->pointers != 0 };
    shell_set_caps(&SH, caps);
    if (fs_mounted()) {                                          /* a usable AuroraFS: the registry loads (or reports why it fell back to defaults) */
        int st = settings_load();
        int g = settings_grant(PRINCIPAL_SHELL, CAP_SET_APPEARANCE | CAP_SET_ATTENTION);
        console_puts("shell: settings store=disk load="); console_dec((uint64_t)st); console_puts(" grant="); console_dec((uint64_t)(g < 0 ? -g : g));
        console_puts(g == SETTINGS_OK ? " (appearance+attention, no system)\n" : " FAILED\n");
        if (g == SETTINGS_OK) shell_set_store(&SH, &STORE);
    } else console_puts("shell: settings store=none (changes last for this session only)\n");
    console_puts("shell: appearance text="); console_dec(SH.text_pct); console_puts(" contrast="); console_dec(SH.contrast); console_puts("\n");
    present();
    console_puts("shell: ready keyboards="); console_dec((uint64_t)vi->keyboards);
    console_puts(" pointers="); console_dec((uint64_t)vi->pointers); console_puts("\n");
#ifdef AURORA_SHELL_CLIENTS
    launch_clients();
#endif
    sched_set_preempt(1, SHELL_QUANTUM_TICKS);
    int timer_on = 1;
    uint32_t slices = 0, slices_logged = 0, last_sec = 0;
    timer_start_periodic(100);
    irq_cpu_enable();
    uint64_t t0 = timer_count(), hz = timer_freq();
    uint32_t screen = SH.screen, space = SH.space, modal = SH.about_open; int32_t focus = -2;
    uint32_t last_act = SH.activations, opens = SH.index_opens, qsig = 0xffffffffu, corner = SH.corner_open;
#ifdef AURORA_SHELL_LAYOUT_LOG
    uint32_t lsig = 0;
#endif
    uint32_t sseq = SH.set_seq;
    for (;;) {
        vinput_poll(in);
        struct input_event e;
        while (input_pop(in, &e)) shell_handle(&SH, &e);
        shell_set_uptime(&SH, (uint32_t)((timer_count() - t0) / hz));
        log_state(&screen, &space, &focus, &modal, &opens, &qsig, &corner);
#ifdef AURORA_SHELL_LAYOUT_LOG
        log_layout(&lsig);
#endif
        if (SH.set_seq != sseq) {
            sseq = SH.set_seq;
            console_puts("shell: setting row="); console_dec(SH.set_row); console_puts(" msg="); console_dec(SH.set_msg);
            console_puts(" text="); console_dec(SH.text_pct); console_puts(" contrast="); console_dec(SH.contrast); console_puts("\n");
        }
        if (SH.activations != last_act) { last_act = SH.activations; console_puts("shell: activate "); console_dec(SH.last_activated); console_puts("\n"); }
        if (SH.dirty) present();
        reap();
#if defined(AURORA_SHELL_CLIENTS) && (AURORA_SHELL_CLIENTS & 32)
        churn_step();
#endif
        if (sched_has_ready()) {                                 /* EL0 work to do: give it a bounded slice, then come straight back to the input */
            sched_run_slice(SHELL_SLICE_TICKS);                  /* returns with the timer stopped */
            timer_on = 0; slices++;
            uint32_t sec = (uint32_t)((timer_count() - t0) / hz);
            if (sec != last_sec && slices != slices_logged) {     /* at most one summary line per second */
                last_sec = sec; slices_logged = slices;
                const struct sched_stats *st = sched_get_stats();
                console_puts("shell: sched slices="); console_dec(slices); console_puts(" switches="); console_dec(st->switches);
                console_puts(" preemptions="); console_dec(st->preemptions); console_puts(" ticks="); console_dec(sched_ticks()); console_puts("\n");
            }
            continue;
        }
        if (!timer_on) { timer_start_periodic(100); timer_on = 1; }   /* nothing runnable: sleep until the next tick or interrupt */
        __asm__ volatile ("wfi");
    }
}
