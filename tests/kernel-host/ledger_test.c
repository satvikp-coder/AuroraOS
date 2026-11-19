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
#include <trail.h>

static int passed, failed;
#define CHECK(name, cond) do { if (cond) { passed++; } else { failed++; printf("  FAIL: %s (line %d)\n", name, __LINE__); } } while (0)

/* ---------------- fake filesystem ---------------- */
#define NF 4
static struct { int used; uint32_t owner; char name[32]; uint32_t size; uint8_t d[2048]; } F[NF];
static int mounted = 1, ro = 0;
static int nwrites;                                   /* successful + attempted writes since the last reset() */
static int crash_before = 0, crash_after = 0, uncertain_at = 0, uncertain_applies = 0, fail_at = 0, fail_code = 0, corrupt_readback_at = 0, dead = 0;
static void reset_faults(void) { nwrites = 0; crash_before = crash_after = uncertain_at = uncertain_applies = fail_at = fail_code = corrupt_readback_at = dead = 0; }
static void wipe(void) { memset(F, 0, sizeof F); mounted = 1; ro = 0; reset_faults(); }

static int f_mounted(void) { return mounted; }
static int f_readonly(void) { return ro; }
static int find(const char *n) { for (int i = 0; i < NF; i++) if (F[i].used && !strcmp(F[i].name, n)) return i; return -1; }
static int f_lookup(uint32_t o, const char *n, uint64_t *id) { if (dead) return FS_EIO; int i = find(n); if (i < 0 || F[i].owner != o) return FS_ENOENT; *id = (uint64_t)i + 1; return FS_OK; }
static int f_create(uint32_t o, const char *n, uint64_t *id) { if (dead) return FS_EIO; if (ro) return FS_ERO; for (int i = 0; i < NF; i++) if (!F[i].used) { F[i].used = 1; F[i].owner = o; strcpy(F[i].name, n); F[i].size = 0; *id = (uint64_t)i + 1; return FS_OK; } return FS_ENFILE; }
static int f_size(uint64_t id, uint32_t *s) { if (dead) return FS_EIO; if (id < 1 || id > NF || !F[id - 1].used) return FS_EBADID; *s = F[id - 1].size; return FS_OK; }
static int f_read(uint64_t id, void *b, uint32_t len) { if (dead) return FS_EIO; if (id < 1 || id > NF || !F[id - 1].used) return FS_EBADID; uint32_t n = len < F[id - 1].size ? len : F[id - 1].size; memcpy(b, F[id - 1].d, n); return (int)n; }
static int f_write(uint32_t o, uint64_t id, const void *b, uint32_t len)
{
    if (dead) return FS_EIO;
    if (ro) return FS_ERO;
    if (id < 1 || id > NF || !F[id - 1].used) return FS_EBADID;
    if (F[id - 1].owner != o) return FS_EACCES;                                          /* only the owner writes its file */
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


/* ---- Trail helpers ---- */
static uint32_t anchor_pp(void) { int i = find("anchor.v1"); return i < 0 ? 0 : (uint32_t)F[i].d[8] | (uint32_t)F[i].d[9] << 8 | (uint32_t)F[i].d[10] << 16 | (uint32_t)F[i].d[11] << 24; }
static uint32_t anchor_tn(void) { int i = find("anchor.v1"); return i < 0 ? 0 : (uint32_t)F[i].d[12] | (uint32_t)F[i].d[13] << 8 | (uint32_t)F[i].d[14] << 16 | (uint32_t)F[i].d[15] << 24; }
static uint64_t anchor_th(void) { int i = find("anchor.v1"); uint64_t v = 0; if (i < 0) return 0; for (int k = 7; k >= 0; k--) v = (v << 8) | F[i].d[16 + k]; return v; }
static uint32_t tfile_commit(void) { int i = find("trail.v1"); return i < 0 ? 0 : (uint32_t)F[i].d[8] | (uint32_t)F[i].d[9] << 8 | (uint32_t)F[i].d[10] << 16 | (uint32_t)F[i].d[11] << 24; }
static void set_anchor(uint8_t *d, uint32_t pp, uint32_t tn, uint64_t th) { memset(d, 0, 32); memcpy(d, "ANCH", 4); d[4] = 1; for (int k = 0; k < 4; k++) { d[8 + k] = (uint8_t)(pp >> (8 * k)); d[12 + k] = (uint8_t)(tn >> (8 * k)); } for (int k = 0; k < 8; k++) d[16 + k] = (uint8_t)(th >> (8 * k)); uint32_t c = crc32(d, 28); for (int k = 0; k < 4; k++) d[28 + k] = (uint8_t)(c >> (8 * k)); }
static void fix_crc_tcontainer(int i) { uint32_t c = crc32(F[i].d, 1996); for (int k = 0; k < 4; k++) F[i].d[1996 + k] = (uint8_t)(c >> (8 * k)); }
static struct trail_rec mkrec(int i) { struct trail_rec r; memset(&r, 0, sizeof r); r.kind = (uint8_t)(12 + i % 3); r.principal = (uint32_t)(1000 + i); r.state = 1; r.uptime_ms = (uint32_t)(i * 10); r.arg = i; return r; }
static int nullw(const uint8_t *b, size_t l) { (void)b; (void)l; return 0; }
static const struct tr_store NULLSTORE = { nullw, 0 };
static void model_upto(struct trail *m, int k) { trail_init(m); for (int i = 0; i < k; i++) { struct trail_rec r = mkrec(i); trail_append(m, &NULLSTORE, &r); } }

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
        #define RESTORE() do { wipe(); F[0].used = 1; F[0].owner = PRINCIPAL_PASSPORT; strcpy(F[0].name, "passport.v1"); F[0].size = ps_n; memcpy(F[0].d, ps, ps_n); F[1].used = 1; F[1].owner = PRINCIPAL_PASSPORT; strcpy(F[1].name, "anchor.v1"); F[1].size = as_n; memcpy(F[1].d, as, as_n); } while (0)
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
        { RESTORE(); set_commit(F[1].d, 0); fix_crc_anchor(1); SNAP(); int st = reboot();                 /* a valid anchor that vouches for NO passport commit while the file is at commit 2 */
          CHECK("an anchor vouching for passport commit 0 while the file is at commit 2: ROLLBACK", st == LG_ROLLBACK && DENIES() && UNTOUCHED()); }
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
    /* ======================= Trail ======================= */
    { /* basics, ownership, eviction */
        wipe(); reboot();
        CHECK("fresh: the Trail is OK and empty, commit 0, nothing created by loading", ledger_trail_ok() && ledger_trail_commit() == 0 && ledger_trail()->n == 0 && find("trail.v1") < 0);
        CHECK("append 3 records: OK each, commit 3, sequence 1..3", ({ int ok = 1; for (int i = 0; i < 3; i++) { struct trail_rec r = mkrec(i); if (ledger_trail_append(&r) != LG_E_OK) ok = 0; } ok && ledger_trail_commit() == 3 && ledger_trail()->n == 3 && ledger_trail()->r[0].seq == 1 && ledger_trail()->r[2].seq == 3; }));
        CHECK("trail.v1 is owned by the Trail principal, 2000 bytes; passport and anchor by the Passport principal", F[find("trail.v1")].owner == PRINCIPAL_TRAIL && F[find("trail.v1")].size == LEDGER_TRAIL_BYTES && F[find("anchor.v1")].owner == PRINCIPAL_PASSPORT);
        CHECK("the anchor vouches for 3 records and the chain head", anchor_tn() == 3 && anchor_th() == trail_head(ledger_trail()) && anchor_th() != 0);
        uint64_t head3 = trail_head(ledger_trail());
        CHECK("reboot: the three records survive, chain verifies, same head", reboot() == LG_OK && ledger_trail_ok() && ledger_trail()->n == 3 && trail_verify(ledger_trail()) == TR_OK && trail_head(ledger_trail()) == head3 && ledger_trail_commit() == 3);
        CHECK("an invalid record (kind 99) and a NULL record are refused with no I/O", ({ int w = nwrites; struct trail_rec bad = mkrec(0); bad.kind = 99; int a = ledger_trail_append(&bad), b = ledger_trail_append(0); a == LG_E_ARG && b == LG_E_ARG && nwrites == w && ledger_trail_ok() && ledger_trail_commit() == 3; }));
        CHECK("a Passport grant leaves the Trail alone and the anchor keeps vouching for it", ledger_grant(1001, PP_NAVIGATE) == LG_E_OK && anchor_tn() == 3 && anchor_th() == head3 && anchor_pp() == 1 && reboot() == LG_OK && ledger_trail_commit() == 3 && has(1001, PP_NAVIGATE));
        CHECK("a Trail append leaves the Passport alone and the anchor keeps vouching for it", ({ struct trail_rec r = mkrec(3); ledger_trail_append(&r) == LG_E_OK && anchor_pp() == 1 && anchor_tn() == 4 && reboot() == LG_OK && ledger_commit() == 1 && ledger_trail_commit() == 4; }));
        wipe(); reboot(); int ok = 1; for (int i = 0; i < 60; i++) { struct trail_rec r = mkrec(i); if (ledger_trail_append(&r) != LG_E_OK) ok = 0; }
        CHECK("60 appends: the ring holds the newest 48, 12 evicted, the chain still verifies", ok && ledger_trail()->n == TR_MAX && ledger_trail()->evicted == 12 && ledger_trail_commit() == 60 && trail_verify(ledger_trail()) == TR_OK && ledger_trail()->r[0].seq == 13);
        uint64_t h60 = trail_head(ledger_trail());
        CHECK("... and survives a reboot exactly (same head, same retained records)", reboot() == LG_OK && ledger_trail()->n == TR_MAX && ledger_trail()->r[0].seq == 13 && trail_head(ledger_trail()) == h60 && ledger_trail_commit() == 60);
    }
    { /* the exhaustive crash and uncertain-write enumeration for Trail appends */
        enum { NA = 6 };
        int bad = 0, total = 0, ahead_seen = 0, behind_seen = 0;
        for (int kind = 0; kind < 4; kind++)                     /* 0 power cut before the write, 1 right after it, 2 uncertain (not applied), 3 uncertain (applied) */
            for (int w = 1; w <= 2 * NA; w++) {
                wipe(); reboot();
                if (kind == 0) crash_before = w; else if (kind == 1) crash_after = w; else { uncertain_at = w; uncertain_applies = kind == 3; }
                int completed = 0, inflight = -1;
                for (int i = 0; i < NA; i++) { struct trail_rec r = mkrec(i); int rc = ledger_trail_append(&r); if (rc == LG_E_OK) completed = i + 1; else { inflight = i; if (kind >= 2 && (rc != LG_E_UNKNOWN || ledger_trail_state() != LG_FAILED || ledger_trail_ok())) bad++; break; } }
                if (kind >= 2 && inflight >= 0) { struct trail_rec r = mkrec(9); if (ledger_trail_append(&r) != LG_E_STATE) bad++; }       /* frozen until the next boot */
                int st = reboot(); total++;
                struct trail m1, m2; model_upto(&m1, completed); model_upto(&m2, inflight >= 0 ? inflight + 1 : completed);
                int is1 = ledger_trail_commit() == (uint32_t)completed && trail_head(ledger_trail()) == trail_head(&m1) && ledger_trail()->n == m1.n;
                int is2 = ledger_trail_commit() == (uint32_t)(inflight >= 0 ? inflight + 1 : completed) && trail_head(ledger_trail()) == trail_head(&m2) && ledger_trail()->n == m2.n;
                if (st != LG_OK || ledger_trail_state() != LG_OK || (!is1 && !is2) || trail_verify(ledger_trail()) != TR_OK) { bad++; printf("    trail crash kind %d write %d: state %s/%s is1=%d is2=%d\n", kind, w, ledger_state_name(st), ledger_state_name(ledger_trail_state()), is1, is2); continue; }
                if (is2 && !is1) ahead_seen++; else behind_seen++;
                if (anchor_tn() != ledger_trail_commit() || tfile_commit() != ledger_trail_commit()) { bad++; printf("    trail crash kind %d write %d: anchor %u file %u commit %u\n", kind, w, anchor_tn(), tfile_commit(), ledger_trail_commit()); }
                { struct trail_rec r = mkrec(8); if (ledger_trail_append(&r) != LG_E_OK) bad++; }                                       /* still works afterwards */
            }
        CHECK("EVERY Trail crash point and uncertain write (4 kinds x 12 writes): the next boot is healthy, the chain is exactly the state before or after the interrupted append, the anchor is repaired, appends keep working", bad == 0);
        CHECK("... and both outcomes (appended-but-unacknowledged, not-appended) really occurred", ahead_seen > 0 && behind_seen > 0 && total == 4 * 2 * NA);
    }
    { /* damaged, newer, rolled-back and inconsistent Trail files; the other ledger is HELD and every file stays untouched */
        static uint8_t pbak[3][2048]; uint32_t szs[3];
        wipe(); reboot(); ledger_grant(1001, PP_NAVIGATE); ledger_grant(1002, PP_TIMER);
        for (int i = 0; i < 3; i++) { struct trail_rec r = mkrec(i); ledger_trail_append(&r); }              /* passport commit 2, trail commit 3 */
        for (int i = 0; i < 3; i++) { const char *nm = i == 0 ? "passport.v1" : i == 1 ? "anchor.v1" : "trail.v1"; int k = find(nm); memcpy(pbak[i], F[k].d, F[k].size); szs[i] = F[k].size; }
        struct trail m2, m3; model_upto(&m2, 2); model_upto(&m3, 3);
        #define TRESTORE() do { wipe(); const char *nm_[3] = { "passport.v1", "anchor.v1", "trail.v1" }; for (int i_ = 0; i_ < 3; i_++) { F[i_].used = 1; F[i_].owner = i_ == 2 ? PRINCIPAL_TRAIL : PRINCIPAL_PASSPORT; strcpy(F[i_].name, nm_[i_]); F[i_].size = szs[i_]; memcpy(F[i_].d, pbak[i_], szs[i_]); } } while (0)
        #define TSNAP() static uint8_t tsnap[3][2048]; uint32_t tsz[3]; for (int i_ = 0; i_ < 3; i_++) { tsz[i_] = F[i_].size; memcpy(tsnap[i_], F[i_].d, sizeof tsnap[i_]); }
        #define TUNTOUCHED() ({ int u_ = 1; for (int i_ = 0; i_ < 3; i_++) if (F[i_].size != tsz[i_] || memcmp(tsnap[i_], F[i_].d, sizeof tsnap[i_])) u_ = 0; u_; })
        TRESTORE(); CHECK("control: a consistent Passport+Trail set loads, both OK", reboot() == LG_OK && ledger_ok() && ledger_commit() == 2 && ledger_trail_commit() == 3 && has(1001, PP_NAVIGATE));
        { TRESTORE(); F[2].d[100] ^= 1; TSNAP(); int st = reboot(); int w = nwrites; struct trail_rec r = mkrec(0); int a = ledger_trail_append(&r), g = ledger_grant(5000, PP_NAVIGATE);
          CHECK("a flipped byte in trail.v1: Trail CORRUPT, the Passport is HELD (its verified grants still honoured, but not OK), every write refused, all files untouched",
                ledger_trail_state() == LG_CORRUPT && st == LG_HELD && !ledger_ok() && !ledger_trail_ok() && !ledger_passport_ok() && has(1001, PP_NAVIGATE) && a == LG_E_STATE && g == LG_E_STATE && nwrites == w && TUNTOUCHED()); }
        { TRESTORE(); F[2].size = 100; TSNAP(); int st = reboot(); CHECK("trail.v1 truncated: CORRUPT, Passport HELD, untouched", ledger_trail_state() == LG_CORRUPT && st == LG_HELD && TUNTOUCHED()); }
        { TRESTORE(); F[2].d[4] = 2; TSNAP(); int st = reboot(); CHECK("a NEWER trail.v1: NEWER, Passport HELD, untouched", ledger_trail_state() == LG_NEWER && st == LG_HELD && TUNTOUCHED()); }
        { TRESTORE(); set_anchor(F[1].d, 2, 5, trail_head(&m3)); TSNAP(); int st = reboot(); CHECK("trail.v1 behind its anchor (an old file restored): ROLLBACK, Passport HELD, untouched", ledger_trail_state() == LG_ROLLBACK && st == LG_HELD && TUNTOUCHED()); }
        { TRESTORE(); set_anchor(F[1].d, 2, 1, 0); TSNAP(); int st = reboot(); CHECK("trail.v1 two records ahead of its anchor: ROLLBACK", ledger_trail_state() == LG_ROLLBACK && st == LG_HELD && TUNTOUCHED()); }
        { TRESTORE(); set_anchor(F[1].d, 2, 3, trail_head(&m3) ^ 1); TSNAP(); int st = reboot(); CHECK("same record count but a different chain head (the chain was rewritten): ROLLBACK", ledger_trail_state() == LG_ROLLBACK && st == LG_HELD && TUNTOUCHED()); }
        { TRESTORE(); F[2].used = 0; TSNAP(); int st = reboot(); CHECK("trail.v1 deleted while the anchor vouches for 3 records: ROLLBACK", ledger_trail_state() == LG_ROLLBACK && st == LG_HELD && TUNTOUCHED()); }
        { TRESTORE(); F[1].used = 0; F[0].used = 0; TSNAP(); int st = reboot(); (void)st; CHECK("no anchor at all but trail.v1 holds 3 records: ROLLBACK (only a FIRST record that crashed before its anchor may lack one)", ledger_trail_state() == LG_ROLLBACK && TUNTOUCHED()); }
        { TRESTORE(); F[2].d[8] = 9; fix_crc_tcontainer(2); TSNAP(); int st = reboot(); (void)st; CHECK("the container's record count disagrees with the chain inside it: CORRUPT", ledger_trail_state() == LG_CORRUPT && TUNTOUCHED()); }
        { TRESTORE(); F[2].d[16 + 4] = 2; fix_crc_tcontainer(2); TSNAP(); int st = reboot(); (void)st; CHECK("a valid container around a NEWER Trail image version: NEWER, untouched", ledger_trail_state() == LG_NEWER && TUNTOUCHED()); }
        { TRESTORE(); F[1].d[24] = 1; { uint32_t c = crc32(F[1].d, 28); for (int k = 0; k < 4; k++) F[1].d[28 + k] = (uint8_t)(c >> (8 * k)); } TSNAP(); int st = reboot(); CHECK("an anchor with non-zero reserved bytes (a format this code does not know): CORRUPT, untouched", st == LG_CORRUPT && TUNTOUCHED()); }
        { TRESTORE(); set_anchor(F[1].d, 2, 2, trail_head(&m2)); int st = reboot();             /* exactly one record ahead, and the anchor vouches for the chain minus its newest record: the crash window */
          CHECK("trail.v1 exactly one record ahead of its anchor (the crash window): verified and REPAIRED", st == LG_OK && ledger_trail_state() == LG_OK && ledger_trail_commit() == 3 && anchor_tn() == 3 && anchor_th() == trail_head(&m3)); }
        { TRESTORE(); set_anchor(F[1].d, 2, 2, trail_head(&m2) ^ 1); TSNAP(); int st = reboot(); CHECK("one ahead but the anchor's head is NOT the chain minus its newest record: ROLLBACK, not repaired", ledger_trail_state() == LG_ROLLBACK && st == LG_HELD && TUNTOUCHED()); }
        { TRESTORE(); F[0].d[100] ^= 1; TSNAP(); int st = reboot(); struct trail_rec r = mkrec(0); int a = ledger_trail_append(&r);
          CHECK("a corrupt passport.v1 HOLDS the Trail: its records stay visible but nothing is appended and nothing is written", st == LG_CORRUPT && ledger_trail_state() == LG_HELD && ledger_trail()->n == 3 && a == LG_E_STATE && TUNTOUCHED()); }
        { TRESTORE(); F[1].d[10] ^= 1; TSNAP(); int st = reboot(); CHECK("a damaged anchor.v1: both ledgers CORRUPT (neither can be validated), untouched", st == LG_CORRUPT && ledger_trail_state() == LG_CORRUPT && TUNTOUCHED()); }
        { TRESTORE(); F[2].size = 0; F[1].used = 0; F[0].used = 0; int st = reboot(); CHECK("only an empty trail.v1 shell left: a fresh healthy ledger", st == LG_OK && ledger_trail_ok() && ledger_trail_commit() == 0); }
        { TRESTORE(); ro = 1; set_anchor(F[1].d, 2, 2, trail_head(&m2)); TSNAP(); int st = reboot(); struct trail_rec r = mkrec(0); int w = nwrites; int a = ledger_trail_append(&r);
          CHECK("read-only: the Trail is loaded and visible, appends are refused, and the pending anchor repair waits for a writable boot", st == LG_RDONLY && ledger_trail_state() == LG_RDONLY && ledger_trail()->n == 3 && a == LG_E_RDONLY && nwrites == w && TUNTOUCHED());
          ro = 0; reboot(); CHECK("the next writable boot completes the repair", ledger_trail_ok() && anchor_tn() == 3); }
        { wipe(); mounted = 0; reboot(); struct trail_rec r = mkrec(0); CHECK("no disk: the Trail is unavailable and appends are refused", ledger_trail_state() == LG_NODISK && !ledger_trail_ok() && ledger_trail_append(&r) == LG_E_STATE); }
    }
    { /* randomized: Passport and Trail operations interleaved with random faults; two models; the interrupted operation may have gone either way, nothing else may change */
        uint32_t s = 4242; int bad = 0, reboots = 0;
        wipe(); reboot();
        unsigned pmask = 0; int tcount = 0;
        for (int it = 0; it < 4000 && !bad; it++) {
            s = s * 1664525u + 1013904223u;
            int is_trail = (s >> 5) % 2; int fault = (s >> 8) % 9;
            reset_faults();
            if (fault == 0) crash_before = nwrites + 1 + (int)((s >> 20) % 2); else if (fault == 1) crash_after = nwrites + 1 + (int)((s >> 20) % 2);
            else if (fault == 2) { uncertain_at = nwrites + 1 + (int)((s >> 20) % 2); uncertain_applies = (int)((s >> 22) & 1); }
            else if (fault == 3) { fail_at = nwrites + 1; fail_code = FS_ENOSPC; }
            unsigned pm_before = pmask, pm_after = pmask; int tc_before = tcount, tc_after = tcount; int rc;
            if (is_trail) { struct trail_rec r = mkrec(tcount); tc_after = tcount + 1; rc = ledger_trail_append(&r); }
            else {
                uint32_t pr = 1000 + (s >> 12) % 3; uint8_t pm = (uint8_t)(1 + (s >> 16) % 2); int bit = (int)((pr - 1000) * 2 + (pm - 1)); int revoke = (s >> 18) % 3 == 0;
                if (revoke) pm_after &= ~(1u << bit); else pm_after |= 1u << bit;
                rc = revoke ? ledger_revoke(pr, pm) : ledger_grant(pr, pm);
                if (rc == LG_E_PASSPORT) pm_after = pm_before;                                  /* revoking what is not granted */
            }
            if (rc == LG_E_OK) { pmask = pm_after; tcount = tc_after; }
            else if (rc == LG_E_UNKNOWN || dead) {
                reboot(); reboots++;
                unsigned got = 0; for (int b = 0; b < 6; b++) if (has(1000 + (unsigned)b / 2, (uint8_t)(1 + b % 2))) got |= 1u << b;
                int tgot = (int)ledger_trail_commit();
                int pok = got == pm_before || got == pm_after, tok = tgot == tc_before || tgot == tc_after;
                if (!pok || !tok || !ledger_ok() || (is_trail && got != pm_before) || (!is_trail && tgot != tc_before)) { bad++; printf("    interleave it %d: got %x/%d before %x/%d after %x/%d state %s/%s\n", it, got, tgot, pm_before, tc_before, pm_after, tc_after, ledger_state_name(ledger_state()), ledger_state_name(ledger_trail_state())); break; }
                pmask = got; tcount = tgot;
            } else if (rc != LG_E_PASSPORT && rc != LG_E_NOSPACE) { bad++; printf("    interleave it %d: unexpected rc %d\n", it, rc); break; }
            { unsigned got = 0; for (int b = 0; b < 6; b++) if (has(1000 + (unsigned)b / 2, (uint8_t)(1 + b % 2))) got |= 1u << b;
              if (got != pmask || (int)ledger_trail_commit() != tcount) { bad++; printf("    interleave it %d: live %x/%d model %x/%d\n", it, got, (int)ledger_trail_commit(), pmask, tcount); break; } }
            if ((it % 101) == 0) { reboot(); reboots++; unsigned got = 0; for (int b = 0; b < 6; b++) if (has(1000 + (unsigned)b / 2, (uint8_t)(1 + b % 2))) got |= 1u << b; if (got != pmask || (int)ledger_trail_commit() != tcount) { bad++; printf("    interleave it %d: reboot mismatch\n", it); break; } }
        }
        CHECK("4,000 interleaved Passport grants/revocations and Trail appends with random crashes, uncertain outcomes and refusals agree with both models; the interrupted operation went either way and nothing else changed", bad == 0 && reboots > 150);
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
