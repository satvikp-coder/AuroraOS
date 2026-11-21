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
static void udw(struct proc *p, uint64_t off, const void *src, size_t n) __attribute__((unused));
static void udw(struct proc *p, uint64_t off, const void *src, size_t n) { volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)(p->data_pa + off); const uint8_t *q = (const uint8_t *)src; for (size_t i = 0; i < n; i++) d[i] = q[i]; }
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

#ifdef AURORA_SELFTEST_HOOKS
        /* ---- FP/SIMD: every process has its own V0-V31, FPCR and FPSR, preserved across timer preemption and never visible to another process ---- */
        {
            destroy_all(); sched_stats_reset();
            int f1 = proc_create(22, 0, "fp-insn");
            uint64_t r1 = sched_run(2000);
            struct proc *pf1 = proc_get(f1);
            check("FP/SIMD: an EL0 floating-point instruction now executes (no trap) and the process exits 0",
                  r1 == SCHED_ALL_DONE && pf1 && pf1->state == PROC_TERMINATED && pf1->exit_code == 0);
            destroy_all(); sched_stats_reset();
            int fa = proc_create_image(80, 30, "fp-a", user_fp_blob_start, (size_t)(user_fp_blob_end - user_fp_blob_start)), fb = proc_create_image(80, 30, "fp-b", user_fp_blob_start, (size_t)(user_fp_blob_end - user_fp_blob_start)), fc = proc_create_image(80, 30, "fp-c", user_fp_blob_start, (size_t)(user_fp_blob_end - user_fp_blob_start));
            sched_set_preempt(1, 2);
            uint64_t rr = sched_run(5000);
            sched_set_preempt(0, 0);
            struct proc *qa = proc_get(fa), *qb = proc_get(fb), *qc = proc_get(fc);
            int ok3 = rr == SCHED_ALL_DONE && qa && qb && qc && qa->state == PROC_TERMINATED && qb->state == PROC_TERMINATED && qc->state == PROC_TERMINATED &&
                      qa->exit_code == 0 && qb->exit_code == 0 && qc->exit_code == 0 && ud(qa, 8) == 0xDDDD && ud(qb, 8) == 0xDDDD && ud(qc, 8) == 0xDDDD;
            check("FP/SIMD: three processes that never yield, each with its own pattern in V0-V31 (and FPCR/FPSR), all ran to completion", ok3 && ud(qa, 0) > 0 && ud(qb, 0) > 0 && ud(qc, 0) > 0);
            check("FP/SIMD: they were switched by the timer several times each, and every one of their 64 doublewords, FPCR and FPSR was intact after every pass (0 mismatches)",
                  ok3 && qa->preemptions >= 2 && qb->preemptions >= 2 && qc->preemptions >= 2 && ud(qa, 0x10) == 0 && ud(qb, 0x10) == 0 && ud(qc, 0x10) == 0 &&
                  ud(qa, 0x28) == 0 && ud(qb, 0x28) == 0 && ud(qc, 0x28) == 0);
            destroy_all(); sched_stats_reset();
            int pz = proc_create_image(81, 0, "fp-zero", user_fp_blob_start, (size_t)(user_fp_blob_end - user_fp_blob_start));          /* the slot of an earlier FP process; the CPU registers still hold that process's values */
            uint64_t rz = sched_run(2000);
            struct proc *qz = proc_get(pz);
            check("FP/SIMD: a NEW process (same slot as an earlier one) starts with all-zero V0-V31, FPCR and FPSR: nothing leaks from a previous process",
                  rz == SCHED_ALL_DONE && qz && qz->state == PROC_TERMINATED && ud(qz, 8) == 0xEEEE && ud(qz, 0x10) == 0);
            destroy_all(); sched_stats_reset();
            int pd = proc_create_image(82, 0, "fp-dirty", user_fp_blob_start, (size_t)(user_fp_blob_end - user_fp_blob_start));
            uint64_t rd = sched_run(2000);
            struct proc *qd = proc_get(pd);
            int faulted = rd == SCHED_ALL_DONE && qd && qd->state == PROC_FAULTED && ((qd->fault_esr >> 26) & 0x3F) == 0x24;
            destroy_all(); sched_stats_reset();
            int pz2 = proc_create_image(81, 0, "fp-zero", user_fp_blob_start, (size_t)(user_fp_blob_end - user_fp_blob_start));
            uint64_t rz2 = sched_run(2000);
            struct proc *qz2 = proc_get(pz2);
            check("FP/SIMD: a process that dirtied every register and then FAULTED leaves nothing behind either: the next process in its slot sees zeros",
                  faulted && rz2 == SCHED_ALL_DONE && qz2 && ud(qz2, 8) == 0xEEEE && ud(qz2, 0x10) == 0);
            destroy_all(); sched_stats_reset();
            int dx = proc_create_image(82, 0, "fp-dirty", user_fp_blob_start, (size_t)(user_fp_blob_end - user_fp_blob_start)), px2 = proc_create_image(81, 0, "fp-zero", user_fp_blob_start, (size_t)(user_fp_blob_end - user_fp_blob_start));        /* a live dirty process and a probe at the same time */
            uint64_t rx = sched_run(2000);
            struct proc *qdx = proc_get(dx), *qpx = proc_get(px2);
            check("FP/SIMD: a probe that runs right after a faulting process (no destroy in between) also sees zeros",
                  rx == SCHED_ALL_DONE && qdx && qdx->state == PROC_FAULTED && qpx && qpx->state == PROC_TERMINATED && ud(qpx, 8) == 0xEEEE && ud(qpx, 0x10) == 0);
            destroy_all();
        }
#endif

#ifdef AURORA_SELFTEST_HOOKS
        /* ---- NEON matrix-vector product computed by a native EL0 process (the guest does the arithmetic; the kernel's C reference below is also guest code) ---- */
        {
            #define FPIMG user_fp_blob_start, (size_t)(user_fp_blob_end - user_fp_blob_start)
            static int16_t MX[5][8][16], VX[5][16];
            uint64_t lcg = 0x2545F4914F6CDD1DULL;
            for (int r = 0; r < 8; r++) for (int c = 0; c < 16; c++) { MX[1][r][c] = (r == c) ? 1 : 0; MX[2][r][c] = (int16_t)-((r * 16 + c + 1) * ((c & 1) ? -1 : 1)); }
            for (int c = 0; c < 16; c++) {
                VX[0][c] = (int16_t)(c + 1);
                VX[1][c] = (int16_t)((c & 1 ? -1 : 1) * (int16_t)(3 + 2 * c * 3 % 17 + c));
                VX[2][c] = (int16_t)-(c + 2);
                VX[4][c] = c < 2 ? 32767 : 1;
            }
            for (int r = 0; r < 8; r++) for (int c = 0; c < 16; c++) { lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL; MX[3][r][c] = (int16_t)((int)((lcg >> 33) % 201) - 100); }
            for (int c = 0; c < 16; c++) { lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL; VX[3][c] = (int16_t)((int)((lcg >> 33) % 201) - 100); }
            for (int c = 0; c < 16; c++) {                                      /* boundary values chosen so that no 32-bit sum overflows (asserted below) */
                MX[4][0][c] = c < 2 ? 32767 : 0;                                /* 2 x 32767 x 32767 = 2,147,352,578 */
                MX[4][1][c] = c >= 2 ? -32768 : 0;
                MX[4][2][c] = c == 0 ? 32767 : (c == 1 ? -32767 : 32767);
                MX[4][3][c] = c == 0 ? -32768 : 0;
                MX[4][4][c] = c == 0 ? 32767 : (c == 1 ? -32768 : 0);
                MX[4][5][c] = 0; MX[4][6][c] = (int16_t)(c & 1 ? -1 : 1); MX[4][7][c] = -32768 * 0 + (c == 15 ? 32767 : 0);
            }
            static const char *kind[5] = { "zero matrix", "identity", "negative inputs", "dense pseudo-random", "boundary values (+32767, -32768; products up to 2^30)" };
            int ref_fits = 1; int32_t REF[5][8];
            for (int k = 0; k < 5; k++) for (int r = 0; r < 8; r++) { int64_t a = 0; for (int c = 0; c < 16; c++) a += (int64_t)MX[k][r][c] * VX[k][c]; if (a != (int64_t)(int32_t)a) ref_fits = 0; REF[k][r] = (int32_t)a; }
            check("NEON: the five test matrices are constructed so that no row sum overflows 32 bits (the reference is exact, no wrapping to excuse)", ref_fits);

            for (int k = 0; k < 5; k++) {
                destroy_all(); sched_stats_reset();
                int mp = proc_create_image(83, 20, "matvec", FPIMG);
                struct proc *pm = proc_get(mp);
                if (pm) { udw(pm, 0x100, MX[k], sizeof MX[k]); udw(pm, 0x200, VX[k], sizeof VX[k]); }
                uint64_t rm = sched_run(5000);
                int exact_n = 1, exact_s = 1;
                for (int r = 0; pm && r < 8; r++) {
                    if ((int32_t)(*(volatile uint32_t *)(uintptr_t)(pm->data_pa + 0x300 + 4 * r)) != REF[k][r]) exact_n = 0;
                    if ((int32_t)(*(volatile uint32_t *)(uintptr_t)(pm->data_pa + 0x340 + 4 * r)) != REF[k][r]) exact_s = 0;
                }
                char msg[160]; int ml = 0; const char *pre = "NEON: an EL0 matrix-vector product (8x16 int16 -> int32), "; while (pre[ml]) { msg[ml] = pre[ml]; ml++; }
                for (int j = 0; kind[k][j] && ml < 110; j++) msg[ml++] = kind[k][j];
                const char *suf = ": NEON == scalar == the kernel reference, exactly, over 20 repetitions (0 mismatches)";
                for (int j = 0; suf[j] && ml < 159; j++) msg[ml++] = suf[j];
                msg[ml] = 0;
                check(msg, rm == SCHED_ALL_DONE && pm && pm->state == PROC_TERMINATED && ud(pm, 8) == 0xAAAA && ud(pm, 0x18) == 20 && ud(pm, 0x10) == 0 && exact_n && exact_s);
            }

            /* two NEON processes at once under timer preemption, plus a third that FAULTS with live vector registers */
            destroy_all(); sched_stats_reset();
            int ma = proc_create_image(83, 30000, "mv-a", FPIMG), mb = proc_create_image(83, 30000, "mv-b", FPIMG), mc = proc_create_image(83, (1ULL << 32) | 30, "mv-fault", FPIMG);
            struct proc *pa2 = proc_get(ma), *pb2 = proc_get(mb), *pc2 = proc_get(mc);
            if (pa2) { udw(pa2, 0x100, MX[3], sizeof MX[3]); udw(pa2, 0x200, VX[3], sizeof VX[3]); }
            if (pb2) { udw(pb2, 0x100, MX[4], sizeof MX[4]); udw(pb2, 0x200, VX[4], sizeof VX[4]); }
            if (pc2) { udw(pc2, 0x100, MX[2], sizeof MX[2]); udw(pc2, 0x200, VX[2], sizeof VX[2]); }
            sched_set_preempt(1, 2);
            uint64_t rc3 = sched_run(200000);
            sched_set_preempt(0, 0);
            int ex_a = 1, ex_b = 1;
            for (int r = 0; pa2 && pb2 && r < 8; r++) {
                if ((int32_t)(*(volatile uint32_t *)(uintptr_t)(pa2->data_pa + 0x300 + 4 * r)) != REF[3][r]) ex_a = 0;
                if ((int32_t)(*(volatile uint32_t *)(uintptr_t)(pb2->data_pa + 0x300 + 4 * r)) != REF[4][r]) ex_b = 0;
            }
            console_puts("[info] neon preemptions a="); console_dec(pa2 ? pa2->preemptions : 0); console_puts(" b="); console_dec(pb2 ? pb2->preemptions : 0); console_puts(" reps a="); console_dec(pa2 ? ud(pa2, 0x18) : 0); console_puts(" b="); console_dec(pb2 ? ud(pb2, 0x18) : 0); console_puts(" mism a="); console_dec(pa2 ? ud(pa2, 0x10) : 99); console_puts(" b="); console_dec(pb2 ? ud(pb2, 0x10) : 99); console_puts(" ex="); console_dec((uint64_t)(ex_a * 2 + ex_b)); console_puts("\n");
            check("NEON: two processes computing 30000 products each at the same time, switched by the timer several times each, kept every NEON result exact (0 mismatches, results equal their own reference): vector registers are preserved across preemption",
                  rc3 == SCHED_ALL_DONE && pa2 && pb2 && pa2->state == PROC_TERMINATED && pb2->state == PROC_TERMINATED && ud(pa2, 0x18) == 30000 && ud(pb2, 0x18) == 30000 && ud(pa2, 0x10) == 0 && ud(pb2, 0x10) == 0 &&
                  pa2->preemptions >= 2 && pb2->preemptions >= 2 && ex_a && ex_b);
            check("NEON: a third process that FAULTED (unmapped read) right after its NEON work, while the other two were still computing, is FAULTED and did not disturb their results",
                  pc2 && pc2->state == PROC_FAULTED && ((pc2->fault_esr >> 26) & 0x3F) == 0x24 && ud(pc2, 0x18) == 30 && ud(pc2, 8) == 0xAAAA && ud(pc2, 0x40) == 0 && ex_a && ex_b);
            destroy_all(); sched_stats_reset();
            int pzn = proc_create_image(81, 0, "fp-zero", FPIMG);
            uint64_t rzn = sched_run(2000);
            struct proc *qzn = proc_get(pzn);
            check("NEON: a NEW process in a slot used by NEON processes starts with all-zero vector registers, FPCR and FPSR: no matrix data leaks between processes",
                  rzn == SCHED_ALL_DONE && qzn && qzn->state == PROC_TERMINATED && ud(qzn, 8) == 0xEEEE && ud(qzn, 0x10) == 0);
            destroy_all(); sched_stats_reset();
            #undef FPIMG
        }
#endif

#ifdef AURORA_SELFTEST_HOOKS
        /* ---- SYS_MEM: the bounded user heap (kernel/proc/memsys.c) ---- */
        {
            #define MEMIMG user_mem_blob_start, (size_t)(user_mem_blob_end - user_mem_blob_start)
            destroy_all(); sched_stats_reset();
            const uint64_t free_before_heap = free_pages();
            int hp = proc_create_image(90, 0, "memprobe", MEMIMG);
            uint64_t rh = sched_run(2000);
            struct proc *ph = proc_get(hp);
            check("heap: the boundary probe ran to its deliberate final fault (a page it had shrunk away): translation fault at the heap start, nothing else went wrong",
                  rh == SCHED_ALL_DONE && ph && ph->state == PROC_FAULTED && ((ph->fault_esr >> 26) & 0x3F) == 0x24 && (ph->fault_esr & 0x3C) == 0x04 && ph->fault_far == USER_HEAP_VA && ud(ph, 8) == 0x9999);
            check("heap: every bad request is refused with the right error and changes nothing: grow(0), grow(2^63) and grow(33) are EINVAL, an unknown op is EINVAL, shrink on an empty heap is EINVAL",
                  ph && ud(ph, 0x10) == (((uint64_t)USER_HEAP_MAX_PAGES << 32) | 0) && (int64_t)ud(ph, 0x18) == AE_EINVAL && (int64_t)ud(ph, 0x20) == AE_EINVAL && (int64_t)ud(ph, 0x28) == AE_EINVAL &&
                  (int64_t)ud(ph, 0x30) == AE_EINVAL && (int64_t)ud(ph, 0x38) == AE_EINVAL);
            check("heap: grow(1) maps one ZERO page that the process can write and read back; growing to the full 32-page budget works and its last doubleword is writable",
                  ph && ud(ph, 0x40) == 1 && ud(ph, 0x48) == 0 && ud(ph, 0x50) == 0xA5A5C3C35A5A3C3CULL && ud(ph, 0x58) == 32 && ud(ph, 0x60) == 0xA5A5C3C35A5A3C3CULL);
            check("heap: one page over the budget is ENOMEM, shrinking by more than it holds is EINVAL, and shrinking all 32 pages leaves an empty heap",
                  ph && (int64_t)ud(ph, 0x68) == AE_ENOMEM && (int64_t)ud(ph, 0x70) == AE_EINVAL && ud(ph, 0x78) == 0 && ph->heap_pages == 0);
            destroy_all();
            check("heap: after the process is destroyed every page it ever held (tables, fixed pages, heap) is back in the allocator", free_pages() == free_before_heap);

            destroy_all(); sched_stats_reset();
            int hx = proc_create_image(91, 0, "heapexec", MEMIMG);
            uint64_t rx = sched_run(2000);
            struct proc *phx = proc_get(hx);
            check("heap: branching into a heap page is an instruction abort with a PERMISSION fault: the heap is never executable (W^X holds for grown memory)",
                  rx == SCHED_ALL_DONE && phx && phx->state == PROC_FAULTED && ((phx->fault_esr >> 26) & 0x3F) == 0x20 && (phx->fault_esr & 0x3C) == 0x0C && phx->fault_far == USER_HEAP_VA && ud(phx, 0x40) == 0);
            destroy_all(); sched_stats_reset();

            const uint64_t free_before_pat = free_pages();
            int h1 = proc_create_image(92, 25, "heap-a", MEMIMG), h2 = proc_create_image(92, 25, "heap-b", MEMIMG);
            sched_set_preempt(1, 2);
            uint64_t rp = sched_run(5000);
            sched_set_preempt(0, 0);
            struct proc *pq1 = proc_get(h1), *pq2 = proc_get(h2);
            check("heap: two processes that never yield, each with 8 heap pages filled with its own pid-derived pattern, kept every one of 4096 doublewords across timer preemption (0 mismatches, several preemptions, own pages)",
                  rp == SCHED_ALL_DONE && pq1 && pq2 && pq1->state == PROC_TERMINATED && pq2->state == PROC_TERMINATED && ud(pq1, 8) == 0xDDDD && ud(pq2, 8) == 0xDDDD && ud(pq1, 0x10) == 0 && ud(pq2, 0x10) == 0 &&
                  pq1->preemptions >= 2 && pq2->preemptions >= 2 && pq1->heap_pages == 8 && pq2->heap_pages == 8 && pq1->as.l1_pa != pq2->as.l1_pa);
            destroy_all();
            check("heap: processes that exit WITHOUT shrinking leak nothing (exit reclaims the heap with the address space)", free_pages() == free_before_pat);

            /* memory pressure: the kernel's reserve (256 pages) is never given to a heap, and the check is exact at the boundary */
            destroy_all(); sched_stats_reset();
            int e1 = proc_create_image(93, 0, "memtry", MEMIMG);
            uintptr_t held = 0; uint64_t nheld = 0;
            for (;;) { uintptr_t pg = pmm_alloc_page(); if (!pg) break; *(volatile uint64_t *)pg = held; held = pg; nheld++; }
            for (int i = 0; i < 256; i++) { uintptr_t nx = *(volatile uint64_t *)held; pmm_free_page(held); held = nx; nheld--; }      /* exactly 256 pages free: the whole reserve and nothing more */
            uint64_t re = sched_run(2000);
            struct proc *pe1 = proc_get(e1);
            check("heap: with only the 256-page reserve left, grow(1) is refused (ENOMEM) and the heap stays empty",
                  re == SCHED_ALL_DONE && pe1 && pe1->state == PROC_TERMINATED && (int64_t)ud(pe1, 0x10) == AE_ENOMEM && ud(pe1, 0x18) == (((uint64_t)USER_HEAP_MAX_PAGES << 32) | 0) && pe1->heap_pages == 0);
            destroy_all();                                                                       /* gives back the process's pages */
            int e2 = proc_create_image(93, 0, "memtry", MEMIMG);                                 /* takes some pages again */
            while (free_pages() > 257) { uintptr_t pg = pmm_alloc_page(); if (!pg) break; *(volatile uint64_t *)pg = held; held = pg; nheld++; }
            while (free_pages() < 257 && held) { uintptr_t nx = *(volatile uint64_t *)held; pmm_free_page(held); held = nx; nheld--; }
            uint64_t fp257 = free_pages();
            uint64_t re2 = sched_run(2000);
            struct proc *pe2 = proc_get(e2);
            check("heap: with exactly reserve + 1 pages free, grow(1) succeeds: the boundary is exact (256 refused, 257 granted)",
                  fp257 == 257 && re2 == SCHED_ALL_DONE && pe2 && pe2->state == PROC_TERMINATED && ud(pe2, 0x10) == 1);
            destroy_all();
            while (held) { uintptr_t nx = *(volatile uint64_t *)held; pmm_free_page(held); held = nx; nheld--; }
            (void)nheld;
            check("heap: after the pressure test every page is back (no leak, nothing double-freed)", free_pages() == free_before_heap);
            /* ---- guard pages: the heap is bracketed by unmapped space that aspace_map never fills, and EL0 faults on every side of it (kernel/include/uvlayout.h) ---- */
            {
                struct aspace ga;
                const uint64_t free_before_ga = free_pages();
                int gcr = aspace_create(&ga, 201);
                const uint64_t guards[] = { USER_VA_BASE + 0x2000, USER_VA_BASE + 0x5000, USER_VA_BASE + 0xF000, ELF_VA_MAX, USER_HEAP_VA - PAGE_SIZE, USER_HEAP_VA - USER_HEAP_GUARD_BYTES,
                                            USER_HEAP_END, USER_HEAP_END + USER_HEAP_GUARD_BYTES - PAGE_SIZE, USER_VA_BASE + 0x1FF000 };
                const int npg0 = gcr == 0 ? ga.npages : -1; const uint64_t fr0 = free_pages();
                int all_refused = gcr == 0;
                for (unsigned i = 0; gcr == 0 && i < sizeof guards / sizeof *guards; i++) if (aspace_map(&ga, guards[i], UPERM_RW, 0, 0, 0) != -7) all_refused = 0;
                check("guards: aspace_map refuses every guard page (between data and stack, above the stack, below and above the ELF window, both sides of the heap, the end of the table) with -7, and changes nothing",
                      all_refused && ga.npages == npg0 && free_pages() == fr0);
                int ok_maps = gcr == 0 && aspace_map(&ga, USER_HEAP_VA, UPERM_RW, 0, 0, 0) == 0 && aspace_map(&ga, USER_HEAP_END - PAGE_SIZE, UPERM_RW, 0, 0, 0) == 0 &&
                              aspace_map(&ga, ELF_VA_MIN, UPERM_RW, 0, 0, 0) == 0 && aspace_map(&ga, ELF_VA_MAX - PAGE_SIZE, UPERM_RW, 0, 0, 0) == 0;
                check("guards: the first and last heap page and the first and last ELF-window page are still mappable (the guards do not eat the regions they protect)", ok_maps);
                if (gcr == 0) aspace_destroy(&ga);
                check("guards: destroying that address space returns every page", free_pages() == free_before_ga);
            }
            {
                static const struct { uint64_t off_is_minus; uint64_t off; uint32_t pages; int ec; int store; const char *what; } gp[6] = {
                    { 1, 8,        1,  0x24, 0, "the last word of the guard just BELOW the heap (a load)" },
                    { 0, 0x1000,   1,  0x24, 0, "the page just ABOVE a 1-page heap (a load)" },
                    { 0, 0x20000,  32, 0x24, 1, "the first page past a FULL 32-page heap (a store)" },
                    { 0, 0x2FFF8,  32, 0x24, 0, "the last word of the guard above a full heap (a load)" },
                    { 1, 0x10000,  1,  0x24, 0, "the first page of the guard below the heap (a load)" },
                    { 1, 0x1000,   1,  0x20, 0, "an instruction fetch from the guard just below the heap (a branch)" } };
                const uint64_t free_before_gp = free_pages();
                int last_gpid = 0;
                for (int k = 0; k < 6; k++) {
                    destroy_all(); sched_stats_reset();
                    int gpid = proc_create_image(94, (uint64_t)k, "guard", MEMIMG);
                    uint64_t rg = sched_run(2000);
                    struct proc *pg = proc_get(gpid);
                    uint64_t want = gp[k].off_is_minus ? USER_HEAP_VA - gp[k].off : USER_HEAP_VA + gp[k].off;
                    char msg[200]; int ml = 0; const char *pre = "guards: EL0 touching "; while (pre[ml]) { msg[ml] = pre[ml]; ml++; }
                    for (int j = 0; gp[k].what[j] && ml < 150; j++) msg[ml++] = gp[k].what[j];
                    const char *suf = " is a translation fault at exactly that address; the process is FAULTED, did nothing after, and its heap pages are intact";
                    for (int j = 0; suf[j] && ml < 199; j++) msg[ml++] = suf[j];
                    msg[ml] = 0;
                    check(msg, rg == SCHED_ALL_DONE && pg && pg->state == PROC_FAULTED && ((pg->fault_esr >> 26) & 0x3F) == (uint32_t)gp[k].ec && (pg->fault_esr & 0x3C) == 0x04 &&
                               (gp[k].ec != 0x24 || (((pg->fault_esr >> 6) & 1) == (uint32_t)gp[k].store)) && pg->fault_far == want && ud(pg, 8) == 0x9999 && ud(pg, 0x40) == 0 && pg->heap_pages == gp[k].pages);
                    last_gpid = gpid;
                }
                destroy_all();
                check("guards: after six processes faulted WITH a heap allocated every page they held is back (no leak on a fault)", free_pages() == free_before_gp);

                /* pid reuse: the next process in a faulted process's slot starts with an empty heap and its own budget */
                int rp = proc_create_image(93, 0, "memtry", MEMIMG);
                uint64_t rr = sched_run(2000);
                struct proc *pr = proc_get(rp);
                check("guards: a new process that reuses the slot of a faulted one starts with an EMPTY heap (grow(1) gives 1 page, info says 1) and no stale mapping",
                      rp == last_gpid && rr == SCHED_ALL_DONE && pr && pr->state == PROC_TERMINATED && ud(pr, 0x10) == 1 && (ud(pr, 0x18) & 0xFFFFFFFFULL) == 1 && pr->heap_pages == 1);
                destroy_all(); sched_stats_reset();

                /* repeated grow/shrink: a regrown heap is all zero (no stale data from a freed page), eight times over the full budget */
                const uint64_t free_before_cy = free_pages();
                int cy = proc_create_image(95, 0, "heapcycle", MEMIMG);
                uint64_t rc2 = sched_run(8000);
                struct proc *pcy = proc_get(cy);
                check("guards: eight grow(32)/fill/shrink(32) cycles: every regrown heap is entirely ZERO (0 stale doublewords of 16384 x 8), every grow and shrink returned its exact value, and the heap ends empty",
                      rc2 == SCHED_ALL_DONE && pcy && pcy->state == PROC_TERMINATED && ud(pcy, 8) == 0xCCCC && ud(pcy, 0x10) == 0 && ud(pcy, 0x18) == 0 && pcy->heap_pages == 0);
                destroy_all();
                check("guards: and the cycles leaked nothing", free_pages() == free_before_cy);
            }
            /* ---- per-PRINCIPAL heap budget: processes of one kernel-chosen principal share 64 pages; usage is derived from live processes, never a stored counter ---- */
            {
                extern int mem_test_fail_page;
                #define GROWRES(pr) ((int64_t)ud(proc_get(pr), 0x10))
                #define HEAPOF(pr)  (ud(proc_get(pr), 0x18) & 0xFFFFFFFFULL)
                destroy_all(); sched_stats_reset();
                const uint64_t free_before_pb = free_pages();
                int pa = proc_create_image(96, 32, "pb-a", MEMIMG), pb = proc_create_image(96, 32, "pb-b", MEMIMG);
                if (pa > 0) proc_set_principal(pa, 150);
                if (pb > 0) proc_set_principal(pb, 150);
                uint64_t r1 = sched_run(3000);
                check("principal budget: two processes of ONE principal each grow 32 pages (the principal's full 64): the budget is per principal, shared by its processes",
                      pa > 0 && pb > 0 && r1 == SCHED_ALL_DONE && GROWRES(pa) == 32 && GROWRES(pb) == 32);
                int pc = proc_create_image(96, 1, "pb-c", MEMIMG), pd = proc_create_image(96, 32, "pb-d", MEMIMG);
                if (pc > 0) proc_set_principal(pc, 150);
                if (pd > 0) proc_set_principal(pd, 151);
                uint64_t r2 = sched_run(3000);
                check("principal budget: a THIRD process of that principal is refused even 1 page (ENOMEM, heap stays empty) because starting more instances does not buy more memory; a process of ANOTHER principal still gets its 32",
                      pc > 0 && pd > 0 && r2 == SCHED_ALL_DONE && GROWRES(pc) == AE_ENOMEM && proc_get(pc)->heap_pages == 0 && GROWRES(pd) == 32);
                proc_destroy(pb);                                                                    /* only pb's pages and charge go */
                int pe = proc_create_image(96, 32, "pb-e", MEMIMG);
                if (pe > 0) proc_set_principal(pe, 150);
                uint64_t r3 = sched_run(3000);
                check("principal budget: destroying ONE process releases only its own charge: the principal's next process gets its 32 pages at once, and the surviving sibling still holds exactly its 32",
                      pe > 0 && r3 == SCHED_ALL_DONE && GROWRES(pe) == 32 && proc_get(pa)->heap_pages == 32);
                check("principal budget: the new process reused the destroyed process's slot and started with an empty heap (grow 32 from nothing): no stale charge or mapping from the previous occupant", pe == pb && HEAPOF(pe) == 32);
                int pf = proc_create_image(96, 1, "pb-f", MEMIMG);
                if (pf > 0) proc_set_principal(pf, 150);
                uint64_t r4 = sched_run(3000);
                check("principal budget: with the sibling and the replacement holding 64 again, one more page is refused (the charge came back exactly)", pf > 0 && r4 == SCHED_ALL_DONE && GROWRES(pf) == AE_ENOMEM);

                destroy_all(); sched_stats_reset();
                int pg = proc_create_image(96, 32, "np-g", MEMIMG), ph = proc_create_image(96, 32, "np-h", MEMIMG), pi = proc_create_image(96, 1, "np-i", MEMIMG);
                uint64_t r5 = sched_run(3000);
                check("principal budget: processes with NO principal share ONE budget: two take 32 each, a third is refused (having no identity is not a way around the limit)",
                      pg > 0 && ph > 0 && pi > 0 && r5 == SCHED_ALL_DONE && GROWRES(pg) == 32 && GROWRES(ph) == 32 && GROWRES(pi) == AE_ENOMEM);

                /* a grow that fails PART-WAY must undo itself completely and leave no charge */
                destroy_all(); sched_stats_reset();
                int pj = proc_create_image(96, 8, "pf-j", MEMIMG);
                if (pj > 0) proc_set_principal(pj, 152);
                const uint64_t free_before_inj = free_pages();
                mem_test_fail_page = 5;                                                              /* the 6th page of the grow fails after 5 were mapped */
                uint64_t r6 = sched_run(3000);
                mem_test_fail_page = -1;
                check("principal budget: a grow of 8 pages that fails at the 6th page returns ENOMEM, unmaps the 5 it had mapped, leaves the heap empty and every physical page where it was",
                      pj > 0 && r6 == SCHED_ALL_DONE && GROWRES(pj) == AE_ENOMEM && proc_get(pj)->heap_pages == 0 && free_pages() == free_before_inj);
                int pk = proc_create_image(96, 32, "pf-k", MEMIMG), pl = proc_create_image(96, 32, "pf-l", MEMIMG);
                if (pk > 0) proc_set_principal(pk, 152);
                if (pl > 0) proc_set_principal(pl, 152);
                uint64_t r7 = sched_run(3000);
                check("principal budget: after that failed partial grow the principal has no residual charge: two of its processes still get their full 32 + 32", pk > 0 && pl > 0 && r7 == SCHED_ALL_DONE && GROWRES(pk) == 32 && GROWRES(pl) == 32);

                /* a faulted process still owns its pages (and so its charge) until it is destroyed */
                destroy_all(); sched_stats_reset();
                int pq = proc_create_image(94, 1, "pq-fault", MEMIMG);                               /* probe 1: grows 1 page, then faults */
                int pm = proc_create_image(96, 32, "pq-m", MEMIMG);
                if (pq > 0) proc_set_principal(pq, 153);
                if (pm > 0) proc_set_principal(pm, 153);
                uint64_t r8 = sched_run(3000);
                int pn = proc_create_image(96, 32, "pq-n", MEMIMG);
                if (pn > 0) proc_set_principal(pn, 153);
                uint64_t r9 = sched_run(3000);
                check("principal budget: a FAULTED process keeps its 1 page and its charge: with 33 pages held, a sibling asking for 32 more (65) is refused",
                      pq > 0 && pm > 0 && pn > 0 && r8 == SCHED_ALL_DONE && r9 == SCHED_ALL_DONE && proc_get(pq)->state == PROC_FAULTED && proc_get(pq)->heap_pages == 1 && GROWRES(pm) == 32 && GROWRES(pn) == AE_ENOMEM);
                proc_destroy(pq);
                int po = proc_create_image(96, 32, "pq-o", MEMIMG);
                if (po > 0) proc_set_principal(po, 153);
                uint64_t r10 = sched_run(3000);
                check("principal budget: once the faulted process is destroyed its charge is gone: the same request now succeeds (32 + 32 = 64)", po > 0 && r10 == SCHED_ALL_DONE && GROWRES(po) == 32);
                destroy_all();
                check("principal budget: after every scenario each page is back in the allocator (no leak, nothing double-freed)", free_pages() == free_before_pb);
                #undef GROWRES
                #undef HEAPOF
            }
            #undef MEMIMG
        }
#endif

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

    /* ---- bounded IPC: mailboxes, blocking receive, wake-up, authorisation, cleanup ---- */
    {
        check("the user program image still fits its single code page", (size_t)(user_blob_end - user_blob_start) <= PAGE_SIZE);

        /* blocking receiver created FIRST: it must park as BLOCKED (not spin), then be woken by the sender */
        destroy_all();
        int rb = proc_create(61, 3, "recv"), sa = proc_create(60, 1, "send");
        proc_ipc_allow(sa, rb);
        r = sched_run(2000);
        struct proc *R = proc_get(rb), *S = proc_get(sa);
        check("ipc: a blocking receiver parks as BLOCKED, is woken by the send, and receives all three messages in order with the exact bytes and the sender's pid",
              r == SCHED_ALL_DONE && R->state == PROC_TERMINATED && S->state == PROC_TERMINATED && ud(R, 8) == 0x1CC &&
              ud(R, 0x20) == 2 && ud(R, 0x28) == 2 && ud(R, 0x30) == 2 && ud(R, 0x50) == (uint64_t)sa && ud(R, 0x58) == (uint64_t)sa && ud(R, 0x60) == (uint64_t)sa &&
              ud(R, 0x80) == 0x316D && ud(R, 0x88) == 0x326D && ud(R, 0x90) == 0x336D && ud(S, 0x20) == 0 && ud(S, 0x28) == 0 && ud(S, 0x30) == 0);
        check("ipc: the receiver really slept: it ran, blocked, and was resumed exactly once (2 switches in), consuming no CPU while waiting", R->switches_in == 2 && R->yields == 0 && R->mbox.count == 0);
        destroy_all();

        /* the sender exits while its messages are still queued */
        int sa2 = proc_create(60, 2, "send"), rb2 = proc_create(61, 3, "recv");
        proc_ipc_allow(sa2, rb2);
        r = sched_run(2000);
        R = proc_get(rb2); S = proc_get(sa2);
        check("ipc: messages queued by a sender that has since exited are still delivered intact",
              r == SCHED_ALL_DONE && S->state == PROC_TERMINATED && S->switches_in == 1 && R->state == PROC_TERMINATED && ud(R, 0x80) == 0x316D && ud(R, 0x90) == 0x336D && ud(R, 0x50) == (uint64_t)sa2);
        destroy_all();

        /* every rejection path of the syscalls */
        int pp = proc_create(62, (uint64_t)(uintptr_t)&kernel_secret, "probe"), po = proc_create(41, 0, "other");
        proc_get(pp)->ctx.x[2] = (uint64_t)po;                      /* the pid it will try (and must fail) to message */
        proc_ipc_allow(pp, pp);                                     /* self is always allowed; nothing else is granted */
        r = sched_run(2000);
        struct proc *P = proc_get(pp), *O = proc_get(po);
        (void)O;
        check("ipc validation: oversized (-E2BIG), pid 0 and out-of-range pid (-ESRCH), kernel source pointer (-EFAULT), unauthorised destination (-EPERM)",
              r == SCHED_ALL_DONE && P->state == PROC_TERMINATED && (int64_t)ud(P, 0x20) == AE_E2BIG && (int64_t)ud(P, 0x28) == AE_ESRCH && (int64_t)ud(P, 0x30) == AE_ESRCH &&
              (int64_t)ud(P, 0x38) == AE_EFAULT && (int64_t)ud(P, 0x68) == AE_EPERM && ud(P, 0x40) == 0);
        check("ipc validation: a valid self-send works; receive into a kernel address is refused (-EFAULT) and the message stays queued, then arrives (2 bytes); empty mailbox gives -EAGAIN",
              (int64_t)ud(P, 0x48) == 0 && (int64_t)ud(P, 0x50) == AE_EFAULT && (int64_t)ud(P, 0x58) == 2 && (int64_t)ud(P, 0x60) == AE_EAGAIN);
        check("ipc validation: a mailbox holds exactly 4 messages (the 5th send is -EAGAIN); a too-small receive buffer is -E2BIG and a zero capacity -EINVAL",
              ud(P, 0x70) == 0 && ud(P, 0x78) == 0 && ud(P, 0x80) == 0 && ud(P, 0x88) == 0 && (int64_t)ud(P, 0x90) == AE_EAGAIN && (int64_t)ud(P, 0x98) == AE_E2BIG && (int64_t)ud(P, 0xA0) == AE_EINVAL);
        check("ipc authorisation: the process it was NOT allowed to message has an empty mailbox, and an exited process's mailbox is empty (nothing left to read)",
              po > 0 && O->mbox.count == 0 && P->mbox.count == 0);
        destroy_all();

        /* the receiver exits while messages are queued; later sends to it fail */
        int sa3 = proc_create(60, 2, "send"), rb3 = proc_create(41, 0, "exits");
        proc_ipc_allow(sa3, rb3);
        r = sched_run(2000);
        struct proc *RB = proc_get(rb3);
        int first_ok = r == SCHED_ALL_DONE && ud(proc_get(sa3), 0x20) == 0 && ud(proc_get(sa3), 0x28) == 0 && ud(proc_get(sa3), 0x30) == 0 && RB->state == PROC_TERMINATED && RB->exit_code == 7 && RB->mbox.count == 0;
        int sa4 = proc_create(60, 2, "late");
        proc_ipc_allow(sa4, rb3);
        sched_run(2000);
        check("ipc cleanup: a receiver that exits with queued messages loses its mailbox (count 0), and later sends to it fail with -ESRCH",
              first_ok && (int64_t)ud(proc_get(sa4), 0x20) == AE_ESRCH && (int64_t)ud(proc_get(sa4), 0x28) == AE_ESRCH);
        destroy_all();

        /* a full mailbox, then the receiver drains it */
        int fl = proc_create(65, 2, "flood"), dr = proc_create(61, 4, "drain");
        proc_ipc_allow(fl, dr);
        r = sched_run(2000);
        struct proc *F = proc_get(fl), *D = proc_get(dr);
        check("ipc: a flood of six messages into a 4-slot mailbox: 4 accepted, 2 report -EAGAIN; the receiver then drains exactly the 4 in order",
              r == SCHED_ALL_DONE && ud(F, 0x20) == 0 && ud(F, 0x28) == 0 && ud(F, 0x30) == 0 && ud(F, 0x38) == 0 && (int64_t)ud(F, 0x40) == AE_EAGAIN && (int64_t)ud(F, 0x48) == AE_EAGAIN &&
              D->state == PROC_TERMINATED && ud(D, 0x20) == 2 && ud(D, 0x38) == 2 && ud(D, 8) == 0x1CC);
        destroy_all();

        /* several processes waiting at once: no deadlock when senders exist */
        int w1 = proc_create(64, 0, "wait1"), w2 = proc_create(64, 0, "wait2"), ws = proc_create(68, w1, "sender");
        proc_ipc_allow(ws, w1); proc_ipc_allow(ws, w2);
        r = sched_run(2000);
        check("ipc: two processes blocked in receive are both woken by one sender; no deadlock, both get a 2-byte message from the sender",
              r == SCHED_ALL_DONE && proc_get(w1)->state == PROC_TERMINATED && proc_get(w2)->state == PROC_TERMINATED && proc_get(w1)->exit_code == 2 && proc_get(w2)->exit_code == 2 &&
              ud(proc_get(w1), 0x50) == (uint64_t)ws && ud(proc_get(w2), 0x50) == (uint64_t)ws);
        destroy_all();

        /* a receiver nobody will ever send to: the scheduler reports it instead of hanging */
        int lone = proc_create(64, 0, "lonely");
        r = sched_run(2000);
        check("ipc: a lone blocked receiver is reported as SCHED_DEADLOCK (the kernel regains control, state BLOCKED, mailbox untouched) instead of spinning or hanging",
              r == SCHED_DEADLOCK && proc_get(lone)->state == PROC_BLOCKED && proc_get(lone)->rx_pending && !sched_active() && get_ttbr0() == vmm_kernel_l1_pa());
        check("ipc: a BLOCKED process can be destroyed cleanly", proc_destroy(lone) == 0 && proc_get(lone) == 0);

        /* the sender faults after sending */
        int fa = proc_create(63, 2, "sendfault"), fb = proc_create(61, 2, "recv");
        proc_ipc_allow(fa, fb);
        r = sched_run(2000);
        check("ipc: a sender that faults right after sending does not corrupt its queued messages; the receiver still gets both intact",
              r == SCHED_ALL_DONE && proc_get(fa)->state == PROC_FAULTED && proc_get(fb)->state == PROC_TERMINATED && ud(proc_get(fb), 0x80) == 0x316D && ud(proc_get(fb), 0x88) == 0x326D && ud(proc_get(fb), 0x50) == (uint64_t)fa);
        destroy_all();

        /* repeated create/send/receive/destroy */
        uint64_t base = free_pages(); int cyc = 1;
        for (int i = 0; i < 12; i++) {
            int rx = proc_create(61, 3, "recv"), tx = proc_create(60, 1, "send");
            proc_ipc_allow(tx, rx);
            uint64_t rr = sched_run(2000);
            if (rr != SCHED_ALL_DONE || ud(proc_get(rx), 8) != 0x1CC || ud(proc_get(rx), 0x90) != 0x336D) cyc = 0;
            destroy_all();
            if (free_pages() != base) cyc = 0;
        }
        check("ipc: 12 rounds of create / send / blocking receive / destroy: always correct and no page leaks", cyc);
    }

    /* ---- IPC authorisation lifetime: a grant belongs to ONE process instance, never to a reusable pid ---- */
    {
        static const struct { const char *how; int role; uint64_t arg; int run_first; } ends[] = {
            { "exits normally", 41, 0, 1 }, { "faults", 10, 0, 1 }, { "is destroyed while BLOCKED", 64, 0, 1 }, { "is destroyed while READY (never ran)", 41, 0, 0 },
        };
        for (unsigned v = 0; v < sizeof ends / sizeof *ends; v++) {
            destroy_all();
            int a = proc_create(64, 0, "A");                                  /* blocks forever in receive: stays alive */
            int b = proc_create(ends[v].role, ends[v].role == 10 ? (uint64_t)(uintptr_t)&kernel_secret : 0, "B");
            struct proc *pa = proc_get(a);
            int g1 = proc_ipc_allow(a, b);
            int s1 = ipc_send(pa, (uint64_t)b, 0, 0);                         /* authorised while B is alive */
            int alive_ok = g1 == 0 && s1 == AE_OK && proc_get(b)->mbox.count == 1;
            if (ends[v].run_first) sched_run(2000);                           /* B ends; A stays BLOCKED, so the scheduler reports DEADLOCK */
            int gone = proc_destroy(b);
            int c = proc_create(64, 0, "C");                                  /* first free slot: B's pid again */
            struct proc *pc = proc_get(c);
            int reused = c == b && pc && pc->mbox.count == 0;
            int denied = ipc_send(pa, (uint64_t)c, 0, 0);                     /* no grant for THIS instance */
            int still_empty = pc->mbox.count == 0;
            int regrant = proc_ipc_allow(a, c) == 0 && ipc_send(pa, (uint64_t)c, 0, 0) == AE_OK && pc->mbox.count == 1;   /* a fresh, explicit grant works */
            char name[140]; int n = 0;
            const char *p1 = "ipc authorisation: recipient "; while (*p1) name[n++] = *p1++;
            for (const char *q = ends[v].how; *q && n < 100; q++) name[n++] = *q;
            const char *p2 = ", its pid is reused: the old grant is void (-EPERM), the new process gets nothing, an explicit new grant works"; for (const char *q = p2; *q && n < 139; q++) name[n++] = *q;
            name[n] = 0;
            check(name, alive_ok && gone == 0 && reused && denied == AE_EPERM && still_empty && regrant);
        }
        destroy_all();

        /* grants to recipients that are not eligible are refused and leave nothing behind */
        {
            int a = proc_create(64, 0, "A"), b = proc_create(41, 0, "B");
            struct proc *pa = proc_get(a);
            sched_run(2000);                                                  /* A blocks, B exits (TERMINATED) */
            int bad = proc_ipc_allow(a, 3) < 0 && proc_ipc_allow(a, b) < 0 && proc_ipc_allow(a, 0) < 0 && proc_ipc_allow(a, PROC_MAX + 1) < 0 &&
                      proc_ipc_allow(0, a) < 0 && proc_ipc_allow(99, a) < 0 && proc_ipc_allow(b, a) < 0;      /* nonexistent, terminated, out of range, dead granter */
            proc_destroy(b);
            int x = proc_create(64, 0, "X"), y = proc_create(64, 0, "Y");     /* pids 2 and 3 now exist */
            int no_latent = x == 2 && y == 3 && ipc_send(pa, 2, 0, 0) == AE_EPERM && ipc_send(pa, 3, 0, 0) == AE_EPERM;   /* refused grants did not linger */
            int selfok = proc_ipc_allow(a, a) == 0 && proc_ipc_allow(a, a) == 0 && ipc_send(pa, (uint64_t)a, 0, 0) == AE_OK;
            check("ipc authorisation: grants to nonexistent, terminated, out-of-range recipients or from a dead granter are refused and leave nothing behind; self-grant and repeated grant are fine",
                  bad && no_latent && selfok);
        }
        destroy_all();

        /* the SENDER's slot being reused must not inherit its grants either */
        {
            int a = proc_create(64, 0, "A"), b = proc_create(64, 0, "B");
            int g = proc_ipc_allow(a, b) == 0 && ipc_send(proc_get(a), (uint64_t)b, 0, 0) == AE_OK;
            proc_destroy(a);
            int a2 = proc_create(64, 0, "A2");
            check("ipc authorisation: a new process that reuses the SENDER's pid inherits none of its grants (-EPERM), while the old recipient is still alive",
                  g && a2 == a && ipc_send(proc_get(a2), (uint64_t)b, 0, 0) == AE_EPERM);
        }
        destroy_all();

        /* end to end with real user programs: a sender keeps sending to a pid that now belongs to somebody else */
        {
            int sa = proc_create(60, 2, "send"), old = proc_create(41, 0, "old");
            int g = proc_ipc_allow(sa, old) == 0;
            proc_destroy(old);
            int fresh = proc_create(64, 0, "fresh");                           /* reuses pid 2, blocks in receive */
            r = sched_run(2000);
            struct proc *S = proc_get(sa), *Fr = proc_get(fresh);
            check("ipc authorisation (end to end): an EL0 sender whose grant was for the previous occupant of pid 2 gets -EPERM on all three sends; the new occupant's mailbox stays empty",
                  g && fresh == old && r == SCHED_DEADLOCK && (int64_t)ud(S, 0x20) == AE_EPERM && (int64_t)ud(S, 0x28) == AE_EPERM && (int64_t)ud(S, 0x30) == AE_EPERM && Fr->mbox.count == 0 && Fr->state == PROC_BLOCKED);
        }
        destroy_all();

        /* no syscall can grant authority; unknown receive flags are refused */
        {
            int t = proc_create(67, 2, "tryall"), o = proc_create(64, 0, "other");
            r = sched_run(2000);
            struct proc *T = proc_get(t);
            check("ipc authorisation: no system call lets a process grant itself permission (numbers 11-40 all -ENOSYS; number 8 is the file system call, 9 the settings call, which both refuse a process that has no identity, and 10 the memory call, which only ever manages the caller's own heap) and its send to an unauthorised pid is -EPERM",
                  r == SCHED_DEADLOCK && o == 2 && ud(T, 0x20) == 30 && (int64_t)ud(T, 0x28) == AE_EPERM && T->state == PROC_TERMINATED && proc_get(o)->mbox.count == 0);
            destroy_all();
            int f1 = proc_create(66, 2, "flags"), f2 = proc_create(66, 3, "flags"), f3 = proc_create(66, 1, "flags");
            sched_run(2000);
            check("ipc contract: receive flags other than IPC_NONBLOCK are refused with -EINVAL (2 and 3), while 1 gives -EAGAIN on an empty mailbox",
                  (int64_t)ud(proc_get(f1), 0x20) == AE_EINVAL && (int64_t)ud(proc_get(f2), 0x20) == AE_EINVAL && (int64_t)ud(proc_get(f3), 0x20) == AE_EAGAIN);
            destroy_all();
        }

        /* empty messages */
        {
            int p = proc_create(41, 0, "p");
            uint64_t from = 99;
            int s = ipc_send(proc_get(p), (uint64_t)p, 0, 0);
            int d = ipc_dequeue(proc_get(p), 0, 8, &from);
            check("ipc contract: a zero-length message is legal: it is queued, dequeued with length 0 and the right sender, and the mailbox is empty again",
                  s == AE_OK && d == 0 && from == (uint64_t)p && proc_get(p)->mbox.count == 0 && ipc_dequeue(proc_get(p), 0, 8, &from) == AE_EAGAIN);
            destroy_all();
        }

        /* churn: repeated creation/destruction with grants never lets a stale one through, and nothing leaks */
        {
            uint64_t base = free_pages(); int ok = 1;
            for (int i = 0; i < 20; i++) {
                int a = proc_create(64, 0, "A"), b = proc_create(i & 1 ? 64 : 41, 0, "B");
                if (proc_ipc_allow(a, b) != 0 || ipc_send(proc_get(a), (uint64_t)b, 0, 0) != AE_OK) ok = 0;
                proc_destroy(b);
                int c = proc_create(64, 0, "C");
                if (c != b || ipc_send(proc_get(a), (uint64_t)c, 0, 0) != AE_EPERM || proc_get(c)->mbox.count != 0) ok = 0;
                destroy_all();
                if (free_pages() != base) ok = 0;
            }
            check("ipc authorisation: 20 rounds of grant / destroy recipient / reuse its pid never let a stale grant through, and leak no page", ok);
        }
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
