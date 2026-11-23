/*
 * kernel/infertest.c - end-to-end test of the first native inference primitive: an EL0 int8 matrix-vector program (tests/programs/inferclient.c)
 * reading tensor objects of the verified model volume through SYS_MODEL. Compiled only into AURORA_INFER_TEST kernels (tests/qemu/infer-tests.sh).
 *
 * Needs a system disk provisioned by tests/tools/mkafs with an installer-owned "inferclient.elf". The program is registered under an ordinary
 * application identity; for the authorised runs KERNEL code moves it to PRINCIPAL_INFER with proc_set_service_principal() before it runs (exactly
 * as kernel/modeltest.c does). The mode in x0 is 1 + the object index.
 *   volume VALID:              every object k runs once as PRINCIPAL_INFER; it must TERMINATE (never fault) with exit 0 (computed) or 3
 *                              (rejected). The exact code per object is printed and checked by the host script, which knows what each object is.
 *                              Then object 0 runs under the ordinary identity: exit 4 (SYS_MODEL refused it).
 *   volume ABSENT or INVALID:  mode 1 as PRINCIPAL_INFER and as the ordinary identity: both exit 4 (refused), cleanly.
 *   always:                    no page and no process left behind.
 * Output: "[infer-test] <name>: PASS|FAIL" and "AURORA-INFER-SELFTEST: N passed, M failed".
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
#ifdef AURORA_INFER_TEST
static int npass;
static void begin(const char *name) { console_puts("[infer-test] "); console_puts(name); }
static void end(int ok) { console_puts(ok ? ": PASS\n" : ": FAIL\n"); if (ok) npass++; else nfail++; }
static void check(const char *name, int ok) { begin(name); end(ok); }
static void put_code(int code) { if (code < 0) { console_puts("-"); console_dec((uint64_t)(-(int64_t)code)); } else console_dec((uint64_t)code); }
static uint64_t free_pages(void) { struct pmm_stats s; pmm_get_stats(&s); return s.free_pages; }
static int live_procs(void) { int n = 0; for (int pid = 1; pid <= PROC_MAX; pid++) if (proc_get(pid)) n++; return n; }
static void destroy_all(void) { for (int pid = 1; pid <= PROC_MAX; pid++) if (proc_get(pid)) proc_destroy(pid); }

#define APP_ID 131u

static int run(const char *app, int infer, enum proc_state *st, int *code)
{
    int pid = 0, detail = 0;
    *st = PROC_UNUSED; *code = -1000;
    if (launch_app(app, &pid, &detail) != LAUNCH_OK) return 0;
    struct proc *p = proc_get(pid);
    if (!p || p->principal != APP_ID) { destroy_all(); return 0; }
    if (infer && (proc_set_service_principal(pid, PRINCIPAL_INFER) != 0 || p->principal != PRINCIPAL_INFER)) { destroy_all(); return 0; }
    uint64_t r = sched_run(120000);
    *st = p->state; *code = p->exit_code;
    destroy_all();
    return r == SCHED_ALL_DONE;
}

static const char *app_name(uint32_t k) { static char n[8]; n[0] = 'i'; n[1] = (char)('a' + k); n[2] = 0; return n; }

void infertest_run(void)
{
    fs_id_t id;
    if (!fs_mounted() || fs_lookup_owner(PRINCIPAL_INSTALLER, "inferclient.elf", &id) != FS_OK) {
        console_puts("[infer-test] no provisioned disk (no installer-owned inferclient.elf): not run\n");
        return;
    }
    destroy_all();
    launch_reset();
    uint32_t n = modelvol_state() == MODELVOL_VALID ? modelvol_objects() : 0;
    if (n > LAUNCH_MAX_APPS) n = LAUNCH_MAX_APPS;
    uint32_t nreg = n ? n : 1;
    int reg = 1;
    for (uint32_t k = 0; k < nreg; k++) reg &= launch_register(app_name(k), "inferclient.elf", APP_ID, k + 1) == LAUNCH_OK;
    check("setup: inferclient.elf registered (one mode per object) under the ordinary identity 131", reg);
    uint64_t pages0 = free_pages();
    enum proc_state st; int code;
    if (n) {
        for (uint32_t k = 0; k < n; k++) {
            int r = run(app_name(k), 1, &st, &code);
            begin("object "); console_dec(k); console_puts(" as PRINCIPAL_INFER: terminated with exit "); put_code(code);
            console_puts(st == PROC_FAULTED ? " (FAULTED)" : "");
            end(r && st == PROC_TERMINATED && (code == 0 || code == 3));
        }
        int r = run(app_name(0), 0, &st, &code);
        begin("denied: object 0 under an ordinary application identity: exit "); put_code(code); end(r && st == PROC_TERMINATED && code == 4);
    } else {
        int r = run(app_name(0), 1, &st, &code);
        begin("novol: PRINCIPAL_INFER with no valid model volume: exit "); put_code(code); end(r && st == PROC_TERMINATED && code == 4);
        r = run(app_name(0), 0, &st, &code);
        begin("novol: an ordinary identity: exit "); put_code(code); end(r && st == PROC_TERMINATED && code == 4);
    }
    check("no leaks: every page is back and no process remains", free_pages() == pages0 && live_procs() == 0);
    launch_reset();
}
#else
void infertest_run(void) {}
#endif

int infertest_summary(void)
{
#ifdef AURORA_INFER_TEST
    if (npass + nfail == 0) return 0;
    console_puts("AURORA-INFER-SELFTEST: "); console_dec((uint64_t)npass); console_puts(" passed, "); console_dec((uint64_t)nfail); console_puts(" failed\n");
#endif
    return nfail;
}
