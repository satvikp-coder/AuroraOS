/* kernel/ui/ledger_store.c - the durable Passport and Trail ledgers (see kernel/include/ledger.h for the commit protocol and its crash analysis). Pure: all
 * storage goes through struct lg_io, so every failure and every crash point is testable on the host. No allocation, no libc, no clock.
 *
 * Files: passport.v1 and anchor.v1 (owner PRINCIPAL_PASSPORT), trail.v1 (owner PRINCIPAL_TRAIL). The anchor vouches for both ledgers:
 *   pp_commit (Passport commits), trail_commit (records ever appended = next_seq - 1) and trail_head (the chain value of the newest record).
 * Each ledger compares its file with the anchor: equal = healthy; exactly one ahead = a crash between the file and the anchor (verified, then repaired);
 * anything else = rollback. If EITHER ledger is damaged / newer / rolled back, the other becomes HELD: nothing is written at all, so the evidence stays intact. */
#include <ledger.h>
#include <fs.h>
#include <principal.h>

#define PAY 556                                       /* Passport container bytes covered by the CRC (16 header + PP_IMAGE_MAX 536 + 4 pad) */
#define TPAY 1996                                     /* Trail container bytes covered by the CRC (16 header + TR_IMAGE_MAX 1948 + 32 pad) */
_Static_assert(16 + PP_IMAGE_MAX <= PAY, "the Passport image must fit the fixed container");
_Static_assert(PAY + 4 == LEDGER_PASSPORT_BYTES, "passport container size");
_Static_assert(16 + TR_IMAGE_MAX <= TPAY, "the Trail image must fit the fixed container");
_Static_assert(TPAY + 4 == LEDGER_TRAIL_BYTES, "trail container size");

static const struct lg_io *IO;
static int pstate = LG_NODISK, tstate = LG_NODISK;
static struct passport live, tmp;
static struct trail ltr, ttmp;
static uint32_t commit;                               /* the Passport commit counter of the verified state */
static uint32_t A_pp, A_tn; static uint64_t A_th;     /* what the anchor on disk (as last verified/written by us) vouches for */
static int last_fs;
static uint8_t cont[LEDGER_PASSPORT_BYTES + 16], tcont[LEDGER_TRAIL_BYTES + 16], back[LEDGER_TRAIL_BYTES + 16], anc[LEDGER_ANCHOR_BYTES + 16];
static uint8_t img[PP_IMAGE_MAX + 16], timg[TR_IMAGE_MAX + 16];

static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }
static uint64_t r64(const uint8_t *p) { return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32); }
static uint32_t crc32(const uint8_t *p, uint32_t n)           /* CRC-32 (IEEE), bitwise: small and enough for 2 KB */
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1)); }
    return ~c;
}
static void zero(void *p, uint32_t n) { uint8_t *b = p; for (uint32_t i = 0; i < n; i++) b[i] = 0; }
static int same(const uint8_t *a, const uint8_t *b, uint32_t n) { for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }

static void build_container(uint8_t *o, const uint8_t *image, uint32_t imglen, uint32_t cmt)
{
    zero(o, LEDGER_PASSPORT_BYTES);
    o[0] = 'L'; o[1] = 'G'; o[2] = 'P'; o[3] = 'P';
    w32(o + 4, LEDGER_CONTAINER_VERSION); w32(o + 8, cmt); w32(o + 12, imglen);
    for (uint32_t i = 0; i < imglen; i++) o[16 + i] = image[i];
    w32(o + PAY, crc32(o, PAY));
}
static void build_tcontainer(uint8_t *o, const uint8_t *image, uint32_t imglen, uint32_t records_ever)
{
    zero(o, LEDGER_TRAIL_BYTES);
    o[0] = 'L'; o[1] = 'G'; o[2] = 'T'; o[3] = 'R';
    w32(o + 4, LEDGER_CONTAINER_VERSION); w32(o + 8, records_ever); w32(o + 12, imglen);
    for (uint32_t i = 0; i < imglen; i++) o[16 + i] = image[i];
    w32(o + TPAY, crc32(o, TPAY));
}
static void build_anchor(uint8_t *o, uint32_t pp, uint32_t tn, uint64_t th)
{
    zero(o, LEDGER_ANCHOR_BYTES);
    o[0] = 'A'; o[1] = 'N'; o[2] = 'C'; o[3] = 'H';
    w32(o + 4, LEDGER_CONTAINER_VERSION); w32(o + 8, pp); w32(o + 12, tn); w64(o + 16, th);     /* 24..27 reserved, zero */
    w32(o + 28, crc32(o, 28));
}

enum { P_OK = 0, P_CORRUPT, P_NEWER };
/* Parse a container; a different version is judged by its header alone and never interpreted. */
static int parse_container(const uint8_t *b, uint32_t n, uint32_t fsize, uint32_t *cmt, uint32_t *imglen)
{
    if (n < 8 || !(b[0] == 'L' && b[1] == 'G' && b[2] == 'P' && b[3] == 'P')) return P_CORRUPT;
    if (r32(b + 4) != LEDGER_CONTAINER_VERSION) return P_NEWER;
    if (fsize != LEDGER_PASSPORT_BYTES || n != LEDGER_PASSPORT_BYTES) return P_CORRUPT;
    if (r32(b + PAY) != crc32(b, PAY)) return P_CORRUPT;
    *cmt = r32(b + 8); *imglen = r32(b + 12);
    if (*imglen == 0 || *imglen > PP_IMAGE_MAX || *cmt == 0) return P_CORRUPT;
    return P_OK;
}
static int parse_tcontainer(const uint8_t *b, uint32_t n, uint32_t fsize, uint32_t *records_ever, uint32_t *imglen)
{
    if (n < 8 || !(b[0] == 'L' && b[1] == 'G' && b[2] == 'T' && b[3] == 'R')) return P_CORRUPT;
    if (r32(b + 4) != LEDGER_CONTAINER_VERSION) return P_NEWER;
    if (fsize != LEDGER_TRAIL_BYTES || n != LEDGER_TRAIL_BYTES) return P_CORRUPT;
    if (r32(b + TPAY) != crc32(b, TPAY)) return P_CORRUPT;
    *records_ever = r32(b + 8); *imglen = r32(b + 12);
    if (*imglen < 28 || *imglen > TR_IMAGE_MAX || *records_ever == 0) return P_CORRUPT;
    return P_OK;
}
static int parse_anchor(const uint8_t *b, uint32_t n, uint32_t fsize, uint32_t *pp, uint32_t *tn, uint64_t *th)
{
    if (n < 8 || !(b[0] == 'A' && b[1] == 'N' && b[2] == 'C' && b[3] == 'H')) return P_CORRUPT;
    if (r32(b + 4) != LEDGER_CONTAINER_VERSION) return P_NEWER;
    if (fsize != LEDGER_ANCHOR_BYTES || n != LEDGER_ANCHOR_BYTES) return P_CORRUPT;
    if (r32(b + 28) != crc32(b, 28) || r32(b + 24) != 0) return P_CORRUPT;
    *pp = r32(b + 8); *tn = r32(b + 12); *th = r64(b + 16);
    if (*pp == 0 && *tn == 0 && *th != 0) return P_CORRUPT;         /* a head without records */
    return P_OK;
}

/* Read a whole file (up to cap bytes) and report its size. Returns FS_OK or an error. */
static int read_file(uint64_t id, uint8_t *buf, uint32_t cap, uint32_t *n, uint32_t *fsize)
{
    uint32_t sz = 0; int rc = IO->size(id, &sz);
    if (rc != FS_OK) return rc;
    *fsize = sz; *n = 0;
    uint32_t want = sz < cap ? sz : cap;
    if (want == 0) return FS_OK;
    int got = IO->read(id, buf, want);
    if (got < 0) return got;
    *n = (uint32_t)got;
    return FS_OK;
}
static int definite_refusal(int rc) { return rc == FS_ENOSPC || rc == FS_EQUOTA || rc == FS_EFBIG || rc == FS_ENFILE || rc == FS_ERO; }   /* nothing was changed */

/* Write the whole file (creating it if missing) and read it back. FS_OK only when the bytes on the medium are exactly buf. */
static int store_file(uint32_t owner, const char *name, const uint8_t *buf, uint32_t len)
{
    uint64_t id; int rc = IO->lookup(owner, name, &id);
    if (rc == FS_ENOENT) rc = IO->create(owner, name, &id);
    if (rc != FS_OK) return rc;
    int w = IO->write(owner, id, buf, len);
    if (w < 0) return w;
    if ((uint32_t)w != len) return FS_EIO;
    uint32_t n = 0, fsz = 0;
    rc = read_file(id, back, len, &n, &fsz);
    if (rc != FS_OK) return rc;
    if (fsz != len || n != len || !same(back, buf, len)) return FS_EIO;       /* the medium does not hold what we wrote */
    return FS_OK;
}
static int write_anchor(uint32_t pp, uint32_t tn, uint64_t th)
{
    build_anchor(anc, pp, tn, th);
    return store_file(PRINCIPAL_PASSPORT, LEDGER_ANCHOR_FILE, anc, LEDGER_ANCHOR_BYTES);
}

static int evidence(int s) { return s == LG_CORRUPT || s == LG_NEWER || s == LG_ROLLBACK; }
static void both(int s, int fsrc) { pstate = tstate = s; last_fs = fsrc; }
static void empty_passport(void) { passport_init(&live); live.loaded_ok = 0; }

int ledger_attach(const struct lg_io *io)
{
    IO = io; commit = 0; last_fs = 0; A_pp = 0; A_tn = 0; A_th = 0;
    empty_passport(); trail_init(&ltr);
    pstate = tstate = LG_NODISK;
    if (!io || !io->mounted || !io->mounted()) return pstate;
    int ro = io->readonly ? io->readonly() : 0;
    uint64_t pid = 0, aid = 0, tid = 0;
    int rp = io->lookup(PRINCIPAL_PASSPORT, LEDGER_PASSPORT_FILE, &pid), ra = io->lookup(PRINCIPAL_PASSPORT, LEDGER_ANCHOR_FILE, &aid), rt = io->lookup(PRINCIPAL_TRAIL, LEDGER_TRAIL_FILE, &tid);
    if ((rp != FS_OK && rp != FS_ENOENT) || (ra != FS_OK && ra != FS_ENOENT) || (rt != FS_OK && rt != FS_ENOENT)) {
        both(LG_FAILED, rp != FS_OK && rp != FS_ENOENT ? rp : (ra != FS_OK && ra != FS_ENOENT ? ra : rt)); ltr.healthy = 0; return pstate;
    }

    /* ---- the anchor ---- */
    uint32_t apc = 0, atn = 0, n = 0, fsz = 0; uint64_t ath = 0; int apresent = 0;
    if (ra == FS_OK) {
        if (read_file(aid, anc, LEDGER_ANCHOR_BYTES, &n, &fsz) != FS_OK) { both(LG_CORRUPT, FS_EIO); ltr.healthy = 0; return pstate; }
        if (fsz != 0) {                                                       /* a created-but-never-written anchor is the same as none */
            int s = parse_anchor(anc, n, fsz, &apc, &atn, &ath);
            if (s != P_OK) { both(s == P_NEWER ? LG_NEWER : LG_CORRUPT, 0); ltr.healthy = 0; return pstate; }   /* neither ledger can be validated without its anchor */
            apresent = 1;
        }
    }

    /* ---- Passport ---- */
    int ps = LG_OK, repair_p = 0, have_p = 0; uint32_t pcm = 0, ilen = 0;
    if (rp == FS_OK) {
        if (read_file(pid, cont, LEDGER_PASSPORT_BYTES, &n, &fsz) != FS_OK) ps = LG_CORRUPT;
        else if (fsz != 0) {
            int s = parse_container(cont, n, fsz, &pcm, &ilen);
            if (s == P_NEWER) ps = LG_NEWER; else if (s != P_OK) ps = LG_CORRUPT; else have_p = 1;
        }
    }
    if (ps == LG_OK) {
        if (!have_p) { if (apresent && apc > 0) ps = LG_ROLLBACK; }                       /* the anchor vouches for a file that is gone or emptied */
        else if (!apresent) { if (pcm != 1) ps = LG_ROLLBACK; else repair_p = 1; }        /* only a first commit that crashed before its anchor may lack one */
        else if (pcm == apc + 1) repair_p = 1;                                            /* crash between the file and its anchor: the file is the committed state */
        else if (pcm != apc) ps = LG_ROLLBACK;                                            /* behind the anchor, or more than one commit ahead */
    }
    if (ps == LG_OK && have_p) {
        if (passport_decode(&live, cont + 16, ilen) != PP_OK) ps = LG_CORRUPT;
        else for (int i = 0; i < PP_MAX_GRANTS; i++) if (live.g[i].used && !principal_assignable(live.g[i].principal)) { ps = LG_CORRUPT; break; }   /* a durable grant to a reserved identity can only be tampering */
    }
    if (ps != LG_OK) empty_passport(); else { live.loaded_ok = 1; if (have_p) commit = pcm; }

    /* ---- Trail ---- */
    int ts = LG_OK, repair_t = 0, have_t = 0; uint32_t tcm = 0, tilen = 0;
    if (rt == FS_OK) {
        if (read_file(tid, tcont, LEDGER_TRAIL_BYTES, &n, &fsz) != FS_OK) ts = LG_CORRUPT;
        else if (fsz != 0) {
            int s = parse_tcontainer(tcont, n, fsz, &tcm, &tilen);
            if (s == P_NEWER) ts = LG_NEWER; else if (s != P_OK) ts = LG_CORRUPT; else have_t = 1;
        }
    }
    if (ts == LG_OK && have_t) {
        int rc = trail_decode(&ltr, tcont + 16, tilen);                                    /* keeps the verified prefix of a damaged chain for display */
        if (rc == TR_E_VERSION) ts = LG_NEWER;
        else if (rc != TR_OK || ltr.next_seq != tcm + 1) ts = LG_CORRUPT;
    }
    if (ts == LG_OK) {
        if (!have_t) { if (apresent && atn > 0) ts = LG_ROLLBACK; }
        else if (!apresent) { if (tcm != 1 || ltr.n != 1 || ltr.base_chain != 0) ts = LG_ROLLBACK; else repair_t = 1; }
        else if (tcm == atn) { if (trail_head(&ltr) != ath) ts = LG_ROLLBACK; }           /* same count but a different chain: rewritten */
        else if (tcm == atn + 1) {                                                         /* crash between the file and its anchor: the chain minus its newest record must be what the anchor vouches for */
            uint64_t before = ltr.n >= 2 ? ltr.r[ltr.n - 2].chain : ltr.base_chain;
            if (before != ath || (atn == 0 && (ltr.n != 1 || ltr.base_chain != 0))) ts = LG_ROLLBACK; else repair_t = 1;
        } else ts = LG_ROLLBACK;
    }
    if (ts != LG_OK) ltr.healthy = 0;

    /* ---- the combination ---- */
    if (ps == LG_OK && evidence(ts)) ps = LG_HELD;
    if (ts == LG_OK && evidence(ps)) ts = LG_HELD;
    pstate = ps; tstate = ts;
    commit = (have_p && !evidence(ps)) ? pcm : 0;
    A_pp = apc; A_tn = atn; A_th = ath;                                                 /* what the anchor on disk says; updated below only when a repair really wrote it */
    if (ro) { if (pstate == LG_OK) pstate = LG_RDONLY; if (tstate == LG_OK) tstate = LG_RDONLY; return pstate; }   /* verified, but nothing may be written (a pending repair waits for a writable boot) */
    if (pstate == LG_HELD || tstate == LG_HELD) return pstate;                                                       /* evidence is never touched */
    if ((repair_p && pstate == LG_OK) || (repair_t && tstate == LG_OK)) {
        uint32_t npp = (repair_p && pstate == LG_OK) ? pcm : apc, ntn = (repair_t && tstate == LG_OK) ? tcm : atn;
        uint64_t nth = (repair_t && tstate == LG_OK) ? trail_head(&ltr) : ath;
        int rc = write_anchor(npp, ntn, nth);
        if (rc != FS_OK) { both(LG_FAILED, rc); return pstate; }                      /* could not complete the repair: protected actions stay off */
        A_pp = npp; A_tn = ntn; A_th = nth;
    }
    return pstate;
}

int ledger_state(void) { return pstate; }
int ledger_trail_state(void) { return tstate; }
const char *ledger_state_name(int s)
{
    static const char *const N[] = { "no-disk", "ok", "read-only", "corrupt", "newer-version", "rollback-detected", "failed", "held" };
    return s >= 0 && s <= LG_HELD ? N[s] : "?";
}
int ledger_passport_ok(void) { return pstate == LG_OK; }
int ledger_trail_ok(void) { return tstate == LG_OK; }
int ledger_ok(void) { return pstate == LG_OK && tstate == LG_OK; }
uint32_t ledger_commit(void) { return commit; }
uint32_t ledger_trail_commit(void) { return ltr.next_seq ? ltr.next_seq - 1 : 0; }
int ledger_last_fs_error(void) { return last_fs; }
const struct passport *ledger_passport(void) { return &live; }
const struct trail *ledger_trail(void) { return &ltr; }

int ledger_peek(uint32_t principal, uint64_t gen, uint8_t perm)
{
    if (pstate != LG_FAILED) return passport_peek(&live, principal, gen, perm);
    /* After a store failure the durable table may no longer match the disk (a revocation may have been committed): honour NO durable grant, only RAM-only
     * instance-bound ones, which were never stored and cannot be stale. */
    if (principal == 0 || principal == 0xFFFFFFFFu || gen == 0) return 0;
    for (int i = 0; i < PP_MAX_GRANTS; i++) {
        const struct pp_grant *g = &live.g[i];
        if (g->used && g->principal == principal && g->perm == perm && g->gen != 0 && g->gen == gen) return 1;
    }
    return 0;
}

static int mutate(int revoke, uint32_t principal, uint8_t perm)
{
    if (pstate == LG_RDONLY) return LG_E_RDONLY;
    if (pstate != LG_OK) return LG_E_STATE;
    if (IO->readonly && IO->readonly()) { pstate = LG_RDONLY; return LG_E_RDONLY; }
    if (!principal_assignable(principal)) return LG_E_ARG;                 /* a durable grant names a stable APPLICATION identity: never the kernel, none or a reserved service */
    tmp = live;
    struct pp_actor k; k.kind = PP_ACTOR_KERNEL; k.pid = 0; k.gen = 0; k.principal = 0;
    int rc = revoke ? passport_revoke(&tmp, &k, principal, perm) : passport_grant(&tmp, &k, principal, 0, perm);
    if (rc != PP_OK) return LG_E_PASSPORT;                                   /* refused by the Passport itself (bad principal, quota, no such grant): no I/O */
    { int same_tbl = 1; for (int i = 0; i < PP_MAX_GRANTS; i++) { const struct pp_grant *a = &live.g[i], *b = &tmp.g[i]; if (a->used != b->used || a->principal != b->principal || a->gen != b->gen || a->perm != b->perm) same_tbl = 0; }
      if (same_tbl) return LG_E_OK; }                                       /* already exactly so (an idempotent grant): nothing to write, nothing to claim */
    size_t il = passport_encode(&tmp, img, sizeof img);
    if (il == 0 || commit == 0xFFFFFFFFu) return LG_E_ARG;
    build_container(cont, img, (uint32_t)il, commit + 1);
    rc = store_file(PRINCIPAL_PASSPORT, LEDGER_PASSPORT_FILE, cont, LEDGER_PASSPORT_BYTES);
    if (rc != FS_OK) {
        last_fs = rc;
        if (rc == FS_ERO) { pstate = LG_RDONLY; return LG_E_RDONLY; }
        if (definite_refusal(rc)) return LG_E_NOSPACE;                       /* the file is unchanged: still healthy */
        pstate = LG_FAILED; return LG_E_UNKNOWN;                             /* the file may or may not hold the new state: the next boot decides */
    }
    rc = write_anchor(commit + 1, A_tn, A_th);
    if (rc != FS_OK) { last_fs = rc; pstate = LG_FAILED; return LG_E_UNKNOWN; }   /* the file is already the NEW state, the anchor is not: committed on disk, unknown to the caller */
    live = tmp; commit++; A_pp = commit;
    return LG_E_OK;
}
int ledger_grant(uint32_t principal, uint8_t perm) { return mutate(0, principal, perm); }
int ledger_revoke(uint32_t principal, uint8_t perm) { return mutate(1, principal, perm); }

/* The Trail's write hook: trail_append hands us the encoded image; we keep it and write nothing here, so the anchor can be written after the file. */
static size_t cap_len;
static int cap_write(const uint8_t *buf, size_t len) { if (len > sizeof timg) return 1; for (size_t i = 0; i < len; i++) timg[i] = buf[i]; cap_len = len; return 0; }
static const struct tr_store CAP = { cap_write, 0 };

int ledger_trail_append(const struct trail_rec *in)
{
    if (tstate == LG_RDONLY) return LG_E_RDONLY;
    if (tstate != LG_OK) return LG_E_STATE;
    if (IO->readonly && IO->readonly()) { tstate = LG_RDONLY; return LG_E_RDONLY; }
    if (!in) return LG_E_ARG;
    ttmp = ltr; cap_len = 0;
    if (trail_append(&ttmp, &CAP, in) != TR_OK || cap_len == 0) return LG_E_ARG;      /* invalid fields / exhausted sequence: nothing written */
    uint32_t records_ever = ttmp.next_seq - 1;
    build_tcontainer(tcont, timg, (uint32_t)cap_len, records_ever);
    int rc = store_file(PRINCIPAL_TRAIL, LEDGER_TRAIL_FILE, tcont, LEDGER_TRAIL_BYTES);
    if (rc != FS_OK) {
        last_fs = rc;
        if (rc == FS_ERO) { tstate = LG_RDONLY; return LG_E_RDONLY; }
        if (definite_refusal(rc)) return LG_E_NOSPACE;
        tstate = LG_FAILED; return LG_E_UNKNOWN;
    }
    rc = write_anchor(A_pp, records_ever, trail_head(&ttmp));
    if (rc != FS_OK) { last_fs = rc; tstate = LG_FAILED; return LG_E_UNKNOWN; }
    ltr = ttmp; A_tn = records_ever; A_th = trail_head(&ltr);
    return LG_E_OK;
}

int ledger_grant_instance(uint32_t principal, uint64_t gen, uint8_t perm)
{
    if (gen == 0) return LG_E_ARG;                                          /* durable grants go through ledger_grant only */
    struct pp_actor k; k.kind = PP_ACTOR_KERNEL; k.pid = 0; k.gen = 0; k.principal = 0;
    return passport_grant(&live, &k, principal, gen, perm) == PP_OK ? LG_E_OK : LG_E_PASSPORT;
}
int ledger_drop_instance(uint64_t gen) { return passport_drop_instance(&live, gen); }
