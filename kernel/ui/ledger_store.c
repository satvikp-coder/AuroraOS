/* kernel/ui/ledger_store.c - the durable Passport ledger (see kernel/include/ledger.h for the commit protocol and its crash analysis). Pure: all storage goes
 * through struct lg_io, so every failure and every crash point is testable on the host. No allocation, no libc, no clock. */
#include <ledger.h>
#include <fs.h>
#include <principal.h>

#define PAY 556                                       /* container bytes covered by the CRC (16 header + PP_IMAGE_MAX 536 + 4 pad) */
_Static_assert(16 + PP_IMAGE_MAX <= PAY, "the Passport image must fit the fixed container");
_Static_assert(PAY + 4 == LEDGER_PASSPORT_BYTES, "container size");

static const struct lg_io *IO;
static int state = LG_NODISK;
static struct passport live, tmp;
static uint32_t commit;
static int last_fs;
static uint8_t cont[LEDGER_PASSPORT_BYTES + 16], back[LEDGER_PASSPORT_BYTES + 16], anc[LEDGER_ANCHOR_BYTES + 16], img[PP_IMAGE_MAX + 16];

static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t crc32(const uint8_t *p, uint32_t n)           /* CRC-32 (IEEE), bitwise: small and enough for 560 bytes */
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
static void build_anchor(uint8_t *o, uint32_t pp_commit)
{
    zero(o, LEDGER_ANCHOR_BYTES);
    o[0] = 'A'; o[1] = 'N'; o[2] = 'C'; o[3] = 'H';
    w32(o + 4, LEDGER_CONTAINER_VERSION); w32(o + 8, pp_commit);                   /* 12..27: reserved Trail fields, zero until Trail is persisted */
    w32(o + 28, crc32(o, 28));
}

enum { P_OK = 0, P_CORRUPT, P_NEWER };
/* Parse the container in b (n bytes were read of a file of fsize bytes). A different version is judged by its header alone and never interpreted. */
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
static int parse_anchor(const uint8_t *b, uint32_t n, uint32_t fsize, uint32_t *pp_commit)
{
    if (n < 8 || !(b[0] == 'A' && b[1] == 'N' && b[2] == 'C' && b[3] == 'H')) return P_CORRUPT;
    if (r32(b + 4) != LEDGER_CONTAINER_VERSION) return P_NEWER;
    if (fsize != LEDGER_ANCHOR_BYTES || n != LEDGER_ANCHOR_BYTES) return P_CORRUPT;
    if (r32(b + 28) != crc32(b, 28)) return P_CORRUPT;
    *pp_commit = r32(b + 8);
    if (*pp_commit == 0) return P_CORRUPT;
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
static int store_file(const char *name, const uint8_t *buf, uint32_t len)
{
    uint64_t id; int rc = IO->lookup(name, &id);
    if (rc == FS_ENOENT) rc = IO->create(name, &id);
    if (rc != FS_OK) return rc;
    int w = IO->write(id, buf, len);
    if (w < 0) return w;
    if ((uint32_t)w != len) return FS_EIO;
    uint32_t n = 0, fsz = 0;
    rc = read_file(id, back, len, &n, &fsz);
    if (rc != FS_OK) return rc;
    if (fsz != len || n != len || !same(back, buf, len)) return FS_EIO;       /* the medium does not hold what we wrote */
    return FS_OK;
}

static int fail_state(int s, int fsrc) { state = s; last_fs = fsrc; return s; }

int ledger_attach(const struct lg_io *io)
{
    IO = io; commit = 0; last_fs = 0;
    passport_init(&live); live.loaded_ok = 0;
    if (!io || !io->mounted || !io->mounted()) return state = LG_NODISK;
    int ro = io->readonly ? io->readonly() : 0;
    uint64_t pid = 0, aid = 0;
    int rp = io->lookup(LEDGER_PASSPORT_FILE, &pid), ra = io->lookup(LEDGER_ANCHOR_FILE, &aid);
    if ((rp != FS_OK && rp != FS_ENOENT) || (ra != FS_OK && ra != FS_ENOENT)) return fail_state(LG_FAILED, rp != FS_OK && rp != FS_ENOENT ? rp : ra);
    if (rp == FS_ENOENT && ra == FS_ENOENT) { live.loaded_ok = 1; return state = ro ? LG_RDONLY : LG_OK; }          /* a fresh disk: empty, deny-by-default table */

    uint32_t acm = 0, pcm = 0, ilen = 0, n = 0, fsz = 0;
    int pstate = P_OK, astate = P_OK;
    if (ra == FS_OK) {
        if (read_file(aid, anc, LEDGER_ANCHOR_BYTES, &n, &fsz) != FS_OK) return fail_state(LG_CORRUPT, FS_EIO);
        if (fsz == 0) ra = FS_ENOENT;                                       /* created but never written (a first commit that died inside the write): the same as no anchor */
        else {
            astate = parse_anchor(anc, n, fsz, &acm);
            if (astate == P_NEWER) return state = LG_NEWER;
            if (astate != P_OK) return state = LG_CORRUPT;
        }
    }
    if (rp == FS_OK) {
        if (read_file(pid, cont, LEDGER_PASSPORT_BYTES, &n, &fsz) != FS_OK) return fail_state(LG_CORRUPT, FS_EIO);
        if (fsz == 0) rp = FS_ENOENT;                                       /* created but never written: not a damaged ledger, an interrupted creation */
        else {
            pstate = parse_container(cont, n, fsz, &pcm, &ilen);
            if (pstate == P_NEWER) return state = LG_NEWER;
            if (pstate != P_OK) return state = LG_CORRUPT;
        }
    }
    if (rp == FS_ENOENT && ra == FS_ENOENT) { live.loaded_ok = 1; return state = ro ? LG_RDONLY : LG_OK; }   /* nothing (or only empty shells) was ever committed */
    int repair = 0;
    if (rp == FS_ENOENT) return state = LG_ROLLBACK;                         /* an anchor vouches for a file that is gone */
    if (ra == FS_ENOENT) { if (pcm != 1) return state = LG_ROLLBACK; repair = 1; }   /* only a first commit that crashed before its anchor may lack one */
    else if (pcm == acm + 1) repair = 1;                                    /* crash between the file and its anchor: the file is the newer, committed state */
    else if (pcm != acm) return state = LG_ROLLBACK;                         /* behind the anchor, or more than one commit ahead */
    if (passport_decode(&live, cont + 16, ilen) != PP_OK) { passport_init(&live); live.loaded_ok = 0; return state = LG_CORRUPT; }
    for (int i = 0; i < PP_MAX_GRANTS; i++) if (live.g[i].used && !principal_assignable(live.g[i].principal)) { passport_init(&live); live.loaded_ok = 0; return state = LG_CORRUPT; }   /* a durable grant to a reserved identity can only be tampering */
    commit = pcm; live.loaded_ok = 1;
    if (ro) return state = LG_RDONLY;                                       /* verified, but nothing may be written (the pending anchor repair waits for a writable boot) */
    if (repair) {
        build_anchor(anc, pcm);
        int rc = store_file(LEDGER_ANCHOR_FILE, anc, LEDGER_ANCHOR_BYTES);
        if (rc != FS_OK) return fail_state(LG_FAILED, rc);                   /* could not complete the repair: protected actions stay off */
    }
    return state = LG_OK;
}

int ledger_state(void) { return state; }
const char *ledger_state_name(int s)
{
    static const char *const N[] = { "no-disk", "ok", "read-only", "corrupt", "newer-version", "rollback-detected", "failed" };
    return s >= 0 && s <= LG_FAILED ? N[s] : "?";
}
int ledger_passport_ok(void) { return state == LG_OK; }
uint32_t ledger_commit(void) { return commit; }
int ledger_last_fs_error(void) { return last_fs; }
const struct passport *ledger_passport(void) { return &live; }

int ledger_peek(uint32_t principal, uint64_t gen, uint8_t perm)
{
    if (state != LG_FAILED) return passport_peek(&live, principal, gen, perm);
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
    if (state == LG_RDONLY) return LG_E_RDONLY;
    if (state != LG_OK) return LG_E_STATE;
    if (IO->readonly && IO->readonly()) { state = LG_RDONLY; return LG_E_RDONLY; }
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
    rc = store_file(LEDGER_PASSPORT_FILE, cont, LEDGER_PASSPORT_BYTES);
    if (rc != FS_OK) {
        last_fs = rc;
        if (rc == FS_ERO) { state = LG_RDONLY; return LG_E_RDONLY; }
        if (definite_refusal(rc)) return LG_E_NOSPACE;                       /* the file is unchanged: still healthy */
        fail_state(LG_FAILED, rc); return LG_E_UNKNOWN;                      /* the file may or may not hold the new state: the next boot decides */
    }
    build_anchor(anc, commit + 1);
    rc = store_file(LEDGER_ANCHOR_FILE, anc, LEDGER_ANCHOR_BYTES);
    if (rc != FS_OK) { fail_state(LG_FAILED, rc); return LG_E_UNKNOWN; }     /* the file is already the NEW state, the anchor is not: committed on disk, unknown to the caller */
    live = tmp; commit++;
    return LG_E_OK;
}
int ledger_grant(uint32_t principal, uint8_t perm) { return mutate(0, principal, perm); }
int ledger_revoke(uint32_t principal, uint8_t perm) { return mutate(1, principal, perm); }

int ledger_grant_instance(uint32_t principal, uint64_t gen, uint8_t perm)
{
    if (gen == 0) return LG_E_ARG;                                          /* durable grants go through ledger_grant only */
    struct pp_actor k; k.kind = PP_ACTOR_KERNEL; k.pid = 0; k.gen = 0; k.principal = 0;
    return passport_grant(&live, &k, principal, gen, perm) == PP_OK ? LG_E_OK : LG_E_PASSPORT;
}
int ledger_drop_instance(uint64_t gen) { return passport_drop_instance(&live, gen); }
