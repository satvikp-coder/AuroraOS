/* kernel/ui/passport.c - the Passport permission ledger (see passport.h). */
#include <passport.h>

#define NO_PRINCIPAL 0xFFFFFFFFu

/* Mutations (grant, revoke, load, save) and denied checks go to SEPARATE rings: a flood of denials must never evict a grant or revoke record. */
static void audit(struct passport *p, uint8_t op, uint8_t perm, int result, const struct pp_actor *by, uint32_t principal)
{
    int denial = op == PP_OP_CHECK_DENY;
    uint32_t *sq = denial ? &p->dseq : &p->seq;
    struct pp_audit_rec *r = denial ? &p->dlog[*sq % PP_AUDIT] : &p->log[*sq % PP_AUDIT];
    r->seq = *sq; r->op = op; r->perm = perm; r->result = (int8_t)result;
    r->actor_kind = by ? by->kind : 0; r->actor_pid = by ? by->pid : 0; r->principal = principal;
    (*sq)++;
}

void passport_init(struct passport *p)
{
    uint8_t *z = (uint8_t *)p;
    for (size_t i = 0; i < sizeof *p; i++) z[i] = 0;
    p->loaded_ok = 1;                                           /* an empty table made on purpose is a valid state */
}

static int principal_ok(uint32_t pr) { return pr != 0 && pr != NO_PRINCIPAL; }

static int actor_ok(const struct pp_actor *by) { return by && by->kind == PP_ACTOR_KERNEL; }

int passport_grant(struct passport *p, const struct pp_actor *by, uint32_t principal, uint64_t gen, uint8_t perm)
{
    int rc = PP_OK;
    if (!p) return PP_E_ARG;
    if (!actor_ok(by)) rc = PP_E_ACTOR;                         /* a process can never grant: itself, anyone, anything */
    else if (perm == PP_NONE || perm >= PP_COUNT) rc = PP_E_PERM;
    else if (!principal_ok(principal)) rc = PP_E_PRINCIPAL;
    else {
        int slot = -1;
        for (int i = 0; i < PP_MAX_GRANTS; i++)
            if (p->g[i].used && p->g[i].principal == principal && p->g[i].perm == perm && p->g[i].gen == gen) { slot = i; break; }     /* already granted: idempotent */
        if (slot < 0) {
            int held = 0;
            for (int i = 0; i < PP_MAX_GRANTS; i++) if (p->g[i].used && p->g[i].principal == principal) held++;
            if (held >= PP_PER_PRINCIPAL) rc = PP_E_QUOTA;                                                                          /* one principal cannot fill the shared table */
            else {
                for (int i = 0; i < PP_MAX_GRANTS; i++) if (!p->g[i].used) { slot = i; break; }
                if (slot < 0) rc = PP_E_FULL;
            }
        }
        if (rc == PP_OK) { p->g[slot].used = 1; p->g[slot].principal = principal; p->g[slot].perm = perm; p->g[slot].gen = gen; }
    }
    audit(p, PP_OP_GRANT, perm, rc, by, principal);
    return rc;
}

int passport_revoke(struct passport *p, const struct pp_actor *by, uint32_t principal, uint8_t perm)
{
    int rc = PP_E_NOENT, n = 0;
    if (!p) return PP_E_ARG;
    if (!actor_ok(by)) rc = PP_E_ACTOR;
    else if (perm == PP_NONE || perm >= PP_COUNT) rc = PP_E_PERM;
    else if (!principal_ok(principal)) rc = PP_E_PRINCIPAL;
    else {
        for (int i = 0; i < PP_MAX_GRANTS; i++)
            if (p->g[i].used && p->g[i].principal == principal && p->g[i].perm == perm) { p->g[i].used = 0; p->g[i].principal = 0; p->g[i].gen = 0; p->g[i].perm = 0; n++; }   /* every generation of it */
        rc = n ? PP_OK : PP_E_NOENT;
    }
    audit(p, PP_OP_REVOKE, perm, rc, by, principal);
    return rc;
}

int passport_peek(const struct passport *p, uint32_t principal, uint64_t gen, uint8_t perm)
{
    if (!p || perm == PP_NONE || perm >= PP_COUNT || !principal_ok(principal)) return 0;
    for (int i = 0; i < PP_MAX_GRANTS; i++) {
        const struct pp_grant *g = &p->g[i];
        if (!g->used || g->principal != principal || g->perm != perm) continue;
        if (g->gen == 0 || (gen != 0 && g->gen == gen)) return 1;      /* a generation-bound grant needs that exact generation; an unknown generation (0) never matches one */
    }
    return 0;
}

int passport_check(struct passport *p, uint32_t principal, uint64_t gen, uint8_t perm)
{
    if (!p) return 0;
    int allowed = passport_peek(p, principal, gen, perm);
    if (!allowed) audit(p, PP_OP_CHECK_DENY, perm, (perm == PP_NONE || perm >= PP_COUNT || !principal_ok(principal)) ? PP_E_ARG : PP_E_PERM, 0, principal);   /* denials are recorded (in their own ring); allowed checks are not */
    return allowed;
}

int passport_drop_instance(struct passport *p, uint64_t gen)
{
    int n = 0;
    if (!p || gen == 0) return 0;
    for (int i = 0; i < PP_MAX_GRANTS; i++)
        if (p->g[i].used && p->g[i].gen == gen) { p->g[i].used = 0; p->g[i].principal = 0; p->g[i].gen = 0; p->g[i].perm = 0; n++; }
    return n;
}

static uint32_t ring_count(uint32_t seq) { return seq < PP_AUDIT ? seq : PP_AUDIT; }
uint32_t passport_audit_count(const struct passport *p) { return ring_count(p->seq); }
const struct pp_audit_rec *passport_audit_at(const struct passport *p, uint32_t i)
{
    uint32_t n = passport_audit_count(p);
    if (i >= n) return 0;
    uint32_t first = p->seq - n;
    return &p->log[(first + i) % PP_AUDIT];
}
uint32_t passport_denial_count(const struct passport *p) { return ring_count(p->dseq); }
const struct pp_audit_rec *passport_denial_at(const struct passport *p, uint32_t i)
{
    uint32_t n = passport_denial_count(p);
    if (i >= n) return 0;
    uint32_t first = p->dseq - n;
    return &p->dlog[(first + i) % PP_AUDIT];
}

/* ---- image: "PPRT" version(1) count(1) reserved(2) seq(4) | count x { principal(4) gen(8, always 0) perm(1) pad(3) } | checksum(4, FNV-1a over everything before it) ----
 * Only generation-free grants are stored: a process generation is meaningful for one boot only. */
static void w32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static uint32_t r32(const uint8_t *b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static uint32_t fnv(const uint8_t *b, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; } return h; }

size_t passport_encode(const struct passport *p, uint8_t *buf, size_t cap)
{
    uint8_t n = 0;
    for (int i = 0; i < PP_MAX_GRANTS; i++) if (p->g[i].used && p->g[i].gen == 0) n++;
    size_t need = 12 + (size_t)n * 16 + 4;
    if (!buf || cap < need) return 0;
    buf[0] = 'P'; buf[1] = 'P'; buf[2] = 'R'; buf[3] = 'T'; buf[4] = PP_VERSION; buf[5] = n; buf[6] = 0; buf[7] = 0; w32(buf + 8, p->seq);
    size_t o = 12;
    for (int i = 0; i < PP_MAX_GRANTS; i++) {
        if (!p->g[i].used || p->g[i].gen != 0) continue;
        w32(buf + o, p->g[i].principal); w32(buf + o + 4, 0); w32(buf + o + 8, 0);
        buf[o + 12] = p->g[i].perm; buf[o + 13] = buf[o + 14] = buf[o + 15] = 0; o += 16;
    }
    w32(buf + o, fnv(buf, o));
    return o + 4;
}

int passport_decode(struct passport *p, const uint8_t *buf, size_t len)
{
    passport_init(p); p->loaded_ok = 0;                         /* any failure below leaves this empty, denying table */
    if (!buf || len < 16) return PP_E_FORMAT;
    if (buf[0] != 'P' || buf[1] != 'P' || buf[2] != 'R' || buf[3] != 'T') return PP_E_FORMAT;
    if (buf[4] != PP_VERSION) return PP_E_VERSION;              /* a newer image is not guessed at */
    uint32_t n = buf[5];
    if (n > PP_MAX_GRANTS || buf[6] || buf[7]) return PP_E_FORMAT;
    size_t body = 12 + (size_t)n * 16;
    if (len != body + 4) return PP_E_FORMAT;
    if (r32(buf + body) != fnv(buf, body)) return PP_E_FORMAT;
    struct passport t; passport_init(&t); t.seq = r32(buf + 8);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = buf + 12 + i * 16;
        uint32_t pr = r32(e); uint8_t perm = e[12];
        if (!principal_ok(pr) || perm == PP_NONE || perm >= PP_COUNT || r32(e + 4) || r32(e + 8) || e[13] || e[14] || e[15]) return PP_E_FORMAT;      /* an instance-bound record on disk is refused */
        for (uint32_t k = 0; k < i; k++) if (t.g[k].principal == pr && t.g[k].perm == perm) return PP_E_FORMAT;                                          /* duplicates are corruption */
        uint32_t held = 0; for (uint32_t k = 0; k < i; k++) if (t.g[k].principal == pr) held++;
        if (held >= PP_PER_PRINCIPAL) return PP_E_FORMAT;
        t.g[i].used = 1; t.g[i].principal = pr; t.g[i].gen = 0; t.g[i].perm = perm;
    }
    t.loaded_ok = 1;
    *p = t;
    return PP_OK;
}

int passport_save(struct passport *p, const struct pp_store *st)
{
    uint8_t buf[PP_IMAGE_MAX];
    size_t n = passport_encode(p, buf, sizeof buf);
    int rc = (!st || !st->write || !n) ? PP_E_STORE : (st->write(buf, n) ? PP_E_STORE : PP_OK);
    audit(p, PP_OP_SAVE, 0, rc, 0, 0);
    return rc;
}

int passport_load(struct passport *p, const struct pp_store *st)
{
    uint8_t buf[PP_IMAGE_MAX]; size_t len = 0;
    if (!st || !st->read || st->read(buf, sizeof buf, &len)) { passport_init(p); p->loaded_ok = 0; audit(p, PP_OP_LOAD, 0, PP_E_STORE, 0, 0); return PP_E_STORE; }
    int rc = passport_decode(p, buf, len);
    audit(p, PP_OP_LOAD, 0, rc, 0, 0);
    return rc;
}
