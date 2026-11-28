#include <model_signed_volume.h>
#include "monocypher-ed25519.h"

/* Shared geometry implementation in modelstore.c. */
int ms_parse_v2_geometry(const uint8_t *, const uint8_t *, size_t, struct ms_volume *);

static int check_signature(const uint8_t sig[64], const uint8_t *message, size_t len,
                           const uint8_t public_key[32])
{
    return crypto_ed25519_check(sig, public_key, message, len);
}

int ms_preflight_signed(ms_read_fn rd, void *ctx, uint64_t volume_sectors,
                        const struct mt_trust_table *trust, struct ms_signed_scratch *s,
                        struct ms_volume *out)
{
    if (!rd || !trust || !s || !out || out == &s->candidate) return MS_EARG;
    if (volume_sectors < 2 || volume_sectors > MS_MAX_TOTAL_SECTORS) return MS_EGEOM;
    if (rd(ctx, 0, 1, s->header)) return MS_EIO;
    if (mt_get_le32(s->header + 8) != 2) return MS_EVERSION;
    uint64_t declared = mt_get_le32(s->header + 16) |
                        ((uint64_t)mt_get_le32(s->header + 20) << 32);
    if (declared != volume_sectors) return MS_EGEOM;
    uint32_t sectors = mt_get_le32(s->header + 32);
    if (sectors > sizeof(s->table) / MS_SECTOR || sectors > volume_sectors - 2)
        return MS_EGEOM;
    if (sectors && rd(ctx, 1, sectors, s->table)) return MS_EIO;
    int rc = ms_parse_v2_geometry(s->header, s->table, sectors * MS_SECTOR, &s->candidate);
    if (rc != MS_OK) return rc;

    if (rd(ctx, volume_sectors - 1, 1, s->trailer)) return MS_EIO;
    for (size_t i = MT_TRAILER_BYTES; i < MS_SECTOR; ++i)
        if (s->trailer[i]) return MS_EFORMAT;
    rc = ms_verify_v2_trailer(s->header, trust, s->trailer, MT_TRAILER_BYTES,
                              check_signature, NULL);
    if (rc != MT_AUTHENTICATED) return MS_EAUTH;
    *out = s->candidate;
    return MS_OK;
}
