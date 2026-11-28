#include <modelvol.h>
#include <modelstore.h>
#include <model_signed_volume.h>
#include <blk.h>
#include <console.h>

/* Static, bounded storage: the header sector, the largest legal table (MS_MAX_OBJECTS entries) and the parsed volume. Object data is streamed in 4 KiB batches by
 * ms_verify_object(). The amount of scratch stays fixed for all volume sizes. */
static uint8_t HDR[MS_SECTOR];
static uint8_t TBL[((MS_MAX_OBJECTS * MS_ENTRY + MS_SECTOR - 1) / MS_SECTOR) * MS_SECTOR];
static struct ms_volume VOL;
static struct ms_signed_scratch SIGNED_SCRATCH;
static struct mt_trust_table TRUST;
static int g_require_signed = 0;
static int g_state = MODELVOL_ABSENT, g_error = MS_OK;
static uint32_t g_objects;

static int rd(void *ctx, uint64_t sector, uint32_t count, uint8_t *buf) { (void)ctx; return blkm_read(sector, buf, count) == BLK_OK ? 0 : -1; }

static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t g64(const uint8_t *p) { return (uint64_t)g32(p) | ((uint64_t)g32(p + 4) << 32); }

const char *modelvol_strerror(int rc)
{
    switch (rc) {
    case MS_OK: return "ok";
    case MS_EARG: return "bad argument";
    case MS_EMAGIC: return "bad magic";
    case MS_EVERSION: return "unsupported version";
    case MS_EGEOM: return "bad geometry";
    case MS_ECOUNT: return "too many objects";
    case MS_EHDRDIG: return "header digest mismatch";
    case MS_ETBLDIG: return "table digest mismatch";
    case MS_ENAME: return "bad object name";
    case MS_EDUP: return "duplicate object name";
    case MS_EEXTENT: return "bad object extent";
    case MS_EFORMAT: return "nonzero reserved field";
    case MS_EDATA: return "object digest mismatch";
    case MS_EIO: return "read error";
    case MS_EAUTH: return "signature authentication failed";
    case MODELVOL_ETRUNC: return "volume shorter than header claims";
    default: return "unknown error";
    }
}

static int verify(void)
{
    if (blkm_read(0, HDR, 1) != BLK_OK) return MS_EIO;
    uint32_t ver = g32(HDR + 8);
    if (ver == 2u) {
        return ms_verify_signed_volume(rd, (void *)0, blkm_sectors(), &TRUST, &SIGNED_SCRATCH, &VOL);
    }
    if (g_require_signed) return MS_EAUTH;
    /* Header first, with no table: ms_parse() checks every header field and the header digest before it looks at the table, and then asks for the table
     * with MS_EARG (or returns MS_OK for an empty volume). Any other result is a header problem. */
    int rc = ms_parse(HDR, (const uint8_t *)0, 0, &VOL);
    if (rc != MS_OK && rc != MS_EARG) return rc;
    /* The header is now known good, so these fields are in range: table_sectors <= sizeof TBL / MS_SECTOR, total_sectors <= MS_MAX_TOTAL_SECTORS. */
    uint64_t total = g64(HDR + 16); uint32_t tn = g32(HDR + 32);
    if (total > blkm_sectors()) return MODELVOL_ETRUNC;
    if (tn * MS_SECTOR > sizeof TBL) return MS_EGEOM;                                 /* unreachable after a good header; kept as the buffer's own bound */
    if (tn && blkm_read(1, TBL, tn) != BLK_OK) return MS_EIO;
    rc = ms_parse(HDR, TBL, (size_t)tn * MS_SECTOR, &VOL);
    if (rc != MS_OK) return rc;
    for (uint32_t i = 0; i < VOL.nobjects; i++) { rc = ms_verify_object(&VOL, i, rd, (void *)0); if (rc != MS_OK) return rc; }
    return MS_OK;
}

void modelvol_check(void)
{
    g_state = MODELVOL_ABSENT; g_error = MS_OK; g_objects = 0;
    if (!blkm_present()) return;
    int rc = verify();
    if (rc == MS_OK) {
        g_state = MODELVOL_VALID; g_objects = VOL.nobjects;
        console_puts("[model] volume valid: "); console_dec(g_objects); console_puts(" objects\n");
    } else {
        g_state = MODELVOL_INVALID; g_error = rc;
        console_puts("[model] volume INVALID: "); console_puts(modelvol_strerror(rc)); console_puts("\n");
    }
}

void modelvol_set_trust(const struct mt_trust_table *trust, int require_signed)
{
    if (trust) TRUST = *trust;
    else {
        uint8_t *p = (uint8_t *)&TRUST;
        for (size_t i = 0; i < sizeof(TRUST); ++i) p[i] = 0;
    }
    g_require_signed = require_signed ? 1 : 0;
}

int modelvol_state(void) { return g_state; }
int modelvol_error(void) { return g_error; }
uint32_t modelvol_objects(void) { return g_objects; }
const struct ms_object *modelvol_object(uint32_t index) { return (g_state == MODELVOL_VALID && index < g_objects) ? &VOL.obj[index] : (const struct ms_object *)0; }
