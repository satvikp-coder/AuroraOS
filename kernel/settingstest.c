/*
 * kernel/settingstest.c — tests of the native settings registry. Compiled only into AURORA_SET_TEST kernels, run on a disposable disk provisioned by
 * tests/tools/mkafs that holds the installer-owned EL0 client "setapp.elf". The registry persists in "settings.v1" on that disk, so every boot starts from
 * what the previous boots saved: boot 1 finds NO settings file (defaults), later boots must find exactly what the last boot's EL0 program saved.
 * Output: "[set-test] <name>: PASS|FAIL", "[set] ..." evidence lines, "AURORA-SET-SELFTEST: N passed, M failed".
 */
#include <blk.h>
#include <console.h>
#include <fs.h>
#include <launch.h>
#include <mm.h>
#include <proc.h>
#include <settings.h>

static int nfail;
#ifdef AURORA_SET_TEST
static int npass;
static void check(const char *name, int ok)
{
    console_puts("[set-test] "); console_puts(name); console_puts(ok ? ": PASS\n" : ": FAIL\n");
    if (ok) npass++; else nfail++;
}
static uint64_t free_pages(void) { struct pmm_stats s; pmm_get_stats(&s); return s.free_pages; }
static void destroy_all(void) { for (int pid = 1; pid <= PROC_MAX; pid++) if (proc_get(pid)) proc_destroy(pid); }
static uint32_t gv(uint32_t id) { uint32_t v = 0xFFFFFFFFu; settings_get(0, id, &v); return v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static int same(const uint8_t *a, const uint8_t *b, uint32_t n) { for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }

/* an independent encoder of the on-disk record (the test does not call the service's own) */
static void record(uint8_t *o, uint32_t version, const uint32_t *v)
{
    o[0] = 'A'; o[1] = 'S'; o[2] = 'E'; o[3] = 'T'; put32(o + 4, version); put32(o + 8, 6);
    for (int i = 0; i < 6; i++) put32(o + 12 + 4 * i, v[i]);
    put32(o + 36, fs_crc32(o, 36));
}
static int file_id(fs_id_t *id) { return fs_lookup_owner(PRINCIPAL_SETTINGS, "settings.v1", id) == FS_OK; }
static int write_raw(const uint8_t *b, uint32_t n)             /* put arbitrary bytes into the settings file as its owner, the way a bug or a newer kernel could */
{
    fs_id_t id; int rc;
    if (!file_id(&id)) { rc = fs_create(PRINCIPAL_SETTINGS, "settings.v1", FS_PERM_OWNER_R | FS_PERM_OWNER_W, &id); if (rc) return 0; }
    else if (fs_delete(PRINCIPAL_SETTINGS, id) != FS_OK || fs_create(PRINCIPAL_SETTINGS, "settings.v1", FS_PERM_OWNER_R | FS_PERM_OWNER_W, &id) != FS_OK) return 0;   /* also lets the size change */
    return n == 0 || fs_write(PRINCIPAL_SETTINGS, id, 0, b, n) == (int)n;
}
static int all_default(void) { for (uint32_t i = 1; i <= 6; i++) if (gv(i) != settings_default(i)) return 0; return 1; }

/* launch an application, run it to completion and return its exit code (or -1000 if it could not run to a normal exit) */
static int run_app(const char *app)
{
    int pid = 0, detail = 0;
    if (launch_app(app, &pid, &detail) != LAUNCH_OK) return -1000;
    struct proc *p = proc_get(pid);
    uint64_t r = sched_run(20000);
    int code = (r == SCHED_ALL_DONE && p && p->state == PROC_TERMINATED) ? p->exit_code : -1000;
    destroy_all();
    return code;
}

void settingstest_run(void)
{
    if (!fs_mounted()) { console_puts("[set] no mounted filesystem: settings tests not run\n"); return; }
    uint64_t pages0 = free_pages();
    destroy_all(); launch_reset(); settings_revoke_all(); blk_test_inject_clear();

    /* ---- identities and capabilities (kernel code decides) ---- */
    int g = settings_grant(201, CAP_SET_APPEARANCE | CAP_SET_ATTENTION) == SETTINGS_OK && settings_grant(203, CAP_SET_SYSTEM) == SETTINGS_OK;
    check("capabilities: kernel code grants an identity capabilities; the kernel, none, the installer and the settings owner cannot be granted any, and empty or unknown capability bits are refused",
          g && settings_grant(0, CAP_SET_ALL) != 0 && settings_grant(PRINCIPAL_NONE, CAP_SET_ALL) != 0 && settings_grant(PRINCIPAL_INSTALLER, CAP_SET_ALL) != 0 &&
          settings_grant(PRINCIPAL_SETTINGS, CAP_SET_ALL) != 0 && settings_grant(210, 0) != 0 && settings_grant(210, 0x100) != 0);

    /* ---- load: boot 1 has nothing saved, later boots find what the previous boot saved ---- */
    int st = settings_load();
    uint32_t prev = gv(SET_TEXT_SCALE);
    int first = st == SETLOAD_MISSING;
    console_puts("[set] boot: status="); console_dec((uint64_t)st); console_puts(first ? " (no saved settings: defaults)" : " (loaded from disk)");
    console_puts(" theme="); console_dec(gv(SET_THEME)); console_puts(" text_scale="); console_dec(prev); console_puts(" reduced_motion="); console_dec(gv(SET_REDUCED_MOTION));
    console_puts(" focus="); console_dec(gv(SET_FOCUS_MODE)); console_puts(" aura="); console_dec(gv(SET_AURA_MODE)); console_puts(" notifications="); console_dec(gv(SET_NOTIFICATIONS)); console_puts("\n");
    check(first ? "boot 1: no settings file exists; the registry starts from the typed defaults (theme auto, text 100%, motion normal, focus off, aura suggest, notifications all)"
                : "after the RESTART: the settings saved by the previous boot's EL0 program are loaded from AuroraFS exactly (theme dark, reduced motion, focus rest, notifications priority, aura act-with-review)",
          first ? (all_default() && gv(SET_TEXT_SCALE) == 100)
                : (st == SETLOAD_OK && gv(SET_THEME) == 2 && gv(SET_REDUCED_MOTION) == 1 && gv(SET_FOCUS_MODE) == 2 && gv(SET_NOTIFICATIONS) == 1 && gv(SET_AURA_MODE) == 2 && prev >= 75 && prev <= 200));
    uint32_t newscale = prev >= 190 ? 100 : prev + 10;

    /* ---- validation and authorisation matrix, kernel API: refused calls change nothing and write NOTHING to the device ---- */
    {
        uint32_t snap[7]; for (uint32_t i = 1; i <= 6; i++) snap[i] = gv(i);
        uint64_t w0 = blk_writes_done(); uint32_t v;
        int ok = 1;
        ok &= settings_set(202, SET_THEME, 1) == SETTINGS_EPERM;                  /* a real identity with no write capability */
        ok &= settings_get(202, SET_THEME, &v) == SETTINGS_OK && v == snap[SET_THEME];   /* ...can read */
        ok &= settings_set(201, SET_AURA_MODE, 0) == SETTINGS_EPERM;              /* appearance + attention do not include system policy */
        ok &= settings_set(203, SET_THEME, 1) == SETTINGS_EPERM;                  /* system does not include appearance */
        ok &= settings_set(PRINCIPAL_NONE, SET_THEME, 1) == SETTINGS_EPERM && settings_get(PRINCIPAL_NONE, SET_THEME, &v) == SETTINGS_EPERM;
        ok &= settings_set(201, 99, 1) == SETTINGS_EID && settings_set(201, 0, 1) == SETTINGS_EID && settings_get(201, 0, &v) == SETTINGS_EID;
        ok &= settings_set(201, SET_THEME, 3) == SETTINGS_EVALUE && settings_set(201, SET_THEME, 0x100000002ULL) == SETTINGS_EVALUE;
        ok &= settings_set(201, SET_TEXT_SCALE, 74) == SETTINGS_EVALUE && settings_set(201, SET_TEXT_SCALE, 201) == SETTINGS_EVALUE && settings_set(201, SET_TEXT_SCALE, ~0ULL) == SETTINGS_EVALUE;
        ok &= settings_set(201, SET_REDUCED_MOTION, 2) == SETTINGS_EVALUE && settings_set(201, SET_FOCUS_MODE, 4) == SETTINGS_EVALUE;
        ok &= settings_set(201, SET_NOTIFICATIONS, 3) == SETTINGS_EVALUE && settings_set(203, SET_AURA_MODE, 3) == SETTINGS_EVALUE;
        ok &= settings_get(201, SET_THEME, 0) == SETTINGS_EARG;
        for (uint32_t i = 1; i <= 6; i++) if (gv(i) != snap[i]) ok = 0;
        check("authorisation and validation: a read-only identity, a wrong-capability identity, no identity, unknown ids, wrong types, out-of-range and 64-bit-truncating values are all refused with the exact error; every value is unchanged and no write reached the device",
              ok && blk_writes_done() == w0);
    }

    /* ---- the EL0 client programs (loaded from the disk): writer, guest, system UI, reader ---- */
    int reg = launch_register("setwriter", "setapp.elf", 201, ((uint64_t)1 << 32) | newscale) == LAUNCH_OK && launch_register("setguest", "setapp.elf", 202, (uint64_t)2 << 32) == LAUNCH_OK &&
              launch_register("setsysui", "setapp.elf", 203, (uint64_t)3 << 32) == LAUNCH_OK && launch_register("setreader", "setapp.elf", 201, ((uint64_t)4 << 32) | newscale) == LAUNCH_OK;
    int wcode = reg ? run_app("setwriter") : -2000;
    console_puts("[set] EL0 writer exit code "); console_dec((uint64_t)(wcode < 0 ? 999 : wcode)); console_puts("\n");
    check("EL0 writer (identity 201, capabilities appearance + attention): saves text scale, theme, reduced motion, focus and notifications, reads them back, is refused the system-only aura mode, and every invalid request (unknown id, wrong type, out of range, 2^32+2, unknown operation); exit 0",
          wcode == 0 && gv(SET_TEXT_SCALE) == newscale && gv(SET_THEME) == 2 && gv(SET_REDUCED_MOTION) == 1 && gv(SET_FOCUS_MODE) == 2 && gv(SET_NOTIFICATIONS) == 1);
    console_puts("[set] saved text_scale="); console_dec(gv(SET_TEXT_SCALE)); console_puts("\n");
    {
        uint32_t ram[7]; for (uint32_t i = 1; i <= 6; i++) ram[i] = gv(i);
        int lr = settings_load(); int eq = lr == SETLOAD_OK;
        for (uint32_t i = 1; i <= 6; i++) if (gv(i) != ram[i]) eq = 0;
        console_puts("[set] re-read from the disk: text_scale="); console_dec(gv(SET_TEXT_SCALE)); console_puts("\n");
        check("saved before it took effect: re-reading the record from the disk (not RAM) gives exactly the values the EL0 writer set", eq);
    }
    {
        uint32_t before[7]; for (uint32_t i = 1; i <= 6; i++) before[i] = gv(i);
        uint64_t w0 = blk_writes_done();
        int code = reg ? run_app("setguest") : -2000; int unchanged = 1;
        for (uint32_t i = 1; i <= 6; i++) if (gv(i) != before[i]) unchanged = 0;
        check("EL0 guest (identity 202, no capability): reads every setting, every set and reset is 'not permitted', nothing changes and nothing is written", code == 0 && unchanged && blk_writes_done() == w0);
        int sc2 = reg ? run_app("setsysui") : -2000;
        check("EL0 system UI (identity 203, system capability only): may write the aura mode (and is refused appearance); the aura mode is saved and survives a re-read",
              sc2 == 0 && gv(SET_AURA_MODE) == 2 && gv(SET_THEME) == 2 && settings_load() == SETLOAD_OK && gv(SET_AURA_MODE) == 2);
        int rc4 = reg ? run_app("setreader") : -2000;
        check("EL0 reader (a new process, identity 201): reads back exactly the saved values from the registry", rc4 == 0);
    }

    /* ---- the settings file is invisible and untouchable to every program ---- */
    {
        fs_id_t id; struct fs_stat st2; uint8_t b[64];
        int have = file_id(&id) && fs_stat(0, id, &st2) == FS_OK;
        int ok = have && st2.owner == PRINCIPAL_SETTINGS && st2.perm == (FS_PERM_OWNER_R | FS_PERM_OWNER_W) && st2.size == 40;
        ok &= fs_read(0, id, 0, b, 40) == 40 && b[0] == 'A' && b[1] == 'S' && b[2] == 'E' && b[3] == 'T' && b[4] == 1;
        fs_id_t x;
        ok &= fs_lookup(201, "settings.v1", &x) == FS_ENOENT && fs_read(201, id, 0, b, 40) == FS_EBADID && fs_write(201, id, 0, b, 4) == FS_EBADID &&
              fs_delete(201, id) == FS_EBADID && fs_set_perm(201, id, 0xF) == FS_EBADID && fs_stat(201, id, &st2) == FS_EBADID;
        check("the saved record (40 bytes, 'ASET' version 1) is owned by the settings principal with no permission for anyone else: a program cannot find, read, write, chmod or delete it, and its id is indistinguishable from a bad id", ok);
    }

    /* ---- corrupt records: defaults, never wrong values; the next write repairs ---- */
    {
        uint32_t good[6] = { 2, newscale, 1, 2, 2, 1 }, bad_val[6] = { 7, newscale, 1, 2, 2, 1 };
        uint8_t r[48]; int ok = 1;
        for (int variant = 0; variant < 6; variant++) {
            record(r, 1, good); uint32_t n = 40;
            if (variant == 0) r[20] ^= 0x01;                                   /* a flipped bit behind the filesystem's own checksum: the record's CRC catches it */
            else if (variant == 1) record(r, 1, bad_val);                      /* valid CRC, theme = 7: out of range */
            else if (variant == 2) n = 20;                                     /* truncated */
            else if (variant == 3) r[0] = 'X';                                 /* wrong magic */
            else if (variant == 4) n = 3;                                      /* garbage */
            else { put32(r + 8, 5); put32(r + 36, fs_crc32(r, 36)); }          /* wrong count with a valid CRC */
            if (!write_raw(r, n)) { ok = 0; break; }
            int lr = settings_load();
            if (lr != SETLOAD_CORRUPT || !all_default()) ok = 0;
        }
        int heal = settings_set(201, SET_THEME, 1) == SETTINGS_OK && settings_load() == SETLOAD_OK && gv(SET_THEME) == 1;     /* a write replaces the broken record */
        check("corrupt record (flipped bit, out-of-range value behind a valid CRC, truncated, wrong magic, garbage, wrong count): load reports CORRUPT and every setting is its default (never a guessed value); the next valid write replaces the broken file", ok && heal);
        settings_set(0, SET_THEME, 2);
    }

    /* ---- a record from a different version: kept, never overwritten ---- */
    {
        uint32_t v2[6] = { 1, 150, 0, 3, 0, 2 };
        uint8_t r[40], back[40]; record(r, 2, v2);
        int ok = write_raw(r, 40) && settings_load() == SETLOAD_VERSION && all_default();
        ok &= settings_set(201, SET_THEME, 1) == SETTINGS_EVERSION && settings_set(0, SET_THEME, 1) == SETTINGS_EVERSION && settings_reset(0, SET_THEME) == SETTINGS_EVERSION;
        fs_id_t id; ok &= file_id(&id) && fs_read(0, id, 0, back, 40) == 40 && same(r, back, 40);      /* byte for byte untouched */
        ok &= settings_set(202, SET_THEME, 1) == SETTINGS_EPERM && settings_set(201, SET_THEME, 9) == SETTINGS_EVALUE;   /* validation and authorisation still come first */
        check("version mismatch (a valid record of version 2): load reports VERSION, RAM holds defaults, every write (even the kernel's) is SETTINGS_EVERSION, and the file is byte-for-byte untouched", ok);
        uint32_t good[6] = { 2, newscale, 1, 2, 2, 1 }; record(r, 1, good);
        write_raw(r, 40); settings_load();
    }

    /* ---- missing file ---- */
    {
        fs_id_t id; int ok = file_id(&id) && fs_delete(PRINCIPAL_SETTINGS, id) == FS_OK;
        ok &= settings_load() == SETLOAD_MISSING && all_default();
        ok &= settings_set(201, SET_TEXT_SCALE, 125) == SETTINGS_OK && file_id(&id) && settings_load() == SETLOAD_OK && gv(SET_TEXT_SCALE) == 125;
        check("missing file: load reports MISSING with defaults (not an error); the first valid write creates a private, valid record that loads back", ok);
        settings_set(0, SET_TEXT_SCALE, newscale); settings_set(0, SET_THEME, 2); settings_set(0, SET_REDUCED_MOTION, 1); settings_set(0, SET_FOCUS_MODE, 2); settings_set(0, SET_NOTIFICATIONS, 1); settings_set(0, SET_AURA_MODE, 2);
        settings_load();
    }

    /* ---- storage failure: never claim a value is saved when it may not be ---- */
    {
        int ok = 1;
        settings_set(0, SET_THEME, 1); settings_load();
        /* A. definite: the data-block write is rejected before it reaches the device */
        blk_test_inject_clear(); blk_test_inject_write(BLK_INJ_REJECT, 0);
        int r1 = settings_set(201, SET_THEME, 2);
        ok &= r1 == SETTINGS_ESTORE && gv(SET_THEME) == 1 && fs_mounted() && settings_last_store_error() != FS_EUNCERTAIN && settings_last_store_error() < 0;
        blk_test_inject_clear();
        ok &= settings_load() == SETLOAD_OK && gv(SET_THEME) == 1 && settings_set(201, SET_THEME, 2) == SETTINGS_OK && settings_load() == SETLOAD_OK && gv(SET_THEME) == 2;   /* a retry works */
        check("storage failure A (definite): a rejected device write returns SETTINGS_ESTORE, RAM and disk keep the old value, the filesystem stays mounted, a retry succeeds and is read back", ok);
        /* B. the record write is lost with an unknown outcome */
        settings_set(0, SET_THEME, 1); settings_load();
        blk_test_inject_clear(); blk_test_inject_write(BLK_INJ_FAIL_UNSENT, 1);
        int r2 = settings_set(201, SET_THEME, 2);
        int ok2 = r2 == SETTINGS_ESTORE && settings_last_store_error() == FS_EUNCERTAIN && gv(SET_THEME) == 1 && !fs_mounted();
        ok2 &= settings_set(201, SET_THEME, 2) == SETTINGS_ESTORE;               /* the filesystem is unmounted: still no claim of success */
        blk_test_inject_clear();
        ok2 &= fs_mount() == FS_OK && settings_load() == SETLOAD_OK && gv(SET_THEME) == 1;
        check("storage failure B (uncertain, lost): SETTINGS_ESTORE with FS_EUNCERTAIN, RAM keeps the old value, further writes keep failing while unmounted, and after a remount the disk shows the OLD value", ok2);
        /* C. the record write landed but was reported as failed */
        blk_test_inject_write(BLK_INJ_FAIL_SENT, 1);
        int r3 = settings_set(201, SET_THEME, 2);
        int ok3 = r3 == SETTINGS_ESTORE && gv(SET_THEME) == 1 && !fs_mounted();
        blk_test_inject_clear();
        ok3 &= fs_mount() == FS_OK && settings_load() == SETLOAD_OK && gv(SET_THEME) == 2;
        check("storage failure C (uncertain, landed): SETTINGS_ESTORE and RAM unchanged (the caller is never told 'saved'), but the remount shows the NEW value because the write did reach the disk - the reload is the truth", ok3);
        settings_set(0, SET_THEME, 2);
    }

    /* ---- quota: the settings principal has the same 8-file limit as anybody ---- */
    {
        fs_id_t id, f[8]; int made = 0, ok = 1; char nm[4];
        settings_set(0, SET_TEXT_SCALE, newscale); settings_load();
        for (int i = 0; i < 7; i++) { nm[0] = 'q'; nm[1] = (char)('0' + i); nm[2] = 0; if (fs_create(PRINCIPAL_SETTINGS, nm, 3, &f[made]) == FS_OK) made++; }
        ok &= made == 7;                                                           /* 7 fillers + the settings file = 8 */
        ok &= settings_set(201, SET_THEME, 1) == SETTINGS_OK && settings_load() == SETLOAD_OK && gv(SET_THEME) == 1;      /* updating an existing record needs no new file or block */
        uint32_t u, ub2; ok &= fs_usage(PRINCIPAL_SETTINGS, &u, &ub2) == FS_OK && u == 8;
        file_id(&id); ok &= fs_delete(PRINCIPAL_SETTINGS, id) == FS_OK && fs_create(PRINCIPAL_SETTINGS, "q7", 3, &f[made]) == FS_OK;   /* now 8 fillers, no settings file */
        ok &= settings_load() == SETLOAD_MISSING;
        int r = settings_set(201, SET_THEME, 2);
        ok &= r == SETTINGS_ESTORE && settings_last_store_error() == FS_EQUOTA && gv(SET_THEME) == 0 && fs_mounted();
        for (int i = 0; i <= made && i < 8; i++) fs_delete(PRINCIPAL_SETTINGS, f[i]);
        ok &= settings_set(201, SET_THEME, 2) == SETTINGS_OK && settings_load() == SETLOAD_OK && gv(SET_THEME) == 2;
        check("quota: with the settings principal at its file limit an update of the existing record still works, but creating the record when it is missing fails with SETTINGS_ESTORE / FS_EQUOTA, RAM keeps its value, and freeing quota lets the write succeed", ok);
    }

    /* ---- leave the canonical state on the disk for the next boot ---- */
    {
        settings_set(0, SET_TEXT_SCALE, newscale); settings_set(0, SET_THEME, 2); settings_set(0, SET_REDUCED_MOTION, 1); settings_set(0, SET_FOCUS_MODE, 2); settings_set(0, SET_NOTIFICATIONS, 1); settings_set(0, SET_AURA_MODE, 2);
        int lr = settings_load();
        console_puts("[set] final saved text_scale="); console_dec(gv(SET_TEXT_SCALE)); console_puts("\n");
        check("the final state is on the disk for the next boot: text scale advanced by 10 (wrapping to 100 above 190), theme dark, motion reduced, focus rest, notifications priority, aura act-with-review",
              lr == SETLOAD_OK && gv(SET_TEXT_SCALE) == newscale && gv(SET_THEME) == 2 && gv(SET_REDUCED_MOTION) == 1 && gv(SET_FOCUS_MODE) == 2 && gv(SET_NOTIFICATIONS) == 1 && gv(SET_AURA_MODE) == 2);
    }
    check("no leaks: all physical pages are back after the settings scenarios", free_pages() == pages0);
}
#else
void settingstest_run(void) {}
#endif

int settingstest_summary(void)
{
#ifdef AURORA_SET_TEST
    if (npass + nfail == 0) return 0;
    console_puts("AURORA-SET-SELFTEST: "); console_dec((uint64_t)npass); console_puts(" passed, "); console_dec((uint64_t)nfail); console_puts(" failed\n");
#endif
    return nfail;
}
