/* tests/kernel-host/ledger_test.c - the durable Passport ledger against a fake AuroraFS with fault injection: whole-file copy-on-write writes that are atomic,
 * a crash (power cut) injected BEFORE or AFTER every single write of a mutation sequence, FS_EUNCERTAIN at every write, definite refusals, read-back
 * mismatches, and every damaged/newer/rolled-back file combination at load. The invariant under test: after ANY failure and a reboot the table is exactly
 * the state before or the state after the interrupted mutation (never a mix, never a grant nobody made), an operation that returned OK is durable, and a
 * damaged or inconsistent ledger denies everything and is left untouched. Host, ASan/UBSan. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fs.h>
#include <ledger.h>
#include <principal.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

/* ---------------- fake filesystem ---------------- */
#define NF 4
static struct { int used; char name[32]; uint32_t size; uint8_t d[1024]; } F[NF];
static int mounted = 1, ro = 0;
static int nwrites;                                   /* successful + attempted writes since the last reset() */
static int crash_before = 0, crash_after = 0, uncertain_at = 0, uncertain_applies = 0, fail_at = 0, fail_code = 0, corrupt_readback_at = 0, dead = 0;
static void reset_faults(void) { nwrites = 0; crash_before = crash_after = uncertain_at = uncertain_applies = fail_at = fail_code = corrupt_readback_at = dead = 0; }
static void wipe(void) { memset(F, 0, sizeof F); mounted = 1; ro = 0; reset_faults(); }

static int f_mounted(void) { return mounted; }
static int f_readonly(void) { return ro; }
static int find(const char *n) { for (int i = 0; i < NF; i++) if (F[i].used && !strcmp(F[i].name, n)) return i; return -1; }
static int f_lookup(const char *n, uint64_t *id) { if (dead) return FS_EIO; int i = find(n); if (i < 0) return FS_ENOENT; *id = (uint64_t)i + 1; return FS_OK; }
static int f_create(const char *n, uint64_t *id) { if (dead) return FS_EIO; if (ro) return FS_ERO; for (int i = 0; i < NF; i++) if (!F[i].used) { F[i].used = 1; strcpy(F[i].name, n); F[i].size = 0; *id = (uint64_t)i + 1; return FS_OK; } return FS_ENFILE; }
static int f_size(uint64_t id, uint32_t *s) { if (dead) return FS_EIO; if (id < 1 || id > NF || !F[id - 1].used) return FS_EBADID; *s = F[id - 1].size; return FS_OK; }
static int f_read(uint64_t id, void *b, uint32_t len) { if (dead) return FS_EIO; if (id < 1 || id > NF || !F[id - 1].used) return FS_EBADID; uint32_t n = len < F[id - 1].size ? len : F[id - 1].size; memcpy(b, F[id - 1].d, n); return (int)n; }
static int f_write(uint64_t id, const void *b, uint32_t len)
{
    if (dead) return FS_EIO;
    if (ro) return FS_ERO;
    if (id < 1 || id > NF || !F[id - 1].used) return FS_EBADID;
    int k = ++nwrites;
    if (crash_before == k) { dead = 1; return FS_EIO; }                                  /* power cut before the write reached the medium */
    if (fail_at == k) return fail_code;                                                    /* a definite refusal: nothing changed */
    if (uncertain_at == k) { if (uncertain_applies) { memcpy(F[id - 1].d, b, len); F[id - 1].size = len; } return FS_EUNCERTAIN; }
    memcpy(F[id - 1].d, b, len); F[id - 1].size = len;                                   /* whole-file copy-on-write: old or new, never a mix */
    if (corrupt_readback_at == k) F[id - 1].d[len / 2] ^= 0x5A;                           /* the medium silently stored something else */
    if (crash_after == k) { dead = 1; }                                                    /* power cut right after the write reached the medium (the return is lost) */
    return dead ? FS_EIO : (int)len;
}
static const struct lg_io IOV = { f_mounted, f_readonly, f_lookup, f_create, f_size, f_read, f_write };
static int reboot(void) { reset_faults(); return ledger_attach(&IOV); }            /* a fresh boot on whatever the disk holds */

/* ---------------- helpers ---------------- */
static int has(uint32_t p, uint8_t perm) { return ledger_peek(p, 0, perm) || ledger_peek(p, 1, perm); }
static uint32_t pfile_commit(void) { int i = find("passport.v1"); return i < 0 ? 0 : (uint32_t)F[i].d[8] | (uint32_t)F[i].d[9] << 8 | (uint32_t)F[i].d[10] << 16 | (uint32_t)F[i].d[11] << 24; }
static uint32_t afile_commit(void) { int i = find("anchor.v1"); return i < 0 ? 0 : (uint32_t)F[i].d[8] | (uint32_t)F[i].d[9] << 8 | (uint32_t)F[i].d[10] << 16 | (uint32_t)F[i].d[11] << 24; }
static uint32_t crc32(const uint8_t *p, uint32_t n) { uint32_t c = 0xFFFFFFFFu; for (uint32_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1)); } return ~c; }
static void fix_crc_container(int i) { uint32_t c = crc32(F[i].d, 556); F[i].d[556] = (uint8_t)c; F[i].d[557] = (uint8_t)(c >> 8); F[i].d[558] = (uint8_t)(c >> 16); F[i].d[559] = (uint8_t)(c >> 24); }
static void fix_crc_anchor(int i) { uint32_t c = crc32(F[i].d, 28); F[i].d[28] = (uint8_t)c; F[i].d[29] = (uint8_t)(c >> 8); F[i].d[30] = (uint8_t)(c >> 16); F[i].d[31] = (uint8_t)(c >> 24); }
static void set_commit(uint8_t *d, uint32_t v) { d[8] = (uint8_t)v; d[9] = (uint8_t)(v >> 8); d[10] = (uint8_t)(v >> 16); d[11] = (uint8_t)(v >> 24); }
static int table_eq(const uint32_t *pr, const uint8_t *pm, int n, int *extra)   /* the live table holds exactly these durable grants */
{
    int cnt = 0;
    for (int i = 0; i < PP_MAX_GRANTS; i++) if (ledger_passport()->g[i].used && ledger_passport()->g[i].gen == 0) cnt++;
    *extra = cnt - n;
    for (int i = 0; i < n; i++) if (!ledger_peek(pr[i], 0, pm[i])) return 0;
    return cnt == n;
}

/* the mutation script used by the crash enumeration */
struct step { int revoke; uint32_t pr; uint8_t pm; };
static const struct step SCRIPT[] = { {0, 1001, PP_FILES_WRITE}, {0, 1002, PP_NAVIGATE}, {0, 1001, PP_TIMER}, {1, 1001, PP_FILES_WRITE}, {0, 1003, PP_SETTINGS_PROPOSE}, {1, 1002, PP_NAVIGATE} };
#define NS ((int)(sizeof SCRIPT / sizeof SCRIPT[0]))
static int run_step(const struct step *s) { return s->revoke ? ledger_revoke(s->pr, s->pm) : ledger_grant(s->pr, s->pm); }
/* the expected durable table after the first n steps */
static void expect_after(int n, uint32_t *pr, uint8_t *pm, int *cnt)
{
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (!SCRIPT[i].revoke) { int dup = 0; for (int k = 0; k < c; k++) if (pr[k] == SCRIPT[i].pr && pm[k] == SCRIPT[i].pm) dup = 1; if (!dup) { pr[c] = SCRIPT[i].pr; pm[c] = SCRIPT[i].pm; c++; } }
        else for (int k = 0; k < c; k++) if (pr[k] == SCRIPT[i].pr && pm[k] == SCRIPT[i].pm) { pr[k] = pr[c - 1]; pm[k] = pm[c - 1]; c--; break; }
    }
    *cnt = c;
}

int main(void)
{
    printf("ledger tests\n");

    { /* basics: fresh disk, grant, persistence, revoke, idempotence */
        wipe(); int st = reboot();
        CHECK("a fresh disk: state OK, empty table, commit 0, no file created by merely loading", st == LG_OK && ledger_passport_ok() && ledger_commit() == 0 && find("passport.v1") < 0 && find("anchor.v1") < 0);
        CHECK("nothing is granted by default", !has(1001, PP_FILES_WRITE) && !has(1001, PP_NAVIGATE));
        CHECK("grant: OK, durable, commit 1", ledger_grant(1001, PP_FILES_WRITE) == LG_E_OK && ledger_commit() == 1 && has(1001, PP_FILES_WRITE));
        CHECK("the files exist with the fixed sizes and agree (passport commit == anchor commit == 1)", F[find("passport.v1")].size == LEDGER_PASSPORT_BYTES && F[find("anchor.v1")].size == LEDGER_ANCHOR_BYTES && pfile_commit() == 1 && afile_commit() == 1);
        CHECK("an idempotent grant writes nothing and does not advance the commit", ({ int w = nwrites; int r = ledger_grant(1001, PP_FILES_WRITE); r == LG_E_OK && nwrites == w && ledger_commit() == 1; }));
        CHECK("reboot: the grant survives", reboot() == LG_OK && ledger_commit() == 1 && has(1001, PP_FILES_WRITE) && !has(1001, PP_TIMER) && !has(1002, PP_FILES_WRITE));
        CHECK("a second grant and a third principal", ledger_grant(1001, PP_TIMER) == LG_E_OK && ledger_grant(1002, PP_NAVIGATE) == LG_E_OK && ledger_commit() == 3);
        CHECK("revoke: OK, commit 4, gone at once", ledger_revoke(1001, PP_FILES_WRITE) == LG_E_OK && ledger_commit() == 4 && !has(1001, PP_FILES_WRITE) && has(1001, PP_TIMER));
        CHECK("reboot: the revocation survives and the others remain", reboot() == LG_OK && ledger_commit() == 4 && !has(1001, PP_FILES_WRITE) && has(1001, PP_TIMER) && has(1002, PP_NAVIGATE));
        CHECK("revoking what was never granted is refused by the Passport and writes nothing", ({ int w = nwrites; int r = ledger_revoke(4242, PP_TIMER); r == LG_E_PASSPORT && nwrites == w && ledger_commit() == 4 && ledger_passport_ok(); }));
        CHECK("principal 0, none and the reserved identities cannot be granted (nothing written)", ({ int w = nwrites; int r = ledger_grant(0, PP_NAVIGATE) == LG_E_ARG && ledger_grant(0xFFFFFFFFu, PP_NAVIGATE) == LG_E_ARG && ledger_grant(PRINCIPAL_PASSPORT, PP_NAVIGATE) == LG_E_ARG && ledger_grant(0x7F000003u, PP_NAVIGATE) == LG_E_ARG; r && nwrites == w; }));
        CHECK("bad permissions are refused by the Passport (nothing written)", ({ int w = nwrites; int r = ledger_grant(1001, PP_NONE) == LG_E_PASSPORT && ledger_grant(1001, PP_COUNT) == LG_E_PASSPORT; r && nwrites == w; }));
    }
    { /* instance-bound grants are RAM only */
        wipe(); reboot(); ledger_grant(1001, PP_NAVIGATE);
        CHECK("an instance-bound grant works for exactly that generation", ledger_grant_instance(1003, 5, PP_TIMER) == LG_E_OK && ledger_peek(1003, 5, PP_TIMER) && !ledger_peek(1003, 6, PP_TIMER) && !ledger_peek(1003, 0, PP_TIMER));
        CHECK("it did not touch the disk (commit and files unchanged)", ledger_commit() == 1 && pfile_commit() == 1 && afile_commit() == 1);
        CHECK("generation 0 cannot be used through the instance API", ledger_grant_instance(1003, 0, PP_TIMER) == LG_E_ARG);
        ledger_grant(1002, PP_NAVIGATE);
        CHECK("a later durable commit still does not persist it", pfile_commit() == 2);
        CHECK("after a reboot the instance grant is gone and the durable ones remain", reboot() == LG_OK && !ledger_peek(1003, 5, PP_TIMER) && has(1001, PP_NAVIGATE) && has(1002, PP_NAVIGATE));
        ledger_grant_instance(1003, 5, PP_TIMER);
        CHECK("dropping the instance removes it", ledger_drop_instance(5) == 1 && !ledger_peek(1003, 5, PP_TIMER));
    }
    { /* the exhaustive crash enumeration: a power cut BEFORE and AFTER every write of the script */
        int bad = 0, total = 0, committed_seen = 0, notcommitted_seen = 0;
        for (int mode = 0; mode < 2; mode++)                         /* 0 = before the write, 1 = right after it */
            for (int w = 1; w <= 2 * NS; w++) {
                wipe(); reboot();
                if (mode == 0) crash_before = w; else crash_after = w;
                int completed = 0; int inflight = -1;
                for (int s = 0; s < NS; s++) {
                    int rc = run_step(&SCRIPT[s]);
                    if (rc == LG_E_OK) completed = s + 1; else { inflight = s; break; }
                }
                /* a power cut: the process is gone; the next boot sees only the disk */
                int st = reboot();
                total++;
                uint32_t pr[8]; uint8_t pm[8]; int cnt, extra;
                int ok = (st == LG_OK && ledger_passport_ok());
                if (!ok) { bad++; printf("    crash mode %d write %d: state %s\n", mode, w, ledger_state_name(st)); continue; }
                /* the table must be exactly the state before or after the interrupted step; every step that returned OK must be durable */
                int lo = completed, hi = inflight >= 0 ? inflight + 1 : completed;
                int match_lo, match_hi;
                expect_after(lo, pr, pm, &cnt); match_lo = table_eq(pr, pm, cnt, &extra);
                expect_after(hi, pr, pm, &cnt); match_hi = table_eq(pr, pm, cnt, &extra);
                if (!match_lo && !match_hi) { bad++; printf("    crash mode %d write %d: table matches neither step %d nor %d\n", mode, w, lo, hi); }
                if (match_hi && !match_lo) committed_seen++; else notcommitted_seen++;
                /* after the repair boot the files agree */
                if (pfile_commit() != afile_commit() || ledger_commit() != pfile_commit()) { bad++; printf("    crash mode %d write %d: files disagree after the repair (%u vs %u)\n", mode, w, pfile_commit(), afile_commit()); }
                /* and the ledger keeps working */
                if (ledger_grant(9000, PP_NAVIGATE) != LG_E_OK || !has(9000, PP_NAVIGATE)) { bad++; printf("    crash mode %d write %d: cannot mutate after recovery\n", mode, w); }
            }
        CHECK("EVERY crash point (before/after each of 12 writes): the next boot is healthy, the table is exactly the state before or after the interrupted mutation, OK results are durable, the anchor is repaired", bad == 0);
        CHECK("the enumeration really saw both outcomes (committed-but-unacknowledged and not-committed)", committed_seen > 0 && notcommitted_seen > 0 && total == 4 * NS);
    }
    { /* FS_EUNCERTAIN at every write, applied or not */
        int bad = 0;
        for (int applies = 0; applies < 2; applies++)
            for (int w = 1; w <= 2 * NS; w++) {
                wipe(); reboot();
                uncertain_at = w; uncertain_applies = applies;
                int completed = 0, inflight = -1, unk = 0;
                for (int s = 0; s < NS; s++) { int rc = run_step(&SCRIPT[s]); if (rc == LG_E_OK) completed = s + 1; else { inflight = s; unk = (rc == LG_E_UNKNOWN); break; } }
                if (!unk || ledger_state() != LG_FAILED || ledger_passport_ok()) { bad++; printf("    uncertain %d/%d: unk=%d state=%s\n", applies, w, unk, ledger_state_name(ledger_state())); }
                /* frozen: no further mutation, no durable grant honoured */
                if (ledger_grant(9000, PP_NAVIGATE) != LG_E_STATE || ledger_revoke(1001, PP_FILES_WRITE) != LG_E_STATE) bad++;
                for (int s = 0; s < NS; s++) if (ledger_peek(SCRIPT[s].pr, 0, SCRIPT[s].pm)) bad++;                /* fail closed: no durable grant is honoured */
                reset_faults();
                int st = reboot();
                uint32_t pr[8]; uint8_t pm[8]; int cnt, extra;
                expect_after(completed, pr, pm, &cnt); int lo = table_eq(pr, pm, cnt, &extra);
                expect_after(inflight + 1, pr, pm, &cnt); int hi = table_eq(pr, pm, cnt, &extra);
                if (st != LG_OK || (!lo && !hi)) { bad++; printf("    uncertain %d/%d: after reboot state=%s lo=%d hi=%d\n", applies, w, ledger_state_name(st), lo, hi); }
            }
        CHECK("FS_EUNCERTAIN at every write (applied or not): UNKNOWN is reported, the ledger freezes and honours no durable grant, mutations are refused; the reboot is healthy and exactly one of the two states", bad == 0);
    }
    { /* definite refusals keep the ledger healthy; read-back mismatch is treated as unknown */
        wipe(); reboot(); ledger_grant(1001, PP_NAVIGATE);
        fail_at = nwrites + 1; fail_code = FS_ENOSPC;
        int r = ledger_grant(1002, PP_NAVIGATE);
        CHECK("no space on the passport write: NOSPACE, still healthy, table and disk unchanged", r == LG_E_NOSPACE && ledger_passport_ok() && !has(1002, PP_NAVIGATE) && ledger_commit() == 1 && pfile_commit() == 1);
        reset_faults(); fail_at = nwrites + 1; fail_code = FS_EQUOTA;
        CHECK("the owner's quota: NOSPACE, still healthy", ledger_grant(1002, PP_NAVIGATE) == LG_E_NOSPACE && ledger_passport_ok());
        reset_faults(); CHECK("and a retry succeeds", ledger_grant(1002, PP_NAVIGATE) == LG_E_OK && has(1002, PP_NAVIGATE) && ledger_commit() == 2);
        fail_at = nwrites + 1; fail_code = FS_ERO;
        CHECK("a read-only refusal: RDONLY, nothing changed", ledger_grant(1003, PP_NAVIGATE) == LG_E_RDONLY && ledger_state() == LG_RDONLY && !has(1003, PP_NAVIGATE));
        reboot();
        corrupt_readback_at = nwrites + 1;
        CHECK("the medium stores something else than was written: caught by the read-back, UNKNOWN, frozen", ledger_grant(1003, PP_NAVIGATE) == LG_E_UNKNOWN && ledger_state() == LG_FAILED && !has(1003, PP_NAVIGATE));
        CHECK("the damaged file is not mistaken for a good one at the next boot", reboot() == LG_CORRUPT && !has(1001, PP_NAVIGATE) && !has(1002, PP_NAVIGATE));
        wipe(); reboot(); ledger_grant(1001, PP_NAVIGATE); corrupt_readback_at = nwrites + 2;                      /* the ANCHOR read-back fails */
        CHECK("an anchor read-back mismatch is UNKNOWN too", ledger_grant(1002, PP_NAVIGATE) == LG_E_UNKNOWN && ledger_state() == LG_FAILED);
        CHECK("the damaged anchor is detected at the next boot (corrupt), nothing is honoured", reboot() == LG_CORRUPT && !has(1001, PP_NAVIGATE));
    }
    { /* load: damaged, truncated, newer, rolled-back and inconsistent file sets. Files must be left byte-for-byte untouched. */
        struct { const char *name; } dummy; (void)dummy;
        uint8_t ps[1024], as[1024]; uint32_t ps_n, as_n;
        wipe(); reboot(); ledger_grant(1001, PP_NAVIGATE); ledger_grant(1002, PP_TIMER);        /* commit 2, consistent */
        int ip = find("passport.v1"), ia = find("anchor.v1");
        memcpy(ps, F[ip].d, F[ip].size); ps_n = F[ip].size; memcpy(as, F[ia].d, F[ia].size); as_n = F[ia].size;
        #define RESTORE() do { wipe(); F[0].used = 1; strcpy(F[0].name, "passport.v1"); F[0].size = ps_n; memcpy(F[0].d, ps, ps_n); F[1].used = 1; strcpy(F[1].name, "anchor.v1"); F[1].size = as_n; memcpy(F[1].d, as, as_n); } while (0)
        #define SNAP() uint8_t snap[2][1024]; uint32_t sn0 = F[0].size, sn1 = F[1].size; memcpy(snap[0], F[0].d, sizeof snap[0]); memcpy(snap[1], F[1].d, sizeof snap[1])
        #define UNTOUCHED() (F[0].size == sn0 && F[1].size == sn1 && !memcmp(snap[0], F[0].d, sizeof snap[0]) && !memcmp(snap[1], F[1].d, sizeof snap[1]))
        #define DENIES() (!has(1001, PP_NAVIGATE) && !has(1002, PP_TIMER) && !ledger_passport_ok())
        RESTORE(); CHECK("control: the consistent pair loads", reboot() == LG_OK && ledger_commit() == 2 && has(1001, PP_NAVIGATE) && has(1002, PP_TIMER));
        { RESTORE(); F[0].d[100] ^= 1; SNAP(); int st = reboot(); int w = nwrites; int m = ledger_grant(5000, PP_NAVIGATE);
          CHECK("a flipped byte in passport.v1: CORRUPT, empty table, nothing honoured, mutation refused, files untouched", st == LG_CORRUPT && DENIES() && m == LG_E_STATE && nwrites == w && UNTOUCHED()); }
        { RESTORE(); F[0].size = 100; SNAP(); int st = reboot();
          CHECK("passport.v1 truncated: CORRUPT, untouched", st == LG_CORRUPT && DENIES() && UNTOUCHED()); }
        { RESTORE(); F[0].size = 0; SNAP(); int st = reboot();
          CHECK("passport.v1 emptied while the anchor vouches for commit 2: ROLLBACK (not a fresh ledger), untouched", st == LG_ROLLBACK && DENIES() && UNTOUCHED()); }
        { RESTORE(); memset(F[0].d, 0xAB, 560); SNAP(); int st = reboot();
          CHECK("passport.v1 garbage: CORRUPT, untouched", st == LG_CORRUPT && DENIES() && UNTOUCHED()); }
        { RESTORE(); F[0].size = 0; F[1].size = 0; SNAP(); int st = reboot();
          CHECK("both files empty shells (an interrupted first creation): a fresh, empty, healthy ledger", st == LG_OK && ledger_commit() == 0 && !has(1001, PP_NAVIGATE)); (void)snap; (void)sn0; (void)sn1; }
        { RESTORE(); F[0].d[4] = 2; SNAP(); int st = reboot(); int w = nwrites; int m = ledger_grant(5000, PP_NAVIGATE);          /* newer container version, otherwise junk */
          CHECK("a NEWER passport.v1: NEWER, empty table, never overwritten by this code, mutation refused", st == LG_NEWER && DENIES() && m == LG_E_STATE && nwrites == w && UNTOUCHED()); }
        { RESTORE(); F[1].d[4] = 7; SNAP(); int st = reboot();
          CHECK("a NEWER anchor.v1: NEWER, untouched", st == LG_NEWER && DENIES() && UNTOUCHED()); }
        { RESTORE(); F[1].d[10] ^= 1; SNAP(); int st = reboot();
          CHECK("a damaged anchor.v1 (bad CRC): CORRUPT, untouched", st == LG_CORRUPT && DENIES() && UNTOUCHED()); }
        { RESTORE(); F[1].size = 10; SNAP(); int st = reboot();
          CHECK("a truncated anchor.v1: CORRUPT, untouched", st == LG_CORRUPT && DENIES() && UNTOUCHED()); }
        { RESTORE(); set_commit(F[1].d, 3); fix_crc_anchor(1); SNAP(); int st = reboot();               /* the anchor vouches for a LATER commit than the file holds */
          CHECK("passport.v1 behind its anchor (an old file restored): ROLLBACK, empty table, untouched", st == LG_ROLLBACK && DENIES() && UNTOUCHED()); }
        { RESTORE(); set_commit(F[1].d, 1); fix_crc_anchor(1); SNAP(); int st = reboot();              /* the file is one ahead of the anchor: the repairable crash window */
          CHECK("passport.v1 exactly one commit ahead of the anchor: verified and REPAIRED", st == LG_OK && has(1001, PP_NAVIGATE) && has(1002, PP_TIMER) && afile_commit() == 2); (void)snap; (void)sn0; (void)sn1; }
        { RESTORE(); set_commit(F[1].d, 0); /* commit 0 is invalid in an anchor */ fix_crc_anchor(1); SNAP(); int st = reboot();
          CHECK("an anchor claiming commit 0: CORRUPT", st == LG_CORRUPT && DENIES() && UNTOUCHED()); }
        { RESTORE(); set_commit(F[0].d, 4); fix_crc_container(0); SNAP(); int st = reboot();              /* TWO commits ahead: more than one crash window can explain */
          CHECK("passport.v1 two commits ahead of the anchor: ROLLBACK (only exactly one is a repairable crash), untouched", st == LG_ROLLBACK && DENIES() && UNTOUCHED()); }
        { RESTORE(); set_commit(F[0].d, 9); fix_crc_container(0); SNAP(); int st = reboot();               /* far ahead of the anchor */
          CHECK("passport.v1 many commits ahead of the anchor: ROLLBACK (not a single crash window), untouched", st == LG_ROLLBACK && DENIES() && UNTOUCHED()); }
        { RESTORE(); F[1].used = 0; SNAP(); int st = reboot();                                           /* anchor deleted, passport at commit 2 */
          CHECK("anchor.v1 deleted while passport.v1 is at commit 2: ROLLBACK", st == LG_ROLLBACK && DENIES() && UNTOUCHED()); }
        { RESTORE(); F[0].used = 0; SNAP(); int st = reboot();
          CHECK("passport.v1 deleted while anchor.v1 exists: ROLLBACK", st == LG_ROLLBACK && DENIES() && UNTOUCHED()); }
        { wipe(); reboot(); ledger_grant(1001, PP_NAVIGATE); F[find("anchor.v1")].used = 0;             /* crash window of the FIRST commit: no anchor yet, passport at commit 1 */
          int st = reboot();
          CHECK("passport.v1 at commit 1 with no anchor (a first commit that crashed): repaired", st == LG_OK && has(1001, PP_NAVIGATE) && afile_commit() == 1); }
        { RESTORE(); F[0].d[16 + 12 + 4] = 5; /* an instance-bound record inside the image */ { uint8_t *img = F[0].d + 16; uint32_t n = img[5]; uint32_t body = 12 + n * 16; uint32_t h = 2166136261u; for (uint32_t i = 0; i < body; i++) { h ^= img[i]; h *= 16777619u; } img[body] = (uint8_t)h; img[body + 1] = (uint8_t)(h >> 8); img[body + 2] = (uint8_t)(h >> 16); img[body + 3] = (uint8_t)(h >> 24); } fix_crc_container(0);
          int st = reboot(); CHECK("an image holding an instance-bound grant is refused (CORRUPT): generations never persist", st == LG_CORRUPT && DENIES()); }
        { RESTORE(); { uint8_t *img = F[0].d + 16; img[12] = 0x04; img[13] = 0; img[14] = 0; img[15] = 0x7F;  /* first entry's principal := 0x7F000004 (Passport itself) */
              uint32_t n = img[5], body = 12 + n * 16, h = 2166136261u; for (uint32_t i = 0; i < body; i++) { h ^= img[i]; h *= 16777619u; } img[body] = (uint8_t)h; img[body + 1] = (uint8_t)(h >> 8); img[body + 2] = (uint8_t)(h >> 16); img[body + 3] = (uint8_t)(h >> 24); } fix_crc_container(0);
          int st = reboot(); CHECK("a durable grant to a reserved identity (0x7F000004) is refused as tampering", st == LG_CORRUPT && !has(PRINCIPAL_PASSPORT, PP_NAVIGATE)); }
        { RESTORE(); F[ip].used = 0; F[ia].used = 0; F[2].used = 1; strcpy(F[2].name, "other"); int st = reboot();
          CHECK("an unrelated file does not matter: a disk with neither ledger file is a fresh ledger", st == LG_OK && ledger_commit() == 0); }
    }
    { /* read-only medium, no disk */
        wipe(); reboot(); ledger_grant(1001, PP_NAVIGATE); ledger_grant(1002, PP_TIMER); ro = 1;
        int st = reboot();
        CHECK("read-only: the verified table is loaded but the ledger is not OK (protected actions unavailable)", st == LG_RDONLY && !ledger_passport_ok() && has(1001, PP_NAVIGATE) && has(1002, PP_TIMER) && ledger_commit() == 2);
        CHECK("read-only: grants and revocations are refused and write nothing", ({ int w = nwrites; int a = ledger_grant(1003, PP_NAVIGATE), b = ledger_revoke(1001, PP_NAVIGATE); a == LG_E_RDONLY && b == LG_E_RDONLY && nwrites == w && has(1001, PP_NAVIGATE); }));
        wipe(); ro = 1; CHECK("read-only fresh disk: RDONLY, nothing created", reboot() == LG_RDONLY && find("passport.v1") < 0 && ledger_grant(1001, PP_NAVIGATE) == LG_E_RDONLY);
        wipe(); reboot(); ledger_grant(1001, PP_NAVIGATE); crash_after = nwrites + 1; ledger_grant(1002, PP_TIMER);        /* passport written, anchor not */
        ro = 1; st = reboot();
        CHECK("read-only boot after the crash window: committed state loaded, repair deferred (anchor untouched)", st == LG_RDONLY && has(1002, PP_TIMER) && afile_commit() == 1 && pfile_commit() == 2);
        ro = 0; st = reboot();
        CHECK("the next writable boot completes the repair", st == LG_OK && afile_commit() == 2 && has(1002, PP_TIMER));
        mounted = 0; st = reboot();
        CHECK("no disk: NODISK, empty durable table, durable mutations refused", st == LG_NODISK && !ledger_passport_ok() && ledger_grant(1001, PP_NAVIGATE) == LG_E_STATE && !has(1001, PP_NAVIGATE));
        CHECK("no disk: RAM-only instance grants still work (essential S0 rights do not need a disk)", ledger_grant_instance(1001, 3, PP_NAVIGATE) == LG_E_OK && ledger_peek(1001, 3, PP_NAVIGATE));
        CHECK("a NULL io is NODISK and safe", ledger_attach(0) == LG_NODISK);
    }
    { /* table limits: per-principal quota and the full table, without breaking the ledger */
        wipe(); reboot(); int ok = 1;
        for (uint32_t p = 0; p < PP_MAX_GRANTS; p++) if (ledger_grant(2000 + p, PP_NAVIGATE) != LG_E_OK) ok = 0;
        CHECK("32 principals fill the table", ok && ledger_commit() == PP_MAX_GRANTS);
        int w = nwrites;
        CHECK("the 33rd is refused by the Passport (FULL), writes nothing, the ledger stays healthy", ledger_grant(3000, PP_NAVIGATE) == LG_E_PASSPORT && nwrites == w && ledger_passport_ok() && ledger_commit() == PP_MAX_GRANTS);
        CHECK("after a reboot all 32 are there", reboot() == LG_OK && has(2000, PP_NAVIGATE) && has(2031, PP_NAVIGATE) && !has(3000, PP_NAVIGATE));
        CHECK("revoking one frees a slot", ledger_revoke(2005, PP_NAVIGATE) == LG_E_OK && ledger_grant(3000, PP_NAVIGATE) == LG_E_OK);
    }
    { /* randomized: random mutations with random single faults, a model of the durable table; after every reboot the table is one of the two admissible states */
        uint32_t s = 99; int bad = 0, reboots = 0;
        wipe(); reboot();
        uint32_t mpr[PP_MAX_GRANTS * 2]; uint8_t mpm[PP_MAX_GRANTS * 2]; int mc = 0;
        for (int it = 0; it < 4000; it++) {
            s = s * 1664525u + 1013904223u;
            uint32_t pr = 1000 + (s >> 8) % 6; uint8_t pm = (uint8_t)(1 + (s >> 16) % 7); int revoke = (s >> 24) % 3 == 0;
            int fault = (s >> 4) % 11;
            reset_faults();
            if (fault == 0) crash_before = nwrites + 1 + (int)((s >> 20) % 2);
            else if (fault == 1) crash_after = nwrites + 1 + (int)((s >> 20) % 2);
            else if (fault == 2) { uncertain_at = nwrites + 1 + (int)((s >> 20) % 2); uncertain_applies = (int)((s >> 22) & 1); }
            else if (fault == 3) { fail_at = nwrites + 1; fail_code = FS_ENOSPC; }
            /* the admissible tables: before and after this mutation */
            uint32_t bpr[PP_MAX_GRANTS * 2], apr[PP_MAX_GRANTS * 2]; uint8_t bpm[PP_MAX_GRANTS * 2], apm[PP_MAX_GRANTS * 2]; int bc = mc, ac = mc;
            memcpy(bpr, mpr, sizeof(uint32_t) * (size_t)mc); memcpy(bpm, mpm, (size_t)mc); memcpy(apr, mpr, sizeof(uint32_t) * (size_t)mc); memcpy(apm, mpm, (size_t)mc);
            int idx = -1; for (int k = 0; k < ac; k++) if (apr[k] == pr && apm[k] == pm) idx = k;
            if (revoke) { if (idx >= 0) { apr[idx] = apr[ac - 1]; apm[idx] = apm[ac - 1]; ac--; } } else if (idx < 0) {
                int held = 0; for (int k = 0; k < ac; k++) if (apr[k] == pr) held++;
                if (held < PP_PER_PRINCIPAL && ac < PP_MAX_GRANTS) { apr[ac] = pr; apm[ac] = pm; ac++; }
            }
            int rc = revoke ? ledger_revoke(pr, pm) : ledger_grant(pr, pm);
            int st = ledger_state();
            if (rc == LG_E_OK) { memcpy(mpr, apr, sizeof(uint32_t) * (size_t)ac); memcpy(mpm, apm, (size_t)ac); mc = ac; }
            if (st == LG_FAILED || dead) {                                       /* an unknown outcome or a power cut: reboot and accept either state */
                reboot(); reboots++;
                int e1, e2; int m1 = table_eq(bpr, bpm, bc, &e1), m2 = table_eq(apr, apm, ac, &e2);
                if (ledger_state() != LG_OK || (!m1 && !m2)) { bad++; break; }
                if (m2 && !m1) { memcpy(mpr, apr, sizeof(uint32_t) * (size_t)ac); memcpy(mpm, apm, (size_t)ac); mc = ac; } else { memcpy(mpr, bpr, sizeof(uint32_t) * (size_t)bc); memcpy(mpm, bpm, (size_t)bc); mc = bc; }
            } else if (rc != LG_E_OK && rc != LG_E_PASSPORT && rc != LG_E_NOSPACE) { bad++; break; }
            int e3; if (!table_eq(mpr, mpm, mc, &e3)) { bad++; break; }            /* the live table always equals the model */
            if ((it % 97) == 0) { reboot(); reboots++; if (!table_eq(mpr, mpm, mc, &e3)) { bad++; break; } }   /* and a clean reboot reproduces it */
        }
        CHECK("4,000 random mutations with random crashes, uncertain outcomes and refusals agree with the model (rebooted many times)", bad == 0 && reboots > 200);
    }
    printf("ledger: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
