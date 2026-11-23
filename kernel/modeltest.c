/*
 * kernel/modeltest.c - end-to-end tests of SYS_MODEL (kernel/proc/modelsys.c). Compiled only into AURORA_MODEL_TEST kernels (tests/qemu/model-tests.sh).
 *
 * Needs a system disk provisioned by tests/tools/mkafs with an installer-owned "modelclient.elf" (tests/programs/modelclient.c). The program is launched
 * by name through the launch registry under an ordinary application identity; for the authorised runs KERNEL code then moves it to PRINCIPAL_INFER with
 * proc_set_service_principal() before it ever runs (no registration or EL0 path can name that identity). The mode travels in x0.
 *   volume VALID:              FULL as PRINCIPAL_INFER (exit 0), DENIED as an ordinary identity (exit 0 = every op EPERM), then 5 rounds of
 *                              CRASH (faults after one chunk) + QUIT (exits mid-object) followed by a FULL run that must still pass, with no page,
 *                              process or slot left behind
 *   volume ABSENT or INVALID:  NOVOL as PRINCIPAL_INFER (exit 0 = every op ENOSYS), DENIED as an ordinary identity (EPERM comes first)
 * Output: "[model-test] <name>: PASS|FAIL" and "AURORA-MODEL-SELFTEST: N passed, M failed".
 */
#include <console.h>
#include <fs.h>
#include <launch.h>
#include <mm.h>
#include <modelsys.h>
#include <modelvol.h>
#include <principal.h>
#include <proc.h>

static int nfail;
#ifdef AURORA_MODEL_TEST
static int npass;
static void check(const char *name, int ok)
{
    console_puts("[model-test] "); console_puts(name); console_puts(ok ? ": PASS\n" : ": FAIL\n");
    if (ok) npass++; else nfail++;
}
static uint64_t free_pages(void) { struct pmm_stats s; pmm_get_stats(&s); return s.free_pages; }
static int live_procs(void) { int n = 0; for (int pid = 1; pid <= PROC_MAX; pid++) if (proc_get(pid)) n++; return n; }
static void destroy_all(void) { for (int pid = 1; pid <= PROC_MAX; pid++) if (proc_get(pid)) proc_destroy(pid); }

#define APP_ID 131u                /* the ordinary identity every registration uses */

/* Launch `app`; if `infer`, kernel code moves it to PRINCIPAL_INFER before it runs. Returns the final state and exit code; destroys it afterwards. */
static int run(const char *app, int infer, enum proc_state *st, int *code)
{
    int pid = 0, detail = 0;
    *st = PROC_UNUSED; *code = -1000;
    if (launch_app(app, &pid, &detail) != LAUNCH_OK) return 0;
    struct proc *p = proc_get(pid);
    if (!p || p->principal != APP_ID) { destroy_all(); return 0; }
    if (infer && (proc_set_service_principal(pid, PRINCIPAL_INFER) != 0 || p->principal != PRINCIPAL_INFER)) { destroy_all(); return 0; }
    uint64_t r = sched_run(60000);
    *st = p->state; *code = p->exit_code;
    destroy_all();
    return r == SCHED_ALL_DONE;
}

void modeltest_run(void)
{
    fs_id_t id;
    if (!fs_mounted() || fs_lookup_owner(PRINCIPAL_INSTALLER, "modelclient.elf", &id) != FS_OK) {
        console_puts("[model-test] no provisioned disk (no installer-owned modelclient.elf): not run\n");
        return;
    }
    destroy_all();
    launch_reset();
    int reg = launch_register("mfull", "modelclient.elf", APP_ID, 1) == LAUNCH_OK && launch_register("mdenied", "modelclient.elf", APP_ID, 2) == LAUNCH_OK &&
              launch_register("mnovol", "modelclient.elf", APP_ID, 3) == LAUNCH_OK && launch_register("mcrash", "modelclient.elf", APP_ID, 4) == LAUNCH_OK &&
              launch_register("mquit", "modelclient.elf", APP_ID, 5) == LAUNCH_OK;
    check("setup: modelclient.elf registered (5 modes) under the ordinary identity 131", reg);
    {   /* the inference identity is reachable ONLY through proc_set_service_principal, and nothing else is reachable through it */
        int pid = 0, d = 0, l = launch_app("mfull", &pid, &d);
        struct proc *p = l == LAUNCH_OK ? proc_get(pid) : 0;
        int ok = p && launch_register("evil", "modelclient.elf", PRINCIPAL_INFER, 1) == LAUNCH_EREG && proc_set_principal(pid, PRINCIPAL_INFER) < 0 &&
                 proc_set_service_principal(pid, PRINCIPAL_PASSPORT) < 0 && proc_set_service_principal(pid, PRINCIPAL_TRAIL) < 0 && proc_set_service_principal(pid, PRINCIPAL_SHELL) < 0 &&
                 proc_set_service_principal(pid, PRINCIPAL_INSTALLER) < 0 && proc_set_service_principal(pid, 0) < 0 && proc_set_service_principal(pid, PRINCIPAL_NONE) < 0 &&
                 proc_set_service_principal(pid, 101) < 0 && proc_set_service_principal(99, PRINCIPAL_INFER) < 0 && p->principal == APP_ID;
        destroy_all();
        check("identity: PRINCIPAL_INFER is refused by launch_register and proc_set_principal; proc_set_service_principal refuses every other identity and a dead pid", ok);
    }
    uint64_t pages0 = free_pages();
    enum proc_state st; int code;
    if (modelvol_state() == MODELVOL_VALID) {
        int r = run("mfull", 1, &st, &code);
        check("full: modelclient as PRINCIPAL_INFER reads the count, every INFO, every object whole and gets the exact code for every hostile call (exit 0)", r && st == PROC_TERMINATED && code == 0);
        r = run("mdenied", 0, &st, &code);
        check("denied: the SAME program under an ordinary application identity gets EPERM for every op (exit 0)", r && st == PROC_TERMINATED && code == 0);
        r = run("mfull", 0, &st, &code);
        check("denied: the FULL mode under an ordinary identity fails its checks (exit 1): the authority is the identity, not the program", r && st == PROC_TERMINATED && code == 1);
        int crash = 1, quit = 1, again = 1;
        for (int i = 0; i < 5; i++) {
            r = run("mcrash", 1, &st, &code); crash &= r && st == PROC_FAULTED;
            r = run("mquit", 1, &st, &code); quit &= r && st == PROC_TERMINATED && code == 7;
            r = run("mfull", 1, &st, &code); again &= r && st == PROC_TERMINATED && code == 0;
        }
        check("lifecycle: 5 processes that FAULT after reading one chunk are contained (FAULTED)", crash);
        check("lifecycle: 5 processes that EXIT in the middle of an object exit 7", quit);
        check("lifecycle: after each crash and each mid-object exit a fresh FULL run still passes every check (nothing per process is held)", again);
    } else {
        int r = run("mnovol", 1, &st, &code);
        check("novol: with no valid model volume, PRINCIPAL_INFER gets ENOSYS for every op (exit 0)", r && st == PROC_TERMINATED && code == 0);
        r = run("mdenied", 0, &st, &code);
        check("novol: an ordinary identity still gets EPERM (checked before the volume) for every op (exit 0)", r && st == PROC_TERMINATED && code == 0);
    }
    check("no leaks: every page is back and no process remains", free_pages() == pages0 && live_procs() == 0);
    launch_reset();
}
#else
void modeltest_run(void) {}
#endif

int modeltest_summary(void)
{
#ifdef AURORA_MODEL_TEST
    if (npass + nfail == 0) return 0;
    console_puts("AURORA-MODEL-SELFTEST: "); console_dec((uint64_t)npass); console_puts(" passed, "); console_dec((uint64_t)nfail); console_puts(" failed\n");
#endif
    return nfail;
}
