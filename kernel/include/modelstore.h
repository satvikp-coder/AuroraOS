#ifndef AURORA_MODELSTORE_H
#define AURORA_MODELSTORE_H
#include <stddef.h>
#include <stdint.h>

/*
 * Model asset store ("AURMODEL" v1): a read-only, content-checked container for LARGE immutable objects (model weights, tokenizer data) that cannot live in AuroraFS
 * (24 KiB files, 32 files). It is a separate region/volume, not a pile of small AuroraFS files. This file is the pure on-disk format and its validation; nothing here
 * touches a device, and nothing in the kernel uses it yet (docs/kernel/model-asset-store.md).
 *
 *   sector 0   header:   0 magic "AURMODEL" | 8 u32 version=1 | 12 u32 sector_size=512 | 16 u64 total_sectors | 24 u32 nobjects | 28 u32 table_sector=1 | 32 u32 table_sectors
 *                         | 36 u32 reserved=0 | 40..63 zero | 64 sha256 over the table bytes (nobjects*128) | 96 sha256 over header bytes 0..95 | 128..511 zero
 *   table      nobjects entries of 128 bytes: 0 name[32] (1..31 chars [A-Za-z0-9._-], NUL padded) | 32 u64 size | 40 u64 first_sector | 48 u64 sectors | 56 u32 kind | 60 u32 flags=0
 *                         | 64 sha256 of the object's bytes | 96..127 zero
 *   data       each object occupies sectors [first_sector, first_sector+sectors), sectors = ceil(size/512); the unused tail of its last sector is zero (and is not hashed).
 *
 * Every byte of the header sector and of the table must have exactly the value the format gives it: a single flipped byte anywhere in them is rejected. All digests are
 * UNKEYED SHA-256. They detect damage and truncation; they do NOT authenticate: whoever can rewrite the volume can recompute them. Authenticity needs a signature over
 * the header digest, checked against a public key the kernel or installer already trusts (not implemented).
 */
#define MS_SECTOR 512u
#define MS_VERIFY_BATCH_SECTORS 8u  /* one virtio-blk bounce page */
#define MS_MAX_OBJECTS 16u
#define MS_ENTRY 128u
#define MS_NAME_MAX 31u
#define MS_MAX_TOTAL_SECTORS (1ULL << 40)               /* 512 TiB: far above any real volume, small enough that no 64-bit sum here can wrap */

enum { MS_OK = 0, MS_EARG = -1, MS_EMAGIC = -2, MS_EVERSION = -3, MS_EGEOM = -4, MS_ECOUNT = -5, MS_EHDRDIG = -6, MS_ETBLDIG = -7, MS_ENAME = -8, MS_EDUP = -9,
       MS_EEXTENT = -10, MS_EFORMAT = -11, MS_EDATA = -12, MS_EIO = -13 };

struct ms_object { char name[MS_NAME_MAX + 1]; uint64_t size, first_sector, sectors; uint32_t kind; uint8_t digest[32]; };
struct ms_volume { uint64_t total_sectors; uint32_t nobjects, table_sector, table_sectors; struct ms_object obj[MS_MAX_OBJECTS]; };

/* Validate header (exactly 512 bytes) and table (table_len bytes available, whole sectors) and fill `out`. Returns MS_OK or the first problem found. Reads nothing else. */
int ms_parse(const uint8_t *header, const uint8_t *table, size_t table_len, struct ms_volume *out);

/* Read `count` sectors starting at `sector` into buf; return 0 on success. */
typedef int (*ms_read_fn)(void *ctx, uint64_t sector, uint32_t count, uint8_t *buf);
/* Stream an object in bounded batches through SHA-256. Requires an ms_parse-validated volume
 * and a reader accepting up to MS_VERIFY_BATCH_SECTORS sectors. Nonzero tail bytes fail
 * verification. Returns MS_OK, MS_EDATA for mismatch or MS_EIO for a read failure. */
int ms_verify_object(const struct ms_volume *v, uint32_t index, ms_read_fn rd, void *ctx);

/* Verify a v2 signature trailer (kernel/include/model_trust.h) bound to the header's SHA-256 digest (bytes 96..127). */
struct mt_trust_table;
struct mt_trailer;
typedef int (*ms_sig_verify_fn)(const uint8_t sig[64], const uint8_t *msg, size_t msg_len, const uint8_t pubkey[32]);
int ms_verify_v2_trailer(const uint8_t *header,
                         const struct mt_trust_table *trust,
                         const uint8_t *trailer_buf,
                         size_t trailer_len,
                         ms_sig_verify_fn verify_fn,
                         struct mt_trailer *out_parsed);
#endif
