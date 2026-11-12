/*
 * kernel/fstest.c — AuroraFS tests on a disposable QEMU virtual disk.
 *
 * DESTRUCTIVE, so compiled only into kernels built with AURORA_FS_TEST=1; a normal kernel only reports what mount finds.
 * The test adapts to the disk it finds:
 *   - empty disk (mount says ENOFS)     => boot 1: format and run the whole functional / authorization / failure suite, then
 *                                           leave persistent files behind;
 *   - disk holding the persistent files => boot 2, 3, ...: verify those files byte for byte, prove authorization still holds after
 *                                           the restart, bump a counter, then run the corruption suite (injected on the disk and
 *                                           RESTORED), so every later boot starts from a good filesystem;
 *   - read-only disk                    => reads verified, writes must be refused.
 * Output: "[fs-test] <name>: PASS|FAIL" and "AURORA-FS-SELFTEST: N passed, M failed".
 */
#include <blk.h>
#include <console.h>
#include <fs.h>

static int nfail;
#ifdef AURORA_FS_TEST
static int npass;

static void check(const char *name, int ok)
{
    console_puts("[fs-test] "); console_puts(name); console_puts(ok ? ": PASS\n" : ": FAIL\n");
    if (ok) npass++; else nfail++;
}

static uint8_t big[FS_MAX_FILE + 1024], big2[FS_MAX_FILE + 1024];
static uint8_t pat(uint32_t seed, uint32_t i) { return (uint8_t)((seed * 131u + i * 7u + (i >> 8) * 13u) ^ 0x3Cu); }
static void fill(uint8_t *p, uint32_t n, uint32_t seed) { for (uint32_t i = 0; i < n; i++) p[i] = pat(seed, i); }
static int same(const uint8_t *a, const uint8_t *b, uint32_t n) { for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static void zero(void *p, uint32_t n) { uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0; }
static void copy(void *d, const void *s, uint32_t n) { uint8_t *a = (uint8_t *)d; const uint8_t *b = (const uint8_t *)s; while (n--) *a++ = *b++; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* write a whole file's content with a verified read back */
static int put_file(uint32_t who, const char *name, uint32_t perm, const uint8_t *data, uint32_t n, fs_id_t *id)
{
    if (fs_create(who, name, perm, id)) return 0;
    if (n && fs_write(who, *id, 0, data, n) != (int)n) return 0;
    zero(big2, sizeof big2);
    return fs_read(who, *id, 0, big2, n) == (int)n && same(big2, data, n);
}
static int file_is(uint32_t who, const char *name, const uint8_t *data, uint32_t n)
{
    fs_id_t id; if (fs_lookup(who, name, &id)) return 0;
    zero(big2, sizeof big2);
    return fs_read(who, id, 0, big2, n + 1) == (int)n && same(big2, data, n);
}

#define PERSIST_HEAD "AURORA-FILE-CONTENT-v1\n"
static void persist_content(uint8_t *out, uint32_t *n)
{
    uint32_t h = 0; const char *s = PERSIST_HEAD; while (s[h]) { out[h] = (uint8_t)s[h]; h++; }
    fill(out + h, 3000, 11); *n = h + 3000;
}

/* ---- helpers for the error-injection tests ---- */
static uint8_t A0[700], B0[900], A1[1500], B1[900];
static fs_id_t g_ia, g_ib;
static int inj_setup(void)
{
    fill(A0, 700, 31); fill(B0, 900, 32); fill(A1, 1500, 33); fill(B1, 900, 34);
    blk_test_inject_clear();
    return fs_format(0) == FS_OK && fs_mount() == FS_OK && put_file(3, "A", 3, A0, 700, &g_ia) && put_file(3, "B", 3, B0, 900, &g_ib);
}
static int state_is(const uint8_t *a, uint32_t na, const uint8_t *b, uint32_t nb)
{ return fs_mount() == FS_OK && file_is(3, "A", a, na) && file_is(3, "B", b, nb); }
static int all_refused(void)
{
    fs_id_t x; struct fs_stat st[2]; uint8_t t[8]; zero(t, sizeof t);
    return !fs_mounted() && fs_read(3, g_ia, 0, t, 8) == FS_EMOUNT && fs_write(3, g_ib, 0, t, 8) == FS_EMOUNT && fs_create(3, "Z", 3, &x) == FS_EMOUNT &&
           fs_delete(3, g_ib) == FS_EMOUNT && fs_set_perm(3, g_ia, 1) == FS_EMOUNT && fs_lookup(3, "A", &x) == FS_EMOUNT &&
           fs_list(3, st, 2) == FS_EMOUNT && fs_stat(3, g_ia, st) == FS_EMOUNT;
}
/* the block list of inode idx exactly as it is ON THE DISK (not the cache) */
static int disk_blocks(uint32_t idx, uint32_t *bl, uint32_t *n)
{
    uint8_t s[BLK_SECTOR]; if (blk_read(1 + idx / 2, s, 1)) return 0;
    const uint8_t *p = s + (idx & 1) * 256; *n = get32(p + 20); if (*n > FS_MAX_BLOCKS) return 0;
    for (uint32_t i = 0; i < *n; i++) bl[i] = get32(p + 60 + 4 * i);
    return 1;
}

static void phase_functional(void)
{
    fs_id_t a, c;
    static struct fs_stat ls[FS_MAX_FILES];

    /* ---- empty disk, formatting ---- */
    check("empty disk: mount reports FS_ENOFS (nothing is formatted behind your back)", fs_mount() == FS_ENOFS && !fs_mounted());
    check("operations on an unmounted filesystem fail with FS_EMOUNT", fs_create(1, "x", 3, &a) == FS_EMOUNT && fs_lookup(1, "x", &a) == FS_EMOUNT && fs_list(1, ls, 4) == FS_EMOUNT);
    check("format refuses an absurd size and succeeds for a valid one", fs_format(10) == FS_EARG && fs_format((uint32_t)blk_sectors() + 1) == FS_EARG && fs_format(0) == FS_OK);
    check("mount after format: empty, free blocks = disk - 17 metadata sectors", fs_mount() == FS_OK && fs_file_count() == 0 && fs_free_blocks() == blk_sectors() - 17);

    /* ---- names ---- */
    check("names: empty, 32 chars, slash, space, control character, NULL are rejected (FS_ENAME)",
          fs_create(1, "", 3, &a) == FS_ENAME && fs_create(1, "abcdefghijklmnopqrstuvwxyz0123456", 3, &a) == FS_ENAME && fs_create(1, "a/b", 3, &a) == FS_ENAME &&
          fs_create(1, "a b", 3, &a) == FS_ENAME && fs_create(1, "a\x01", 3, &a) == FS_ENAME && fs_create(1, 0, 3, &a) == FS_ENAME);
    check("names: a 31-character name is accepted", fs_create(1, "abcdefghijklmnopqrstuvwxyz01234", 3, &a) == FS_OK && fs_delete(1, a) == FS_OK);
    check("create arguments: the kernel principal 0 cannot own files, bad permission bits and NULL id are refused",
          fs_create(0, "k", 3, &a) == FS_EARG && fs_create(1, "k", 0x10, &a) == FS_EARG && fs_create(1, "k", 3, 0) == FS_EARG && fs_file_count() == 0);

    /* ---- basic file I/O ---- */
    fill(big, 3000, 1);
    check("create, write 3000 bytes, read back: exact bytes and size", put_file(1, "a.txt", 3, big, 3000, &a));
    fs_id_t again;
    check("creating the same name twice as the SAME principal is FS_EEXIST; lookup finds the file with the same id", fs_create(1, "a.txt", 3, &again) == FS_EEXIST && fs_lookup(1, "a.txt", &again) == FS_OK && again == a);
    {
        /* Names are per owner. Before this rule a stranger got FS_EEXIST for a name it could not even see (an existence oracle), and one
         * principal could squat on a name forever. */
        fs_id_t other, x, c3;
        int c1 = fs_create(2, "a.txt", 3, &other) == FS_OK && other != a;
        check("another principal creating a name that owner 1 holds as a HIDDEN file succeeds with its own id: no 'file exists' oracle, no name squatting", c1);
        check("each principal's lookup of that name finds its OWN file; the hidden file is never returned to the stranger and the stranger's is never returned to the owner",
              fs_lookup(2, "a.txt", &x) == FS_OK && x == other && fs_lookup(1, "a.txt", &x) == FS_OK && x == a && fs_lookup(3, "a.txt", &c3) == FS_ENOENT);
        check("the stranger cannot read or delete the owner's same-named file through its id, but can delete its own",
              fs_read(2, a, 0, big2, 4) == FS_EACCES && fs_delete(2, a) == FS_EACCES && fs_delete(2, other) == FS_OK && fs_lookup(2, "a.txt", &x) == FS_ENOENT);
        fs_id_t w1, w2;
        int s1 = fs_create(1, "pub.txt", 3 | FS_PERM_OTHER_R, &w1) == FS_OK, s2 = fs_create(2, "pub.txt", 3, &w2) == FS_OK;
        check("a world-readable file and a hidden file of the same name from two owners: owner 2 gets its own, an unrelated principal gets the readable one, never the hidden one",
              s1 && s2 && fs_lookup(2, "pub.txt", &x) == FS_OK && x == w2 && fs_lookup(3, "pub.txt", &x) == FS_OK && x == w1 && fs_lookup(1, "pub.txt", &x) == FS_OK && x == w1);
        check("cleanup of the same-named test files leaves exactly the original file", fs_delete(1, w1) == FS_OK && fs_delete(2, w2) == FS_OK && fs_file_count() == 1);
    }
    {
        static const uint32_t sizes[] = { 0, 1, 511, 512, 513, 1023, 1024, 1025, 4096, 10000, FS_MAX_FILE };
        int ok = 1;
        for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
            fill(big, sizes[i], 20 + i);
            fs_id_t t; char nm[8] = { 's', (char)('a' + i), 0 };
            if (!put_file(1, nm, 3, big, sizes[i], &t)) ok = 0;
            struct fs_stat st; if (fs_stat(1, t, &st) || st.size != sizes[i]) ok = 0;
        }
        check("sizes 0, 1, 511, 512, 513, 1023..1025, 4096, 10000 and the 24 KiB maximum all round-trip exactly", ok);
        for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) { char nm[8] = { 's', (char)('a' + i), 0 }; fs_id_t t; if (fs_lookup(1, nm, &t) == FS_OK) fs_delete(1, t); }
    }
    fill(big, 3000, 1);
    fill(big2, 100, 99);
    check("overwrite in the middle changes only those bytes", fs_write(1, a, 1000, big2, 100) == 100 && (copy(big + 1000, big2, 100), file_is(1, "a.txt", big, 3000)));
    fill(big2, 700, 55);
    check("append at the end extends the file; the old part is unchanged", fs_write(1, a, 3000, big2, 700) == 700 && (copy(big + 3000, big2, 700), file_is(1, "a.txt", big, 3700)));
    {
        uint8_t part[200]; zero(part, sizeof part);
        int r1 = fs_read(1, a, 3650, part, 200), r2 = fs_read(1, a, 3700, part, 10), r3 = fs_read(1, a, 3701, part, 10), r4 = fs_read(1, a, 0, part, 0);
        check("read at an offset returns the bytes to EOF; at EOF returns 0; beyond EOF is FS_EARG; zero length is 0", r1 == 50 && same(part, big + 3650, 50) && r2 == 0 && r3 == FS_EARG && r4 == 0);
        check("write beyond the end (a hole) is FS_EARG and changes nothing; zero-length write is 0", fs_write(1, a, 3701, part, 4) == FS_EARG && fs_write(1, a, 0, part, 0) == 0 && file_is(1, "a.txt", big, 3700));
        check("NULL buffers with a length are FS_EARG", fs_read(1, a, 0, 0, 5) == FS_EARG && fs_write(1, a, 0, 0, 5) == FS_EARG);
    }

    /* ---- oversized writes ---- */
    fill(big, FS_MAX_FILE + 1, 7);
    fs_id_t big_id;
    check("oversized: writing 24 KiB + 1 byte is FS_EFBIG and the file is unchanged", fs_create(1, "big", 3, &big_id) == FS_OK && fs_write(1, big_id, 0, big, FS_MAX_FILE + 1) == FS_EFBIG && file_is(1, "big", big, 0));
    check("oversized: exactly 24 KiB is accepted; one more byte appended is FS_EFBIG; a huge length and offset+length overflow are refused",
          fs_write(1, big_id, 0, big, FS_MAX_FILE) == FS_MAX_FILE && fs_write(1, big_id, FS_MAX_FILE, big, 1) == FS_EFBIG &&
          fs_write(1, big_id, 0, big, 0xFFFFFFFFu) == FS_EFBIG && fs_write(1, big_id, FS_MAX_FILE, big, 0xFFFFFFFFu) == FS_EFBIG && file_is(1, "big", big, FS_MAX_FILE));
    fs_delete(1, big_id);

    /* ---- invalid and stale ids ---- */
    {
        fs_id_t t; fs_create(3, "gone", 3, &t); fs_write(3, t, 0, big, 100);
        int d = fs_delete(3, t);
        uint8_t tmp[8];
        check("deleted file: its id is invalid for read, write, stat and delete (FS_EBADID)",
              d == FS_OK && fs_read(3, t, 0, tmp, 8) == FS_EBADID && fs_write(3, t, 0, tmp, 8) == FS_EBADID && fs_delete(3, t) == FS_EBADID);
        fs_id_t t2; fs_create(3, "gone2", 3, &t2);
        check("the slot is reused by a new file but with a NEW generation: the old id still does not work", (t2 & 0xFF) == (t & 0xFF) && t2 != t && fs_read(3, t, 0, tmp, 1) == FS_EBADID && fs_read(3, t2, 0, tmp, 1) == 0);
        check("invalid ids: 0, out-of-range index, wrong generation, bits above the id width, all-ones",
              fs_read(1, 0, 0, tmp, 1) == FS_EBADID && fs_read(1, 200, 0, tmp, 1) == FS_EBADID && fs_read(1, (a & ~0xFFULL) + 0x100 + (a & 0xFF), 0, tmp, 1) == FS_EBADID &&
              fs_read(1, a | (1ULL << 50), 0, tmp, 1) == FS_EBADID && fs_read(1, ~0ULL, 0, tmp, 1) == FS_EBADID);
        fs_delete(3, t2);
    }

    /* ---- authorization ---- */
    {
        uint8_t tmp[64]; fill(big, 300, 4);
        fs_id_t priv, pub, ro;
        int ok = put_file(10, "private", FS_PERM_OWNER_R | FS_PERM_OWNER_W, big, 300, &priv);
        ok &= put_file(10, "public-ro", FS_PERM_OWNER_R | FS_PERM_OWNER_W | FS_PERM_OTHER_R, big, 300, &pub);
        ok &= put_file(10, "public-rw", FS_PERM_OWNER_R | FS_PERM_OWNER_W | FS_PERM_OTHER_R | FS_PERM_OTHER_W, big, 300, &ro);
        check("authorization setup: owner 10 creates three files with different permissions", ok);
        check("owner-only file: another principal gets FS_EACCES for read, write, stat, set_perm and delete, and cannot find it by name (FS_ENOENT)",
              fs_read(11, priv, 0, tmp, 64) == FS_EACCES && fs_write(11, priv, 0, tmp, 8) == FS_EACCES && fs_stat(11, priv, &(struct fs_stat){ 0 }) == FS_EACCES &&
              fs_set_perm(11, priv, 0xF) == FS_EACCES && fs_delete(11, priv) == FS_EACCES && fs_lookup(11, "private", &c) == FS_ENOENT);
        check("a principal that GUESSES the exact id (index and generation are not secret) is still refused by the permission check",
              fs_read(11, (fs_id_t)((priv >> 8) << 8 | (priv & 0xFF)), 0, tmp, 8) == FS_EACCES);
        check("other-readable file: others can read, cannot write, cannot change permissions or delete",
              fs_read(11, pub, 0, tmp, 64) == 64 && fs_write(11, pub, 0, tmp, 8) == FS_EACCES && fs_set_perm(11, pub, 0xF) == FS_EACCES && fs_delete(11, pub) == FS_EACCES && file_is(10, "public-ro", big, 300));
        check("other-writable file: others can write it, but still not delete it or change its permissions", fs_write(11, ro, 0, tmp, 8) == 8 && fs_delete(11, ro) == FS_EACCES && fs_set_perm(11, ro, 0) == FS_EACCES);
        check("the owner can change permissions: revoking other-read takes effect at once", fs_set_perm(10, pub, FS_PERM_OWNER_R | FS_PERM_OWNER_W) == FS_OK && fs_read(11, pub, 0, tmp, 8) == FS_EACCES && fs_set_perm(10, pub, 0x10) == FS_EARG);
        check("owner without the owner-write bit cannot write (permissions bind owners too); the kernel principal can do anything",
              fs_set_perm(10, pub, FS_PERM_OWNER_R) == FS_OK && fs_write(10, pub, 0, tmp, 8) == FS_EACCES && fs_write(0, pub, 0, tmp, 8) == 8 && fs_read(10, pub, 0, tmp, 8) == 8);
        int n1 = fs_list(10, ls, FS_MAX_FILES), n2 = fs_list(11, ls, FS_MAX_FILES);
        check("listing shows a principal only the files it may read (owner sees 3, a stranger only the other-readable one)", n1 >= 3 && n2 >= 1 && n2 < n1 && fs_list(11, ls, 0) == 0);
        fs_delete(10, priv); fs_delete(10, pub); fs_delete(10, ro);
    }
    fs_delete(1, a);

    /* ---- repeated create / write / read / delete, no leaks ---- */
    {
        uint32_t free0 = fs_free_blocks(), files0 = fs_file_count(); int ok = 1;
        fs_id_t prev = 0;
        for (uint32_t i = 0; i < 100; i++) {
            fs_id_t t; fill(big, 700 + i, i);
            if (!put_file(4, "cycle", 3, big, 700 + i, &t)) ok = 0;
            if (i && t == prev) ok = 0;                                    /* every incarnation has a different id */
            if (fs_delete(4, t) != FS_OK) ok = 0;
            prev = t;
        }
        check("100 create/write/read/delete cycles: always exact, every incarnation gets a new id, no blocks or files leak", ok && fs_free_blocks() == free0 && fs_file_count() == files0);
    }

    /* ---- inode exhaustion ---- */
    {
        int made = 0, last = 0; fs_id_t ids[FS_MAX_FILES + 2]; char nm[8];
        for (int i = 0; i < FS_MAX_FILES + 1; i++) { nm[0] = 'i'; nm[1] = (char)('a' + i / 26); nm[2] = (char)('a' + i % 26); nm[3] = 0; last = fs_create(5, nm, 3, &ids[i]); if (last == FS_OK) made++; else break; }
        check("32 files fit; the 33rd is FS_ENFILE and nothing is damaged", made == FS_MAX_FILES && last == FS_ENFILE && fs_file_count() == FS_MAX_FILES);
        for (int i = 0; i < made; i++) fs_delete(5, ids[i]);
        check("deleting them all frees every slot again", fs_file_count() == 0 && fs_create(5, "again", 3, &ids[0]) == FS_OK && fs_delete(5, ids[0]) == FS_OK);
    }

    /* ---- full disk (a small filesystem so the limit is hit deterministically: 100 sectors = 83 data blocks) ---- */
    {
        fs_id_t f0, f1;
        int fm = fs_format(100) == FS_OK && fs_mount() == FS_OK && fs_free_blocks() == 83;
        fill(big, FS_MAX_FILE, 3);
        int w0 = put_file(6, "f0", 3, big, FS_MAX_FILE, &f0);                      /* 48 blocks */
        fs_create(6, "f1", 3, &f1);
        int e1 = fs_write(6, f1, 0, big, FS_MAX_FILE);                                /* needs 48, only 35 free */
        struct fs_stat st; fs_stat(6, f1, &st);
        check("full disk: a write that does not fit is FS_ENOSPC, the target file is still empty and the other file is intact", fm && w0 && e1 == FS_ENOSPC && st.size == 0 && file_is(6, "f0", big, FS_MAX_FILE) && fs_free_blocks() == 35);
        fill(big2, 35 * 512, 8);
        int w1 = fs_write(6, f1, 0, big2, 35 * 512);
        check("full disk: a write that fits exactly fills the last free block (free = 0)", w1 == 35 * 512 && fs_free_blocks() == 0);
        uint8_t one = 1;
        int e2 = fs_write(6, f1, 35 * 512, &one, 1);
        check("full disk: whole-file copy-on-write needs room for the new copy, so even a 1-byte append is FS_ENOSPC (documented limit) and nothing changes",
              e2 == FS_ENOSPC && file_is(6, "f1", big2, 35 * 512) && file_is(6, "f0", big, FS_MAX_FILE));
        int d = fs_delete(6, f0);
        check("after deleting a file the space is reusable: free = 48, the 1-byte append now succeeds, and the file has 36 blocks (free = 47)",
              d == FS_OK && fs_free_blocks() == 48 && fs_write(6, f1, 35 * 512, &one, 1) == 1 && fs_free_blocks() == 47);
        fs_id_t f2; int e3 = fs_create(6, "f2", 3, &f2);
        check("a remount of the full-then-freed filesystem validates and shows the same files", e3 == FS_OK && fs_mount() == FS_OK && fs_file_count() == 2);
    }

    /* ---- crash injection: drop every write after the k-th, remount, expect the old OR the new file, never a mix ---- */
    {
        fs_format(0); fs_mount();
        fs_id_t t; uint8_t oldc[1000], newc[1800]; fill(oldc, 1000, 61); fill(newc, 1800, 62);
        put_file(7, "w0", 3, oldc, 1000, &t);
        uint64_t before = blk_writes_done(); fs_write(7, t, 0, newc, 1800); uint32_t W = (uint32_t)(blk_writes_done() - before);   /* writes one update needs */
        int ok = 1, sawold = 0, sawnew = 0, mono = 1, prev_new = 0;
        for (uint32_t k = 0; k <= W + 2; k++) {
            char nm[8] = { 'c', (char)('a' + k), 0 }; fs_id_t id;
            if (!put_file(7, nm, 3, oldc, 1000, &id)) { ok = 0; break; }
            blk_test_drop_writes_after((int)k);
            fs_write(7, id, 0, newc, 1800);
            blk_test_drop_writes_after(-1);
            int mr = fs_mount();                                                    /* "reboot": discard RAM state, read the disk */
            fs_id_t id2; zero(big2, sizeof big2);
            int lr = fs_lookup(7, nm, &id2), n = lr == FS_OK ? fs_read(7, id2, 0, big2, 2000) : -99;
            int isold = n == 1000 && same(big2, oldc, 1000), isnew = n == 1800 && same(big2, newc, 1800);
            if (mr != FS_OK || !(isold || isnew)) ok = 0;
            if (k < W && !isold) ok = 0;                                            /* power cut before the inode landed: must still be the old file */
            if (k >= W && !isnew) ok = 0;                                           /* everything landed: the new file */
            if (prev_new && !isnew) mono = 0;
            prev_new = isnew; sawold |= isold; sawnew |= isnew;
        }
        check("crash injection (write k of the update is the last to reach the disk, for every k): after remount the file is exactly the OLD or exactly the NEW content, never a mix; old up to the inode write, new from then on",
              ok && mono && sawold && sawnew && W >= 2);
        console_puts("[fs-test] info: one 1800-byte update issues "); console_dec(W); console_puts(" device writes (4 data sectors + 1 inode sector)\n");
        int c_ok = 1;
        for (int k = 0; k < 3; k++) {
            char nm[8] = { 'n', (char)('a' + k), 0 }; fs_id_t id;
            blk_test_drop_writes_after(k); fs_create(7, nm, 3, &id); blk_test_drop_writes_after(-1);
            int mr = fs_mount(); fs_id_t x; int present = mr == FS_OK && fs_lookup(7, nm, &x) == FS_OK;
            if (mr != FS_OK || present != (k >= 1)) c_ok = 0;
        }
        for (int k = 0; k < 3; k++) {
            char nm[8] = { 'd', (char)('a' + k), 0 }; fs_id_t id; put_file(7, nm, 3, oldc, 1000, &id);
            blk_test_drop_writes_after(k); fs_delete(7, id); blk_test_drop_writes_after(-1);
            int mr = fs_mount(); fs_id_t x; int present = mr == FS_OK && fs_lookup(7, nm, &x) == FS_OK;
            if (mr != FS_OK || present != (k == 0)) c_ok = 0;
        }
        check("crash injection for create and delete: the file exists afterwards exactly when its inode sector reached the disk; the filesystem always mounts", c_ok);
    }

    /* ---- error injection: updates that are KNOWN not to have committed vs. updates whose durability is UNCERTAIN ---- */
    {
        fs_id_t x; int rc, ok;
        uint8_t tmp8[8];
        (void)tmp8;

        /* 1. inode write rejected before reaching the device: definite, the filesystem stays usable */
        ok = inj_setup();
        blk_test_inject_write(BLK_INJ_REJECT, 3);
        rc = fs_write(3, g_ia, 0, A1, 1500);
        int still = fs_mounted() && rc < 0 && rc != FS_EUNCERTAIN && file_is(3, "A", A0, 700) && file_is(3, "B", B0, 900);
        int retry = fs_write(3, g_ia, 0, A1, 1500) == 1500 && file_is(3, "A", A1, 1500) && state_is(A1, 1500, B0, 900);
        check("inject: an inode write rejected before reaching the device is a DEFINITE failure: error returned, filesystem stays mounted and unchanged, a retry succeeds and survives a remount", ok && still && retry);

        /* 2. inode write lost, outcome unknown */
        ok = inj_setup();
        blk_test_inject_write(BLK_INJ_FAIL_UNSENT, 3);
        rc = fs_write(3, g_ia, 0, A1, 1500);
        int refused = rc == FS_EUNCERTAIN && all_refused();
        check("inject: an inode write that fails with unknown outcome returns FS_EUNCERTAIN, UNMOUNTS the filesystem, and every operation (read, write, create, delete, chmod, lookup, list, stat) then fails with FS_EMOUNT", ok && refused);
        check("inject: ... and a fresh mount finds the OLD state exactly (this time the write had not reached the disk); the other file is intact", state_is(A0, 700, B0, 900));

        /* 3. inode write reached the disk, completion reported as an error */
        ok = inj_setup();
        blk_test_inject_write(BLK_INJ_FAIL_SENT, 3);
        rc = fs_write(3, g_ia, 0, A1, 1500);
        refused = rc == FS_EUNCERTAIN && all_refused();
        check("inject: an inode write that LANDED but was reported as failed: FS_EUNCERTAIN, unmounted, all operations refused", ok && refused);
        check("inject: ... and a fresh mount finds the NEW state exactly (the operation had committed despite the error); the other file is intact", state_is(A1, 1500, B0, 900));

        /* 4. the dangerous follow-up: with a stale allocation map a second write would reuse the sectors the new inode references */
        ok = inj_setup();
        {
            blk_test_inject_write(BLK_INJ_FAIL_SENT, 3);
            rc = fs_write(3, g_ia, 0, A1, 1500);
            uint32_t bl[FS_MAX_BLOCKS], nbk = 0, k; uint8_t before[3][BLK_SECTOR], after[3][BLK_SECTOR];
            int have = disk_blocks(0, bl, &nbk) && nbk == 3;
            for (k = 0; have && k < 3; k++) blk_read(bl[k], before[k], 1);
            int w2 = fs_write(3, g_ib, 0, B1, 900);                          /* would overwrite those sectors if the stale cache were trusted */
            int same_sectors = have;
            for (k = 0; have && k < 3; k++) { blk_read(bl[k], after[k], 1); if (!same(before[k], after[k], BLK_SECTOR)) same_sectors = 0; }
            check("inject: after an uncertain commit a second write is refused (FS_EMOUNT) and the physical sectors referenced by the new inode are byte-for-byte untouched",
                  ok && rc == FS_EUNCERTAIN && have && w2 == FS_EMOUNT && same_sectors);
            int fine = state_is(A1, 1500, B0, 900);
            int w3 = fs_write(3, g_ib, 0, B1, 900) == 900;
            uint32_t ab[FS_MAX_BLOCKS], bb[FS_MAX_BLOCKS], na = 0, nb = 0; int disjoint = disk_blocks(0, ab, &na) && disk_blocks(1, bb, &nb);
            for (k = 0; disjoint && k < na; k++) for (uint32_t j = 0; j < nb; j++) if (ab[k] == bb[j]) disjoint = 0;
            check("inject: after remount both files are exact, a write to the second file succeeds, the first stays exact, and the two files own disjoint sectors (no duplicate ownership)",
                  fine && w3 && disjoint && file_is(3, "A", A1, 1500) && file_is(3, "B", B1, 900) && fs_mount() == FS_OK);
        }

        /* 5. flush after the inode write fails */
        {
            int both = 1;
            for (int kind = BLK_INJ_FAIL_UNSENT; kind <= BLK_INJ_FAIL_SENT; kind++) {
                ok = inj_setup(); blk_test_inject_flush(kind, 1);              /* flush 0 = data, flush 1 = after the inode */
                rc = fs_write(3, g_ia, 0, A1, 1500);
                if (!(ok && rc == FS_EUNCERTAIN && all_refused() && state_is(A1, 1500, B0, 900))) both = 0;
            }
            check("inject: a failed flush AFTER the inode write is uncertain too: FS_EUNCERTAIN, unmounted, and the remount shows the new state (the write had reached the disk)", both);
        }

        /* 6. data-block write fails before the inode is published */
        {
            int both = 1;
            for (int kind = BLK_INJ_REJECT; kind <= BLK_INJ_FAIL_SENT; kind++) {
                ok = inj_setup(); blk_test_inject_write(kind, 1);              /* second data sector */
                rc = fs_write(3, g_ia, 0, A1, 1500);
                int mid = fs_mounted() && rc < 0 && rc != FS_EUNCERTAIN && file_is(3, "A", A0, 700) && file_is(3, "B", B0, 900);
                int rt = fs_write(3, g_ia, 0, A1, 1500) == 1500 && state_is(A1, 1500, B0, 900);
                if (!(ok && mid && rt)) both = 0;
            }
            check("inject: a data-block write failure (rejected, lost, or landed-but-reported) before the inode is published is a definite failure: mounted, old content intact, retry succeeds", both);
        }

        /* 7. data flush fails */
        ok = inj_setup(); blk_test_inject_flush(BLK_INJ_FAIL_SENT, 0);
        rc = fs_write(3, g_ia, 0, A1, 1500);
        int dfl = fs_mounted() && rc < 0 && rc != FS_EUNCERTAIN && file_is(3, "A", A0, 700) && fs_write(3, g_ia, 0, A1, 1500) == 1500 && state_is(A1, 1500, B0, 900);
        check("inject: a failed data flush stops the update before the inode is written: error, still mounted, old content intact, retry succeeds", ok && dfl);

        /* 8. create */
        {
            int c_ok = 1;
            ok = inj_setup(); blk_test_inject_write(BLK_INJ_REJECT, 0); rc = fs_create(3, "C", 3, &x);
            if (!(ok && rc < 0 && rc != FS_EUNCERTAIN && fs_mounted() && fs_lookup(3, "C", &x) == FS_ENOENT && fs_create(3, "C", 3, &x) == FS_OK)) c_ok = 0;
            ok = inj_setup(); blk_test_inject_write(BLK_INJ_FAIL_UNSENT, 0); rc = fs_create(3, "C", 3, &x);
            if (!(ok && rc == FS_EUNCERTAIN && all_refused() && fs_mount() == FS_OK && fs_lookup(3, "C", &x) == FS_ENOENT && fs_file_count() == 2)) c_ok = 0;
            ok = inj_setup(); blk_test_inject_write(BLK_INJ_FAIL_SENT, 0); rc = fs_create(3, "C", 3, &x);
            if (!(ok && rc == FS_EUNCERTAIN && all_refused() && fs_mount() == FS_OK && fs_lookup(3, "C", &x) == FS_OK && fs_file_count() == 3 && state_is(A0, 700, B0, 900))) c_ok = 0;
            check("inject create: rejected = not created and still mounted; lost = uncertain and absent after remount; landed = uncertain and present after remount; other files intact", c_ok);
        }

        /* 9. delete */
        {
            int d_ok = 1;
            ok = inj_setup(); blk_test_inject_write(BLK_INJ_REJECT, 0); rc = fs_delete(3, g_ib);
            if (!(ok && rc < 0 && rc != FS_EUNCERTAIN && fs_mounted() && file_is(3, "B", B0, 900))) d_ok = 0;
            ok = inj_setup(); blk_test_inject_write(BLK_INJ_FAIL_UNSENT, 0); rc = fs_delete(3, g_ib);
            if (!(ok && rc == FS_EUNCERTAIN && all_refused() && state_is(A0, 700, B0, 900))) d_ok = 0;
            ok = inj_setup(); blk_test_inject_write(BLK_INJ_FAIL_SENT, 0); rc = fs_delete(3, g_ib);
            if (!(ok && rc == FS_EUNCERTAIN && all_refused() && fs_mount() == FS_OK && fs_lookup(3, "B", &x) == FS_ENOENT && file_is(3, "A", A0, 700) && fs_file_count() == 1)) d_ok = 0;
            check("inject delete: rejected = file kept and still mounted; lost = uncertain and file present after remount; landed = uncertain and file gone after remount; the other file intact", d_ok);
        }

        /* 10. permission change */
        {
            int p_ok = 1; struct fs_stat st;
            ok = inj_setup(); blk_test_inject_write(BLK_INJ_REJECT, 0); rc = fs_set_perm(3, g_ia, 0xF);
            if (!(ok && rc < 0 && rc != FS_EUNCERTAIN && fs_mounted() && fs_stat(3, g_ia, &st) == FS_OK && st.perm == 3)) p_ok = 0;
            ok = inj_setup(); blk_test_inject_write(BLK_INJ_FAIL_UNSENT, 0); rc = fs_set_perm(3, g_ia, 0xF);
            if (!(ok && rc == FS_EUNCERTAIN && all_refused() && fs_mount() == FS_OK && fs_stat(3, g_ia, &st) == FS_OK && st.perm == 3)) p_ok = 0;
            ok = inj_setup(); blk_test_inject_write(BLK_INJ_FAIL_SENT, 0); rc = fs_set_perm(3, g_ia, 0xF);
            if (!(ok && rc == FS_EUNCERTAIN && all_refused() && fs_mount() == FS_OK && fs_stat(3, g_ia, &st) == FS_OK && st.perm == 0xF)) p_ok = 0;
            check("inject chmod: rejected = unchanged and mounted; lost = uncertain, old permissions after remount; landed = uncertain, new permissions after remount", p_ok);
        }
        blk_test_inject_clear();
    }

    /* ---- leave persistent files for the next boots ---- */
    {
        fs_id_t p, s, bt; static uint8_t content[3100]; uint32_t cn; uint8_t cnt[4];
        persist_content(content, &cn);
        int fm = fs_format(0) == FS_OK && fs_mount() == FS_OK;
        int a1 = put_file(7, "persist.txt", FS_PERM_OWNER_R | FS_PERM_OWNER_W, content, cn, &p);
        fill(big, 700, 77);
        int a2 = put_file(7, "shared.txt", FS_PERM_OWNER_R | FS_PERM_OWNER_W | FS_PERM_OTHER_R, big, 700, &s);
        put32(cnt, 1);
        int a3 = put_file(7, "boots", FS_PERM_OWNER_R | FS_PERM_OWNER_W | FS_PERM_OTHER_R, cnt, 4, &bt);
        check("boot 1: filesystem formatted and persistent files written (persist.txt private to owner 7, shared.txt other-readable, boots counter = 1)", fm && a1 && a2 && a3 && blk_flush() == BLK_OK);
    }
}

/* ---- boots 2+: persistence, authorization after restart, corruption (injected and restored) ---- */
static int sector_rw(uint32_t sec, uint8_t *buf, int write) { return write ? blk_write(sec, buf, 1) : blk_read(sec, buf, 1); }

static void phase_persistent(void)
{
    static uint8_t content[3100]; uint32_t cn; persist_content(content, &cn);
    fs_id_t p, s, bt; uint8_t tmp[16];
    check("mounted WITHOUT formatting: the files from the earlier boot are there", fs_file_count() >= 3 && fs_lookup(7, "persist.txt", &p) == FS_OK && fs_lookup(7, "shared.txt", &s) == FS_OK && fs_lookup(7, "boots", &bt) == FS_OK);
    check("persist.txt: all 3023 bytes written by an EARLIER boot read back identical (data survived the emulator restart)", file_is(7, "persist.txt", content, cn));
    fill(big, 700, 77);
    check("shared.txt: 700 bytes identical, owner 7, permissions preserved", file_is(7, "shared.txt", big, 700));
    struct fs_stat st; fs_stat(7, p, &st);
    check("metadata survived: owner 7, permission owner-rw only, size 3023, name persist.txt", st.owner == 7 && st.perm == 3 && st.size == cn && st.name[0] == 'p');
    check("authorization after the restart: a stranger cannot read or find persist.txt, can read shared.txt but not write it",
          fs_read(9, p, 0, tmp, 16) == FS_EACCES && fs_lookup(9, "persist.txt", &p) == FS_ENOENT && fs_lookup(9, "shared.txt", &s) == FS_OK && fs_read(9, s, 0, tmp, 16) == 16 && fs_write(9, s, 0, tmp, 4) == FS_EACCES);
    uint8_t cb[4]; int r = fs_read(7, bt, 0, cb, 4); uint32_t boots = r == 4 ? get32(cb) : 0;
    put32(cb, boots + 1);
    int w = fs_write(7, bt, 0, cb, 4);
    uint8_t cb2[4]; int r2 = fs_read(7, bt, 0, cb2, 4);
    console_puts("[fs-test] info: boot counter was "); console_dec(boots); console_puts(", now "); console_dec(boots + 1); console_puts("\n");
    check("boot counter: read the value left by the previous boot, wrote value+1, read it back (state changes persist across runs)", r == 4 && boots >= 1 && w == 4 && r2 == 4 && get32(cb2) == boots + 1);
    check("a file keeps its id across remount (generation and slot are on the disk)", ({ fs_id_t q; fs_mount() == FS_OK && fs_lookup(7, "persist.txt", &q) == FS_OK && q == p; }));

    /* ---- corruption suite: damage a sector, expect a precise refusal, restore, expect a clean mount ---- */
    static uint8_t sb[BLK_SECTOR], sb0[BLK_SECTOR], in[BLK_SECTOR], in0[BLK_SECTOR];
    sector_rw(0, sb0, 0); sector_rw(1, in0, 0);
    int e, ok;

    copy(sb, sb0, BLK_SECTOR); sb[16] ^= 0x01; sector_rw(0, sb, 1);
    e = fs_mount(); ok = e == FS_ECORRUPT && !fs_mounted() && fs_create(1, "z", 3, &p) == FS_EMOUNT;
    sector_rw(0, sb0, 1);
    check("corrupted superblock (one bit): mount is FS_ECORRUPT, the filesystem stays unmounted and refuses operations", ok);

    copy(sb, sb0, BLK_SECTOR); put32(sb + 8, 2); put32(sb + 60, fs_crc32(sb, 60)); sector_rw(0, sb, 1);
    e = fs_mount(); sector_rw(0, sb0, 1);
    check("version mismatch (a valid superblock claiming version 2): FS_EVERSION, not mounted, not modified", e == FS_EVERSION && !fs_mounted());

    copy(sb, sb0, BLK_SECTOR); sb[0] = 'X'; sector_rw(0, sb, 1);
    e = fs_mount(); sector_rw(0, sb0, 1);
    check("foreign content (bad magic): FS_ENOFS, never mistaken for a filesystem", e == FS_ENOFS && !fs_mounted());

    copy(in, in0, BLK_SECTOR); in[40] ^= 0x20; sector_rw(1, in, 1);                      /* a byte inside inode 0's name/blocks area */
    e = fs_mount(); sector_rw(1, in0, 1);
    check("corrupted inode (one flipped bit): FS_ECORRUPT from its checksum", e == FS_ECORRUPT);

    copy(in, in0, BLK_SECTOR); put32(in + 60, 99999999u); put32(in + 252, fs_crc32(in, 252)); sector_rw(1, in, 1);   /* block pointer far outside the disk, CRC re-forged */
    e = fs_mount(); sector_rw(1, in0, 1);
    check("a block pointer outside the disk behind a VALID checksum is still rejected (range check)", e == FS_ECORRUPT);

    copy(in, in0, BLK_SECTOR); copy(in + 60, in + 256 + 60, 4);                           /* inode 0 and inode 1 claim the same first sector */
    put32(in + 252, fs_crc32(in, 252)); sector_rw(1, in, 1);
    e = fs_mount(); sector_rw(1, in0, 1);
    check("two files claiming the same sector behind valid checksums are rejected (no block used twice)", e == FS_ECORRUPT);

    copy(in, in0, BLK_SECTOR); copy(in + 28, in + 256 + 28, 32); put32(in + 252, fs_crc32(in, 252)); sector_rw(1, in, 1);
    e = fs_mount(); sector_rw(1, in0, 1);
    check("two files with the same name behind valid checksums are rejected", e == FS_ECORRUPT);

    {
        /* the same name behind valid checksums is only corruption within ONE owner's namespace: with different owners it mounts */
        /* The earlier damage tests leave the filesystem unmounted (fs_file_count() is 0 then), so mount the intact disk first and take the
         * baseline from it: the disk may also hold files created by the EL0 tests. */
        int clean = fs_mount() == FS_OK; uint32_t files0 = fs_file_count();
        copy(in, in0, BLK_SECTOR); copy(in + 28, in + 256 + 28, 32); put32(in + 8, 9); put32(in + 252, fs_crc32(in, 252)); sector_rw(1, in, 1);
        e = fs_mount(); fs_id_t d1, d2;
        int both = e == FS_OK && fs_lookup(9, "shared.txt", &d1) == FS_OK && fs_lookup(7, "shared.txt", &d2) == FS_OK && d1 != d2;
        sector_rw(1, in0, 1);
        check("two files with the same name but DIFFERENT owners behind valid checksums mount (names are per owner); each owner finds its own", both);
        check("... and restoring the original inode mounts the original filesystem again", clean && files0 >= 3 && fs_mount() == FS_OK && fs_file_count() == files0);
    }

    {
        /* A slot whose generation is used up must be skipped, not block creation: craft the first free slot as FREED at the last generation.
         * Files are never deleted in these tests, so the used slots are 0..n-1 and the first free slot is n (inode slot i lives in sector 1 + i/2). */
        static uint8_t g2[BLK_SECTOR], g20[BLK_SECTOR], g30[BLK_SECTOR];
        uint32_t files1 = fs_file_count(), slot = files1, sec = 1 + slot / 2, off = (slot & 1u) * 256u;
        sector_rw(sec, g20, 0); sector_rw(sec + 1, g30, 0); copy(g2, g20, BLK_SECTOR);
        for (uint32_t k = 0; k < 256; k++) g2[off + k] = 0;
        put32(g2 + off, 0xF4EEF4EEu); put32(g2 + off + 4, 0x00FFFFFFu); put32(g2 + off + 252, fs_crc32(g2 + off, 252));
        sector_rw(sec, g2, 1);
        e = fs_mount(); fs_id_t nid = 0; int rc = e == FS_OK ? fs_create(7, "after.txt", 3, &nid) : e;
        int skipped = e == FS_OK && rc == FS_OK && (nid & 0xFF) != slot;
        if (rc == FS_OK) fs_delete(7, nid);
        sector_rw(sec, g20, 1); sector_rw(sec + 1, g30, 1);                                  /* restore both inode sectors exactly */
        check("a slot whose generation is used up (FREED at 0xFFFFFF) is skipped: create still succeeds in another slot while other slots are free", skipped);
        check("... and after restoring the sectors the original filesystem mounts with the same files", fs_mount() == FS_OK && fs_file_count() == files1);
    }

    {
        static uint8_t j[BLK_SECTOR], j0[BLK_SECTOR];
        sector_rw(5, j0, 0); copy(j, j0, BLK_SECTOR); j[5] = 0x77;                           /* sector 5 = inodes 8 and 9: never used */
        sector_rw(5, j, 1); e = fs_mount(); sector_rw(5, j0, 1);
        check("junk in a never-used inode slot is rejected (free slots must be all zero)", e == FS_ECORRUPT);
    }

    /* data corruption: flip one byte inside a data block of persist.txt; the metadata is fine so mount works but the read is refused */
    {
        fs_mount(); fs_lookup(7, "persist.txt", &p);
        uint32_t first_block = 0; for (uint32_t i = 0; i < 2; i++) { uint8_t t[BLK_SECTOR]; sector_rw(1, t, 0); if (get32(t + i * 256) == 0xA11CE5EDu && t[i * 256 + 28] == 'p') first_block = get32(t + i * 256 + 60); }
        uint8_t d[BLK_SECTOR], d0[BLK_SECTOR];
        if (first_block) { sector_rw(first_block, d0, 0); copy(d, d0, BLK_SECTOR); d[100] ^= 0x04; sector_rw(first_block, d, 1); }
        int mr = fs_mount(); fs_lookup(7, "persist.txt", &p); fs_lookup(7, "shared.txt", &s);
        int rr = fs_read(7, p, 0, big2, 100), other = fs_read(7, s, 0, big2, 100);
        if (first_block) sector_rw(first_block, d0, 1);
        fs_mount(); fs_lookup(7, "persist.txt", &p);
        check("silent data corruption (one flipped bit in a data block): mount still works, reading that file is FS_ECORRUPT (never wrong bytes), other files are fine, restoring fixes it",
              first_block && mr == FS_OK && rr == FS_ECORRUPT && other == 100 && file_is(7, "persist.txt", content, cn));
    }

    {
        int again = fs_mount() == FS_OK && fs_file_count() >= 3 && file_is(7, "persist.txt", content, cn);
        check("after all the injected damage was restored the filesystem mounts cleanly again with the same three files", again);
    }
}

static void phase_readonly(void)
{
    static uint8_t content[3100]; uint32_t cn; persist_content(content, &cn);
    fs_id_t p; uint8_t b = 1;
    int m = fs_mount();
    check("read-only disk: mounts and the persisted file reads back exactly", m == FS_OK && file_is(7, "persist.txt", content, cn));
    fs_lookup(7, "persist.txt", &p);
    int w = fs_write(7, p, 0, &b, 1), c = fs_create(7, "new", 3, &p), d = fs_delete(7, p), sp = fs_set_perm(7, p, 0), f = fs_format(0);
    check("read-only disk: write, create, delete, set_perm and format are all refused with FS_ERO, and the content is still intact",
          w == FS_ERO && c == FS_ERO && d == FS_ERO && sp == FS_ERO && f == FS_ERO && file_is(7, "persist.txt", content, cn));
}

void fstest_run(void)
{
    if (!blk_present()) { console_puts("[fs] no block device: filesystem tests not run\n"); return; }
    int m = fs_mount();
    console_puts("[fs] initial mount: "); console_puts(fs_strerror(m)); console_puts("\n");
    if (blk_readonly()) { phase_readonly(); return; }
    if (m == FS_ENOFS) { phase_functional(); return; }
    if (m == FS_OK) { phase_persistent(); return; }
    check("unexpected initial mount result", 0);
}
#else
void fstest_run(void)
{
    if (!blk_present()) return;
    int m = fs_mount();
    console_puts("[fs] mount: ");
    if (m == FS_OK) { console_puts("AuroraFS v1 mounted read-only-safe, "); console_dec(fs_file_count()); console_puts(" files, "); console_dec(fs_free_blocks()); console_puts(" free blocks\n"); }
    else if (m == FS_ENOFS) console_puts("no filesystem on the disk (nothing is formatted automatically)\n");
    else if (m == FS_ECORRUPT) console_puts("filesystem is CORRUPT: refusing to mount; the disk is left untouched\n");
    else if (m == FS_EVERSION) console_puts("unsupported filesystem version: refusing to mount; the disk is left untouched\n");
    else { console_puts("failed: "); console_puts(fs_strerror(m)); console_puts("\n"); }
}
#endif

int fstest_summary(void)
{
#ifdef AURORA_FS_TEST
    if (npass + nfail == 0) return 0;
    console_puts("AURORA-FS-SELFTEST: "); console_dec((uint64_t)npass); console_puts(" passed, ");
    console_dec((uint64_t)nfail); console_puts(" failed\n");
#endif
    return nfail;
}
