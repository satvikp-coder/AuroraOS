/*
 * kernel/fsproctest.c — EL0 file-access tests (milestone C). Compiled only into AURORA_FS_TEST kernels, run against a disposable disk.
 *
 * Real EL0 programs (kernel/proc/user_progs.S roles 70-73) call SYS_FS. Their file identity (principal) is assigned HERE, in kernel code, with
 * proc_set_principal; the programs cannot name or change it. Boot 1 (no file "note" yet): program A creates files, intruders are refused.
 * Later boots (the files exist on the disk from an earlier QEMU run): a NEW process with the same application identity reads the persisted data,
 * everything else is still refused. The refusal checks run on every boot.
 * Output: "[fsproc-test] <name>: PASS|FAIL" and "AURORA-FSPROC-SELFTEST: N passed, M failed".
 */
#include <blk.h>
#include <console.h>
#include <fs.h>
#include <mm.h>
#include <proc.h>

static int nfail;
#ifdef AURORA_FS_TEST
static int npass;
#define APP_A 21u          /* the application identity that owns "note" and "pub" */
#define APP_B 22u          /* a different application */
extern uint8_t fs_bounds_start[], fs_bounds_end[], fs_quota_start[], fs_quota_end[];   /* kernel/proc/user_test_progs.S: separate EL0 images, test kernels only */

static volatile uint64_t kernel_secret = 0x5EC12E7C0FFEEULL;
static const uint8_t DATA16[17] = "AURORA-EL0-FILE!";

static void check(const char *name, int ok)
{
    console_puts("[fsproc-test] "); console_puts(name); console_puts(ok ? ": PASS\n" : ": FAIL\n");
    if (ok) npass++; else nfail++;
}
static uint64_t ud(struct proc *p, uint64_t off) { return *(volatile uint64_t *)(uintptr_t)(p->data_pa + off); }
static int64_t sd(struct proc *p, uint64_t off) { return (int64_t)ud(p, off); }
static const uint8_t *ub(struct proc *p, uint64_t off) { return (const uint8_t *)(uintptr_t)(p->data_pa + off); }
static void destroy_all(void) { for (int pid = 1; pid <= PROC_MAX; pid++) if (proc_get(pid)) proc_destroy(pid); }
static uint64_t free_pages(void) { struct pmm_stats s; pmm_get_stats(&s); return s.free_pages; }
static int eq16(const uint8_t *a) { for (int i = 0; i < 16; i++) if (a[i] != DATA16[i]) return 0; return 1; }

/* run one program to completion under a kernel-assigned principal */
static struct proc *run_as(int role, uint64_t arg, uint32_t principal, uint64_t x2, const char *name)
{
    int pid = proc_create(role, arg, name);
    if (pid < 0) return 0;
    struct proc *p = proc_get(pid);
    if (principal) proc_set_principal(pid, principal);
    p->ctx.x[2] = x2;
    uint64_t r = sched_run(3000);
    return r == SCHED_ALL_DONE ? p : 0;
}

#define EFS(e) (AE_FS_BASE + (e))

void fsproc_run(void)
{
    if (!blk_present()) {
        destroy_all();
        struct proc *z = run_as(72, 0, APP_A, 0, "nodisk"); int zok = z && sd(z, 0x20) == EFS(FS_EMOUNT) && sd(z, 0x28) == EFS(FS_EMOUNT) && sd(z, 0x30) == EFS(FS_EMOUNT) && sd(z, 0x38) == EFS(FS_EMOUNT);
        destroy_all();
        struct proc *c = run_as(70, 0, APP_A, 0, "nodisk-create"); int cok = c && sd(c, 0x20) == EFS(FS_EMOUNT);
        destroy_all();
        check("no block device: every file system call from EL0 (lookup, read, stat, list, create) returns FS_EMOUNT and the kernel keeps running", zok && cok);
        return;
    }
    if (blk_readonly() && fs_mounted()) {
        destroy_all();
        struct proc *c = run_as(70, 0, 29u, 0, "ro-create");      /* an identity that owns no "note" yet, so the create reaches the write path (an existing name would be FS_EEXIST first) */
        int ok = c && sd(c, 0x20) == EFS(FS_ERO);
        destroy_all();
        check("read-only disk: an EL0 program's file create is refused with FS_ERO (no write reaches the device) and the kernel keeps running", ok);
        return;
    }
    if (blk_readonly() || !fs_mounted()) { console_puts("[fsproc] no writable mounted filesystem: EL0 file tests not run\n"); return; }
    uint64_t pages0 = free_pages();
    destroy_all();
    fs_id_t note = 0, pub = 0;
    int first = fs_lookup(0, "note", &note) != FS_OK;
    console_puts(first ? "[fsproc] boot 1 for this disk: creating files from EL0\n" : "[fsproc] files from an earlier boot found\n");

    check("identity: a new process has NO file identity; the kernel principal 0 and PRINCIPAL_NONE can never be assigned to a process",
          ({ int pid = proc_create(72, 0, "id"); struct proc *p = proc_get(pid); int r = p->principal == PRINCIPAL_NONE && proc_set_principal(pid, 0) < 0 &&
             proc_set_principal(pid, PRINCIPAL_NONE) < 0 && proc_set_principal(99, 5) < 0 && proc_set_principal(pid, APP_A) == 0 && p->principal == APP_A; proc_destroy(pid); r; }));

    if (first) {
        struct proc *a = run_as(70, 0, APP_A, 0, "A");
        int ok = a && sd(a, 0x20) > 0 && sd(a, 0x28) == 16 && sd(a, 0x30) == 16 && eq16(ub(a, 0x400)) && sd(a, 0x38) > 0 && sd(a, 0x50) == 16;
        note = a ? (fs_id_t)ud(a, 0x20) : 0; pub = a ? (fs_id_t)ud(a, 0x38) : 0;
        destroy_all();
        check("program A (EL0, identity 21): creates file 'note' and 'pub', writes 16 bytes into each through SYS_FS, reads 'note' back identical", ok);
        struct fs_stat st; uint8_t b[16];
        int meta = fs_stat(0, note, &st) == FS_OK && st.owner == APP_A && st.perm == 3 && st.size == 16 && fs_read(0, note, 0, b, 16) == 16 && eq16(b);
        int meta2 = fs_stat(0, pub, &st) == FS_OK && st.owner == APP_A && st.perm == 7 && st.size == 16;
        check("the filesystem records owner 21 (assigned by the kernel, not supplied by the program), permissions 3 and 7, and the exact bytes", meta && meta2);
    } else {
        fs_lookup(0, "pub", &pub);
    }

    /* ---- the same refusal checks on every boot ---- */
    {
        struct proc *b = run_as(71, note, APP_B, 0, "B");
        uint8_t pr[16] = { 0 }; int pubread = b && eq16(ub(b, 0x500));
        check("program B (EL0, identity 22) with a GUESSED exact id for 'note': lookup = not found; read, write, delete and chmod = the invalid-id error a nonexistent file gives (the file's existence is not disclosed)",
              b && sd(b, 0x20) == EFS(FS_ENOENT) && sd(b, 0x28) == EFS(FS_EBADID) && sd(b, 0x30) == EFS(FS_EBADID) && sd(b, 0x38) == EFS(FS_EBADID) && sd(b, 0x50) == EFS(FS_EBADID));
        check("program B can read the other-readable file 'pub' (16 exact bytes) but its write to it is permission denied", b && sd(b, 0x58) > 0 && sd(b, 0x60) == 16 && pubread && sd(b, 0x68) == EFS(FS_EACCES));
        destroy_all();
        uint8_t n[16]; struct fs_stat st;
        check("after program B's attempts 'note' and 'pub' are unchanged and still owned by 21",
              fs_read(0, note, 0, n, 16) == 16 && eq16(n) && fs_read(0, pub, 0, pr, 16) == 16 && eq16(pr) && fs_stat(0, note, &st) == FS_OK && st.owner == APP_A && st.perm == 3);
    }
    {
        struct proc *z = run_as(72, 0, 0, 0, "anon");                           /* never given a principal */
        check("a process WITHOUT a file identity gets AE_EPERM for lookup, read, stat and list", z && sd(z, 0x20) == AE_EPERM && sd(z, 0x28) == AE_EPERM && sd(z, 0x30) == AE_EPERM && sd(z, 0x38) == AE_EPERM);
        destroy_all();
    }
    {
        struct proc *h = run_as(73, (uint64_t)(uintptr_t)&kernel_secret, APP_A, note, "host");
        check("hostile arguments from EL0: kernel pointers for read, name, write, list and stat are AE_EFAULT; length -1 is AE_E2BIG; offset -1 is a filesystem argument error",
              h && sd(h, 0x20) == AE_EFAULT && sd(h, 0x28) == AE_EFAULT && sd(h, 0x30) == AE_EFAULT && sd(h, 0x78) == AE_EFAULT && sd(h, 0x80) == AE_EFAULT && sd(h, 0x38) == AE_E2BIG && sd(h, 0x50) == EFS(FS_EARG));
        check("hostile arguments: an unknown operation, a 'format'-like operation number, a 40-byte name and an empty name are all AE_EINVAL (there is no format or raw-disk operation)",
              h && sd(h, 0x58) == AE_EINVAL && sd(h, 0x60) == AE_EINVAL && sd(h, 0x68) == AE_EINVAL && sd(h, 0x70) == AE_EINVAL);
        destroy_all();
        uint8_t n[16];
        check("the kernel survived and 'note' is intact after the hostile calls; the kernel secret was not disclosed or changed", fs_read(0, note, 0, n, 16) == 16 && eq16(n) && kernel_secret == 0x5EC12E7C0FFEEULL && fs_mounted());
    }
    {
        struct proc *r = run_as(72, 0, APP_A, 0, "reader");
        struct user_fs_stat *us = r ? (struct user_fs_stat *)(uintptr_t)(r->data_pa + 0x500) : 0;
        struct user_fs_stat *ul = r ? (struct user_fs_stat *)(uintptr_t)(r->data_pa + 0x600) : 0;
        int n = r ? (int)sd(r, 0x38) : -1, has_note = 0, has_pub = 0, leak = 0;
        for (int i = 0; i < n && i < 8; i++) {
            const char *nm = ul[i].name;
            if (nm[0] == 'n' && nm[1] == 'o' && nm[2] == 't' && nm[3] == 'e' && !nm[4]) has_note = 1;
            if (nm[0] == 'p' && nm[1] == 'u' && nm[2] == 'b' && !nm[3]) has_pub = 1;
            if (nm[0] == 'p' && nm[1] == 'e') leak = 1;                          /* persist.txt belongs to principal 7 and is private */
        }
        check(first ? "program R (a NEW process, identity 21): looks up 'note', reads the 16 bytes, stat shows owner 21 / perm 3 / size 16, list shows its files"
                    : "after the RESTART: a new process with identity 21 finds 'note' written by an earlier boot, reads the same 16 bytes, stat shows owner 21 / perm 3 / size 16, list shows its files",
              r && (fs_id_t)sd(r, 0x20) == note && sd(r, 0x28) == 16 && eq16(ub(r, 0x400)) && sd(r, 0x30) == 0 && us->size == 16 && us->owner == APP_A && us->perm == 3 &&
              us->name[0] == 'n' && has_note && has_pub);
        check("list shows a program only files it may read: the private file 'persist.txt' of another owner is not listed", r && !leak);
        destroy_all();
    }
    {
        /* ---- syscall boundary matrix (role 74): the checks that need a REAL EL0 caller. Identity 24 owns a 16-byte file, 'note' (owner 21) is hidden from it. ---- */
        #define APP_C 24u
        fs_id_t own = 0, stale = 0; uint8_t back[16];
        int ok = fs_create(APP_C, "own", 3, &own) == FS_OK && fs_write(APP_C, own, 0, DATA16, 16) == 16 && fs_create(APP_C, "tmp", 3, &stale) == FS_OK && fs_delete(APP_C, stale) == FS_OK;
        uint32_t files0 = fs_file_count(), uf = 99, ubk = 99;
        int pid = ok ? proc_create_image(0, own, "bounds", fs_bounds_start, (size_t)(fs_bounds_end - fs_bounds_start)) : -1;
        struct proc *p = pid > 0 ? proc_get(pid) : 0;
        if (p) { proc_set_principal(pid, APP_C); p->ctx.x[2] = note; p->ctx.x[3] = stale; p->ctx.x[4] = (uint64_t)(uintptr_t)&kernel_secret; }
        int ran = p && sched_run(4000) == SCHED_ALL_DONE;
        uint64_t sentinel = ran ? ud(p, 0xFF8) : 0;
        int size_ok = 0, owner_ok = 0;
        if (ran) {
            const uint8_t *m = ub(p, 0x501);                                  /* the misaligned stat record: id(8) size(4) owner(4) perm(4) ... */
            size_ok = m[8] == 16 && m[9] == 0 && m[10] == 0 && m[11] == 0;
            owner_ok = m[12] == APP_C && m[13] == 0 && m[14] == 0 && m[15] == 0;
        }
        #define S(off) (ran ? sd(p, (off)) : 0x7FFFFFFF)
        check("EL0 boundary: a name or buffer that straddles the unmapped guard page after the data page is AE_EFAULT for lookup, create, read (copy_to_user) and write (copy_from_user); the 8 bytes of user memory before the guard are untouched",
              ran && S(0x100) == AE_EFAULT && S(0x108) == AE_EFAULT && S(0x110) == AE_EFAULT && S(0x118) == AE_EFAULT && sentinel == 0x5A5A5A5A5A5A5A5AULL);
        check("EL0 boundary: a name with an embedded NUL is AE_EINVAL, a name with a slash is FS_ENAME, CREATE permission bits above 0xF (0x10 and -1) and SET_PERM bits 0x10 / -1 are AE_EINVAL",
              ran && S(0x120) == AE_EINVAL && S(0x128) == EFS(FS_ENAME) && S(0x130) == AE_EINVAL && S(0x138) == AE_EINVAL && S(0x140) == AE_EINVAL && S(0x148) == AE_EINVAL);
        check("EL0 boundary: LIST with 0 entries, 17 entries (the maximum is 16) and -1 entries is AE_EINVAL", ran && S(0x150) == AE_EINVAL && S(0x158) == AE_EINVAL && S(0x160) == AE_EINVAL);
        check("EL0 boundary: writing at offset size+1 (a hole) is a filesystem argument error and the file keeps its 16 bytes", ran && S(0x168) == EFS(FS_EARG) && fs_read(APP_C, own, 0, back, 16) == 16 && eq16(back));
        check("EL0 boundary: READ into the read-only code page, READ into a kernel address, STAT into a buffer straddling the guard page and STAT into the code page are AE_EFAULT; a misaligned STAT target is accepted and receives the exact record (size 16, owner 24)",
              ran && S(0x170) == AE_EFAULT && S(0x178) == AE_EFAULT && S(0x180) == AE_EFAULT && S(0x190) == AE_EFAULT && S(0x188) == 0 && size_ok && owner_ok && kernel_secret == 0x5EC12E7C0FFEEULL);
        check("EL0 boundary: 64-bit values whose low 32 bits look harmless are not truncated: offset 0xFFFF...F0 and 2^32+1 are FS_EARG, lengths 2^32+16 and 2^32+4 are AE_E2BIG, name length 2^32+4 is AE_EINVAL, pointers that wrap past 2^64 are AE_EFAULT",
              ran && S(0x198) == EFS(FS_EARG) && S(0x1A8) == EFS(FS_EARG) && S(0x1A0) == AE_E2BIG && S(0x1B0) == AE_E2BIG && S(0x1C8) == AE_EINVAL && S(0x1B8) == AE_EFAULT && S(0x1C0) == AE_EFAULT);
        int same5 = ran;
        for (int i = 0; ran && i < 5; i++) if (S(0x200 + 8 * i) != EFS(FS_EBADID) || S(0x240 + 8 * i) != EFS(FS_EBADID)) same5 = 0;
        check("EL0 identities: read, write, stat, delete and chmod on a file this identity cannot discover return exactly the invalid-id error that a stale (deleted) id returns, so the caller cannot tell the two apart",
              same5);
        int fsok = fs_mounted() && fs_file_count() == files0 && fs_usage(APP_C, &uf, &ubk) == FS_OK && uf == 1 && ubk == 1 && fs_read(APP_C, own, 0, back, 16) == 16 && eq16(back);
        uint8_t nb[16]; int note_ok = fs_read(0, note, 0, nb, 16) == 16 && eq16(nb);
        #undef S
        destroy_all();
        check("after the whole boundary matrix the kernel is alive, no file was created or changed (count, usage, 'own' and 'note' identical), and the kernel secret is intact", fsok && note_ok && kernel_secret == 0x5EC12E7C0FFEEULL);
        fs_delete(APP_C, own);
        #undef APP_C
    }
    {
        /* ---- quota seen from EL0 (role 75): identity 25 may own 8 files and 96 blocks ---- */
        #define APP_Q 25u
        int qpid = proc_create_image(0, 0, "quota", fs_quota_start, (size_t)(fs_quota_end - fs_quota_start));
        struct proc *q = qpid > 0 ? proc_get(qpid) : 0;
        if (q) proc_set_principal(qpid, APP_Q);
        if (q && sched_run(8000) != SCHED_ALL_DONE) q = 0;
        int made = q != 0, refused = q != 0, wrote = q != 0, wq = q && sd(q, 0x5F8) == EFS(FS_EQUOTA);
        for (int i = 0; q && i < 8; i++) if (sd(q, 0x400 + 8 * i) <= 0) made = 0;
        if (q && (sd(q, 0x440) != EFS(FS_EQUOTA) || sd(q, 0x448) != EFS(FS_EQUOTA))) refused = 0;
        for (int i = 0; q && i < 12; i++) if (sd(q, 0x500 + 8 * i) != 4096) wrote = 0;
        uint32_t uf = 0, ubk = 0; fs_id_t x = 0;
        int acct = fs_usage(APP_Q, &uf, &ubk) == FS_OK && uf == 8 && ubk == 96;
        int other = fs_create(APP_B, "q-other", 3, &x) == FS_OK && fs_delete(APP_B, x) == FS_OK;
        destroy_all();
        { static struct fs_stat l[FS_MAX_FILES]; int n = fs_list(0, l, FS_MAX_FILES); for (int i = 0; i < n; i++) if (l[i].owner == APP_Q) fs_delete(0, l[i].id); }
        check("EL0 quota: an identity creates 8 files, the 9th and 10th are FS_EQUOTA; two 24 KiB files (96 blocks) fit and one more byte is FS_EQUOTA; the kernel sees 8 files / 96 blocks and another identity is unaffected",
              made && refused && wrote && wq && acct && other);
        #undef APP_Q
    }
    {
        /* ---- no filesystem: every file call from EL0 reports the mount error ---- */
        fs_unmount();
        struct proc *u = run_as(72, 0, APP_A, 0, "unmounted");
        int ok = u && sd(u, 0x20) == EFS(FS_EMOUNT) && sd(u, 0x28) == EFS(FS_EMOUNT) && sd(u, 0x30) == EFS(FS_EMOUNT) && sd(u, 0x38) == EFS(FS_EMOUNT);
        destroy_all();
        fs_id_t again = 0;
        check("EL0 with the filesystem unmounted: lookup, read, stat and list all return FS_EMOUNT (no panic), and a remount restores access to the same files",
              ok && fs_mount() == FS_OK && fs_lookup(0, "note", &again) == FS_OK && again == note);
    }
    {
        /* PID reuse: the process that held file identity 21 exits; a later process in the SAME pid slot must not inherit it */
        int pid1 = proc_create(72, 0, "first"); proc_set_principal(pid1, APP_A); proc_destroy(pid1);
        /* Role 71 is handed the REAL id of 'note' (like the guessed-id check above): the read must be refused for a PERMISSION reason.
         * (An earlier version ran the lookup-then-read program here, so the read used the failed lookup's error code as an id and was
         * rejected as a bad id, which proved nothing about permissions.) */
        struct proc *x = run_as(71, note, APP_B, 0, "reuseB");
        int same_slot = x && x->pid == pid1;
        uint8_t kept[16];
        check("PID reuse: a new process created in the pid slot of an identity-21 process, given identity 22, cannot find 'note' by name, gets the invalid-id error (not application 21's data and not a revealing permission error) when it reads, writes, deletes or changes it by its real id, and the file is unchanged",
              same_slot && sd(x, 0x20) == EFS(FS_ENOENT) && sd(x, 0x28) == EFS(FS_EBADID) && sd(x, 0x30) == EFS(FS_EBADID) && sd(x, 0x38) == EFS(FS_EBADID) && sd(x, 0x50) == EFS(FS_EBADID) &&
              fs_read(0, note, 0, kept, 16) == 16 && eq16(kept));
        destroy_all();
        struct proc *y = run_as(72, 0, 0, 0, "reuseNone");
        check("PID reuse: a new process in the same slot with no identity is refused (AE_EPERM)", y && y->pid == pid1 && sd(y, 0x20) == AE_EPERM);
        destroy_all();
        struct proc *w = run_as(72, 0, APP_A, 0, "reuseA");
        check("identity belongs to the application, not the pid: a new process the KERNEL explicitly gives identity 21 can read 'note' (by design)", w && sd(w, 0x28) == 16 && eq16(ub(w, 0x400)));
        destroy_all();
    }
    check("no leaks: all physical pages are back after the file-access scenarios", free_pages() == pages0);
}
#else
void fsproc_run(void) {}
#endif

int fsproc_summary(void)
{
#ifdef AURORA_FS_TEST
    if (npass + nfail == 0) return 0;
    console_puts("AURORA-FSPROC-SELFTEST: "); console_dec((uint64_t)npass); console_puts(" passed, ");
    console_dec((uint64_t)nfail); console_puts(" failed\n");
#endif
    return nfail;
}
