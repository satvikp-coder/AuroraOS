/* kernel/ui/trail.c - the Trail record chain (see trail.h). */
#include <trail.h>

static void w32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static uint32_t r32(const uint8_t *b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static void w64(uint8_t *b, uint64_t v) { w32(b, (uint32_t)v); w32(b + 4, (uint32_t)(v >> 32)); }
static uint64_t r64(const uint8_t *b) { return (uint64_t)r32(b) | ((uint64_t)r32(b + 4) << 32); }
static uint32_t fnv32(const uint8_t *b, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; } return h; }

/* the record's 32 content bytes (everything but the chain value) */
static void rec_bytes(const struct trail_rec *r, uint8_t *o)
{
    w32(o, r->seq); w32(o + 4, r->pid); w32(o + 8, r->principal);
    o[12] = r->kind; o[13] = r->target; o[14] = r->risk; o[15] = r->decision; o[16] = r->reason; o[17] = r->state; o[18] = o[19] = 0;
    w32(o + 20, (uint32_t)r->arg); w32(o + 24, r->uptime_ms); w32(o + 28, 0);
}
static uint64_t chain_of(uint64_t prev, const struct trail_rec *r)
{
    uint8_t b[8 + 32]; w64(b, prev); rec_bytes(r, b + 8);
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < sizeof b; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}
static int fields_ok(const struct trail_rec *r) { return r->kind < 16 && r->risk <= 3 && r->decision <= 4 && r->reason <= 16 && r->state <= 6; }

void trail_init(struct trail *t)
{
    uint8_t *z = (uint8_t *)t; for (size_t i = 0; i < sizeof *t; i++) z[i] = 0;
    t->healthy = 1; t->next_seq = 1;
}

size_t trail_encode(const struct trail *t, uint8_t *buf, size_t cap)
{
    size_t need = 24 + (size_t)t->n * TR_REC_BYTES + 4;
    if (!buf || cap < need) return 0;
    buf[0] = 'T'; buf[1] = 'R'; buf[2] = 'L'; buf[3] = '1'; buf[4] = TR_VERSION; buf[5] = (uint8_t)t->n; buf[6] = buf[7] = 0;
    w32(buf + 8, t->next_seq); w64(buf + 12, t->base_chain); w32(buf + 20, t->evicted);
    size_t o = 24;
    for (uint32_t i = 0; i < t->n; i++) { rec_bytes(&t->r[i], buf + o); w64(buf + o + 32, t->r[i].chain); o += TR_REC_BYTES; }
    w32(buf + o, fnv32(buf, o));
    return o + 4;
}

int trail_verify(const struct trail *t)
{
    uint64_t prev = t->base_chain; uint32_t seq = 0;
    if (t->n > TR_MAX) return TR_E_FORMAT;
    for (uint32_t i = 0; i < t->n; i++) {
        const struct trail_rec *r = &t->r[i];
        if (!fields_ok(r)) return TR_E_FORMAT;
        if (i && r->seq != seq + 1) return TR_E_CHAIN;
        if (r->chain != chain_of(prev, r)) return TR_E_CHAIN;
        prev = r->chain; seq = r->seq;
    }
    if (t->n && t->next_seq != seq + 1) return TR_E_CHAIN;
    return TR_OK;
}

int trail_append(struct trail *t, const struct tr_store *st, const struct trail_rec *in)
{
    if (!t || !st || !st->write || !in || !fields_ok(in)) return TR_E_ARG;
    if (!t->healthy) return TR_E_STORE;                                   /* a damaged trail is not extended */
    if (t->next_seq == 0xFFFFFFFFu) return TR_E_ARG;                       /* the sequence never wraps */
    struct trail c = *t;                                                   /* work on a copy: memory changes only if the write succeeded */
    struct trail_rec r = *in; r.seq = c.next_seq; r.chain = 0;
    uint64_t prev = c.n ? c.r[c.n - 1].chain : c.base_chain;
    r.chain = chain_of(prev, &r);
    if (c.n == TR_MAX) {                                                   /* evict the oldest; its chain value becomes the new base */
        c.base_chain = c.r[0].chain;
        for (uint32_t i = 1; i < TR_MAX; i++) c.r[i - 1] = c.r[i];
        c.n--; c.evicted++;
    }
    c.r[c.n++] = r; c.next_seq++;
    uint8_t img[TR_IMAGE_MAX]; size_t len = trail_encode(&c, img, sizeof img);
    if (!len || st->write(img, len)) return TR_E_STORE;                    /* not committed: the caller must NOT run the action */
    *t = c;
    return TR_OK;
}

int trail_decode(struct trail *t, const uint8_t *buf, size_t len)
{
    trail_init(t); t->healthy = 0;
    if (!buf || len < 28) return TR_E_FORMAT;
    if (buf[0] != 'T' || buf[1] != 'R' || buf[2] != 'L' || buf[3] != '1') return TR_E_FORMAT;
    if (buf[4] != TR_VERSION) return TR_E_VERSION;
    uint32_t n = buf[5];
    if (n > TR_MAX || buf[6] || buf[7]) return TR_E_FORMAT;
    if (len != 24 + (size_t)n * TR_REC_BYTES + 4) return TR_E_FORMAT;     /* a torn or padded image is not guessed at */
    int whole = r32(buf + 24 + (size_t)n * TR_REC_BYTES) == fnv32(buf, 24 + (size_t)n * TR_REC_BYTES);
    struct trail_rec tmp[TR_MAX]; uint32_t good = 0;
    uint64_t prev = r64(buf + 12);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = buf + 24 + (size_t)i * TR_REC_BYTES;
        struct trail_rec r;
        r.seq = r32(e); r.pid = r32(e + 4); r.principal = r32(e + 8); r.kind = e[12]; r.target = e[13]; r.risk = e[14]; r.decision = e[15]; r.reason = e[16]; r.state = e[17];
        r.arg = (int32_t)r32(e + 20); r.uptime_ms = r32(e + 24); r.chain = r64(e + 32);
        if (e[18] || e[19] || r32(e + 28) || !fields_ok(&r)) break;
        if (i && r.seq != tmp[i - 1].seq + 1) break;
        if (r.chain != chain_of(prev, &r)) break;
        tmp[good++] = r; prev = r.chain;
    }
    t->base_chain = r64(buf + 12); t->evicted = r32(buf + 20);
    for (uint32_t i = 0; i < good; i++) t->r[i] = tmp[i];
    t->n = good; t->recovered = good; t->lost = n - good;
    t->next_seq = good ? tmp[good - 1].seq + 1 : r32(buf + 8);
    if (good == n && whole && r32(buf + 8) == t->next_seq) { t->healthy = 1; return TR_OK; }
    t->healthy = 0;
    return TR_E_CHAIN;
}

int trail_load(struct trail *t, const struct tr_store *st)
{
    uint8_t buf[TR_IMAGE_MAX]; size_t len = 0;
    if (!st || !st->read || st->read(buf, sizeof buf, &len)) { trail_init(t); t->healthy = 0; return TR_E_STORE; }
    return trail_decode(t, buf, len);
}
