/*
 * kernel/drivers/virtio_input.c - virtio input devices (keyboard, tablet) over the modern virtio-mmio transport.
 * Original code written from the OASIS VIRTIO 1.x specification: 2.1 status, 2.7 split virtqueues, 4.2 MMIO transport,
 * 5.8 input device (device ID 18, queue 0 = eventq, queue 1 = statusq).
 *
 * Design (G5-C):
 *  - Every virtio-mmio slot named by the device tree is probed by reading its registers; nothing assumes which slot is which.
 *  - The eventq is filled once with QN device-writable 8-byte event buffers {le16 type, le16 code, le32 value}. After the driver
 *    consumes a completed buffer it re-posts the same buffer. The statusq (LED feedback) is not used and left disabled.
 *  - Completion is POLLED by vinput_poll() (bounded work, never waits); the caller decides when to call it.
 *  - All device data is untrusted: used-ring ids are range- and ownership-checked, lengths must be exactly one event, and a
 *    device that violates the ring protocol is reset and never trusted again.
 *  - Only VIRTIO_F_VERSION_1 is negotiated. Legacy transports are skipped and reported.
 */
#include <arch.h>
#include <console.h>
#include <input.h>
#include <mm.h>
#include <virtio_mmio.h>

#define QN 16u                                   /* queue size, power of two, must be <= QueueNumMax */
#define EVT_SIZE 8u
#define CFG_EV_BITS 0x11u
#define CFG_ABS_INFO 0x12u
#define DESC_WRITE 2u

struct desc { uint64_t addr; uint32_t len; uint16_t flags, next; };

struct dev {
    volatile uint8_t *regs;
    int up, keyboard, pointer;
    uint16_t last_used, avail_idx;
    uint32_t posted;                             /* bitmask of buffers currently owned by the device */
    uintptr_t page;
    struct desc *desc;
    volatile uint16_t *avail, *used;
    volatile uint8_t *evt;
};

static struct dev DEV[VINPUT_MAX_DEVICES];
static struct vinput_info INFO;
static int inited;

static uint32_t rd(const struct dev *d, uint32_t off) { return *(volatile uint32_t *)(d->regs + off); }
static void wr(const struct dev *d, uint32_t off, uint32_t v) { *(volatile uint32_t *)(d->regs + off) = v; }
static void mb(void) { __asm__ volatile ("dsb sy" ::: "memory"); }

/* Select a config item and return its size byte (0 = not available). Config bytes are then read with cfg8(). */
static uint32_t cfg_select(const struct dev *d, uint8_t sel, uint8_t sub)
{
    volatile uint8_t *c = d->regs + VIO_R_CONFIG;
    c[0] = sel; c[1] = sub; mb();
    return c[2];
}
static uint8_t cfg8(const struct dev *d, uint32_t i) { return d->regs[VIO_R_CONFIG + 8 + i]; }

static void post(struct dev *d, uint32_t i)
{
    d->desc[i].addr = (uint64_t)(uintptr_t)(d->evt + i * EVT_SIZE);
    d->desc[i].len = EVT_SIZE; d->desc[i].flags = DESC_WRITE; d->desc[i].next = 0;
    for (uint32_t k = 0; k < EVT_SIZE; k++) d->evt[i * EVT_SIZE + k] = 0;
    d->avail[2 + (d->avail_idx % QN)] = (uint16_t)i;
    mb();
    d->avail_idx++;
    d->avail[1] = d->avail_idx;
    d->posted |= 1u << i;
}

static void kill_dev(struct dev *d)
{
    if (d->regs) wr(d, VIO_R_STATUS, 0);         /* reset first so the device stops using memory we free */
    if (d->page) pmm_free_page(d->page);
    d->page = 0; d->up = 0; d->keyboard = d->pointer = 0;
}

static int32_t le32(uint32_t b0, uint32_t b1, uint32_t b2, uint32_t b3) { return (int32_t)(b0 | b1 << 8 | b2 << 16 | b3 << 24); }

static int bring_up(struct dev *d, struct input_state *s)
{
    wr(d, VIO_R_STATUS, 0);
    for (uint32_t spin = 0; rd(d, VIO_R_STATUS) != 0; ) if (++spin > 1000000u) return -1;
    wr(d, VIO_R_STATUS, VIO_ST_ACK);
    wr(d, VIO_R_STATUS, VIO_ST_ACK | VIO_ST_DRIVER);
    wr(d, VIO_R_DEVFEATSEL, 1);
    if (!(rd(d, VIO_R_DEVFEAT) & VIO_F_VERSION_1_HI)) { wr(d, VIO_R_STATUS, VIO_ST_FAILED); return -1; }
    wr(d, VIO_R_DRVFEATSEL, 0); wr(d, VIO_R_DRVFEAT, 0);
    wr(d, VIO_R_DRVFEATSEL, 1); wr(d, VIO_R_DRVFEAT, VIO_F_VERSION_1_HI);
    wr(d, VIO_R_STATUS, VIO_ST_ACK | VIO_ST_DRIVER | VIO_ST_FEATURES_OK);
    if (!(rd(d, VIO_R_STATUS) & VIO_ST_FEATURES_OK)) { wr(d, VIO_R_STATUS, VIO_ST_FAILED); return -1; }

    /* Capabilities (spec 5.8.5): a key bitmap with KEY_A set means keyboard; absolute X and Y axes mean a pointer. */
    uint32_t n = cfg_select(d, CFG_EV_BITS, EV_KEY);
    if (n > 0 && n <= 128 && (30u >> 3) < n && (cfg8(d, 30u >> 3) & (1u << (30u & 7)))) d->keyboard = 1;
    if (cfg_select(d, CFG_ABS_INFO, ABS_X) >= 8 && cfg_select(d, CFG_ABS_INFO, ABS_Y) >= 8) {
        int32_t mn[2], mx[2];
        for (uint32_t a = 0; a < 2; a++) {
            if (cfg_select(d, CFG_ABS_INFO, (uint8_t)a) < 8) return -1;
            mn[a] = le32(cfg8(d, 0), cfg8(d, 1), cfg8(d, 2), cfg8(d, 3));
            mx[a] = le32(cfg8(d, 4), cfg8(d, 5), cfg8(d, 6), cfg8(d, 7));
        }
        if (input_set_axis(s, 0, mn[0], mx[0]) == 0 && input_set_axis(s, 1, mn[1], mx[1]) == 0) d->pointer = 1;
    }

    wr(d, VIO_R_QSEL, 0);
    if (rd(d, VIO_R_QREADY) != 0 || rd(d, VIO_R_QNUMMAX) < QN) { wr(d, VIO_R_STATUS, VIO_ST_FAILED); return -1; }
    uint8_t *p = (uint8_t *)d->page;             /* pmm_alloc_page returns a zeroed page: clean indices, no stale entries */
    d->desc = (struct desc *)p;                  /* 16*16 = 256 bytes */
    d->avail = (volatile uint16_t *)(p + 256);   /* 4 + 2*16 + 2 */
    d->used = (volatile uint16_t *)(p + 512);    /* 4 + 8*16 + 2 */
    d->evt = p + 1024;                           /* 16 * 8 bytes */
    d->last_used = 0; d->avail_idx = 0; d->posted = 0;
    for (uint32_t i = 0; i < QN; i++) post(d, i);
    wr(d, VIO_R_QNUM, QN);
    wr(d, VIO_R_QDESCLO, (uint32_t)d->page); wr(d, VIO_R_QDESCHI, (uint32_t)(d->page >> 32));
    wr(d, VIO_R_QDRVLO, (uint32_t)(d->page + 256)); wr(d, VIO_R_QDRVHI, (uint32_t)((d->page + 256) >> 32));
    wr(d, VIO_R_QDEVLO, (uint32_t)(d->page + 512)); wr(d, VIO_R_QDEVHI, (uint32_t)((d->page + 512) >> 32));
    mb();
    wr(d, VIO_R_QREADY, 1);
    wr(d, VIO_R_STATUS, VIO_ST_ACK | VIO_ST_DRIVER | VIO_ST_FEATURES_OK | VIO_ST_DRIVER_OK);
    if (rd(d, VIO_R_STATUS) & (VIO_ST_NEEDS_RESET | VIO_ST_FAILED)) return -1;
    wr(d, VIO_R_QNOTIFY, 0);                     /* tell the device buffers are available */
    return 0;
}

int vinput_init(const void *fdt, struct input_state *s)
{
    static struct fdt_virtio nodes[32];
    if (inited) return INFO.devices;
    inited = 1;
    int n = virtio_map_window(fdt, nodes, 32);
    if (n < 0) return -1;
    for (int i = 0; i < n && INFO.devices < VINPUT_MAX_DEVICES; i++) {
        volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)nodes[i].base;
        if (r[VIO_R_MAGIC / 4] != VIO_MAGIC || r[VIO_R_DEVID / 4] != VIO_DEV_INPUT) continue;
        if (r[VIO_R_VERSION / 4] != 2) { console_puts("[input] a virtio input device uses the LEGACY transport; add -global virtio-mmio.force-legacy=false\n"); INFO.failed++; continue; }
        struct dev *d = &DEV[INFO.devices];
        d->regs = (volatile uint8_t *)(uintptr_t)nodes[i].base;
        d->page = pmm_alloc_page();
        if (!d->page) { INFO.failed++; d->regs = 0; continue; }
        if (bring_up(d, s)) { kill_dev(d); d->regs = 0; INFO.failed++; continue; }
        d->up = 1;
        INFO.devices++; INFO.keyboards += d->keyboard; INFO.pointers += d->pointer;
    }
    return INFO.devices;
}

int vinput_poll(struct input_state *s)
{
    int done = 0;
    for (int di = 0; di < INFO.devices; di++) {
        struct dev *d = &DEV[di];
        if (!d->up) continue;
        mb();
        for (uint32_t budget = QN; budget && d->used[1] != d->last_used; budget--) {
            uint32_t slot = d->last_used % QN;
            volatile uint8_t *ue = (volatile uint8_t *)d->used + 4 + slot * 8;
            uint32_t id = *(volatile uint32_t *)ue, len = *(volatile uint32_t *)(ue + 4);
            d->last_used++;
            if (id >= QN || !(d->posted & (1u << id))) {   /* not a buffer we handed out: the device broke the protocol */
                INFO.bad_used++; INFO.failed++; kill_dev(d); input_reset(s);
                break;
            }
            d->posted &= ~(1u << id);
            if (len == EVT_SIZE) {
                volatile uint8_t *e = d->evt + id * EVT_SIZE;
                uint16_t type = (uint16_t)(e[0] | e[1] << 8), code = (uint16_t)(e[2] | e[3] << 8);
                input_feed(s, type, code, le32(e[4], e[5], e[6], e[7]));
                done++; INFO.polled_events++;
            } else {
                s->st.ignored++;                           /* wrong length: discard, but still recycle the buffer */
            }
            post(d, id);
            wr(d, VIO_R_QNOTIFY, 0);
        }
        uint32_t isr = rd(d, VIO_R_INTSTATUS);
        if (isr) wr(d, VIO_R_INTACK, isr);
    }
    return done;
}

const struct vinput_info *vinput_get_info(void) { return &INFO; }
