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
#include <ui_sys.h>
#include <broker_kernel.h>
#include <timersys.h>

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
static const struct { const char *app; const char *file; uint32_t principal; uint64_t mode; uint32_t bit; int ui; } CLIENTS[] = {
    { "c_spin1", "shclient.elf", 301, 1, 1, 0 }, { "c_spin2", "shclient.elf", 302, 1, 1, 0 }, { "c_yield", "shclient.elf", 303, 2, 2, 0 },
    { "c_block", "shclient.elf", 304, 3, 4, 0 }, { "c_exit", "shclient.elf", 305, 4, 8, 0 }, { "c_fault", "shclient.elf", 306, 5, 16, 0 },
    /* EL0 UI clients (uiclient.elf): 64 a demo surface, 128 a second demo surface, 256 a hostile client, 512 a client that faults after drawing,
     * 1024 a client WITHOUT the UI capability. The kernel grants the capability here (kernel code): the clients cannot. */
    /* 2048: a client that HOLDS the navigate right (granted here, by kernel code) and asks the broker to navigate; 4096: one that holds nothing;
     * 8192: one that asks and then faults at once (its request must not outlive it). */
    { "c_nav", "uiclient.elf", 406, 6, 2048, 0 }, { "c_navbad", "uiclient.elf", 407, 7, 4096, 0 }, { "c_navcrash", "uiclient.elf", 408, 8, 8192, 0 }, { "c_navflood", "uiclient.elf", 409, 9, 16384, 0 },
    /* SYS_TIMER clients (ui field 2 = the timer capability; 1 = the UI capability) (timerclient.elf): 32768 basic, 65536 hostile, 131072 without the capability, 262144 a holder of a 3 s timer, 524288 a spy on foreign ids,
     * 1048576 starts two timers and exits, 2097152 starts two and faults, 4194304 a late client that must get its full quota. The capability is granted here, by kernel code. */
    { "c_tmbasic", "timerclient.elf", 501, 1, 32768, 2 }, { "c_tmhostile", "timerclient.elf", 502, 2, 65536, 2 }, { "c_tmnocap", "timerclient.elf", 503, 3, 131072, 0 },
    { "c_tmholder", "timerclient.elf", 504, 4, 262144, 2 }, { "c_tmspy", "timerclient.elf", 505, 5, 524288, 2 }, { "c_tmexit", "timerclient.elf", 506, 6, 1048576, 2 },
    { "c_tmfault", "timerclient.elf", 507, 7, 2097152, 2 }, { "c_tmlate", "timerclient.elf", 508, 8, 4194304, 2 },
    { "c_ui1", "uiclient.elf", 401, 1, 64, 1 }, { "c_ui2", "uiclient.elf", 402, 2, 128, 1 }, { "c_uibad", "uiclient.elf", 403, 3, 256, 1 },
    { "c_uifault", "uiclient.elf", 404, 4, 512, 1 }, { "c_uinocap", "uiclient.elf", 405, 5, 1024, 0 },
};
static void launch_clients(void)
{
    if (!fs_mounted()) { console_puts("shell: clients: no filesystem, none started\n"); return; }
    for (unsigned i = 0; i < sizeof CLIENTS / sizeof CLIENTS[0]; i++) {
        if (!(AURORA_SHELL_CLIENTS & CLIENTS[i].bit)) continue;
        int pid = 0, detail = 0;
        if ((CLIENTS[i].ui & 1) && ui_sys_grant(CLIENTS[i].principal) != 0) { console_puts("shell: ui_sys_grant failed for "); console_puts(CLIENTS[i].app); console_puts("\n"); }
        if ((CLIENTS[i].ui & 2) && timersys_grant(CLIENTS[i].principal) != 0) { console_puts("shell: timersys_grant failed for "); console_puts(CLIENTS[i].app); console_puts("\n"); }
        int r = launch_register(CLIENTS[i].app, CLIENTS[i].file, CLIENTS[i].principal, CLIENTS[i].mode);
        if (r == LAUNCH_OK) r = launch_app(CLIENTS[i].app, &pid, &detail);
        if (r == LAUNCH_OK && (CLIENTS[i].bit == 2048 || CLIENTS[i].bit == 8192 || CLIENTS[i].bit == 16384)) { int g = broker_grant_navigate(pid); console_puts("shell: navigate right granted to "); console_puts(CLIENTS[i].app); console_puts(g == 0 ? "\n" : " FAILED\n"); }
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
        ui_sys_client_gone(pid, p->gen);                         /* its surfaces and queued events go with it */
        timersys_owner_gone(pid, p->gen);                        /* and its timers */
        proc_destroy(pid);
    }
}

static struct shell SH;

/* The Clock screen drives the timer service as the trusted shell itself (owner pid 0): its own quota, no way to touch another owner's timers. */
static const struct shell_timers SHELL_TIMERS = { timersys_shell_start, timersys_shell_query, timersys_shell_cancel, timersys_shell_release };

/* The native Aura screen runs its command as the trusted shell itself (requester pid 0): parse, validate, policy, narrow S0 executor, verified result. */
static int aura_glue(struct shell *s, const char *text, size_t len, uint8_t *state, char *msg, size_t cap)
{
    static uint64_t nonce;
    struct aura_outcome o;
    aura_run_text(s, broker_env(), broker_replay(), BP_SHELL_PID, BP_SHELL_GEN, ++nonce, text, len, &o);
    *state = o.state;
    size_t i = 0; for (; o.msg[i] && i + 1 < cap; i++) msg[i] = o.msg[i]; msg[i] = 0;
    console_puts("aura: shell ran kind="); console_puts(aura_kind_name(o.intent.kind)); console_puts(" decision="); console_puts(bp_decision_name(o.decision));
    console_puts(" reason="); console_puts(bp_reason_name(o.reason)); console_puts(" state="); console_dec(o.state);
    console_puts(" ["); console_puts(o.msg); console_puts("] screen="); console_puts(shell_screen_name(s->screen)); console_puts(" space="); console_puts(SHELL_SPACE_NAMES[s->space]); console_puts("\n");
    return 1;
}

static const char *client_name(uint8_t pid) { struct proc *p = proc_get(pid); return p && p->name[0] ? p->name : 0; }   /* the kernel's name for the process, never the client's */

/* Input first goes to the compositor logic here: a modal or sheet withholds everything from clients; Tab and Escape always belong to the shell
 * (and take focus back from a client); a press on a client surface gives that surface focus; keys go only to the FOCUSED surface's owner. Returns 1 if
 * the event was consumed and the shell must not see it. */
static int route_client(const struct input_event *e)
{
    int blocked = SH.about_open || SH.corner_open;
    switch (e->kind) {
    case IN_PTR_MOVE:
        if (!blocked) ui_sys_pointer(e->x, e->y, 0, 0);
        return 0;                                                /* the shell still tracks the cursor */
    case IN_PTR_DOWN: case IN_PTR_UP: {
        int down = e->kind == IN_PTR_DOWN;
        if (!blocked && ui_sys_pointer(e->x, e->y, down ? 1 : 2, 1)) { shell_set_pointer(&SH, e->x, e->y); SH.dirty = 1; return 1; }
        if (down) ui_sys_focus_clear();                          /* a press anywhere else returns focus to the shell */
        return 0;
    }
    case IN_KEY_DOWN: case IN_KEY_UP: case IN_TEXT:
        if (blocked || !ui_sys_has_focus()) return 0;
        if (e->kind != IN_TEXT && (e->key == KEY_TAB || e->key == KEY_ESC)) { if (e->kind == IN_KEY_DOWN) ui_sys_focus_clear(); return 0; }
        if (e->kind == IN_TEXT) return 1;                        /* clients get key codes, not text */
        ui_sys_key(e->key, e->mods, e->kind == IN_KEY_DOWN, e->repeat);
        return 1;
    default: return 0;
    }
}
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
    broker_init();
    timersys_init();
    ui_sys_init(40, 96 + UIP_CHROME_H, 944, 520 - UIP_CHROME_H);          /* the area client surfaces may occupy: inside the content band, clear of the header and dock */
    shell_set_clients(&SH, ui_sys_state(), client_name);
    shell_set_pages(&SH, procsnap_take_kernel);
    shell_set_aura(&SH, aura_glue);
    shell_set_timers(&SH, &SHELL_TIMERS);
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
    uint32_t sseq = SH.set_seq, cdig = 0, clk_seq_logged = SH.clk_seq, toast_logged = 0;
    uint64_t pg_q = 0, pg_digest = 0; int pg_logged = 0;
    for (;;) {
        /* Execute broker-approved requests of EL0 clients that are STILL the same live instance BEFORE reading input, and show the result at once: the user's
         * next click is then resolved against the screen they actually see, and a press that began on the old layout is dropped (an EL0 navigation must never
         * redirect a click onto a control of a screen the user has not seen). */
        if (broker_drain(&SH) > 0) { SH.fx.pressed = -1; if (SH.dirty) present(); }
        vinput_poll(in);
        struct input_event e;
        while (input_pop(in, &e)) { if (!route_client(&e)) shell_handle(&SH, &e); }
        { uint32_t d = uip_digest(ui_sys_state()); if (d != cdig) { cdig = d; SH.dirty = 1; } }     /* a client committed, appeared, vanished or changed focus */
        shell_set_uptime(&SH, (uint32_t)((timer_count() - t0) / hz));
        shell_tick(&SH, timersys_now_ms());                      /* timers and the toast follow the monotonic counter, whatever screen is showing */
        if (SH.toast_on != toast_logged) { toast_logged = SH.toast_on; console_puts(toast_logged ? "shell: toast on\n" : "shell: toast off\n"); }
        if (SH.clk_seq != clk_seq_logged) { clk_seq_logged = SH.clk_seq; console_puts("shell: say "); console_puts(SH.clk_live); console_puts("\n"); }
        if (SH.screen != SCR_PAGES) pg_logged = 0;
        if (SH.screen == SCR_PAGES) { uint64_t q = (timer_count() - t0) / (hz / 4 ? hz / 4 : 1); if (q != pg_q) { pg_q = q; shell_pages_refresh(&SH); if (!pg_logged || SH.ps_digest != pg_digest) { pg_logged = 1; pg_digest = SH.ps_digest;
            console_puts("shell: pages n="); console_dec(SH.ps.n);
            for (uint32_t i = 0; i < SH.ps.n; i++) { console_puts(" "); console_dec(SH.ps.e[i].pid); console_puts("/"); console_puts(SH.ps.e[i].name); console_puts("/"); console_puts(procsnap_state_name(SH.ps.e[i].state)); }
            console_putc(10); } } }   /* Pages: re-read the table four times a second; it repaints only if something shown changed */
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
        timersys_tick();                                         /* completion depends only on the monotonic counter, never on the visible screen */
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
