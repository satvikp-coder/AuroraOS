/*
 * kernel/blktest.c — block-device tests against a disposable QEMU virtual disk.
 *
 * DESTRUCTIVE: these tests write to the disk. They run only in kernels built with AURORA_BLK_TEST=1 (never in a normal
 * build) and only touch sectors inside the attached virtio disk. The test scripts attach a freshly created image file.
 *
 * Output: "[blk-test] <name>: PASS|FAIL" and "AURORA-BLK-SELFTEST: N passed, M failed".
 * Persistence: sector PERSIST_SECTOR holds a marker written by the FIRST boot on a disk and verified byte for byte by every
 * later boot on the same disk image (tests/qemu/storage-tests.sh boots twice on one image).
 */
#include <blk.h>
#include <console.h>
#include <mm.h>

uint64_t boot_dtb_address(void);   /* kernel.c */
static int nfail;
#ifdef AURORA_BLK_TEST
static int npass;
#endif

#ifdef AURORA_BLK_TEST
#define PERSIST_SECTOR 100u

static void check(const char *name, int ok)
{
    console_puts("[blk-test] "); console_puts(name); console_puts(ok ? ": PASS\n" : ": FAIL\n");
    if (ok) npass++; else nfail++;
}

static uint8_t buf[BLK_SECTOR * 20], buf2[BLK_SECTOR * 20];

static uint8_t pat(uint32_t seed, uint32_t i) { return (uint8_t)((seed * 131u + i * 7u + (i >> 8) * 13u) ^ 0x5Au); }
static void fill(uint8_t *p, uint32_t n, uint32_t seed) { for (uint32_t i = 0; i < n; i++) p[i] = pat(seed, i); }
static int same(const uint8_t *a, const uint8_t *b, uint32_t n) { for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static void zero(uint8_t *p, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = 0; }

void blktest_run(void)
{
    const struct blk_info *in = blk_get_info();
    console_puts("[blk] virtio-mmio nodes in the device tree: "); console_dec((uint64_t)in->nodes_seen);
    console_puts(", block devices: "); console_dec((uint64_t)in->nodes_blk);
    if (!blk_present()) { console_puts("\n[blk] no usable block device: block tests not run\n"); return; }
    console_puts("\n[blk] device at "); console_hex(in->base); console_puts(", transport version "); console_dec(in->version);
    console_puts(", SPI interrupt ID "); console_dec(in->intid); console_puts(" (polled, unused), features hi/lo ");
    console_hex(((uint64_t)in->features_hi << 32) | in->features_lo);
    console_puts(", capacity "); console_dec(blk_sectors()); console_puts(" sectors");
    console_puts(blk_readonly() ? ", READ-ONLY\n" : "\n");

    uint64_t cap = blk_sectors();
    check("identify: modern transport (version 2) with VIRTIO_F_VERSION_1 negotiated", in->version == 2 && (in->features_hi & 1u));
    check("identify: capacity is reported and large enough for the test layout (>= 512 sectors)", cap >= 512);

    if (blk_readonly()) {
        int r = blk_read(0, buf, 1);
        int w = blk_write(0, buf, 1);
        check("read-only disk: reads succeed, writes are refused with BLK_ERO before touching the device", r == BLK_OK && w == BLK_ERO);
        return;
    }

    /* single-sector round trip, neighbours untouched */
    fill(buf, 3 * BLK_SECTOR, 1);
    int w3 = blk_write(48, buf, 3);                                          /* sectors 48..50 hold a known background */
    fill(buf2, BLK_SECTOR, 77);
    int w1 = blk_write(49, buf2, 1);                                         /* overwrite the middle one only */
    zero(buf, sizeof buf);
    int r3 = blk_read(48, buf, 3);
    uint8_t exp[BLK_SECTOR * 3]; fill(exp, 3 * BLK_SECTOR, 1);
    for (uint32_t i = 0; i < BLK_SECTOR; i++) exp[BLK_SECTOR + i] = pat(77, i);
    check("write then read one sector: exact bytes; the neighbouring sectors keep their old contents",
          w3 == BLK_OK && w1 == BLK_OK && r3 == BLK_OK && same(buf, exp, 3 * BLK_SECTOR));

    /* multi-sector transfer spanning several bounce chunks (20 sectors = 8 + 8 + 4) */
    fill(buf, 20 * BLK_SECTOR, 5);
    int wm = blk_write(200, buf, 20);
    zero(buf2, sizeof buf2);
    int rm = blk_read(200, buf2, 20);
    check("20-sector write and read (three bounce chunks): every byte identical", wm == BLK_OK && rm == BLK_OK && same(buf, buf2, 20 * BLK_SECTOR));

    /* repeated cycles with different data at the same place */
    int cyc = 1;
    for (uint32_t k = 0; k < 40 && cyc; k++) {
        fill(buf, 2 * BLK_SECTOR, 100 + k);
        zero(buf2, 2 * BLK_SECTOR);
        if (blk_write(300, buf, 2) || blk_read(300, buf2, 2) || !same(buf, buf2, 2 * BLK_SECTOR)) cyc = 0;
    }
    check("40 write/read cycles at one location: always the newest data", cyc);

    /* first and last sector */
    fill(buf, BLK_SECTOR, 9); fill(buf2, BLK_SECTOR, 10);
    int wl = blk_write(0, buf, 1) == BLK_OK && blk_write(cap - 1, buf2, 1) == BLK_OK;
    uint8_t t1[BLK_SECTOR], t2[BLK_SECTOR];
    int rl = blk_read(0, t1, 1) == BLK_OK && blk_read(cap - 1, t2, 1) == BLK_OK;
    check("first and last sector are readable and writable", wl && rl && same(t1, buf, BLK_SECTOR) && same(t2, buf2, BLK_SECTOR));

    /* bounds and argument errors: refused before any device access, and nothing is changed */
    uint64_t before = blk_requests();
    uint8_t one[BLK_SECTOR]; fill(one, BLK_SECTOR, 3);
    int e1 = blk_read(cap, one, 1), e2 = blk_write(cap, one, 1), e3 = blk_read(cap - 1, one, 2), e4 = blk_write(cap - 1, one, 2);
    int e5 = blk_read(~0ULL, one, 1), e6 = blk_read(1, one, 0xFFFFFFFFu), e7 = blk_write(0xFFFFFFFFFFFFFFFFULL - 1, one, 3);
    int e8 = blk_read(0, one, 0), e9 = blk_read(0, 0, 1), e10 = blk_write(0, 0, 1), e11 = blk_write(0, one, 0);
    check("bounds: sector == capacity, a transfer running past the end, 2^64-1, a huge count and a wrapping range are all BLK_ERANGE",
          e1 == BLK_ERANGE && e2 == BLK_ERANGE && e3 == BLK_ERANGE && e4 == BLK_ERANGE && e5 == BLK_ERANGE && e6 == BLK_ERANGE && e7 == BLK_ERANGE);
    check("arguments: zero sectors and NULL buffers are BLK_EARG", e8 == BLK_EARG && e9 == BLK_EARG && e10 == BLK_EARG && e11 == BLK_EARG);
    check("rejected requests never reached the device (request counter unchanged)", blk_requests() == before);
    uint8_t tl[BLK_SECTOR];
    check("a refused write past the end did not modify the last sector", blk_read(cap - 1, tl, 1) == BLK_OK && same(tl, buf2, BLK_SECTOR));

    /* flush */
    check("flush completes (or is a defined no-op without a volatile cache)", blk_flush() == BLK_OK);

    /* bounded failure: the device never sees the doorbell, the driver must time out, reset, and recover */
    fill(buf, BLK_SECTOR, 42);
    int ok_before = blk_write(400, buf, 1) == BLK_OK;
    blk_test_set_timeout_ms(50);
    blk_test_stall_next(1);
    uint64_t t0 = blk_timeouts();
    int st = blk_write(401, buf, 1);                                         /* never notified: must report a timeout, not hang */
    blk_test_set_timeout_ms(1000);
    int ok_after = blk_write(402, buf, 1) == BLK_OK;
    zero(buf2, BLK_SECTOR);
    int rd_after = blk_read(400, buf2, 1) == BLK_OK && same(buf, buf2, BLK_SECTOR);
    check("a request whose doorbell is lost times out (BLK_ETIMEOUT), the device is reset and re-initialised, and later I/O works with old data intact",
          ok_before && st == BLK_ETIMEOUT && blk_timeouts() == t0 + 1 && ok_after && rd_after && blk_present());

    /* initialisation is idempotent and allocation-neutral */
    {
        struct pmm_stats s0, s1; pmm_get_stats(&s0);
        int again = blk_init((const void *)(uintptr_t)boot_dtb_address());
        pmm_get_stats(&s1);
        check("blk_init called again is a no-op: returns BLK_OK, allocates no page, and the device keeps working", again == BLK_OK && s1.free_pages == s0.free_pages && blk_read(0, buf2, 1) == BLK_OK);
    }

    /* persistence across reboots of the emulator on the same image */
    uint8_t m[BLK_SECTOR], got[BLK_SECTOR];
    for (uint32_t i = 0; i < BLK_SECTOR; i++) m[i] = pat(0xA0A0u, i);
    m[0] = 'A'; m[1] = 'U'; m[2] = 'R'; m[3] = 'O'; m[4] = 'R'; m[5] = 'A'; m[6] = '-'; m[7] = 'P';
    int rr = blk_read(PERSIST_SECTOR, got, 1);
    int has_marker = rr == BLK_OK && got[0] == 'A' && got[1] == 'U' && got[2] == 'R' && got[3] == 'O' && got[4] == 'R' && got[5] == 'A' && got[6] == '-' && got[7] == 'P';
    if (has_marker) {
        check("persistence: the marker written by an EARLIER boot is on the disk, all 512 bytes identical (data survived the emulator restart)", same(got, m, BLK_SECTOR));
    } else {
        int wr = blk_write(PERSIST_SECTOR, m, 1) == BLK_OK && blk_flush() == BLK_OK;
        uint8_t back[BLK_SECTOR];
        check("persistence: first boot on this disk, marker written and flushed", wr && blk_read(PERSIST_SECTOR, back, 1) == BLK_OK && same(back, m, BLK_SECTOR));
    }
}
#else
void blktest_run(void)
{
    if (blk_present()) { console_puts("[blk] block device present ("); console_dec(blk_sectors()); console_puts(" sectors); not touched by a normal boot\n"); }
    else console_puts("[blk] no block device\n");
}
#endif

int blktest_summary(void)
{
#ifdef AURORA_BLK_TEST
    if (!blk_present() && npass + nfail == 0) return 0;
    console_puts("AURORA-BLK-SELFTEST: "); console_dec((uint64_t)npass); console_puts(" passed, ");
    console_dec((uint64_t)nfail); console_puts(" failed\n");
#endif
    return nfail;
}
