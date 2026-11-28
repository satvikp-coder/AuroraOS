#ifndef AURORA_MODEL_SIGNED_VOLUME_H
#define AURORA_MODEL_SIGNED_VOLUME_H

#include <modelstore.h>
#include <model_trust.h>

/* Caller-owned scratch. Its contents have no validity contract after failure. */
struct ms_signed_scratch {
    uint8_t header[MS_SECTOR];
    uint8_t table[MS_MAX_OBJECTS * MS_ENTRY];
    uint8_t trailer[MS_SECTOR];
    struct ms_volume candidate;
};

/* Authenticate V2 metadata from an exact volume-sized read-only view.
 * trust must come from independent trusted policy, never the volume.
 * An empty trust table denies every publisher. On success, returns MS_OK
 * and fills out. Per-object payload verification still requires ms_verify_object.
 * out must be separate from scratch and is left untouched on failure. */
#define MS_EAUTH (-14)
int ms_preflight_signed(ms_read_fn rd, void *ctx, uint64_t volume_sectors,
                        const struct mt_trust_table *trust, struct ms_signed_scratch *scratch,
                        struct ms_volume *out);

/* Verify V2 metadata and every payload against the signed table before publishing
 * the volume. The backing image must remain immutable throughout verification
 * and later use. Returns the specific read or digest error on payload failure. */
int ms_verify_signed_volume(ms_read_fn rd, void *ctx, uint64_t volume_sectors,
                            const struct mt_trust_table *trust, struct ms_signed_scratch *scratch,
                            struct ms_volume *out);

#endif
