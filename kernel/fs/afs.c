/*
 * kernel/fs/afs.c — AuroraFS v1.
 *
 * ON-DISK FORMAT (512-byte sectors, all integers little-endian)
 *   sector 0            superblock: "AURORAFS", version, sector size, total sectors, inode count, inode start, data start, CRC-32
 *   sectors 1..16       inode table: 32 inodes x 256 bytes (two per sector)
 *   sectors 17..        data blocks (one sector each)
 *   There is NO on-disk allocation bitmap: free space is recomputed from the inodes at mount, so it can never disagree with them.
 *   inode: state, generation, owner, perm, size, nblocks, data CRC-32, name[32], blocks[48], inode CRC-32.
 *   State FREE (0) means never used (the whole inode is zero); FREED keeps the generation so an old file id can never name a new file.
 *
 * WRITE ORDERING (copy on write, whole file): new data blocks are written to FREE sectors, then a flush, then the single inode
 * sector (pointing at the new blocks, with the new size and data CRC), then a flush. Until the inode sector lands the old file is
 * untouched. A power cut therefore leaves either the old or the new file, provided the device honours flush and a 512-byte write
 * is atomic. This is tested by dropping every write after the k-th for all k (tests/qemu/storage-tests.sh); it is NOT tested
 * against a real power cut, and no claim is made beyond that.
 *
 * MOUNT validates everything it needs and writes nothing: geometry, version, CRCs, names, sizes, block ranges, no block used
 * twice, no duplicate names. Anything wrong => an error and an unmounted filesystem; there is no automatic repair or reformat.
 * File contents are verified against the inode's data CRC on every read.
 */
#include <blk.h>
#include <console.h>
#include <fs.h>

#define SB_SECTOR 0u
#define INODE_START 1u
#define INODE_SIZE 256u
#define INODE_SECTORS (FS_MAX_FILES * INODE_SIZE / BLK_SECTOR)      /* 16 */
#define DATA_START (INODE_START + INODE_SECTORS)                    /* 17 */
#define FS_VERSION 1u
#define ST_FREE 0u
#define ST_FREED 0xF4EEF4EEu
#define ST_USED 0xA11CE5EDu
#define MAX_SECTORS 0x10000000u                                    /* bitmap bound: 128 GiB; the RAM map below covers MAP_BITS */
#define MAP_BITS 65536u                                            /* the RAM allocation map covers at most 32 MiB of data blocks */

struct inode {
    uint32_t state, gen, owner, perm, size, nblocks, data_crc;
    char name[32];
    uint32_t blocks[FS_MAX_BLOCKS];
    uint32_t crc;
};
_Static_assert(sizeof(struct inode) == INODE_SIZE, "inode must be 256 bytes");

static struct {
    int mounted;
    uint32_t total;                                   /* sectors in use by the filesystem */
    union { struct inode ino[FS_MAX_FILES]; uint8_t raw[INODE_SECTORS * BLK_SECTOR]; } t;
    uint8_t map[MAP_BITS / 8];                        /* 1 = data block used (bit index = sector - DATA_START) */
    uint32_t used_blocks, files;
} F;

static uint8_t scratch[FS_MAX_FILE + BLK_SECTOR];

/* ---- helpers ---- */
static uint32_t crc32(const void *p, uint32_t n)
{
    const uint8_t *b = (const uint8_t *)p; uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) { c ^= b[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}
static void zero(void *p, uint32_t n) { uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0; }
static void copy(void *d, const void *s, uint32_t n) { uint8_t *a = (uint8_t *)d; const uint8_t *b = (const uint8_t *)s; while (n--) *a++ = *b++; }
static int is_zero(const void *p, uint32_t n) { const uint8_t *b = (const uint8_t *)p; while (n--) if (*b++) return 0; return 1; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t inode_crc(const struct inode *n) { return crc32(n, INODE_SIZE - 4); }

const char *fs_strerror(int rc)
{
    switch (rc) {
    case FS_OK: return "ok"; case FS_EARG: return "invalid argument"; case FS_ENOENT: return "no such file"; case FS_EEXIST: return "file exists";
    case FS_ENOSPC: return "no space"; case FS_ENFILE: return "no free file slot"; case FS_EFBIG: return "file too large"; case FS_EACCES: return "permission denied";
    case FS_EBADID: return "invalid file id"; case FS_ECORRUPT: return "corrupt"; case FS_EVERSION: return "unsupported version"; case FS_ENOFS: return "no filesystem";
    case FS_EIO: return "I/O error"; case FS_ENAME: return "invalid name"; case FS_EMOUNT: return "not mounted"; case FS_ENODEV: return "no device"; case FS_ERO: return "read-only"; case FS_EUNCERTAIN: return "update outcome uncertain: remount required"; case FS_EQUOTA: return "quota exceeded";
    default: return "unknown error";
    }
}

int fs_validate_name(const char *name)
{
    if (!name) return FS_ENAME;
    uint32_t n = 0;
    for (; name[n]; n++) {
        char c = name[n];
        if (n >= FS_NAME_MAX) return FS_ENAME;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return FS_ENAME;
    }
    return n ? FS_OK : FS_ENAME;
}

static int map_get(uint32_t sector) { uint32_t b = sector - DATA_START; return (F.map[b >> 3] >> (b & 7)) & 1; }
static void map_set(uint32_t sector, int v) { uint32_t b = sector - DATA_START; if (v) F.map[b >> 3] |= (uint8_t)(1u << (b & 7)); else F.map[b >> 3] &= (uint8_t)~(1u << (b & 7)); }
static uint32_t data_blocks(void) { return F.total - DATA_START; }
static int map_fits(uint32_t total) { return total > DATA_START && total - DATA_START <= MAP_BITS; }

static int blkrc(int rc) { return rc == BLK_OK ? FS_OK : rc == BLK_ENODEV ? FS_ENODEV : rc == BLK_ERO ? FS_ERO : FS_EIO; }

/* ---- superblock ---- */
static void sb_encode(uint8_t *s, uint32_t total, uint32_t version)
{
    zero(s, BLK_SECTOR);
    copy(s, "AURORAFS", 8); put32(s + 8, version); put32(s + 12, BLK_SECTOR); put32(s + 16, total); put32(s + 20, 0);
    put32(s + 24, FS_MAX_FILES); put32(s + 28, INODE_START); put32(s + 32, DATA_START); put32(s + 36, FS_MAX_BLOCKS);
    put32(s + 60, crc32(s, 60));
}

int fs_format(uint32_t total_sectors)
{
    if (!blk_present()) return FS_ENODEV;
    if (blk_readonly()) return FS_ERO;
    uint64_t cap = blk_sectors();
    uint32_t total = total_sectors ? total_sectors : (cap > MAX_SECTORS ? MAX_SECTORS : (uint32_t)cap);
    if (total > cap || total < DATA_START + 8) return FS_EARG;
    if (!map_fits(total)) total = DATA_START + MAP_BITS;                 /* use at most what the RAM map can describe */
    if (total > cap) return FS_EARG;
    fs_unmount();
    static uint8_t zeros[BLK_SECTOR * INODE_SECTORS];
    zero(zeros, sizeof zeros);
    int rc = blkrc(blk_write(INODE_START, zeros, INODE_SECTORS)); if (rc) return rc;
    rc = blkrc(blk_flush()); if (rc) return rc;
    uint8_t sb[BLK_SECTOR]; sb_encode(sb, total, FS_VERSION);
    rc = blkrc(blk_write(SB_SECTOR, sb, 1)); if (rc) return rc;           /* the superblock lands LAST, so a disk that never held a filesystem is not mountable half-formatted. NOT crash-safe on a disk that already held one: its old superblock plus the zeroed inode table would mount as an empty filesystem. Formatting is explicit and test-only. */
    return blkrc(blk_flush());
}

/* ---- mount ---- */
void fs_unmount(void) { F.mounted = 0; }
int fs_mounted(void) { return F.mounted; }

int fs_mount(void)
{
    F.mounted = 0;
    if (!blk_present()) return FS_ENODEV;
    uint8_t sb[BLK_SECTOR];
    int rc = blkrc(blk_read(SB_SECTOR, sb, 1)); if (rc) return rc;
    if (is_zero(sb, BLK_SECTOR)) return FS_ENOFS;                         /* an empty disk */
    if (!(sb[0] == 'A' && sb[1] == 'U' && sb[2] == 'R' && sb[3] == 'O' && sb[4] == 'R' && sb[5] == 'A' && sb[6] == 'F' && sb[7] == 'S')) return FS_ENOFS;   /* some other content: never touched */
    if (get32(sb + 8) != FS_VERSION) return FS_EVERSION;
    if (get32(sb + 60) != crc32(sb, 60)) return FS_ECORRUPT;
    uint32_t total = get32(sb + 16);
    if (get32(sb + 12) != BLK_SECTOR || get32(sb + 20) != 0 || get32(sb + 24) != FS_MAX_FILES || get32(sb + 28) != INODE_START ||
        get32(sb + 32) != DATA_START || get32(sb + 36) != FS_MAX_BLOCKS) return FS_ECORRUPT;
    if (!map_fits(total) || total > blk_sectors()) return FS_ECORRUPT;
    rc = blkrc(blk_read(INODE_START, F.t.raw, INODE_SECTORS)); if (rc) return rc;

    F.total = total; zero(F.map, sizeof F.map); F.used_blocks = 0; F.files = 0;
    for (uint32_t i = 0; i < FS_MAX_FILES; i++) {
        const struct inode *n = &F.t.ino[i];
        if (n->state == ST_FREE) { if (!is_zero(n, INODE_SIZE)) return FS_ECORRUPT; continue; }
        if (n->crc != inode_crc(n)) return FS_ECORRUPT;
        if (n->state == ST_FREED) continue;
        if (n->state != ST_USED) return FS_ECORRUPT;
        if (n->perm & ~FS_PERM_MASK) return FS_ECORRUPT;
        if (n->size > FS_MAX_FILE || n->nblocks != (n->size + BLK_SECTOR - 1) / BLK_SECTOR) return FS_ECORRUPT;
        char nm[33]; copy(nm, n->name, 32); nm[32] = 0;
        if (n->name[31] != 0 || fs_validate_name(nm) != FS_OK) return FS_ECORRUPT;
        for (uint32_t j = 0; j < i; j++) if (F.t.ino[j].state == ST_USED && F.t.ino[j].owner == n->owner && streq(F.t.ino[j].name, n->name)) return FS_ECORRUPT;   /* names are unique PER OWNER */
        for (uint32_t b = 0; b < FS_MAX_BLOCKS; b++) {
            if (b >= n->nblocks) { if (n->blocks[b]) return FS_ECORRUPT; continue; }
            uint32_t s = n->blocks[b];
            if (s < DATA_START || s >= total) return FS_ECORRUPT;
            if (map_get(s)) return FS_ECORRUPT;                          /* a sector claimed twice */
            map_set(s, 1); F.used_blocks++;
        }
        F.files++;
    }
    F.mounted = 1;
    return FS_OK;
}

uint32_t fs_free_blocks(void) { return F.mounted ? data_blocks() - F.used_blocks : 0; }
uint32_t fs_file_count(void) { return F.mounted ? F.files : 0; }

/* ---- id and permission helpers ---- */
static int resolve(fs_id_t id, struct inode **out)
{
    if (!F.mounted) return FS_EMOUNT;
    if (id >> 40) return FS_EBADID;
    uint32_t idx = (uint32_t)(id & 0xFF), gen = (uint32_t)(id >> 8);
    if (idx >= FS_MAX_FILES) return FS_EBADID;
    struct inode *n = &F.t.ino[idx];
    if (n->state != ST_USED || n->gen != gen) return FS_EBADID;          /* deleted, never existed, or a different incarnation */
    *out = n;
    return FS_OK;
}
static fs_id_t make_id(const struct inode *n, uint32_t idx) { return ((fs_id_t)n->gen << 8) | idx; }
static int can(uint32_t principal, const struct inode *n, int write)
{
    if (principal == 0) return 1;                                         /* the kernel */
    if (principal == n->owner) return !!(n->perm & (write ? FS_PERM_OWNER_W : FS_PERM_OWNER_R));
    return !!(n->perm & (write ? FS_PERM_OTHER_W : FS_PERM_OTHER_R));
}
static int can_see(uint32_t principal, const struct inode *n) { return can(principal, n, 0) || can(principal, n, 1); }

/*
 * EXISTENCE POLICY (issue #6 hardening). A file id is not a secret, so what a caller learns from an id must not depend on whether a file
 * the caller may not discover happens to exist behind it. For every operation that takes an id:
 *   - the kernel (principal 0) always gets the true result;
 *   - a caller that is the file's owner, or that has read or write permission on it, can DISCOVER the file; for such callers a refused operation
 *     is FS_EACCES (the file is legitimately visible, the operation is not allowed);
 *   - any other caller gets FS_EBADID, byte for byte the same result as for an id that never existed, a stale generation or a deleted file.
 * resolve_for() applies this FIRST, and every argument check that does not depend on the file is made BEFORE it, so no argument error can
 * be used as a probe either. This hides existence in the returned error codes only: no claim is made about timing or other side channels.
 * Aggregate-resource errors (FS_ENFILE, FS_ENOSPC) still reflect global usage by design; they reveal no particular file.
 */
static int visible(uint32_t principal, const struct inode *n) { return principal == 0 || principal == n->owner || can_see(principal, n); }
static int resolve_for(uint32_t principal, fs_id_t id, struct inode **out)
{
    struct inode *n; int rc = resolve(id, &n); if (rc) return rc;
    if (!visible(principal, n)) return FS_EBADID;
    *out = n;
    return FS_OK;
}

/* ---- per-principal quotas ----
 * Usage is DERIVED from the inode table (the only authoritative state), never stored: it cannot disagree with the files, survives remount
 * and reboot by construction, and a failed or uncertain update (which leaves the cache unchanged or unmounts) cannot leave a stale counter.
 * Charged to the file's OWNER, whoever writes it (a world-writable file spends its owner's quota: the owner chose to expose it). Logical blocks
 * are what a file occupies when committed; the temporary second copy of copy-on-write is not charged (it is released at commit). */
static void usage_of(uint32_t owner, uint32_t *files, uint32_t *blocks)
{
    uint32_t f = 0, b = 0;
    for (uint32_t i = 0; i < FS_MAX_FILES; i++)
        if (F.t.ino[i].state == ST_USED && F.t.ino[i].owner == owner) { f++; b += F.t.ino[i].nblocks; }   /* <= 32 * 48: cannot overflow */
    *files = f; *blocks = b;
}
int fs_usage(uint32_t principal, uint32_t *files, uint32_t *blocks)
{
    if (!F.mounted) return FS_EMOUNT;
    if (!files || !blocks) return FS_EARG;
    usage_of(principal, files, blocks);
    return FS_OK;
}

/*
 * Publish one inode: write its sector, flush, then (only on success) update the cache.
 *
 * Outcome classes (this is the heart of the recovery design):
 *   - FS_OK                the update is durable and the cache matches the disk.
 *   - a definite error     the block layer REJECTED the write before it reached the device (blk_error_definite): nothing changed on
 *                          the disk, the cache is still exact, the filesystem stays mounted and the caller may retry.
 *   - FS_EUNCERTAIN        the write was submitted (or the flush after a submitted write failed) and its outcome is UNKNOWN: the sector may or
 *                          may not hold the new inode. The cache and the allocation map may now disagree with the disk, and trusting them could
 *                          let a later write reuse sectors the new inode references. So the filesystem UNMOUNTS ITSELF: every operation fails
 *                          with FS_EMOUNT until fs_mount() rebuilds everything from the disk (and validates it). The operation may or may not
 *                          have committed; the next mount shows which. Nothing is repaired or formatted automatically.
 * Failures BEFORE this point (data writes, data flush) never touch metadata, so they are ordinary definite errors.
 */
static int commit_inode(uint32_t idx, struct inode *cand)
{
    cand->crc = inode_crc(cand);
    uint32_t sec = idx / 2;
    uint8_t buf[BLK_SECTOR];
    copy(buf, &F.t.raw[sec * BLK_SECTOR], BLK_SECTOR);
    copy(buf + (idx & 1) * INODE_SIZE, cand, INODE_SIZE);
    int rc = blk_write(INODE_START + sec, buf, 1);
    if (rc) {
        if (blk_error_definite(rc)) return blkrc(rc);           /* never reached the device: cache still exact */
        F.mounted = 0; return FS_EUNCERTAIN;
    }
    rc = blk_flush();
    if (rc) { F.mounted = 0; return FS_EUNCERTAIN; }             /* the sector was submitted; whether it is durable is unknown */
    copy(&F.t.ino[idx], cand, INODE_SIZE);
    return FS_OK;
}

/* ---- operations ---- */
int fs_create(uint32_t principal, const char *name, uint32_t perm, fs_id_t *id)
{
    if (!F.mounted) return FS_EMOUNT;
    if (!id || principal == 0 || (perm & ~FS_PERM_MASK)) return FS_EARG;      /* the kernel principal owns nothing */
    int rc = fs_validate_name(name); if (rc) return rc;
    /* Names are unique per OWNER, not globally: another principal's file with the same name (even a hidden one) must neither
     * block this create nor reveal itself through FS_EEXIST. A slot whose generation is used up can never be reused (ids must
     * never repeat), so it is skipped: one worn-out slot must not stop all creation while other slots are free. */
    int free_idx = -1;
    for (uint32_t i = 0; i < FS_MAX_FILES; i++) {
        if (F.t.ino[i].state == ST_USED) { if (F.t.ino[i].owner == principal && streq(F.t.ino[i].name, name)) return FS_EEXIST; }
        else if (free_idx < 0 && F.t.ino[i].gen < 0x00FFFFFFu) free_idx = (int)i;
    }
    uint32_t qf, qb; usage_of(principal, &qf, &qb); (void)qb;
    if (qf >= FS_QUOTA_FILES) return FS_EQUOTA;                                 /* the principal's own limit: reveals nothing about anyone else */
    if (free_idx < 0) return FS_ENFILE;
    struct inode n; zero(&n, sizeof n);
    n.state = ST_USED; n.gen = F.t.ino[free_idx].gen + 1; n.owner = principal; n.perm = perm; n.size = 0; n.nblocks = 0; n.data_crc = crc32(0, 0);
    for (uint32_t k = 0; name[k]; k++) n.name[k] = name[k];
    if (n.gen == 0 || n.gen >= 0x01000000u) return FS_ENFILE;                  /* generation space exhausted for this slot: never wrap */
    rc = commit_inode((uint32_t)free_idx, &n); if (rc) return rc;
    F.files++;
    *id = make_id(&F.t.ino[free_idx], (uint32_t)free_idx);
    return FS_OK;
}

int fs_lookup(uint32_t principal, const char *name, fs_id_t *id)
{
    if (!F.mounted) return FS_EMOUNT;
    if (!id) return FS_EARG;
    int rc = fs_validate_name(name); if (rc) return rc;
    /* Several principals may own a file of this name. The caller's OWN file wins; otherwise the first file it is allowed to see.
     * A file it may not see is skipped, so a hidden file is indistinguishable from an absent one. */
    int best = -1;
    for (uint32_t i = 0; i < FS_MAX_FILES; i++) {
        if (F.t.ino[i].state != ST_USED || !streq(F.t.ino[i].name, name)) continue;
        if (principal != 0 && F.t.ino[i].owner == principal) { best = (int)i; break; }
        if (best < 0 && can_see(principal, &F.t.ino[i])) best = (int)i;
    }
    if (best < 0) return FS_ENOENT;
    *id = make_id(&F.t.ino[best], (uint32_t)best);
    return FS_OK;
}

/* Kernel-only: the file called `name` that is owned by EXACTLY `owner`; no fallback to anyone else's file. The program launcher uses this so
 * an application can never shadow an installed executable by creating a file of the same name under its own identity. */
int fs_lookup_owner(uint32_t owner, const char *name, fs_id_t *id)
{
    if (!F.mounted) return FS_EMOUNT;
    if (!id || owner == 0) return FS_EARG;
    int rc = fs_validate_name(name); if (rc) return rc;
    for (uint32_t i = 0; i < FS_MAX_FILES; i++)
        if (F.t.ino[i].state == ST_USED && F.t.ino[i].owner == owner && streq(F.t.ino[i].name, name)) { *id = make_id(&F.t.ino[i], i); return FS_OK; }
    return FS_ENOENT;
}

/* load a file's whole content into scratch and verify its CRC */
static int load(const struct inode *n)
{
    for (uint32_t b = 0; b < n->nblocks; b++) {
        int rc = blkrc(blk_read(n->blocks[b], scratch + b * BLK_SECTOR, 1)); if (rc) return rc;
    }
    if (crc32(scratch, n->size) != n->data_crc) return FS_ECORRUPT;           /* silent corruption of the data is detected, not returned */
    return FS_OK;
}

int fs_read(uint32_t principal, fs_id_t id, uint32_t off, void *buf, uint32_t len)
{
    if (!F.mounted) return FS_EMOUNT;
    if (!buf && len) return FS_EARG;                                          /* file-independent checks first: see EXISTENCE POLICY */
    struct inode *n; int rc = resolve_for(principal, id, &n); if (rc) return rc;
    if (!can(principal, n, 0)) return FS_EACCES;
    if (off > n->size) return FS_EARG;
    uint32_t avail = n->size - off, take = len < avail ? len : avail;
    if (take == 0) return 0;
    rc = load(n); if (rc) return rc;
    copy(buf, scratch + off, take);
    return (int)take;
}

int fs_write(uint32_t principal, fs_id_t id, uint32_t off, const void *buf, uint32_t len)
{
    if (!F.mounted) return FS_EMOUNT;
    if (!buf && len) return FS_EARG;
    struct inode *n; int rc = resolve_for(principal, id, &n); if (rc) return rc;
    if (!can(principal, n, 1)) return FS_EACCES;
    if (blk_readonly()) return FS_ERO;
    if (off > n->size) return FS_EARG;                                        /* no holes */
    if (len == 0) return 0;
    if (len > FS_MAX_FILE || off + len > FS_MAX_FILE) return FS_EFBIG;        /* off <= size <= MAX and len <= MAX: no overflow */
    uint32_t newsize = off + len > n->size ? off + len : n->size;
    uint32_t nb = (newsize + BLK_SECTOR - 1) / BLK_SECTOR;
    if (nb > n->nblocks) {                                                    /* only GROWTH is charged: an overwrite in place never fails on quota */
        uint32_t qf, qb; usage_of(n->owner, &qf, &qb); (void)qf;
        if (qb - n->nblocks > FS_QUOTA_BLOCKS || nb > FS_QUOTA_BLOCKS - (qb - n->nblocks)) return FS_EQUOTA;   /* checked before any disk write: a refused write costs nothing */
    }

    rc = load(n); if (rc) return rc;                                          /* also proves the old content is intact before replacing it */
    copy(scratch + off, buf, len);
    for (uint32_t i = newsize; i < nb * BLK_SECTOR; i++) scratch[i] = 0;

    /* choose nb FREE sectors (the old blocks stay allocated until the inode switches over) */
    uint32_t pick[FS_MAX_BLOCKS], got = 0;
    for (uint32_t s = DATA_START; s < F.total && got < nb; s++) if (!map_get(s)) pick[got++] = s;
    if (got < nb) return FS_ENOSPC;                                           /* nothing has been changed */

    for (uint32_t b = 0; b < nb; b++) { rc = blkrc(blk_write(pick[b], scratch + b * BLK_SECTOR, 1)); if (rc) return rc; }
    rc = blkrc(blk_flush()); if (rc) return rc;                               /* data is durable BEFORE the inode points at it */

    struct inode cand; copy(&cand, n, INODE_SIZE);
    cand.size = newsize; cand.nblocks = nb; cand.data_crc = crc32(scratch, newsize);
    for (uint32_t b = 0; b < FS_MAX_BLOCKS; b++) cand.blocks[b] = b < nb ? pick[b] : 0;
    uint32_t old_nb = n->nblocks, old_blocks[FS_MAX_BLOCKS]; copy(old_blocks, n->blocks, sizeof old_blocks);
    rc = commit_inode((uint32_t)(n - F.t.ino), &cand); if (rc) return rc;
    for (uint32_t b = 0; b < old_nb; b++) { map_set(old_blocks[b], 0); F.used_blocks--; }
    for (uint32_t b = 0; b < nb; b++) { map_set(pick[b], 1); F.used_blocks++; }
    return (int)len;
}

int fs_delete(uint32_t principal, fs_id_t id)
{
    struct inode *n; int rc = resolve_for(principal, id, &n); if (rc) return rc;
    if (principal != 0 && principal != n->owner) return FS_EACCES;            /* visible but not the owner */
    if (blk_readonly()) return FS_ERO;
    struct inode cand; zero(&cand, sizeof cand);
    cand.state = ST_FREED; cand.gen = n->gen;                                  /* the generation survives so the old id stays dead */
    uint32_t old_nb = n->nblocks, old_blocks[FS_MAX_BLOCKS]; copy(old_blocks, n->blocks, sizeof old_blocks);
    rc = commit_inode((uint32_t)(n - F.t.ino), &cand); if (rc) return rc;
    for (uint32_t b = 0; b < old_nb; b++) { map_set(old_blocks[b], 0); F.used_blocks--; }
    F.files--;
    return FS_OK;
}

int fs_set_perm(uint32_t principal, fs_id_t id, uint32_t perm)
{
    if (!F.mounted) return FS_EMOUNT;
    if (perm & ~FS_PERM_MASK) return FS_EARG;
    struct inode *n; int rc = resolve_for(principal, id, &n); if (rc) return rc;
    if (principal != 0 && principal != n->owner) return FS_EACCES;
    if (blk_readonly()) return FS_ERO;
    struct inode cand; copy(&cand, n, INODE_SIZE); cand.perm = perm;
    return commit_inode((uint32_t)(n - F.t.ino), &cand);
}

static void fill_stat(struct fs_stat *st, const struct inode *n, uint32_t idx)
{
    st->id = make_id(n, idx); st->size = n->size; st->owner = n->owner; st->perm = n->perm;
    copy(st->name, n->name, FS_NAME_MAX); st->name[FS_NAME_MAX] = 0;
}

int fs_stat(uint32_t principal, fs_id_t id, struct fs_stat *st)
{
    if (!F.mounted) return FS_EMOUNT;
    if (!st) return FS_EARG;
    struct inode *n; int rc = resolve_for(principal, id, &n); if (rc) return rc;
    if (principal != 0 && !can_see(principal, n)) return FS_EACCES;            /* an owner who removed all of its own permission bits */
    fill_stat(st, n, (uint32_t)(n - F.t.ino));
    return FS_OK;
}

int fs_list(uint32_t principal, struct fs_stat *out, int max)
{
    if (!F.mounted) return FS_EMOUNT;
    if (!out || max < 0) return FS_EARG;
    int c = 0;
    for (uint32_t i = 0; i < FS_MAX_FILES && c < max; i++)
        if (F.t.ino[i].state == ST_USED && can(principal, &F.t.ino[i], 0)) fill_stat(&out[c++], &F.t.ino[i], i);
    return c;
}

uint32_t fs_crc32(const void *p, uint32_t n) { return crc32(p, n); }
