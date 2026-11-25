/* kernel/proc/apps.c - the native application registry (see kernel/include/apps.h). */
#include <apps.h>
#include <console.h>
#include <fs.h>
#include <launch.h>
#include <proc.h>
#include <timersys.h>
#include <ui_sys.h>

struct appdef { uint32_t id; const char *key, *label, *file; uint32_t principal; };
static const struct appdef DEFS[] = {
    { APP_CALCULATOR, "calculator", "Calculator", "calc.elf", 601 },
    { APP_NOTES, "notes", "Notes", "notes.elf", 602 },
    { APP_FILES, "files", "Files", "files.elf", 603 },
    { APP_CLOCK, "clock", "Clock", "clock.elf", 604 },
};
static struct { int pid; uint64_t gen; } run[APP_COUNT];
static int registered[APP_COUNT];

static const struct appdef *def(uint32_t app) { for (unsigned i = 0; i < sizeof DEFS / sizeof DEFS[0]; i++) if (DEFS[i].id == app) return &DEFS[i]; return 0; }

void apps_init(void)
{
    for (unsigned i = 0; i < sizeof DEFS / sizeof DEFS[0]; i++) {
        const struct appdef *d = &DEFS[i];
        int rc = launch_register(d->key, d->file, d->principal, 0);
        registered[d->id] = (rc == LAUNCH_OK);
        int g = ui_sys_grant(d->principal);
        int t = 0;
        if (d->principal == 604) t = timersys_grant(d->principal); /* Clock needs timer capability */
        console_puts("apps: "); console_puts(d->key); console_puts(registered[d->id] ? " registered" : " registration refused"); console_puts(g == 0 ? " ui=granted" : " ui=not granted");
        if (d->principal == 604) console_puts(t == 0 ? " timer=granted" : " timer=not granted");
        console_puts("\n");
    }
}

const char *apps_label(uint32_t app) { const struct appdef *d = def(app); return d ? d->label : ""; }

int apps_running(uint32_t app)
{
    const struct appdef *d = def(app);
    if (!d || !run[app].gen) return 0;
    struct proc *p = proc_get(run[app].pid);
    if (!p || p->gen != run[app].gen || p->state == PROC_UNUSED || p->state == PROC_TERMINATED || p->state == PROC_FAULTED) return 0;   /* pid reuse: the generation must match */
    return run[app].pid;
}

int apps_available(uint32_t app)
{
    const struct appdef *d = def(app);
    if (!d || !registered[app] || !fs_mounted()) return 0;
    fs_id_t id;
    return fs_lookup_owner(PRINCIPAL_INSTALLER, d->file, &id) == FS_OK;
}

int apps_launch(uint32_t app)
{
    const struct appdef *d = def(app);
    if (!d || !registered[app]) return APPS_EUNKNOWN;
    if (apps_running(app)) return APPS_EBUSY;
    if (!apps_available(app)) return APPS_ENOTINSTALLED;
    int pid = 0, detail = 0;
    int rc = launch_app(d->key, &pid, &detail);
    if (rc != LAUNCH_OK) { console_puts("apps: launch failed: "); console_puts(launch_strerror(rc)); console_puts("\n"); return APPS_EFAILED; }
    struct proc *p = proc_get(pid);
    run[app].pid = pid; run[app].gen = p ? p->gen : 0;
    console_puts("apps: launched "); console_puts(d->key); console_puts(" pid="); console_dec((uint64_t)pid); console_puts(" principal="); console_dec(d->principal); console_puts("\n");
    return APPS_OK;
}

void apps_gone(int pid, uint64_t gen)
{
    for (uint32_t a = 1; a < APP_COUNT; a++)
        if (run[a].gen && run[a].pid == pid && run[a].gen == gen) { run[a].pid = 0; run[a].gen = 0; console_puts("apps: instance gone "); console_puts(def(a)->key); console_puts("\n"); }
}

const char *apps_describe(int rc)
{
    switch (rc) {
    case APPS_OK: return "Started.";
    case APPS_EBUSY: return "Already open. See Pages.";
    case APPS_ENOTINSTALLED: return "Not installed on this disk.";
    case APPS_EFAILED: return "Could not start it. Nothing changed.";
    default: return "Not available.";
    }
}
