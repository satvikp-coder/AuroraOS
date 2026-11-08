/*
 * kernel/drivers/virtio_blk.c — virtio block device over the virtio-mmio transport (original code, written from the
 * OASIS VIRTIO 1.x specification: 2.1 device status, 2.7 split virtqueues, 3.1 initialisation, 4.2 MMIO transport,
 * 5.2 block device).
 *
 * Scope and choices (verified against the QEMU "virt" device tree and register reads at run time):
 *  - Only the MODERN transport (MMIO Version 2, requires VIRTIO_F_VERSION_1). A legacy (Version 1) device is reported
 *    and refused, never driven with guessed behaviour. QEMU needs  -global virtio-mmio.force-legacy=false.
 *  - Polled completion with a hard time bound (no interrupts): one request in flight, so no state is shared with
 *    interrupt handlers. A timeout resets and re-initialises the device; the failed request returns BLK_ETIMEOUT.
 *  - Negotiated features: VIRTIO_F_VERSION_1, plus VIRTIO_BLK_F_RO / VIRTIO_BLK_F_FLUSH when offered. Nothing else
 *    (no indirect descriptors, no event idx, no multi-queue): the smallest surface that is still correct.
 *  - Transfers go through a kernel-owned bounce page, so a caller buffer is never handed to the device.
 *  - The MMIO window is declared to the MMU policy and mapped as Device memory, only for pages the device tree names.
 */
#include <arch.h>
#include <blk.h>
#include <console.h>
#include <mm.h>
#include <timer.h>

#define R_MAGIC 0x000
#define R_VERSION 0x004
#define R_DEVID 0x008
#define R_DEVFEAT 0x010
#define R_DEVFEATSEL 0x014
#define R_DRVFEAT 0x020
#define R_DRVFEATSEL 0x024
#define R_QSEL 0x030
#define R_QNUMMAX 0x034
#define R_QNUM 0x038
#define R_QREADY 0x044
#define R_QNOTIFY 0x050
#define R_INTSTATUS 0x060
#define R_INTACK 0x064
#define R_STATUS 0x070
#define R_QDESCLO 0x080
#define R_QDESCHI 0x084
#define R_QDRVLO 0x090
#define R_QDRVHI 0x094
#define R_QDEVLO 0x0a0
#define R_QDEVHI 0x0a4
#define R_CFGGEN 0x0fc
#define R_CONFIG 0x100

#define ST_ACK 1u
#define ST_DRIVER 2u
#define ST_DRIVER_OK 4u
#define ST_FEATURES_OK 8u
#define ST_NEEDS_RESET 0x40u
#define ST_FAILED 0x80u

#define F_VERSION_1_HI 1u            /* feature bit 32 = bit 0 of the high word */
#define BLK_F_RO (1u << 5)
#define BLK_F_FLUSH (1u << 9)

#define VIRTIO_MAGIC 0x74726976u     /* "virt" */
#define DEV_BLOCK 2u

#define QN 8u                        /* queue size: power of two, <= QueueNumMax */
#define T_IN 0u
#define T_OUT 1u
#define T_FLUSH 4u
#define DESC_NEXT 1u
#define DESC_WRITE 2u
#define CHUNK_SECTORS 8u             /* one bounce page = 8 sectors per request */

struct desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
struct reqhdr { uint32_t type, reserved; uint64_t sector; };

static struct {
    int present;
    volatile uint8_t *regs;
    uint64_t sectors;
    int readonly, has_flush;
    uint32_t timeout_ms;
    uint16_t last_used, avail_idx;
    struct desc *desc;
    volatile uint16_t *avail;        /* flags, idx, ring[QN] */
    volatile uint16_t *used;         /* flags, idx, then {u32 id,u32 len}[QN] */
    struct reqhdr *hdr;
    volatile uint8_t *status;
    uint8_t *bounce;
    uintptr_t ring_page, req_page, bounce_page;
    int stall_next;
    int dropping;
    int drop_after;                  /* tests only: >= 0 => remaining writes before the simulated power cut */
    uint64_t writes;
    uint64_t requests, timeouts;
    int failed;
} D;
static struct blk_info INFO;

static uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(D.regs + off); }
static void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(D.regs + off) = v; }
static void mb(void) { __asm__ volatile ("dsb sy" ::: "memory"); }

const char *blk_strerror(int rc)
{
    switch (rc) {
    case BLK_OK: return "ok";
    case BLK_EARG: return "invalid argument";
    case BLK_ERANGE: return "out of range";
    case BLK_ENODEV: return "no device";
    case BLK_ERO: return "read-only device";
    case BLK_EIO: return "I/O error";
    case BLK_ETIMEOUT: return "timeout";
    case BLK_EPROTO: return "protocol/negotiation failure";
    default: return "unknown error";
    }
}

static uint64_t ms_to_ticks(uint32_t ms) { return timer_freq() / 1000u * ms; }

/* Bring the device at D.regs from reset to DRIVER_OK with an empty queue. */
static int device_setup(void)
{
    wr(R_STATUS, 0);                                            /* reset */
    for (uint64_t t0 = timer_count(); rd(R_STATUS) != 0;)       /* spec 4.2.2.1: reset is complete when Status reads 0 */
        if (timer_count() - t0 > ms_to_ticks(100)) return BLK_ETIMEOUT;
    wr(R_STATUS, ST_ACK);
    wr(R_STATUS, ST_ACK | ST_DRIVER);

    wr(R_DEVFEATSEL, 0); uint32_t lo = rd(R_DEVFEAT);
    wr(R_DEVFEATSEL, 1); uint32_t hi = rd(R_DEVFEAT);
    INFO.features_lo = lo; INFO.features_hi = hi;
    if (!(hi & F_VERSION_1_HI)) { wr(R_STATUS, ST_FAILED); return BLK_EPROTO; }
    D.readonly = !!(lo & BLK_F_RO);
    D.has_flush = !!(lo & BLK_F_FLUSH);
    wr(R_DRVFEATSEL, 0); wr(R_DRVFEAT, lo & (BLK_F_RO | BLK_F_FLUSH));   /* accept only what this driver understands */
    wr(R_DRVFEATSEL, 1); wr(R_DRVFEAT, F_VERSION_1_HI);
    wr(R_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
    if (!(rd(R_STATUS) & ST_FEATURES_OK)) { wr(R_STATUS, ST_FAILED); return BLK_EPROTO; }   /* device refused our subset */

    wr(R_QSEL, 0);
    if (rd(R_QREADY) != 0) { wr(R_STATUS, ST_FAILED); return BLK_EPROTO; }
    if (rd(R_QNUMMAX) < QN) { wr(R_STATUS, ST_FAILED); return BLK_EPROTO; }
    uint8_t *rp = (uint8_t *)D.ring_page;
    for (uint32_t i = 0; i < PAGE_SIZE; i++) rp[i] = 0;          /* clean idx, no stale used entries */
    D.desc = (struct desc *)rp;                                  /* 128 bytes */
    D.avail = (volatile uint16_t *)(rp + 128);                   /* 4 + 2*QN + 2 = 22 bytes */
    D.used = (volatile uint16_t *)(rp + 256);                    /* 4 + 8*QN + 2 = 70 bytes, 4-aligned */
    D.last_used = 0; D.avail_idx = 0;
    wr(R_QNUM, QN);
    wr(R_QDESCLO, (uint32_t)D.ring_page); wr(R_QDESCHI, (uint32_t)(D.ring_page >> 32));
    wr(R_QDRVLO, (uint32_t)(D.ring_page + 128)); wr(R_QDRVHI, (uint32_t)((D.ring_page + 128) >> 32));
    wr(R_QDEVLO, (uint32_t)(D.ring_page + 256)); wr(R_QDEVHI, (uint32_t)((D.ring_page + 256) >> 32));
    mb();
    wr(R_QREADY, 1);
    wr(R_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
    if (rd(R_STATUS) & (ST_NEEDS_RESET | ST_FAILED)) return BLK_EPROTO;

    uint32_t g1, g2; uint64_t cap;                               /* capacity: 64-bit config field, consistent read */
    do {
        g1 = rd(R_CFGGEN);
        cap = (uint64_t)rd(R_CONFIG) | ((uint64_t)rd(R_CONFIG + 4) << 32);
        g2 = rd(R_CFGGEN);
    } while (g1 != g2);
    D.sectors = cap;
    return BLK_OK;
}

int blk_init(const void *fdt)
{
    static struct fdt_virtio nodes[32];
    D.present = 0; D.failed = 0; D.drop_after = -1; D.dropping = 0;
    if (!fdt) return BLK_ENODEV;
    int n = fdt_virtio_mmio(fdt, nodes, 32);
    if (n <= 0) return BLK_ENODEV;
    INFO.nodes_seen = n;

    /* Map exactly the pages the device tree names, as Device memory, after declaring them to the MMU policy. */
    uint64_t lo = nodes[0].base, hi = nodes[0].base + nodes[0].size;
    for (int i = 1; i < n; i++) {
        if (nodes[i].base < lo) lo = nodes[i].base;
        if (nodes[i].base + nodes[i].size > hi) hi = nodes[i].base + nodes[i].size;
    }
    lo &= ~(PAGE_SIZE - 1); hi = (hi + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (hi - lo > 64 * PAGE_SIZE) return BLK_EPROTO;             /* implausible window: refuse */
    if (vmm_register_mmio(lo, hi - lo) != VMM_OK || vmm_map(lo, lo, hi - lo, VMM_ATTR_DEVICE) != VMM_OK) return BLK_EPROTO;

    int found = -1, legacy = 0;
    for (int i = 0; i < n; i++) {
        volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)nodes[i].base;
        if (r[R_MAGIC / 4] != VIRTIO_MAGIC) continue;
        if (r[R_DEVID / 4] != DEV_BLOCK) continue;
        INFO.nodes_blk++;
        if (r[R_VERSION / 4] != 2) { legacy = 1; continue; }
        if (found < 0) found = i;
    }
    if (found < 0) {
        if (legacy) {
            console_puts("[blk] a virtio block device uses the LEGACY transport; only modern (version 2) is supported: add -global virtio-mmio.force-legacy=false\n");
            return BLK_EPROTO;
        }
        return BLK_ENODEV;
    }
    D.regs = (volatile uint8_t *)(uintptr_t)nodes[found].base;
    INFO.base = nodes[found].base; INFO.intid = nodes[found].intid; INFO.version = rd(R_VERSION);
    D.ring_page = pmm_alloc_page(); D.req_page = pmm_alloc_page(); D.bounce_page = pmm_alloc_page();
    if (!D.ring_page || !D.req_page || !D.bounce_page) return BLK_EPROTO;
    D.hdr = (struct reqhdr *)D.req_page; D.status = (volatile uint8_t *)(D.req_page + 64); D.bounce = (uint8_t *)D.bounce_page;
    D.timeout_ms = 1000;
    int rc = device_setup();
    if (rc) { D.failed = 1; return rc; }
    D.present = 1;
    return BLK_OK;
}

int blk_present(void) { return D.present && !D.failed; }
uint64_t blk_sectors(void) { return D.present ? D.sectors : 0; }
int blk_readonly(void) { return D.readonly; }
const struct blk_info *blk_get_info(void) { return &INFO; }
uint64_t blk_requests(void) { return D.requests; }
uint64_t blk_timeouts(void) { return D.timeouts; }
void blk_test_stall_next(int on) { D.stall_next = on; }
void blk_test_drop_writes_after(int n) { D.drop_after = n < 0 ? -1 : n; D.dropping = 0; }
uint64_t blk_writes_done(void) { return D.writes; }
void blk_test_set_timeout_ms(uint32_t ms) { D.timeout_ms = ms ? ms : 1000; }

/* Submit one request chain (header [+ data] + status) and wait, bounded, for completion. */
static int submit(uint32_t type, uint64_t sector, uint32_t data_len, int device_writes_data)
{
    D.hdr->type = type; D.hdr->reserved = 0; D.hdr->sector = sector;
    *D.status = 0xff;                                            /* a value the device never writes: proves it completed */
    uint32_t n = 0;
    D.desc[n].addr = (uint64_t)(uintptr_t)D.hdr; D.desc[n].len = sizeof *D.hdr; D.desc[n].flags = DESC_NEXT; D.desc[n].next = (uint16_t)(n + 1); n++;
    if (data_len) {
        D.desc[n].addr = (uint64_t)(uintptr_t)D.bounce; D.desc[n].len = data_len;
        D.desc[n].flags = (uint16_t)(DESC_NEXT | (device_writes_data ? DESC_WRITE : 0)); D.desc[n].next = (uint16_t)(n + 1); n++;
    }
    D.desc[n].addr = (uint64_t)(uintptr_t)D.status; D.desc[n].len = 1; D.desc[n].flags = DESC_WRITE; D.desc[n].next = 0;

    D.avail[2 + (D.avail_idx % QN)] = 0;                         /* head descriptor index */
    mb();                                                        /* descriptors and ring entry visible before the index */
    D.avail_idx++;
    D.avail[1] = D.avail_idx;
    mb();
    D.requests++;
    if (D.stall_next) D.stall_next = 0;                          /* tests only: never ring the doorbell */
    else wr(R_QNOTIFY, 0);

    uint64_t t0 = timer_count(), limit = ms_to_ticks(D.timeout_ms);
    for (;;) {
        mb();
        if (D.used[1] != D.last_used) break;
        if (rd(R_STATUS) & (ST_NEEDS_RESET | ST_FAILED)) { D.failed = 1; return BLK_EIO; }
        if (timer_count() - t0 > limit) {
            D.timeouts++;
            if (device_setup() != BLK_OK) { D.failed = 1; D.present = 0; }   /* reset and re-initialise; request abandoned */
            return BLK_ETIMEOUT;
        }
    }
    uint32_t idx = D.last_used % QN;
    uint32_t id = *(volatile uint32_t *)((volatile uint8_t *)D.used + 4 + idx * 8);
    D.last_used++;
    uint32_t isr = rd(R_INTSTATUS);
    if (isr) wr(R_INTACK, isr);                                  /* keep the line quiet; we poll */
    if (id != 0) { D.failed = 1; return BLK_EPROTO; }            /* the only chain ever submitted starts at descriptor 0 */
    return *D.status == 0 ? BLK_OK : BLK_EIO;
}

static int check_range(uint64_t sector, uint32_t nsect, const void *buf)
{
    if (!blk_present()) return BLK_ENODEV;
    if (!buf || nsect == 0) return BLK_EARG;
    if (sector > D.sectors || nsect > D.sectors - sector) return BLK_ERANGE;   /* overflow-safe */
    return BLK_OK;
}

int blk_read(uint64_t sector, void *buf, uint32_t nsect)
{
    int rc = check_range(sector, nsect, buf);
    if (rc) return rc;
    uint8_t *out = (uint8_t *)buf;
    while (nsect) {
        uint32_t c = nsect > CHUNK_SECTORS ? CHUNK_SECTORS : nsect;
        for (uint32_t i = 0; i < c * BLK_SECTOR; i++) D.bounce[i] = 0xA5;   /* a failed read never leaves old data looking valid */
        rc = submit(T_IN, sector, c * BLK_SECTOR, 1);
        if (rc) return rc;
        mb();
        for (uint32_t i = 0; i < c * BLK_SECTOR; i++) out[i] = D.bounce[i];
        out += c * BLK_SECTOR; sector += c; nsect -= c;
    }
    return BLK_OK;
}

int blk_write(uint64_t sector, const void *buf, uint32_t nsect)
{
    int rc = check_range(sector, nsect, buf);
    if (rc) return rc;
    if (D.readonly) return BLK_ERO;
    const uint8_t *in = (const uint8_t *)buf;
    while (nsect) {
        uint32_t c = nsect > CHUNK_SECTORS ? CHUNK_SECTORS : nsect;
        if (D.drop_after >= 0 && (D.dropping || D.drop_after == 0)) { D.dropping = 1; in += c * BLK_SECTOR; sector += c; nsect -= c; continue; }   /* power cut: lost */
        if (D.drop_after > 0) D.drop_after--;
        for (uint32_t i = 0; i < c * BLK_SECTOR; i++) D.bounce[i] = in[i];
        D.writes++;
        rc = submit(T_OUT, sector, c * BLK_SECTOR, 0);
        if (rc) return rc;
        in += c * BLK_SECTOR; sector += c; nsect -= c;
    }
    return BLK_OK;
}

int blk_flush(void)
{
    if (!blk_present()) return BLK_ENODEV;
    if (D.dropping) return BLK_OK;                              /* tests only: after the simulated power cut nothing reaches the device */
    if (D.readonly || !D.has_flush) return BLK_OK;               /* nothing cached to flush */
    return submit(T_FLUSH, 0, 0, 0);
}
