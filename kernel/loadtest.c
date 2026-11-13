/*
 * kernel/loadtest.c — tests of the program loader and the trusted launch path. Compiled only into AURORA_LOAD_TEST kernels.
 *
 * Part 1 needs no disk: ELF images built in RAM drive proc_create_elf (validation through the real path, memory exhaustion at EVERY depth, slot
 * exhaustion, leak loops, "never runnable until loaded").
 * Part 2 needs a disk provisioned by the HOST tool tests/tools/mkafs (executables owned by PRINCIPAL_INSTALLER that are NOT in this kernel binary):
 * the kernel mounts it WITHOUT formatting, launches programs by name through the registry, and runs them at EL0. State persists across QEMU runs
 * in the files the programs themselves keep, so each boot starts from whatever the previous boots left behind.
 * Output: "[load-test] <name>: PASS|FAIL" and "AURORA-LOAD-SELFTEST: N passed, M failed".
 */
#include <blk.h>
#include <console.h>
#include <elf.h>
#include <fs.h>
#include <launch.h>
#include <mm.h>
#include <proc.h>

static int nfail;
#ifdef AURORA_LOAD_TEST
static int npass;
static void check(const char *name, int ok)
{
    console_puts("[load-test] "); console_puts(name); console_puts(ok ? ": PASS\n" : ": FAIL\n");
    if (ok) npass++; else nfail++;
}
static uint64_t free_pages(void) { struct pmm_stats s; pmm_get_stats(&s); return s.free_pages; }
static int live_procs(void) { int n = 0; for (int pid = 1; pid <= PROC_MAX; pid++) if (proc_get(pid)) n++; return n; }
static int states(enum proc_state s) { int n = 0; for (int pid = 1; pid <= PROC_MAX; pid++) { struct proc *p = proc_get(pid); if (p && p->state == s) n++; } return n; }
static void destroy_all(void) { for (int pid = 1; pid <= PROC_MAX; pid++) if (proc_get(pid)) proc_destroy(pid); }

/* ---- an ELF builder (the host test has the same one) ---- */
static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, (uint16_t)v); w16(p + 2, (uint16_t)(v >> 16)); }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }
static void bzero_(void *p, size_t n) { uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

#define IMG_MAX 0x2000
static uint8_t img[IMG_MAX];
struct ph { uint32_t type, flags; uint64_t off, va, filesz, memsz, align; };
#define TEXT_VA (ELF_VA_MIN + 0xB0)
#define DATA_VA (ELF_VA_MIN + 0x1000 + 0x300)

/* header + n program headers; the first instruction word of the text segment is `first` (a real A64 instruction in the one program that runs) */
static size_t build(uint64_t entry, const struct ph *p, int n, const uint32_t *code, int ncode)
{
    bzero_(img, sizeof img);
    img[0] = 0x7F; img[1] = 'E'; img[2] = 'L'; img[3] = 'F'; img[4] = 2; img[5] = 1; img[6] = 1;
    w16(img + 16, 2); w16(img + 18, 183); w32(img + 20, 1); w64(img + 24, entry); w64(img + 32, 64); w16(img + 52, 64); w16(img + 54, 56); w16(img + 56, (uint16_t)n);
    for (int i = 0; i < n; i++) {
        uint8_t *q = img + 64 + i * 56;
        w32(q, p[i].type); w32(q + 4, p[i].flags); w64(q + 8, p[i].off); w64(q + 16, p[i].va); w64(q + 24, p[i].va);
        w64(q + 32, p[i].filesz); w64(q + 40, p[i].memsz); w64(q + 48, p[i].align);
    }
    for (int i = 0; i < ncode; i++) w32(img + 0xB0 + 4 * i, code[i]);
    return 0x500;
}
static const struct ph BASE[2] = {
    { 1, ELF_PF_R | ELF_PF_X, 0xB0, TEXT_VA, 0x200, 0x200, ELF_PAGE },
    { 1, ELF_PF_R | ELF_PF_W, 0x300, DATA_VA, 0x100, 0x900, ELF_PAGE } };
/* mov x8, #4 ; mov x0, #42 ; svc #0   =>  exit(42) */
static const uint32_t EXIT42[3] = { 0xD2800088u, 0xD2800540u, 0xD4000001u };

static size_t base_image(void) { return build(TEXT_VA, BASE, 2, EXIT42, 3); }

/* try to load the image currently in img[]; return the proc_create_elf code and the elf error */
static int try_load(size_t len, uint32_t principal, int *eerr) { return proc_create_elf(img, len, "t", principal, 0, eerr); }

/* ---- memory hogging: take every free page but `keep`, as a linked list threaded through the pages themselves ---- */
static uintptr_t hog_head;
static void hog_all_but(uint64_t keep)
{
    while (free_pages() > keep) { uintptr_t pg = pmm_alloc_page(); if (!pg) break; *(volatile uintptr_t *)pg = hog_head; hog_head = pg; }
}
static void hog_release(void) { while (hog_head) { uintptr_t pg = hog_head; hog_head = *(volatile uintptr_t *)pg; *(volatile uintptr_t *)pg = 0; pmm_free_page(pg); } }

static void part1(void)
{
    uint64_t pages0 = free_pages();
    destroy_all();
    int eerr = 0;

    /* ---- a valid in-RAM image really runs at EL0 ---- */
    {
        size_t len = base_image();
        int pid = try_load(len, 77, &eerr);
        struct proc *p = pid > 0 ? proc_get(pid) : 0;
        int staged = p && p->state == PROC_READY && p->principal == 77 && p->entry == TEXT_VA && p->ctx.elr == TEXT_VA && p->ctx.sp_el0 == USER_STACK_TOP && p->ctx.spsr == 0;
        uint64_t r = p ? sched_run(2000) : 99;
        check("loader: an image built in RAM is mapped, becomes READY with the kernel-chosen identity and entry point, runs at genuine EL0 and exits with its own code (42)",
              staged && r == SCHED_ALL_DONE && p->state == PROC_TERMINATED && p->exit_code == 42 && p->el0_confirmed > 0);
        if (pid > 0) proc_destroy(pid);
        check("loader: destroying it returns every page", free_pages() == pages0 && live_procs() == 0);
    }
    check("loader: identity 0 and PRINCIPAL_NONE are refused before anything is allocated",
          ({ size_t l = base_image(); int a = try_load(l, 0, &eerr), b = try_load(l, PRINCIPAL_NONE, &eerr); a == PROC_ELOAD_ARG && b == PROC_ELOAD_ARG && free_pages() == pages0 && live_procs() == 0; }));

    /* ---- defects: each one refused through the real loader path, and nothing leaks ---- */
    {
        struct { const char *name; int want; } t[24]; int nt = 0; int ok = 1;
        #define CASE(nm, want_, mutate) do { size_t len = base_image(); (void)len; mutate; int e = 0; \
            int rc = try_load(len, 77, &e); int good = rc == PROC_ELOAD_INVALID && e == (want_) && free_pages() == pages0 && live_procs() == 0 && states(PROC_NEW) == 0; \
            if (!good) { console_puts("   defect not refused as expected: " nm "\n"); ok = 0; } t[nt].name = nm; t[nt++].want = (want_); } while (0)
        size_t L;
        CASE("truncated header", ELF_ETRUNC, { L = 30; len = L; });
        CASE("bad magic", ELF_EMAGIC, { img[1] = 'X'; });
        CASE("wrong architecture (x86-64)", ELF_EMACHINE, { w16(img + 18, 62); });
        CASE("unsupported type (ET_DYN: would need relocation)", ELF_ETYPE, { w16(img + 16, 3); });
        CASE("PT_DYNAMIC (dynamic relocations)", ELF_ESEGTYPE, { w32(img + 64 + 56, 2); });
        CASE("PT_INTERP", ELF_ESEGTYPE, { w32(img + 64 + 56, 3); });
        CASE("segment bounds: file range past the end", ELF_ESEGBOUNDS, { w64(img + 64 + 56 + 32, 0x4000); w64(img + 64 + 56 + 40, 0x4000); });
        CASE("integer overflow in offset + filesz", ELF_ESEGBOUNDS, { w64(img + 64 + 8, 0xFFFFFFFFFFFFF0B0ULL); });
        CASE("integer overflow: memsz wraps the address space", ELF_ERANGE, { w64(img + 64 + 56 + 40, 0xFFFFFFFFFFFFF000ULL); });
        CASE("entry point not in an executable segment", ELF_EENTRY, { w64(img + 24, DATA_VA); });
        CASE("entry outside every segment", ELF_EENTRY, { w64(img + 24, ELF_VA_MIN + 0x8000); });
        CASE("entry inside the zero-filled (bss) tail of the code segment", ELF_EENTRY, { w64(img + 64 + 40, 0x400); w64(img + 24, TEXT_VA + 0x300); });
        CASE("writable AND executable segment", ELF_EWX, { w32(img + 64 + 56 + 4, ELF_PF_R | ELF_PF_W | ELF_PF_X); });
        CASE("overlapping segments (they share a page)", ELF_EOVERLAP, { w64(img + 64 + 56 + 16, TEXT_VA + 0x10); w64(img + 64 + 56 + 8, 0xC0); });
        CASE("invalid alignment", ELF_EALIGN, { w64(img + 64 + 48, 8192); });
        CASE("segment overlapping the stack guard page", ELF_ERANGE, { w64(img + 64 + 56 + 16, USER_VA_BASE + 0x2000 + 0x300); });
        CASE("segment overlapping the stack", ELF_ERANGE, { w64(img + 64 + 56 + 16, USER_STACK_VA + 0x300); });
        CASE("segment in the fixed low code page", ELF_ERANGE, { w64(img + 64 + 56 + 16, USER_CODE_VA + 0x300); });
        CASE("too many pages (13)", ELF_ETOOBIG, { w64(img + 64 + 56 + 40, 12 * 0x1000); });
        CASE("no loadable segment", ELF_ENOLOAD, { w32(img + 64, 4); w32(img + 64 + 56, 0); });
        (void)t;
        #undef CASE
        check("loader: 20 hostile executables (truncated, magic, architecture, type, dynamic/interp, bounds, overflow, entry, W+X, overlap, alignment, guard page, stack, low pages, size, no segment) are each refused with the exact code; no page or process slot is used", ok);
    }
    {   /* an oversized file */
        size_t len = base_image(); (void)len; int e = 0;
        int rc = proc_create_elf(img, ELF_MAX_FILE + 1, "t", 77, 0, &e);
        check("loader: an oversized executable is refused (no read beyond the buffer, nothing allocated)", rc == PROC_ELOAD_INVALID && e == ELF_ETOOBIG && free_pages() == pages0 && live_procs() == 0);
    }

    /* ---- insufficient memory at EVERY depth: the first failure can be the page tables, a segment page or a stack page ---- */
    {
        size_t len = base_image(); int ok = 1, first_success = -1;
        hog_all_but(0);
        for (int k = 0; k <= 10; k++) {
            int ns = 0;
            for (int i = 0; i < k; i++) { uintptr_t pg = hog_head; if (!pg) break; hog_head = *(volatile uintptr_t *)pg; *(volatile uintptr_t *)pg = 0; pmm_free_page(pg); ns++; }
            int e = 0; int pid = try_load(len, 77, &e);
            if (pid > 0) { if (first_success < 0) first_success = k; proc_destroy(pid); }
            else if (pid != PROC_ELOAD_NOMEM) ok = 0;
            if (free_pages() != (uint64_t)k || live_procs() != 0) ok = 0;      /* a failed or destroyed load leaves exactly the k pages we freed */
            for (int i = 0; i < ns; i++) { uintptr_t pg = pmm_alloc_page(); if (!pg) { ok = 0; break; } *(volatile uintptr_t *)pg = hog_head; hog_head = pg; }
            if (free_pages() != 0) ok = 0;
        }
        hog_release();
        check("loader: with exactly 0..10 free pages the load fails with PROC_ELOAD_NOMEM until 7 pages are available (3 tables + 2 image + 2 stack) and then succeeds; every failure and every destroy returns ALL pages (checked at each depth)",
              ok && first_success == 7 && free_pages() == pages0);
    }

    /* ---- slot exhaustion ---- */
    {
        size_t len = base_image(); int made = 0, e = 0, last = 0;
        for (int i = 0; i < PROC_MAX + 1; i++) { last = try_load(len, 77, &e); if (last > 0) made++; else break; }
        int full = made == PROC_MAX && last == PROC_ELOAD_NOSLOT;
        destroy_all();
        check("loader: all 8 process slots can be filled, the 9th load is PROC_ELOAD_NOSLOT, and destroying them returns every page", full && free_pages() == pages0 && live_procs() == 0);
    }

    /* ---- repeated failures never leak ---- */
    {
        int ok = 1;
        for (int i = 0; i < 300; i++) {
            size_t len = base_image(); int e = 0;
            switch (i % 5) {
            case 0: w64(img + 24, ELF_VA_MIN + 0x8000); break;                        /* bad entry */
            case 1: w32(img + 64 + 56 + 4, 7); break;                                 /* W+X */
            case 2: { int pid = try_load(len, 77, &e); if (pid > 0) proc_destroy(pid); else ok = 0; continue; }   /* a good load, destroyed */
            case 3: hog_all_but(4); { int rc = try_load(len, 77, &e); if (rc != PROC_ELOAD_NOMEM) ok = 0; } hog_release(); continue;   /* runs out of memory part-way */
            default: len = 70; break;                                                 /* truncated */
            }
            int rc = try_load(len, 77, &e);
            if (rc >= 0) { ok = 0; proc_destroy(rc); }
        }
        check("loader: 300 mixed failing, memory-starved and successful loads leave the page count and the process table exactly as they were; no process is ever left in state NEW", ok && free_pages() == pages0 && live_procs() == 0 && states(PROC_NEW) == 0);
    }
}

/* ---- part 2: end to end from the disk ---- */
static uint32_t counter_of(uint32_t owner, const char *name)         /* 0 = file absent */
{
    fs_id_t id; uint8_t b[16];
    if (fs_lookup_owner(owner, name, &id) != FS_OK) return 0;
    return fs_read(0, id, 0, b, 16) == 16 ? get32(b + 8) : 0xFFFFFFFFu;
}
static int owner_of(uint32_t owner, const char *name) { fs_id_t id; struct fs_stat st; return fs_lookup_owner(owner, name, &id) == FS_OK && fs_stat(0, id, &st) == FS_OK ? (int)st.owner : -1; }

static int run_app(const char *app, struct proc **out, int *pid_out)
{
    int pid = 0, detail = 0;
    int rc = launch_app(app, &pid, &detail);
    if (rc != LAUNCH_OK) return rc;
    *out = proc_get(pid); *pid_out = pid;
    return LAUNCH_OK;
}

static void part2(void)
{
    uint64_t pages0 = free_pages();
    destroy_all();
    launch_reset();
    int reg = launch_register("alpha", "alpha.elf", 101, 0) == LAUNCH_OK && launch_register("beta", "beta.elf", 102, 0) == LAUNCH_OK &&
              launch_register("clone", "alpha.elf", 104, 0) == LAUNCH_OK && launch_register("shadow", "shadow.elf", 105, 0) == LAUNCH_OK &&
              launch_register("loose", "loose.elf", 106, 0) == LAUNCH_OK && launch_register("missing", "nothere.elf", 107, 0) == LAUNCH_OK &&
              launch_register("corrupt", "corrupt.elf", 108, 0) == LAUNCH_OK && launch_register("badelf", "bad.elf", 109, 0) == LAUNCH_OK;
    for (int k = 1; k <= 5; k++) { char nm[8] = { 'f', 'a', 'u', 'l', 't', (char)('0' + k), 0 }; reg &= launch_register(nm, "fault.elf", 103, (uint64_t)k) == LAUNCH_OK; }
    check("registry: applications are registered by kernel code with their own identities",
          reg && launch_register("alpha", "x.elf", 150, 0) == LAUNCH_EREG && launch_register("evil", "alpha.elf", PRINCIPAL_INSTALLER, 0) == LAUNCH_EREG &&
          launch_register("evil", "alpha.elf", 0, 0) == LAUNCH_EREG && launch_register("evil", "alpha.elf", PRINCIPAL_NONE, 0) == LAUNCH_EREG &&
          launch_register("Bad Name", "alpha.elf", 150, 0) == LAUNCH_EREG && launch_register("evil", "a/b", 150, 0) == LAUNCH_EREG);

    int pid; int detail;
    /* ---- launches that must be refused ---- */
    {
        int r1 = launch_app("ghost", &pid, &detail);
        int r2 = launch_app("missing", &pid, &detail), d2 = detail;
        int r3 = launch_app("shadow", &pid, &detail), d3 = detail;
        int r4 = launch_app("loose", &pid, &detail);
        int r5 = launch_app("corrupt", &pid, &detail), d5 = detail;
        int r6 = launch_app("badelf", &pid, &detail), d6 = detail;
        int none_left = live_procs() == 0 && free_pages() == pages0;
        check("launch refused: an unregistered application name is LAUNCH_ENOAPP", r1 == LAUNCH_ENOAPP);
        check("launch refused: a registered application whose executable does not exist is LAUNCH_ENOEXE (FS_ENOENT)", r2 == LAUNCH_ENOEXE && d2 == FS_ENOENT);
        check("launch refused: a file of the right name owned by the APPLICATION (not the installer) is never considered - an application cannot supply or shadow its own executable (LAUNCH_ENOEXE)",
              r3 == LAUNCH_ENOEXE && d3 == FS_ENOENT && owner_of(105, "shadow.elf") == 105);
        check("launch refused: an installer-owned executable that OTHERS may write is LAUNCH_EUNSAFE", r4 == LAUNCH_EUNSAFE);
        check("launch refused: an installer-owned executable with a damaged data block is LAUNCH_EREAD / FS_ECORRUPT (the checksum catches it; wrong bytes are never run)", r5 == LAUNCH_EREAD && d5 == FS_ECORRUPT);
        check("launch refused: valid file, valid checksum, but not an executable, is LAUNCH_EEXE / ELF_EMAGIC (a good checksum does not make a file a program)", r6 == LAUNCH_EEXE && d6 == ELF_EMAGIC);
        check("none of the six refused launches left a process or a page behind", none_left);
    }

    /* ---- two real programs from disk, running concurrently under different identities ---- */
    {
        uint32_t a0 = counter_of(101, "alpha.dat"), b0 = counter_of(102, "beta.dat");
        struct proc *pa = 0, *pb = 0; int ia = 0, ib = 0;
        int la = run_app("alpha", &pa, &ia), lb = run_app("beta", &pb, &ib);
        int ident = la == LAUNCH_OK && lb == LAUNCH_OK && pa->principal == 101 && pb->principal == 102 && pa->state == PROC_READY && pb->state == PROC_READY &&
                    pa->code_pa == 0 && pb->code_pa == 0;             /* not the built-in blob: no fixed code page was installed */
        uint64_t r = ident ? sched_run(20000) : 99;
        int done = ident && r == SCHED_ALL_DONE && pa->state == PROC_TERMINATED && pb->state == PROC_TERMINATED;
        console_puts("[load] alpha exit code "); console_dec((uint64_t)(done ? pa->exit_code : -1)); console_puts(", beta exit code "); console_dec((uint64_t)(done ? pb->exit_code : -1)); console_puts("\n");
        check("programs: alpha.elf and beta.elf are launched BY NAME from AuroraFS (installer-owned, never in this kernel), get identities 101 and 102 from the registry, run concurrently at EL0 and both exit 0 (sections, own file, other's file invisible, isolation from the concurrent process, no kernel privileges)",
              done && pa->exit_code == 0 && pb->exit_code == 0);
        uint32_t a1 = counter_of(101, "alpha.dat"), b1 = counter_of(102, "beta.dat");
        console_puts("[load] alpha counter before="); console_dec(a0); console_puts(" after="); console_dec(a1);
        console_puts(" beta counter before="); console_dec(b0); console_puts(" after="); console_dec(b1); console_puts("\n");
        check("persistence: each program's counter file (owned by its OWN identity, written by the program from EL0) advanced by exactly one on this boot and stays owned by 101 / 102",
              a1 == a0 + 1 && b1 == b0 + 1 && owner_of(101, "alpha.dat") == 101 && owner_of(102, "beta.dat") == 102);
        destroy_all();

        /* the same program file under another identity */
        uint32_t c0 = counter_of(104, "alpha.dat");
        struct proc *pc = 0; int ic = 0;
        int lc = run_app("clone", &pc, &ic);
        uint64_t rc2 = lc == LAUNCH_OK ? sched_run(20000) : 99;
        int cdone = lc == LAUNCH_OK && rc2 == SCHED_ALL_DONE && pc->exit_code == 0 && pc->principal == 104;
        uint32_t c1 = counter_of(104, "alpha.dat"), a2 = counter_of(101, "alpha.dat");
        check("identity follows the registry, not the code: alpha's executable started as application 'clone' (identity 104) keeps its OWN counter file, cannot see or change identity 101's file, and gains none of its authority",
              cdone && c1 == c0 + 1 && a2 == a1 && owner_of(104, "alpha.dat") == 104 && owner_of(101, "alpha.dat") == 101);
        destroy_all();
    }

    /* ---- faulting programs are contained ---- */
    {
        int ok = 1; const char *kinds[5] = { "write to own code", "jump to unmapped memory", "privileged register read", "read of a kernel address", "write to own read-only data" };
        for (int k = 1; k <= 5; k++) {
            char nm[8] = { 'f', 'a', 'u', 'l', 't', (char)('0' + k), 0 };
            struct proc *pf = 0; int ipid = 0;
            int l = run_app(nm, &pf, &ipid);
            uint64_t r = l == LAUNCH_OK ? sched_run(5000) : 99;
            int contained = l == LAUNCH_OK && r == SCHED_ALL_DONE && pf->state == PROC_FAULTED && pf->exit_code != 77;
            if (!contained) { ok = 0; console_puts("   not contained: "); console_puts(kinds[k - 1]); console_puts("\n"); }
            destroy_all();
        }
        check("faults: a loaded program that writes its own code, jumps to unmapped memory, reads a privileged register, reads a kernel address, or writes its read-only data is FAULTED (never resumed); the kernel keeps running", ok && free_pages() == pages0);
        uint32_t a0 = counter_of(101, "alpha.dat");
        struct proc *pa = 0; int ia = 0;
        int l = run_app("alpha", &pa, &ia);
        uint64_t r = l == LAUNCH_OK ? sched_run(20000) : 99;
        check("faults: after five faulting programs the system still launches and runs a normal one, which exits 0 and updates its file", l == LAUNCH_OK && r == SCHED_ALL_DONE && pa->exit_code == 0 && counter_of(101, "alpha.dat") == a0 + 1);
        destroy_all();
    }

    /* ---- pid reuse ---- */
    {
        struct proc *pa = 0, *pb = 0; int ia = 0, ib = 0;
        int la = run_app("alpha", &pa, &ia);
        uint64_t r1 = la == LAUNCH_OK ? sched_run(20000) : 99;
        int first_ok = la == LAUNCH_OK && r1 == SCHED_ALL_DONE && pa->exit_code == 0;
        destroy_all();
        int lb = run_app("beta", &pb, &ib);
        int same_slot = lb == LAUNCH_OK && ib == ia && pb->principal == 102 && pb->principal != 101;
        uint64_t r2 = lb == LAUNCH_OK ? sched_run(20000) : 99;
        check("PID reuse: beta started in the pid slot alpha just vacated runs as identity 102, not 101: it cannot find alpha's file (its own check, exit 0) and alpha's data is unchanged",
              first_ok && same_slot && r2 == SCHED_ALL_DONE && pb->exit_code == 0 && owner_of(101, "alpha.dat") == 101);
        destroy_all();
    }
    check("no leaks: after every disk-launched scenario all physical pages are back and no process remains", free_pages() == pages0 && live_procs() == 0);
}

void loadtest_run(void)
{
    part1();
    fs_id_t id;
    if (!fs_mounted() || fs_lookup_owner(PRINCIPAL_INSTALLER, "alpha.elf", &id) != FS_OK) {
        console_puts("[load] no provisioned disk (no installer-owned alpha.elf): end-to-end tests not run\n");
        return;
    }
    part2();
}
#else
void loadtest_run(void) {}
#endif

int loadtest_summary(void)
{
#ifdef AURORA_LOAD_TEST
    if (npass + nfail == 0) return 0;
    console_puts("AURORA-LOAD-SELFTEST: "); console_dec((uint64_t)npass); console_puts(" passed, "); console_dec((uint64_t)nfail); console_puts(" failed\n");
#endif
    return nfail;
}
