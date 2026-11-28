#include <modelstore.h>
#include <model_trust.h>
#include <sha256.h>

static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t g64(const uint8_t *p) { return (uint64_t)g32(p) | ((uint64_t)g32(p + 4) << 32); }
static int all_zero(const uint8_t *p, size_t n) { uint8_t d = 0; for (size_t i = 0; i < n; i++) d |= p[i]; return d == 0; }
static int name_ok(const uint8_t *n, char *out)
{
    size_t len = 0;
    while (len < MS_NAME_MAX + 1 && n[len]) len++;
    if (len == 0 || len > MS_NAME_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
        out[i] = (char)c;
    }
    for (size_t i = len; i < MS_NAME_MAX + 1; i++) { if (n[i]) return 0; out[i] = 0; }          /* NUL padded, nothing after the terminator */
    return 1;
}
static int same_name(const char *a, const char *b) { for (size_t i = 0; i <= MS_NAME_MAX; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; } return 1; }

int ms_parse(const uint8_t *h, const uint8_t *t, size_t table_len, struct ms_volume *out)
{
    if (!h || !out || (!t && table_len)) return MS_EARG;
    static const uint8_t MAGIC[8] = { 'A', 'U', 'R', 'M', 'O', 'D', 'E', 'L' };
    for (int i = 0; i < 8; i++) if (h[i] != MAGIC[i]) return MS_EMAGIC;
    if (g32(h + 8) != 1) return MS_EVERSION;
    if (g32(h + 12) != MS_SECTOR) return MS_EGEOM;
    uint64_t total = g64(h + 16); uint32_t n = g32(h + 24), ts = g32(h + 28), tn = g32(h + 32);
    if (n > MS_MAX_OBJECTS) return MS_ECOUNT;
    if (total < 2 || total > MS_MAX_TOTAL_SECTORS) return MS_EGEOM;
    if (ts != 1) return MS_EGEOM;                                                      /* the table follows the header directly */
    if (tn != (n * MS_ENTRY + MS_SECTOR - 1) / MS_SECTOR) return MS_EGEOM;
    if ((uint64_t)ts + tn > total) return MS_EGEOM;
    if (g32(h + 36) != 0 || !all_zero(h + 40, 24) || !all_zero(h + 128, MS_SECTOR - 128)) return MS_EFORMAT;
    struct sha256 c; uint8_t d[32];
    sha256_init(&c); sha256_update(&c, h, 96); sha256_final(&c, d);
    if (!sha256_equal(d, h + 96)) return MS_EHDRDIG;
    size_t need = (size_t)n * MS_ENTRY, padded = (size_t)tn * MS_SECTOR;
    if (table_len < padded) return MS_EARG;                                            /* the caller must supply whole table sectors */
    sha256_init(&c); sha256_update(&c, t, need); sha256_final(&c, d);
    if (!sha256_equal(d, h + 64)) return MS_ETBLDIG;
    if (!all_zero(t + need, padded - need)) return MS_EFORMAT;                         /* padding after the last entry is zero */
    uint64_t data_start = (uint64_t)ts + tn;
    out->total_sectors = total; out->nobjects = n; out->table_sector = ts; out->table_sectors = tn;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = t + (size_t)i * MS_ENTRY; struct ms_object *o = &out->obj[i];
        if (!name_ok(e, o->name)) return MS_ENAME;
        o->size = g64(e + 32); o->first_sector = g64(e + 40); o->sectors = g64(e + 48); o->kind = g32(e + 56);
        if (g32(e + 60) != 0 || !all_zero(e + 96, 32)) return MS_EFORMAT;
        for (int k = 0; k < 32; k++) o->digest[k] = e[64 + k];
        if (o->size == 0 || o->size > total * MS_SECTOR) return MS_EEXTENT;
        if (o->sectors != (o->size + MS_SECTOR - 1) / MS_SECTOR) return MS_EEXTENT;    /* size <= 2^49 so the addition cannot wrap */
        if (o->first_sector < data_start || o->first_sector > total || o->sectors > total - o->first_sector) return MS_EEXTENT;
        for (uint32_t j = 0; j < i; j++) {
            if (same_name(out->obj[j].name, o->name)) return MS_EDUP;
            const struct ms_object *p = &out->obj[j];
            if (o->first_sector < p->first_sector + p->sectors && p->first_sector < o->first_sector + o->sectors) return MS_EEXTENT;   /* extents never overlap */
        }
    }
    return MS_OK;
}

int ms_verify_object(const struct ms_volume *v, uint32_t index, ms_read_fn rd, void *ctx)
{
    if (!v || !rd || index >= v->nobjects) return MS_EARG;
    const struct ms_object *o = &v->obj[index];
    struct sha256 c;
    uint8_t buf[MS_VERIFY_BATCH_SECTORS * MS_SECTOR], d[32];
    sha256_init(&c);
    uint64_t left = o->size;
    for (uint64_t sector = 0; sector < o->sectors;) {
        uint64_t remaining = o->sectors - sector;
        uint32_t count = remaining < MS_VERIFY_BATCH_SECTORS ?
                         (uint32_t)remaining : MS_VERIFY_BATCH_SECTORS;
        if (rd(ctx, o->first_sector + sector, count, buf) != 0) return MS_EIO;
        size_t bytes = (size_t)count * MS_SECTOR;
        size_t take = left < bytes ? (size_t)left : bytes;
        sha256_update(&c, buf, take);
        if (take < bytes && !all_zero(buf + take, bytes - take)) return MS_EDATA;
        left -= take;
        sector += count;
    }
    sha256_final(&c, d);
    return sha256_equal(d, o->digest) ? MS_OK : MS_EDATA;
}

int ms_verify_v2_trailer(const uint8_t *header,
                         const struct mt_trust_table *trust,
                         const uint8_t *trailer_buf,
                         size_t trailer_len,
                         ms_sig_verify_fn verify_fn,
                         struct mt_trailer *out_parsed)
{
    if (!header) return MT_E_ARG;
    return mt_verify_envelope(trust, header + 96, trailer_buf, trailer_len, verify_fn, out_parsed);
}
