#ifndef AURORA_MODELVOL_H
#define AURORA_MODELVOL_H
#include <stdint.h>

/*
 * Boot-time check of the optional read-only model volume (blkm_*, kernel/include/blk.h). When a model volume is present, modelvol_check() reads its header and
 * table, validates them with ms_parse() and streams every object through SHA-256 with ms_verify_object() (kernel/include/modelstore.h), then prints one line:
 *     [model] volume valid: N objects        or        [model] volume INVALID: <reason>
 * The result is kept in a small static record for later kernel use. Nothing is written to the device. The only EL0 path is SYS_MODEL
 * (kernel/proc/modelsys.c), which uses modelvol_object() and blkm_read() and never lets EL0 name a sector or the device.
 *
 * "valid" means INTEGRITY only: the header, table and object bytes match their unkeyed SHA-256 digests. It says nothing about who produced the volume
 * (anyone who can rewrite it can recompute the digests); authenticity needs a signature check that does not exist yet.
 */
enum { MODELVOL_ABSENT = 0, MODELVOL_VALID = 1, MODELVOL_INVALID = 2 };
#define MODELVOL_ETRUNC (-100)          /* the device has fewer sectors than the header's total_sectors (not an ms_* code) */

void        modelvol_check(void);        /* call once at boot, after blk_init() */
int         modelvol_state(void);        /* MODELVOL_ABSENT / VALID / INVALID */
int         modelvol_error(void);        /* MS_OK when valid or absent, else the ms_* code (or MODELVOL_ETRUNC) that rejected it */
uint32_t    modelvol_objects(void);      /* number of verified objects when VALID, else 0 */
struct ms_object;
const struct ms_object *modelvol_object(uint32_t index);   /* the verified table entry when VALID and index < modelvol_objects(), else 0 */
const char *modelvol_strerror(int rc);
#endif
