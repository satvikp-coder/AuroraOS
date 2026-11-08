/*
 * kernel/proctest.c — userspace, syscall, scheduler and isolation tests (QEMU virt).
 *
 * The user programs fault ON PURPOSE. For each one the test checks the exception class (ESR EC), the fault status
 * class, the exact faulting address, that the SPSR said EL0t, that the faulted process executed nothing afterwards,
 * and that a healthy partner process kept running and finished correctly.
 */
#include <arch.h>
#include <console.h>
#include <mm.h>
#include <proc.h>
#include <proctest.h>
#include <timer.h>

static int npass, nfail;
static void check(const char *name, int ok)
{
    console_puts("[proc-test] "); console_puts(name); console_puts(ok ? ": PASS\n" : ": FAIL\n");
    if (ok) npass++; else nfail++;
}

static volatile uint64_t kernel_secret = 0x5EC12E7C0FFEEULL;     /* kernel data a user process must never read */

static uint64_t ud(struct proc *p, uint64_t off) { return *(volatile uint64_t *)(uintptr_t)(p->data_pa + off); }
static void set_ttbr0(uint64_t v) { __asm__ volatile ("msr ttbr0_el1, %0\n isb" :: "r"(v) : "memory"); }
static uint64_t get_ttbr0(void) { uint64_t v; __asm__ volatile ("mrs %0, ttbr0_el1" : "=r"(v)); return v; }
static uint64_t free_pages(void) { struct pmm_stats s; pmm_get_stats(&s); return s.free_pages; }
static void destroy_all(void) { for (int pid = 1; pid <= PROC_MAX; pid++) if (proc_get(pid)) proc_destroy(pid); }

struct fault_case {
    const char *name; int role; uint64_t arg; uint64_t ec; int fsc_class;     /* fsc_class: 4 translation, 12 permission, 0 = not applicable */
    int far_checked; uint64_t far; int is_write;
};

static void fault_scenario(const struct fault_case *c, uint64_t arg)
{
    destroy_all();
    int bad = proc_create(c->role, arg, "bad");
    int good = proc_create(2, 0, "good");                       /* a healthy partner, so "another process continues" is observable */
    sched_trace_reset();
    uint64_t r = sched_run(2000);
    struct proc *b = proc_get(bad), *g = proc_get(good);
    int ok = bad > 0 && good > 0 && r == SCHED_ALL_DONE && b && g;
    int stopped = ok && b->state == PROC_FAULTED && ud(b, 0x40) == 0 && b->switches_in == 1;   /* never ran again after the fault */
    uint64_t ec = ok ? (b->fault_esr >> 26) & 0x3F : 0, fsc = ok ? b->fault_esr & 0x3F : 0;
    int syn = ok && ec == c->ec && (b->fault_spsr & 0xF) == 0;                              /* EL0t: it really was user code */
    if (c->fsc_class) syn = syn && (int)(fsc & 0x3C) == c->fsc_class;
    if (c->far_checked) syn = syn && b->fault_far == c->far;
    if (c->is_write) syn = syn && (b->fault_esr & (1u << 6));
    int healthy = ok && g->state == PROC_TERMINATED && g->exit_code == 0 && ud(g, 0) == 7 && ud(g, 0x10) == 0;
    char name[128]; int n = 0;
    const char *pre = "user fault: "; while (*pre) name[n++] = *pre++;
    for (const char *s = c->name; *s && n < 120; s++) name[n++] = *s;
    const char *suf = " -> stopped with the right EL0 syndrome; partner unaffected"; for (const char *s = suf; *s && n < 127; s++) name[n++] = *s;
    name[n] = 0;
    check(name, stopped && syn && healthy);
    if (!(stopped && syn && healthy) && ok) {
        console_puts("[info]   state="); console_puts(proc_state_name(b->state)); console_puts(" EC="); console_hex(ec); console_puts(" FSC="); console_hex(fsc);
        console_puts(" FAR="); console_hex(b->fault_far); console_puts(" spsr="); console_hex(b->fault_spsr); console_puts(" marker="); console_hex(ud(b, 0x40));
        console_puts(" partner="); console_puts(proc_state_name(g->state)); console_puts("\n");
    }
}

void proctest_run(void)
{
    const uint64_t pages0 = free_pages();

    /* ---- the process state machine ---- */
    check("state machine: only the documented transitions are legal",
          proc_transition_ok(PROC_NEW, PROC_READY) && proc_transition_ok(PROC_READY, PROC_RUNNING) && proc_transition_ok(PROC_RUNNING, PROC_BLOCKED) &&
          proc_transition_ok(PROC_BLOCKED, PROC_READY) && !proc_transition_ok(PROC_TERMINATED, PROC_READY) && !proc_transition_ok(PROC_TERMINATED, PROC_RUNNING) &&
          !proc_transition_ok(PROC_FAULTED, PROC_READY) && !proc_transition_ok(PROC_FAULTED, PROC_RUNNING) && !proc_transition_ok(PROC_READY, PROC_TERMINATED) &&
          !proc_transition_ok(PROC_RUNNING, PROC_UNUSED) && !proc_transition_ok(PROC_UNUSED, PROC_RUNNING));

    /* ---- one real EL0 program ---- */
    destroy_all(); sched_trace_reset();
    int a = proc_create(1, 0, "A");
    struct proc *pa = proc_get(a);
    check("process creation: a new process is READY with its own ASID, code, data and stack pages", a > 0 && pa && pa->state == PROC_READY && pa->as.asid == (uint32_t)a && pa->code_pa && pa->data_pa && pa->stack_pa);
    uint64_t r = sched_run(2000);
    int tl; const char *tr = sched_trace(&tl);
    check("EL0 execution: program A ran to completion at EL0 (every exception it raised arrived with SPSR=EL0t) and exited 0",
          r == SCHED_ALL_DONE && pa->state == PROC_TERMINATED && pa->exit_code == 0 && pa->el0_exceptions > 0 && pa->el0_confirmed == pa->el0_exceptions);
    check("EL0 execution: its private data page holds its own results (5 iterations, completion marker)", ud(pa, 0) == 5 && ud(pa, 8) == 0xAAAA && ud(pa, 0x10) == 0);
    check("syscalls: 5 writes + 5 yields + exit were counted, and 5 'A' lines were printed by the kernel", pa->syscalls == 11 && tl == 5 && tr[0] == 'A' && tr[4] == 'A');
    check("scheduler: control returned to the kernel context with the master page table (ASID 0) active", get_ttbr0() == vmm_kernel_l1_pa() && !sched_active());
    destroy_all();
    check("process destruction returns every page (tables and user pages)", free_pages() == pages0);

    /* ---- two processes, cooperative round robin ---- */
    sched_trace_reset();
    int x = proc_create(1, 0, "A"), y = proc_create(2, 0, "B");
    struct proc *px = proc_get(x), *py = proc_get(y);
    r = sched_run(2000);
    tr = sched_trace(&tl);
    check("scheduler: two processes alternate strictly (A,B x5) and the survivor finishes alone (BB)",
          r == SCHED_ALL_DONE && tl == 12 && tr[0] == 'A' && tr[1] == 'B' && tr[8] == 'A' && tr[9] == 'B' && tr[10] == 'B' && tr[11] == 'B' && px->state == PROC_TERMINATED && py->state == PROC_TERMINATED);
    check("isolation: each process kept its OWN counter in the same virtual address (A=5, B=7)", ud(px, 0) == 5 && ud(py, 0) == 7 && px->data_pa != py->data_pa);
    check("context switching preserves user registers: x23-x28 survived every yield in both processes", ud(px, 0x10) == 0 && ud(py, 0x10) == 0 && ud(px, 8) == 0xAAAA && ud(py, 8) == 0xBBBB);
    check("syscalls: getpid returned each process's own pid and ticks never ran backwards", ud(py, 0x18) == (uint64_t)y && ud(py, 0x20) <= sched_ticks() + 1);
    check("processes have distinct ASIDs, distinct page tables and distinct physical pages",
          px->as.asid != py->as.asid && aspace_ttbr(&px->as) != aspace_ttbr(&py->as) && px->code_pa != py->code_pa && px->stack_pa != py->stack_pa);

    /* ---- hardware translation, as EL0, through each process's own tables ---- */
    {
        uint64_t save = get_ttbr0(), pa1 = 0, pa2 = 0, d;
        set_ttbr0(aspace_ttbr(&px->as)); int t1 = vmm_hw_translate_el0(USER_DATA_VA, 1, &pa1);
        int own_ok = t1 == 0 && pa1 == px->data_pa && vmm_hw_translate_el0(USER_STACK_TOP - 8, 1, 0) == 0 && vmm_hw_translate_el0(USER_CODE_VA, 0, 0) == 0;
        int code_ro = vmm_hw_translate_el0(USER_CODE_VA, 1, 0) == -1;                    /* user code is not writable */
        int guard = vmm_hw_translate_el0(USER_VA_BASE + 0x2000, 0, &d) == -1;           /* guard page is unmapped */
        int kern = vmm_hw_translate_el0((uint64_t)(uintptr_t)__text_start, 0, &d) == -1 && vmm_hw_translate_el0((uint64_t)(uintptr_t)&kernel_secret, 0, &d) == -1 &&
                   vmm_hw_translate_el0((uint64_t)(uintptr_t)&kernel_secret, 1, &d) == -1 && vmm_hw_translate_el0(0x09000000ULL, 0, &d) == -1 &&
                   vmm_hw_translate_el0(0x08000000ULL, 0, &d) == -1 && vmm_hw_translate_el0((uint64_t)(uintptr_t)__exc_stack_bottom, 1, &d) == -1;
        int other = vmm_hw_translate_el0(py->data_pa, 0, &d) == -1;                       /* another process's page is not reachable by its physical address */
        int kern_el1 = vmm_hw_translate((uint64_t)(uintptr_t)&kernel_secret, 1, &d) == 0;  /* ...while the kernel itself still can */
        set_ttbr0(aspace_ttbr(&py->as)); int t2 = vmm_hw_translate_el0(USER_DATA_VA, 0, &pa2);
        set_ttbr0(save);
        check("EL0 hardware translation: own code/data/stack are accessible as designed; code read-only, guard unmapped", own_ok && code_ro && guard);
        check("EL0 hardware translation: kernel text/data, MMIO (UART, GIC), the exception stack and other processes' pages are all refused for EL0 (EL1 still has access)", kern && other && kern_el1);
        check("EL0 hardware translation: the same user virtual address maps to different physical pages in different processes", t2 == 0 && pa1 != pa2 && pa2 == py->data_pa);
    }
    destroy_all();

    /* ---- faults: real EL0 accesses, real syndromes ---- */
    struct proc *victim; int vpid;
    static const struct fault_case cases[] = {
        { "read of kernel data", 10, 0, 0x24, 12, 1, 0, 0 },
        { "write to kernel text", 11, 0, 0x24, 12, 1, 0, 1 },
        { "read of the UART (kernel MMIO)", 13, 0, 0x24, 12, 1, 0x09000000ULL, 0 },
        { "read of an unmapped address (0xDEAD0000)", 15, 0, 0x24, 4, 1, 0xDEAD0000ULL, 0 },
        { "privileged instruction (mrs sctlr_el1): UNDEFINED at EL0, EC 0x00", 16, 0, 0x00, 0, 0, 0, 0 },
        { "write to its own read-only code page", 17, 0, 0x24, 12, 1, USER_CODE_VA, 1 },
        { "jump into its own writable data page (never executable)", 18, 0, 0x20, 12, 1, USER_DATA_VA, 0 },
        { "access to the unmapped stack guard page", 19, 0, 0x24, 4, 1, USER_VA_BASE + 0x2008, 0 },
        { "floating-point instruction (trapped, state not saved)", 22, 0, 0x07, 0, 0, 0, 0 },
        { "svc with a non-zero immediate (not the syscall instruction)", 42, 0, 0x15, 0, 0, 0, 0 },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        uint64_t arg = 0; struct fault_case c = cases[i];
        if (c.role == 10) { arg = (uint64_t)(uintptr_t)&kernel_secret; c.far = arg; }
        if (c.role == 11) { arg = (uint64_t)(uintptr_t)__text_start; c.far = arg; }
        fault_scenario(&c, arg);
    }

    /* another process's private memory, by its physical address */
    destroy_all(); sched_trace_reset();
    vpid = proc_create(2, 0, "victim"); victim = proc_get(vpid);
    int att = proc_create(10, victim->data_pa, "attacker"); struct proc *at = proc_get(att);
    r = sched_run(2000);
    uint64_t secret_before = ud(victim, 0);
    check("isolation: a process reading ANOTHER process's private page (by physical address) gets a permission fault; the victim's data is intact",
          r == SCHED_ALL_DONE && at->state == PROC_FAULTED && ((at->fault_esr >> 26) & 0x3F) == 0x24 && (at->fault_esr & 0x3C) == 0x0C && at->fault_far == victim->data_pa &&
          victim->state == PROC_TERMINATED && secret_before == 7 && ud(at, 0x40) == 0);
    destroy_all();

    /* ---- syscall validation ---- */
    destroy_all(); sched_trace_reset();
    int s = proc_create(14, (uint64_t)(uintptr_t)&kernel_secret, "sys"); struct proc *ps = proc_get(s);
    r = sched_run(2000);
    tr = sched_trace(&tl);
    check("syscall validation: kernel pointer, range past the user region, 64-bit overflow, oversized length, unmapped user page => EFAULT/E2BIG; unknown number => ENOSYS",
          r == SCHED_ALL_DONE && ps->state == PROC_TERMINATED && (int64_t)ud(ps, 0x20) == AE_EFAULT && (int64_t)ud(ps, 0x28) == AE_EFAULT && (int64_t)ud(ps, 0x30) == AE_EFAULT &&
          (int64_t)ud(ps, 0x38) == AE_E2BIG && (int64_t)ud(ps, 0x48) == AE_ENOSYS && (int64_t)ud(ps, 0x50) == AE_EFAULT);
    check("syscall validation: after all the rejected calls the process survived, a valid write still worked (returned 2) and a zero-length write returned 0",
          ps->exit_code == 0 && ud(ps, 0x40) == 0 && (int64_t)ud(ps, 0x58) == 2 && (int64_t)ud(ps, 0x60) == 0 && tl == 1 && tr[0] == 'o');
    check("syscall validation: all 8 calls plus the exit reached the dispatcher (9 syscalls), yet only the single valid write produced console output", ps->syscalls == 9 && tl == 1);
    destroy_all();
    {
        uint64_t a1, a2, d;
        check("copy_from_user refuses kernel addresses, overflow, zero-sized destinations and unmapped pages without faulting the kernel",
              copy_from_user(&d, (uint64_t)(uintptr_t)&kernel_secret, 8) == AE_EFAULT && copy_from_user(&d, ~0ULL - 3, 8) == AE_EFAULT && copy_from_user(0, USER_DATA_VA, 8) == AE_EFAULT &&
              copy_from_user(&d, USER_DATA_VA, 8) == AE_EFAULT /* no user tables live in the kernel context */ && copy_from_user(&a1, USER_VA_LIMIT, 8) == AE_EFAULT &&
              copy_from_user(&a2, 0, 0) == AE_OK && !user_range_ok(USER_VA_LIMIT - 4, 8) && user_range_ok(USER_VA_BASE, 8) && !user_range_ok(0x40000000ULL, 8));
    }

    /* ---- exit codes ---- */
    destroy_all();
    int e = proc_create(41, 0, "exit7"), u = proc_create(77, 0, "unknown-role");
    r = sched_run(2000);
    check("exit: the exit code reaches the kernel (7), and an unknown program role exits with 99",
          r == SCHED_ALL_DONE && proc_get(e)->state == PROC_TERMINATED && proc_get(e)->exit_code == 7 && proc_get(u)->exit_code == 99);
    destroy_all();

    /* ---- memory of a finished process is not visible to the next one ---- */
    int sec = proc_create(20, 0, "secret"); struct proc *psec = proc_get(sec);
    r = sched_run(2000);
    uint64_t old_data = psec->data_pa, old_stack = psec->stack_pa;
    int wrote = r == SCHED_ALL_DONE && ud(psec, 0) == 0x5EC4E75EC4E7ULL;
    proc_destroy(sec);
    int st = proc_create(21, 0, "stale"); struct proc *pst = proc_get(st);
    r = sched_run(2000);
    check("freed address spaces: a new process reusing the old physical pages sees only zeros (data page and stack page)",
          wrote && r == SCHED_ALL_DONE && pst->state == PROC_TERMINATED && pst->exit_code == 0 && ud(pst, 0x80) == 0 &&
          (pst->data_pa == old_data || pst->data_pa == old_stack || pst->stack_pa == old_data || pst->stack_pa == old_stack));
    destroy_all();

    /* ---- the watchdog: a program that never yields cannot hang the kernel ---- */
    int sp = proc_create(31, 0, "spin"); struct proc *pp = proc_get(sp);
    uint64_t t0 = timer_count();
    r = sched_run(30);
    check("watchdog: a never-yielding EL0 loop is stopped after the tick limit and the kernel regains control (SCHED_TIMEOUT)",
          r == SCHED_TIMEOUT && pp->state == PROC_READY && ud(pp, 0) > 0 && !sched_active() && get_ttbr0() == vmm_kernel_l1_pa() && timer_count() - t0 > 0);
    destroy_all();

    /* ---- table limits and bookkeeping ---- */
    int ids[PROC_MAX + 1], made = 0;
    for (int i = 0; i < PROC_MAX + 1; i++) { ids[i] = proc_create(41, 0, "fill"); if (ids[i] > 0) made++; }
    int full = made == PROC_MAX && ids[PROC_MAX] < 0;
    destroy_all();
    check("process table: exactly PROC_MAX processes can exist; the next creation is refused; destroying twice and unknown pids are refused",
          full && proc_destroy(1) < 0 && proc_destroy(0) < 0 && proc_destroy(PROC_MAX + 1) < 0 && proc_get(1) == 0);
    check("no leaks: after every scenario all physical pages are back", free_pages() == pages0);
}

int proctest_summary(void)
{
    console_puts("AURORA-PROC-SELFTEST: "); console_dec((uint64_t)npass); console_puts(" passed, ");
    console_dec((uint64_t)nfail); console_puts(" failed\n");
    return nfail;
}

#ifdef AURORA_FAULT_TEST
void proctest_fault(void)
{
    if (AURORA_FAULT_TEST == 8) {
        console_puts("[fault-test] case 8: a KERNEL fault after user processes have run still panics\n");
        (void)*(volatile uint32_t *)0xDEAD0000UL;
        console_puts("ERROR: fault test did not fault!\n");
    }
}
#else
void proctest_fault(void) {}
#endif
