#ifndef AURORA_FS_H
#define AURORA_FS_H
#include <stdint.h>

/*
 * AuroraFS v1 — a deliberately small persistent filesystem on top of the kernel block interface (blk.h).
 * Flat namespace, 32 files (8 and 96 blocks per principal), names of 1..31 characters, files up to 24 KiB. See docs/kernel/storage.md for the on-disk
 * format, the write ordering, the authorization model and the limits.
 *
 * Principals: 0 is the KERNEL (may do anything; never assigned to a process). Any other value identifies an owner; milestone C
 * lets the kernel assign one to an application (see docs/kernel/userspace.md), never the application itself. Authorization is by
 * permission bits on every operation, never by secrecy of a file id. File names are unique PER OWNER: two principals may each own
 * a file of the same name, and fs_lookup returns the caller's own first.
 */
#define FS_NAME_MAX    31
#define FS_MAX_FILES   32
#define FS_MAX_BLOCKS  48
#define FS_MAX_FILE    (FS_MAX_BLOCKS * 512)

#define FS_PERM_OWNER_R 0x1u
#define FS_PERM_OWNER_W 0x2u
#define FS_PERM_OTHER_R 0x4u
#define FS_PERM_OTHER_W 0x8u
#define FS_PERM_MASK    0xFu
#define FS_PERM_DEFAULT (FS_PERM_OWNER_R | FS_PERM_OWNER_W)

/* Per-principal quotas (charged to the file's OWNER). 8 files = a quarter of the table, so at least four principals can always coexist; 96 blocks
 * (48 KiB) = two maximum-size files. Usage is derived from the inodes, never stored. See docs/kernel/storage.md. */
#define FS_QUOTA_FILES  8u
#define FS_QUOTA_BLOCKS 96u

enum {
    FS_OK = 0, FS_EARG = -1, FS_ENOENT = -2, FS_EEXIST = -3, FS_ENOSPC = -4, FS_ENFILE = -5, FS_EFBIG = -6, FS_EACCES = -7,
    FS_EBADID = -8, FS_ECORRUPT = -9, FS_EVERSION = -10, FS_ENOFS = -11, FS_EIO = -12, FS_ENAME = -13, FS_EMOUNT = -14,
    FS_ENODEV = -15, FS_ERO = -16,
    FS_EUNCERTAIN = -17,  /* a metadata update failed in a way that leaves its durability UNKNOWN: the filesystem has been unmounted; remount to learn the outcome */
    FS_EQUOTA = -18       /* the owning principal's file-count or block quota would be exceeded; nothing was changed */
};

typedef uint64_t fs_id_t;      /* (generation << 8) | inode index: a stale or forged id never names a different, newer file */

struct fs_stat { fs_id_t id; uint32_t size, owner, perm; char name[FS_NAME_MAX + 1]; };

int  fs_format(uint32_t total_sectors);          /* WIPES the first total_sectors of the device (0 = whole device). Explicit, never automatic. */
int  fs_mount(void);                             /* read-only validation + load; never formats, never repairs, never writes */
int  fs_mounted(void);
void fs_unmount(void);

int  fs_create(uint32_t principal, const char *name, uint32_t perm, fs_id_t *id);
int  fs_lookup(uint32_t principal, const char *name, fs_id_t *id);   /* the caller's own file of that name, else the first it may see; FS_ENOENT also hides files it may not touch. Names are unique per owner. */
int  fs_read(uint32_t principal, fs_id_t id, uint32_t off, void *buf, uint32_t len);        /* bytes read (short at EOF) or FS_E* */
int  fs_write(uint32_t principal, fs_id_t id, uint32_t off, const void *buf, uint32_t len); /* len, or FS_E*; off <= size (no holes) */
int  fs_delete(uint32_t principal, fs_id_t id);
int  fs_set_perm(uint32_t principal, fs_id_t id, uint32_t perm);                             /* owner or kernel only */
int  fs_stat(uint32_t principal, fs_id_t id, struct fs_stat *st);
int  fs_usage(uint32_t principal, uint32_t *files, uint32_t *blocks);                       /* kernel diagnostic (not a system call): files and logical blocks owned by principal */
int  fs_list(uint32_t principal, struct fs_stat *out, int max);                             /* files the principal may read; count */

uint32_t fs_free_blocks(void);
uint32_t fs_file_count(void);
const char *fs_strerror(int rc);
int  fs_validate_name(const char *name);
uint32_t fs_crc32(const void *p, uint32_t n);                       /* CRC-32 (IEEE) used for every checksum; exposed so tests can forge valid-looking bad metadata */

void fsproc_run(void);        /* EL0 file-access tests (AURORA_FS_TEST kernels only; no-op otherwise) */
int  fsproc_summary(void);
void fstest_run(void);
int  fstest_summary(void);
#endif
