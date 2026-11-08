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

struct pair_result { uint64_t r; struct proc *a, *b; };

/* Two copies of the never-yielding CPU program (role 50), x1 = duration in scheduler ticks, run with timer preemption on. */
static void run_cpu_pair(uint64_t dur_a, uint64_t dur_b, uint32_t quantum, struct pair_result *o, int *pa, int *pb)
{
    destroy_all(); sched_stats_reset();
    *pa = proc_create(50, dur_a, "cpu-a"); *pb = proc_create(50, dur_b, "cpu-b");
    sched_set_preempt(1, quantum);
    o->r = sched_run(5000);
    sched_set_preempt(0, 0);
    o->a = proc_get(*pa); o->b = proc_get(*pb);
}

/* Every recorded timer tick was handled at or after its deadline, and consecutive deadlines are at least one period apart. */
static int timer_hist_sane(void)
{
    uint32_t n; const struct timer_tick_record *h = timer_history(&n);
    if (n < 2) return 1;
    for (uint32_t i = 0; i < n; i++) if (!timer_reached(h[i].seen_at, h[i].deadline)) return 0;
    for (uint32_t i = 1; i < n; i++) if ((h[i].deadline - h[i - 1].deadline) < timer_period()) return 0;
    return 1;
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

    /* ---- address-space mapping API: overflow-safe ranges and failure atomicity ---- */
    {
        struct aspace as;
        uint64_t pa = 0;
        int rc_create = aspace_create(&as, 200);
        const uint64_t *l1 = (const uint64_t *)(uintptr_t)as.l1_pa;
        const uint64_t UMAX = ~0ULL;
        struct { uint64_t va; const char *what; } bad[] = {
            { UMAX - PAGE_SIZE + 1, "UINT64_MAX-PAGE_SIZE+1 (end address wraps to 0)" },
            { UMAX & ~(PAGE_SIZE - 1), "last page of the address space" },
            { USER_VA_BASE - PAGE_SIZE, "the page just below the user region" },
            { USER_VA_LIMIT, "the first page past the user region" },
            { USER_VA_LIMIT + PAGE_SIZE, "a page further past the region" },
            { USER_VA_BASE + 0x123, "an unaligned address" },
            { 0, "address 0" },
            { 0x40000000ULL, "the kernel's RAM slot (L1 slot 1)" },
            { 0x08000000ULL, "the kernel's device slot (L1 slot 0)" },
            { 0x8000000000ULL, "a different L1 slot above the user region" },
            { 0x7FFFFFF000ULL, "the last page of the slot below the user region (crosses an L1 boundary)" },
        };
        int all_bad_rejected = rc_create == 0;
        uint64_t before_pages = (uint64_t)as.npages, before_free = free_pages();
        for (unsigned i = 0; rc_create == 0 && i < sizeof bad / sizeof *bad; i++) {
            int r1 = aspace_map(&as, bad[i].va, UPERM_RW, 0, 0, &pa);
            if (r1 >= 0) { all_bad_rejected = 0; console_puts("[info] aspace_map ACCEPTED: "); console_puts(bad[i].what); console_puts("\n"); }
        }
        check("aspace_map rejects wrapping, out-of-range, unaligned and other-slot addresses (UINT64_MAX-PAGE_SIZE+1, below/at/past the region, kernel slots)", all_bad_rejected);
        int only_expected_slots = rc_create == 0;
        int mapped_slots = 0;
        for (int i = 0; rc_create == 0 && i < 512; i++) if (l1[i] & 1) { mapped_slots++; if (i != 0 && i != 1) only_expected_slots = 0; }
        check("rejected requests changed nothing: no page, no table and no L1 entry beyond the two shared kernel slots, no accounting change",
              only_expected_slots && mapped_slots == 2 && (uint64_t)as.npages == before_pages && free_pages() == before_free);
        int r_ok = rc_create == 0 ? aspace_map(&as, USER_DATA_VA, UPERM_RW, 0, 0, &pa) : -99;
        int r_last = rc_create == 0 ? aspace_map(&as, USER_VA_LIMIT - PAGE_SIZE, UPERM_RW, 0, 0, 0) : -99;
        int r_dup = rc_create == 0 ? aspace_map(&as, USER_DATA_VA, UPERM_RW, 0, 0, 0) : -99;
        uint64_t d;
        check("aspace_map still accepts a valid page and the very last page of the region, and refuses to remap",
              r_ok == 0 && r_last == 0 && r_dup < 0 && pa && pmm_is_allocated(pa) && vmm_hw_translate(pa, 0, &d) == 0);
        uint64_t used_pages = (uint64_t)as.npages, free_before = free_pages();
        uint64_t slot256 = rc_create == 0 ? l1[(USER_VA_BASE >> 30) & 0x1FF] : 0;
        /* exhaustion: leave fewer free pages than a new mapping needs (a new L3 table and the page itself in a fresh 2 MiB area) */
        uintptr_t head = 0; uint64_t held = 0;
        for (;;) { uintptr_t p = pmm_alloc_page(); if (!p) break; *(volatile uint64_t *)p = head; head = p; held++; }
        uintptr_t spare = head; head = *(volatile uint64_t *)spare; pmm_free_page(spare);     /* exactly one free page */
        int r_ex = rc_create == 0 ? aspace_map(&as, USER_VA_BASE + 0x400000, UPERM_RW, 0, 0, 0) : -99;   /* needs L3 table + page = 2 */
        uint64_t free_after_fail = free_pages();
        int atomic = r_ex < 0 && (uint64_t)as.npages == used_pages && free_after_fail == 1 && l1[(USER_VA_BASE >> 30) & 0x1FF] == slot256;
        while (head) { uintptr_t nx = *(volatile uint64_t *)head; pmm_free_page(head); head = nx; }
        (void)held;
        check("under page exhaustion aspace_map fails ATOMICALLY: no table or page was consumed, ownership and the L1 entry are unchanged", atomic && free_pages() == free_before);
        if (rc_create == 0) aspace_destroy(&as);
        check("destroying the address space frees every page it owned", free_pages() == free_before + used_pages);

        /* process creation under exhaustion: either complete success or nothing left behind */
        int clean = 1;
        for (uint64_t keep = 0; keep <= 12; keep++) {
            uintptr_t h = 0;
            for (;;) { uintptr_t p = pmm_alloc_page(); if (!p) break; *(volatile uint64_t *)p = h; h = p; }
            for (uint64_t k = 0; k < keep && h; k++) { uintptr_t nx = *(volatile uint64_t *)h; pmm_free_page(h); h = nx; }
            uint64_t free_now = free_pages();
            int pid = proc_create(41, 0, "exhaust");
            if (pid > 0) { if (proc_get(pid)->state != PROC_READY) clean = 0; proc_destroy(pid); }
            else if (proc_get(1) != 0 || free_pages() != free_now) clean = 0;       /* a failed creation must leave no process and no leaked page */
            if (free_pages() != free_now) clean = 0;
            while (h) { uintptr_t nx = *(volatile uint64_t *)h; pmm_free_page(h); h = nx; }
        }
        check("proc_create under exhaustion (0..12 free pages): each attempt either yields a complete READY process or leaves no process and leaks no page", clean);
    }
    {   /* a failed mapping on a LIVE process's address space leaves its existing mappings and its execution intact */
        destroy_all();
        int v = proc_create(1, 0, "live"); struct proc *pv = proc_get(v);
        int r1 = aspace_map(&pv->as, ~0ULL - PAGE_SIZE + 1, UPERM_RW, 0, 0, 0);
        int r2 = aspace_map(&pv->as, USER_DATA_VA, UPERM_RW, 0, 0, 0);
        uint64_t save = get_ttbr0(), pa1 = 0, pa2 = 0;
        set_ttbr0(aspace_ttbr(&pv->as));
        int t1 = vmm_hw_translate_el0(USER_DATA_VA, 1, &pa1), t2 = vmm_hw_translate_el0(USER_CODE_VA, 0, &pa2);
        set_ttbr0(save);
        uint64_t rr = sched_run(2000);
        check("a live process whose address space received rejected mapping requests keeps its mappings and still runs to completion",
              r1 < 0 && r2 < 0 && t1 == 0 && pa1 == pv->data_pa && t2 == 0 && pa2 == pv->code_pa && rr == SCHED_ALL_DONE && pv->state == PROC_TERMINATED && ud(pv, 0) == 5);
        destroy_all();
    }

    /* ---- timer preemption: processes that NEVER yield must still share the CPU, switched by real timer interrupts ---- */
    {
        struct pair_result pr; int ia, ib;
        const uint64_t t_before = timer_ticks();
        run_cpu_pair(30, 30, 2, &pr, &ia, &ib);
        const struct sched_stats *st = sched_get_stats();
        const uint64_t timer_delta = timer_ticks() - t_before;
        int done = pr.r == SCHED_ALL_DONE && pr.a && pr.b && pr.a->state == PROC_TERMINATED && pr.b->state == PROC_TERMINATED && pr.a->exit_code == 0 && pr.b->exit_code == 0;
        check("preemption: two EL0 processes that never call yield both ran to completion, sharing the CPU",
              done && ud(pr.a, 8) == 0xCCCC && ud(pr.b, 8) == 0xCCCC && ud(pr.a, 0) > 0 && ud(pr.b, 0) > 0);
        check("preemption: every switch between them was a TIMER switch (0 voluntary yields, several preemptions of each)",
              done && pr.a->yields == 0 && pr.b->yields == 0 && pr.a->preemptions >= 2 && pr.b->preemptions >= 2 && st->preemptions == pr.a->preemptions + pr.b->preemptions &&
              st->switches == pr.a->switches_in + pr.b->switches_in && st->preemptions >= 8);
        check("preemption: each timer interrupt hit the middle of user code and returned through ERET: registers x23-x28 and SP_EL0 intact in both processes after every burst",
              done && st->preempt_elr_in_user == st->preemptions && ud(pr.a, 0x10) == 0 && ud(pr.b, 0x10) == 0 && ud(pr.a, 0x28) == 0 && ud(pr.b, 0x28) == 0);
        check("preemption: TTBR0_EL1 (table + ASID) matched the running process on every exception from EL0, and the two processes use different ASIDs",
              st->ttbr_checks > 20 && st->ttbr_mismatch == 0 && pr.a->as.asid != pr.b->as.asid && aspace_ttbr(&pr.a->as) != aspace_ttbr(&pr.b->as));
        check("preemption: tick accounting is coherent (scheduler ticks == timer interrupts taken, timer deadlines never early, each process was charged ticks)",
              sched_ticks() == timer_delta && timer_hist_sane() && pr.a->ticks_run > 0 && pr.b->ticks_run > 0 && pr.a->ticks_run + pr.b->ticks_run <= sched_ticks() + 1);
        destroy_all();

        /* a fault while preempting: the healthy process keeps running and finishes */
        destroy_all(); sched_stats_reset();
        int bad = proc_create(10, (uint64_t)(uintptr_t)&kernel_secret, "bad"), good = proc_create(50, 25, "good");
        sched_set_preempt(1, 2);
        r = sched_run(5000);
        sched_set_preempt(0, 0);
        struct proc *pbad = proc_get(bad), *pgood = proc_get(good);
        check("preemption: a user fault under preemption stops only the faulting process; the healthy one is unaffected and finishes",
              r == SCHED_ALL_DONE && pbad->state == PROC_FAULTED && ((pbad->fault_esr >> 26) & 0x3F) == 0x24 && ud(pbad, 0x40) == 0 && pgood->state == PROC_TERMINATED && ud(pgood, 8) == 0xCCCC && ud(pgood, 0x10) == 0);
        destroy_all();

        /* the survivor continues alone */
        run_cpu_pair(45, 8, 2, &pr, &ia, &ib);
        check("preemption: when one process exits, the single remaining runnable process continues alone and completes",
              pr.r == SCHED_ALL_DONE && pr.a->state == PROC_TERMINATED && pr.b->state == PROC_TERMINATED && ud(pr.a, 8) == 0xCCCC && pr.b->switches_in >= 1 && pr.a->preemptions >= 1 && pr.a->ticks_run > pr.b->ticks_run);
        destroy_all();

        /* the watchdog still works with preemption on, and both spinners made progress before it fired */
        destroy_all(); sched_stats_reset();
        int s1 = proc_create(31, 0, "spin1"), s2 = proc_create(31, 0, "spin2");
        sched_set_preempt(1, 2);
        r = sched_run(40);
        sched_set_preempt(0, 0);
        struct proc *q1 = proc_get(s1), *q2 = proc_get(s2);
        check("preemption: with two never-yielding loops the watchdog still returns control (SCHED_TIMEOUT); both loops made progress, both are READY",
              r == SCHED_TIMEOUT && q1->state == PROC_READY && q2->state == PROC_READY && ud(q1, 0) > 0 && ud(q2, 0) > 0 && sched_get_stats()->timeouts == 1 && !sched_active() && get_ttbr0() == vmm_kernel_l1_pa());
        destroy_all();

        /* invalid scheduling states fail safely */
        r = sched_run(100);
        int none_ok = r == SCHED_ALL_DONE && !sched_active();
        int f1 = proc_create(10, (uint64_t)(uintptr_t)&kernel_secret, "f"); struct proc *pf = proc_get(f1);
        sched_run(2000);
        uint64_t sw = pf->switches_in; int was_faulted = pf->state == PROC_FAULTED;
        r = sched_run(2000);
        check("scheduler safety: running with no processes returns at once; a FAULTED/TERMINATED process is never scheduled again; the kernel context is always restored",
              none_ok && was_faulted && r == SCHED_ALL_DONE && pf->state == PROC_FAULTED && pf->switches_in == sw && get_ttbr0() == vmm_kernel_l1_pa());
        destroy_all();

        /* quantum lengths */
        run_cpu_pair(40, 40, 1, &pr, &ia, &ib);
        uint64_t p_q1 = sched_get_stats()->preemptions; int ok1 = pr.r == SCHED_ALL_DONE && ud(pr.a, 0x10) == 0 && ud(pr.b, 0x10) == 0 && ud(pr.a, 8) == 0xCCCC && ud(pr.b, 8) == 0xCCCC;
        run_cpu_pair(40, 40, 10, &pr, &ia, &ib);
        uint64_t p_q10 = sched_get_stats()->preemptions; int ok10 = pr.r == SCHED_ALL_DONE && ud(pr.a, 0x10) == 0 && ud(pr.b, 0x10) == 0 && ud(pr.a, 8) == 0xCCCC && ud(pr.b, 8) == 0xCCCC;
        run_cpu_pair(40, 40, 3, &pr, &ia, &ib);
        uint64_t p_q3 = sched_get_stats()->preemptions; int ok3 = pr.r == SCHED_ALL_DONE && ud(pr.a, 0x10) == 0 && ud(pr.b, 0x10) == 0;
        check("preemption: quantum lengths 1, 3 and 10 ticks all complete correctly, and a shorter quantum switches proportionally more often (q1 > q3 > q10)",
              ok1 && ok3 && ok10 && p_q1 > p_q3 && p_q3 > p_q10 && p_q10 >= 2);
        destroy_all();

        /* sustained stress: many switches, nothing leaks, nothing corrupts */
        uint64_t base_free = free_pages(), total_pre = 0; int stress_ok = 1;
        for (int i = 0; i < 12; i++) {
            run_cpu_pair(6, 6, 1, &pr, &ia, &ib);
            total_pre += sched_get_stats()->preemptions;
            if (pr.r != SCHED_ALL_DONE || ud(pr.a, 0x10) || ud(pr.b, 0x10) || ud(pr.a, 0x28) || ud(pr.b, 0x28) || sched_get_stats()->ttbr_mismatch) stress_ok = 0;
            destroy_all();
            if (free_pages() != base_free) stress_ok = 0;
        }
        check("preemption stress: 12 rounds of create/run/destroy with a 1-tick quantum: many timer switches, no register or stack corruption, no TTBR mismatch, no leaked page",
              stress_ok && total_pre >= 40);
        sched_set_preempt(0, 0);
    }

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
