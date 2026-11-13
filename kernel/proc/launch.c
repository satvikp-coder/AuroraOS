/*
 * kernel/proc/launch.c — the trusted launch path: registry -> installer-owned executable -> validation -> process with the REGISTERED identity.
 * The policy and its reasons are written down in kernel/include/launch.h and docs/kernel/loader.md.
 */
#include <console.h>
#include <elf.h>
#include <fs.h>
#include <launch.h>
#include <proc.h>

struct app { int used; char name[16]; char file[FS_NAME_MAX + 1]; uint32_t principal; uint64_t arg; };
static struct app apps[LAUNCH_MAX_APPS];
static uint8_t exe[ELF_MAX_FILE];           /* kernel-owned buffer: the file is never executed or parsed in place on the disk path */

static int slen(const char *s, int max) { int n = 0; while (n <= max && s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void scopy(char *d, const char *s) { while ((*d++ = *s++)) ; }

void launch_reset(void) { for (int i = 0; i < LAUNCH_MAX_APPS; i++) apps[i].used = 0; }

int launch_register(const char *app, const char *file, uint32_t principal, uint64_t arg)
{
    if (!app || !file) return LAUNCH_EREG;
    int n = slen(app, 16);
    if (n < 1 || n > 15) return LAUNCH_EREG;
    for (int i = 0; i < n; i++) { char c = app[i]; if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return LAUNCH_EREG; }
    if (fs_validate_name(file) != FS_OK) return LAUNCH_EREG;
    /* The identity must be a real application identity: not the kernel, not "none", not the installer (that would let a program write executables). */
    if (principal == 0 || principal == PRINCIPAL_NONE || principal == PRINCIPAL_INSTALLER) return LAUNCH_EREG;
    int slot = -1;
    for (int i = 0; i < LAUNCH_MAX_APPS; i++) {
        if (!apps[i].used) { if (slot < 0) slot = i; continue; }
        if (seq(apps[i].name, app)) return LAUNCH_EREG;                       /* one registration per application name */
    }
    if (slot < 0) return LAUNCH_EREG;
    apps[slot].used = 1; apps[slot].principal = principal; apps[slot].arg = arg;
    scopy(apps[slot].name, app); scopy(apps[slot].file, file);
    return LAUNCH_OK;
}

void init_run(void)
{
    if (!fs_mounted()) return;
    fs_id_t id;
    if (fs_lookup_owner(PRINCIPAL_INSTALLER, "init.elf", &id) != FS_OK) return;                  /* no installed init: boot continues without userspace */
    int rc = launch_register("init", "init.elf", PRINCIPAL_INIT, 0);
    if (rc != LAUNCH_OK && rc != LAUNCH_EREG) return;                                             /* EREG: already registered earlier in this boot */
    int pid = 0, detail = 0;
    rc = launch_app("init", &pid, &detail);
    if (rc != LAUNCH_OK) { console_puts("[init] could not start init.elf: "); console_puts(launch_strerror(rc)); console_puts(" ("); console_dec((uint64_t)(detail < 0 ? -detail : detail)); console_puts(")\n"); return; }
    console_puts("[init] started init.elf as pid "); console_dec((uint64_t)pid); console_puts(", identity "); console_dec(PRINCIPAL_INIT); console_puts("\n");
    uint64_t r = sched_run(20000);
    struct proc *p = proc_get(pid);
    console_puts("[init] init "); console_puts(r == SCHED_ALL_DONE && p && p->state == PROC_TERMINATED ? "exited with code " : "did not exit normally, state ");
    console_dec((uint64_t)(p ? (p->state == PROC_TERMINATED ? p->exit_code : (int)p->state) : -1)); console_puts("\n");
    if (p && p->state != PROC_RUNNING) proc_destroy(pid);
}

const char *launch_strerror(int rc)
{
    switch (rc) {
    case LAUNCH_OK: return "ok"; case LAUNCH_ENOAPP: return "no such application"; case LAUNCH_ENOEXE: return "executable not found";
    case LAUNCH_EUNSAFE: return "executable has unsafe permissions or size"; case LAUNCH_EREAD: return "executable could not be read";
    case LAUNCH_EEXE: return "not a supported executable"; case LAUNCH_ENOMEM: return "no process slot or memory";
    case LAUNCH_EFS: return "filesystem not mounted"; case LAUNCH_EREG: return "registration refused";
    default: return "unknown error";
    }
}

int launch_app(const char *app, int *pid, int *detail)
{
    int dummy; if (!detail) detail = &dummy;
    *detail = 0;
    if (!pid) return LAUNCH_ENOAPP;
    *pid = 0;
    const struct app *a = 0;
    if (app && slen(app, 16) <= 15) for (int i = 0; i < LAUNCH_MAX_APPS; i++) if (apps[i].used && seq(apps[i].name, app)) a = &apps[i];
    if (!a) return LAUNCH_ENOAPP;
    if (!fs_mounted()) { *detail = FS_EMOUNT; return LAUNCH_EFS; }

    fs_id_t id; int rc = fs_lookup_owner(PRINCIPAL_INSTALLER, a->file, &id);
    if (rc) { *detail = rc; return LAUNCH_ENOEXE; }
    struct fs_stat st;
    rc = fs_stat(0, id, &st);
    if (rc) { *detail = rc; return LAUNCH_ENOEXE; }
    if ((st.perm & FS_PERM_OTHER_W) || st.size < 64 || st.size > ELF_MAX_FILE) return LAUNCH_EUNSAFE;   /* writable by anyone else: not an installed program */
    rc = fs_read(0, id, 0, exe, st.size);                                    /* verifies the data CRC; a corrupt file is FS_ECORRUPT, never wrong bytes */
    if (rc != (int)st.size) { *detail = rc < 0 ? rc : FS_EIO; return LAUNCH_EREAD; }

    int eerr = 0;
    int p = proc_create_elf(exe, st.size, a->name, a->principal, a->arg, &eerr);
    if (p == PROC_ELOAD_INVALID) { *detail = eerr; return LAUNCH_EEXE; }
    if (p < 0) return LAUNCH_ENOMEM;
    *pid = p;
    return LAUNCH_OK;
}
