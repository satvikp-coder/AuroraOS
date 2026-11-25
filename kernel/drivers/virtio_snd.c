/*
 * kernel/drivers/virtio_snd.c - virtio sound device (ID 25) over the modern virtio-mmio transport: DEVICE BRING-UP + PCM PLAYBACK.
 * Original code written from the OASIS VIRTIO 1.2 specification (2.1 status, 2.7 split virtqueues, 3.1.1 initialisation, 4.2 MMIO,
 * 5.14 sound device; extract in docs/kernel/virtio-sound-spec-notes.md). Wire layouts and hostile-input decoders: virtio_snd.h.
 *
 * Scope: discover, reset, negotiate VIRTIO_F_VERSION_1 only, set up controlq/eventq/txq/rxq (shared virtqueue.c bookkeeping), DRIVER_OK,
 * read config (jacks, streams, chmaps) with a generation-consistent read, then ONE polled PCM_INFO request for all streams.
 * PCM playback: SET_PARAMS / START / STOP on the control queue; sine-wave tone generation submitted on the TX queue.
 * Failure policy: every wait is bounded by the ARM generic timer; any protocol violation, timeout or refusal sets FAILED, resets the
 * device, frees its pages only once the reset is confirmed, and returns an error. Boot always continues.
 *
 * AURORA_SND_FAULT=<n> (test builds only, tests/qemu/sound-tests.sh) SIMULATES a hostile device inside the driver, after the real QEMU
 * device answered: 1 never ring the doorbell (timeout), 2 non-zero padding in PCM_INFO record 0, 3 used length one byte short,
 * 4 config reports VSND_MAX_STREAMS+1 streams, 5 unknown status code in the response header.
 */
#include <arch.h>
#include <console.h>
#include <mm.h>
#include <timer.h>
#include <virtio_mmio.h>
#include <virtio_snd.h>
#include <virtqueue.h>
#include <vsnd.h>
#include <proc.h>

/* Fixed-point sine approximation (Q16.16 format) using Bhaskara I's formula.
 * sin(x) ≈ 16x(π - x) / (5π² - 4x(π - x)) for x in [0, π], extended to full period.
 * Input: angle in Q16.16 radians. Output: Q16.16 sine value (-1.0 to 1.0).
 * Uses 64-bit intermediate to avoid overflow. */
static inline int32_t vsnd_sin_fp(int32_t x)
{
    /* Q16.16 constants */
    const int32_t PI = 0x3243F;        /* π ≈ 3.14159 * 65536 */
    const int32_t TWO_PI = 0x6487E;    /* 2π ≈ 6.28318 * 65536 */
    const int32_t PI_HALF = 0x1921F;   /* π/2 ≈ 1.5708 * 65536 */

    /* Normalize to [0, 2π) */
    if (x < 0) x += TWO_PI;
    if (x >= TWO_PI) x -= TWO_PI;

    /* Use symmetry: sin(x) = sin(π - x) for x in [π/2, π], and negative for [π, 2π] */
    int neg = 0;
    if (x > PI) { x -= PI; neg = 1; }
    if (x > PI_HALF) x = PI - x;

    /* Bhaskara I: sin(x) ≈ 16x(π - x) / (5π² - 4x(π - x)) */
    int64_t x_ll = x;
    int64_t pi_ll = PI;
    int64_t pi_minus_x = pi_ll - x_ll;
    int64_t num = 16 * x_ll * pi_minus_x;
    int64_t den = 5 * pi_ll * pi_ll - 4 * x_ll * pi_minus_x;
    int64_t result = (num << 16) / den; /* Q16.16 result */

    return neg ? -(int32_t)result : (int32_t)result;
}

#define NQ 4u
#define QN_WANT 8u                       /* preferred queue size; reduced to a power of two <= QueueNumMax */
#define DESC_NEXT 1u
#define DESC_WRITE 2u
#define OFF_AVAIL 128u                   /* per-queue page: desc[8] at 0 (128 B), avail at 128 (<= 22 B), used at 256 (<= 70 B) */
#define OFF_USED 256u
#define OFF_EVT 1024u                    /* eventq page only: QN event buffers of 8 bytes */
#define OFF_REQ 0u                       /* control page: request at 0, device-writable response at 64 */
#define OFF_RESP 64u
#define RESET_MS 100u
#define CTRL_MS 1000u

struct desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
struct sq { uintptr_t page; uint16_t qn; struct desc *desc; volatile uint16_t *avail, *used; struct vq vq; };

static volatile uint8_t *regs;
static struct sq Q[NQ];
static uintptr_t ctrl_page;
static struct vsnd_config cfg;
static struct vsnd_pcm_info info[VSND_MAX_STREAMS];
static int inited, live, last_rc;

/* ---- Audio capability grants (bounded, per-principal) ---- */
#define AUDIO_MAX_GRANTS 8

static struct {
    uint32_t principal;
    uint8_t  caps;      /* bitmask of AUDIO_CAP_* */
    uint8_t  used;
} audio_grants[AUDIO_MAX_GRANTS];

/* Grant audio capabilities to a principal. Kernel code only.
 * Returns 0 on success, -1 if principal is invalid (kernel, none, reserved) or table full. */
int vsnd_grant_audio(uint32_t principal, uint8_t caps)
{
    if (!principal || principal == 0xFFFFFFFFu) return -1;  /* kernel or none */
    if ((principal & 0xFF000000u) == 0x7F000000u) return -1; /* reserved range */
    if (!(caps & (AUDIO_CAP_PLAY | AUDIO_CAP_RECORD))) return -1; /* no valid caps */

    for (int i = 0; i < AUDIO_MAX_GRANTS; i++) {
        if (audio_grants[i].used && audio_grants[i].principal == principal) {
            audio_grants[i].caps |= caps;  /* add to existing grant */
            return 0;
        }
    }
    for (int i = 0; i < AUDIO_MAX_GRANTS; i++) {
        if (!audio_grants[i].used) {
            audio_grants[i].used = 1;
            audio_grants[i].principal = principal;
            audio_grants[i].caps = caps;
            return 0;
        }
    }
    return -1;  /* table full */
}

/* Check if a principal has the required audio capability.
 * Returns 1 if allowed, 0 if denied.
 * Kernel context (PRINCIPAL_NONE) is treated as the shell principal (PRINCIPAL_SHELL)
 * for capability checks, since the shell runs in kernel mode. */
static int vsnd_check_audio_cap(uint32_t principal, uint8_t cap)
{
    /* Kernel context: use shell principal for capability checks */
    if (principal == PRINCIPAL_NONE) {
        principal = PRINCIPAL_SHELL;
    }
    for (int i = 0; i < AUDIO_MAX_GRANTS; i++) {
        if (audio_grants[i].used && audio_grants[i].principal == principal) {
            return (audio_grants[i].caps & cap) != 0;
        }
    }
    return 0;
}

/* Revoke all audio capabilities for a principal (used when process exits).
 * Returns number of grants revoked. */
int vsnd_revoke_audio(uint32_t principal)
{
    int n = 0;
    for (int i = 0; i < AUDIO_MAX_GRANTS; i++) {
        if (audio_grants[i].used && audio_grants[i].principal == principal) {
            audio_grants[i].used = 0;
            audio_grants[i].principal = 0;
            audio_grants[i].caps = 0;
            n++;
        }
    }
    return n;
}

static uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(regs + off); }
static void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(regs + off) = v; }
static void mb(void) { __asm__ volatile ("dsb sy" ::: "memory"); }
static uint64_t ms_ticks(uint32_t ms) { return timer_freq() / 1000u * ms; }

static void puthex(uint64_t v)
{
    char b[17]; int n = 0;
    do { uint32_t d = (uint32_t)(v & 0xf); b[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; } while (v && n < 16);
    console_puts("0x");
    while (n) console_putc(b[--n]);
}

/* Reset and wait, bounded, for Status to read 0. 1 = reset confirmed. */
static int reset_dev(void)
{
    wr(VIO_R_STATUS, 0);
    for (uint64_t t0 = timer_count(); rd(VIO_R_STATUS) != 0;)
        if (timer_count() - t0 > ms_ticks(RESET_MS)) return 0;
    return 1;
}

static void free_pages(void)
{
    for (uint32_t i = 0; i < NQ; i++) { if (Q[i].page) pmm_free_page(Q[i].page); Q[i].page = 0; }
    if (ctrl_page) pmm_free_page(ctrl_page);
    ctrl_page = 0;
}

/* Give up on the device: FAILED, reset, and free its memory only if the device provably stopped (otherwise the pages are leaked on purpose). */
static int fail(int rc, const char *why)
{
    wr(VIO_R_STATUS, rd(VIO_R_STATUS) | VIO_ST_FAILED);
    int stopped = reset_dev();
    if (stopped) free_pages();
    live = 0;
    console_puts("[snd] virtio-sound: device failed: "); console_puts(why);
    console_puts(stopped ? "; device reset" : "; reset not confirmed, queue memory kept");
    console_puts(", boot continues without sound\n");
    return rc;
}

static int setup_queue(uint32_t idx)
{
    struct sq *q = &Q[idx];
    wr(VIO_R_QSEL, idx);
    if (rd(VIO_R_QREADY) != 0) return SND_EPROTO;
    uint32_t max = rd(VIO_R_QNUMMAX), qn = QN_WANT;
    while (qn > max) qn >>= 1;
    if (qn < 2) return SND_EPROTO;                                /* queue not available (0) or too small to hold a request chain */
    uint8_t *p = (uint8_t *)q->page;                              /* zeroed by pmm_alloc_page: clean indices, no stale used entries */
    q->qn = (uint16_t)qn;
    q->desc = (struct desc *)p;
    q->avail = (volatile uint16_t *)(p + OFF_AVAIL);
    q->used = (volatile uint16_t *)(p + OFF_USED);
    if (vq_init(&q->vq, q->qn, q->avail, q->used, mb) != VQ_OK) return SND_EPROTO;
    if (idx == VIRTIO_SND_VQ_EVENT) {                             /* 5.14.5.1: populate the event queue with device-writable buffers >= 8 bytes */
        for (uint32_t i = 0; i < qn; i++) {
            q->desc[i].addr = (uint64_t)(q->page + OFF_EVT + i * VSND_EVENT_SIZE);
            q->desc[i].len = VSND_EVENT_SIZE; q->desc[i].flags = DESC_WRITE; q->desc[i].next = 0;
            if (vq_submit(&q->vq, (uint16_t)i, VSND_EVENT_SIZE) != VQ_OK) return SND_EPROTO;
        }
    }
    wr(VIO_R_QNUM, qn);
    wr(VIO_R_QDESCLO, (uint32_t)q->page); wr(VIO_R_QDESCHI, (uint32_t)((uint64_t)q->page >> 32));
    wr(VIO_R_QDRVLO, (uint32_t)(q->page + OFF_AVAIL)); wr(VIO_R_QDRVHI, (uint32_t)((uint64_t)(q->page + OFF_AVAIL) >> 32));
    wr(VIO_R_QDEVLO, (uint32_t)(q->page + OFF_USED)); wr(VIO_R_QDEVHI, (uint32_t)((uint64_t)(q->page + OFF_USED) >> 32));
    mb();
    wr(VIO_R_QREADY, 1);
    return SND_OK;
}

/* Config space, generation-consistent (bounded retries). Decoded and bounded by vsnd_dec_config. */
static int read_config(void)
{
    uint8_t b[VSND_CFG_SIZE];
    for (int tries = 0; tries < 16; tries++) {
        uint32_t g1 = rd(VIO_R_CFGGEN);
        for (uint32_t o = 0; o < VSND_CFG_SIZE; o += 4) vsnd_put_le32(b + o, rd(VIO_R_CONFIG + o));
        uint32_t g2 = rd(VIO_R_CFGGEN);
        if (g1 != g2) continue;
#if defined(AURORA_SND_FAULT) && AURORA_SND_FAULT == 4
        vsnd_put_le32(b + VSND_CFG_OFF_STREAMS, VSND_MAX_STREAMS + 1u);   /* LEGACY: a device claiming too many streams */
#endif
        return vsnd_dec_config(b, sizeof b, &cfg) == VSND_OK ? SND_OK : SND_EPROTO;
    }
    return SND_EPROTO;                                            /* config never stable */
}

static const char *vsnd_why(int r)
{
    switch (r) {
    case VSND_EBADMSG: return "PCM_INFO answered BAD_MSG";
    case VSND_ENOTSUPP: return "PCM_INFO answered NOT_SUPP";
    case VSND_EIO: return "PCM_INFO answered IO_ERR";
    default: return "malformed PCM_INFO response";
    }
}

/* One control request (request R, response W) on controlq, polled with a hard time bound. *used_len is validated against resp_len by vq_complete. */
static int control(uint32_t req_len, uint32_t resp_len, uint32_t *used_len)
{
    struct sq *q = &Q[VIRTIO_SND_VQ_CONTROL];
    q->desc[0].addr = (uint64_t)(ctrl_page + OFF_REQ); q->desc[0].len = req_len; q->desc[0].flags = DESC_NEXT; q->desc[0].next = 1;
    q->desc[1].addr = (uint64_t)(ctrl_page + OFF_RESP); q->desc[1].len = resp_len; q->desc[1].flags = DESC_WRITE; q->desc[1].next = 0;
    if (vq_submit(&q->vq, 0, resp_len) != VQ_OK) return SND_EPROTO;
    /* Fault 1 (legacy AURORA_SND_FAULT=1): withhold the doorbell so the wait sees no completion. */
#if defined(AURORA_SND_FAULT) && AURORA_SND_FAULT == 1
    /* LEGACY: never ring the doorbell */
#else
    wr(VIO_R_QNOTIFY, VIRTIO_SND_VQ_CONTROL);
#endif
    uint64_t t0 = timer_count(), limit = ms_ticks(CTRL_MS);
    for (;;) {
        uint16_t id; uint32_t len;
        int r = vq_complete(&q->vq, &id, &len);
        if (r == VQ_OK) {
            if (id != 0) return SND_EPROTO;
            *used_len = len;
            break;
        }
        if (r != VQ_EEMPTY) return SND_EPROTO;                    /* bad id, length over resp_len, impossible used index */
        if (rd(VIO_R_STATUS) & (VIO_ST_NEEDS_RESET | VIO_ST_FAILED)) return SND_EPROTO;
        if (timer_count() - t0 > limit) return SND_ETIMEOUT;
    }
    mb();
    uint32_t isr = rd(VIO_R_INTSTATUS);
    if (isr) wr(VIO_R_INTACK, isr);                               /* polled: keep the line quiet */
    return SND_OK;
}

static int bring_up(uint32_t *resp_used)
{
    if (!reset_dev()) return fail(SND_ETIMEOUT, "reset did not complete");
    wr(VIO_R_STATUS, VIO_ST_ACK);
    wr(VIO_R_STATUS, VIO_ST_ACK | VIO_ST_DRIVER);
    wr(VIO_R_DEVFEATSEL, 1);
    if (!(rd(VIO_R_DEVFEAT) & VIO_F_VERSION_1_HI)) return fail(SND_EPROTO, "device does not offer VIRTIO_F_VERSION_1");
    wr(VIO_R_DRVFEATSEL, 0); wr(VIO_R_DRVFEAT, 0);               /* v1.2 defines no sound feature bits; accept none */
    wr(VIO_R_DRVFEATSEL, 1); wr(VIO_R_DRVFEAT, VIO_F_VERSION_1_HI);
    wr(VIO_R_STATUS, VIO_ST_ACK | VIO_ST_DRIVER | VIO_ST_FEATURES_OK);
    if (!(rd(VIO_R_STATUS) & VIO_ST_FEATURES_OK)) return fail(SND_EPROTO, "device refused VERSION_1-only features");

    for (uint32_t i = 0; i < NQ; i++) if (!(Q[i].page = pmm_alloc_page())) return fail(SND_ENOMEM, "out of memory");
    if (!(ctrl_page = pmm_alloc_page())) return fail(SND_ENOMEM, "out of memory");
    for (uint32_t i = 0; i < NQ; i++) if (setup_queue(i) != SND_OK) return fail(SND_EPROTO, "queue unavailable or already live");
    if (read_config() != SND_OK) return fail(SND_EPROTO, "config counts unstable or above driver bounds");

    wr(VIO_R_STATUS, VIO_ST_ACK | VIO_ST_DRIVER | VIO_ST_FEATURES_OK | VIO_ST_DRIVER_OK);
    if (rd(VIO_R_STATUS) & (VIO_ST_NEEDS_RESET | VIO_ST_FAILED)) return fail(SND_EPROTO, "device signalled failure at DRIVER_OK");
    wr(VIO_R_QNOTIFY, VIRTIO_SND_VQ_EVENT);                       /* event buffers are available */
    live = 1;
    *resp_used = 0;
    if (cfg.streams == 0) return SND_OK;

    uint8_t *req = (uint8_t *)(ctrl_page + OFF_REQ), *resp = (uint8_t *)(ctrl_page + OFF_RESP);
    size_t rlen = vsnd_query_resp_bytes(cfg.streams, VSND_PINFO_SIZE);
    if (!rlen || OFF_RESP + rlen > PAGE_SIZE) return fail(SND_EPROTO, "PCM_INFO response does not fit");
    if (vsnd_enc_query_info(req, VSND_QINFO_SIZE, VIRTIO_SND_R_PCM_INFO, 0, cfg.streams, VSND_PINFO_SIZE, cfg.streams) != VSND_QINFO_SIZE)
        return fail(SND_EPROTO, "could not encode PCM_INFO");
    for (size_t i = 0; i < rlen; i++) resp[i] = 0xff;             /* nothing the device did not write can look valid */
    mb();
    uint32_t used = 0;
    int rc = control(VSND_QINFO_SIZE, (uint32_t)rlen, &used);
    if (rc == SND_ETIMEOUT) return fail(rc, "PCM_INFO timed out");
    if (rc != SND_OK) return fail(rc, "control queue protocol violation");

    /* Legacy compile-time faults (AURORA_SND_FAULT=2,3,5) */
#if defined(AURORA_SND_FAULT) && AURORA_SND_FAULT == 2
    resp[VSND_HDR_SIZE + VSND_PINFO_OFF_PADDING] = 0x5a;          /* LEGACY: non-zero padding in record 0 */
#elif defined(AURORA_SND_FAULT) && AURORA_SND_FAULT == 3
    used -= 1;                                                    /* LEGACY: used length one byte short */
#elif defined(AURORA_SND_FAULT) && AURORA_SND_FAULT == 5
    vsnd_put_le32(resp + VSND_HDR_OFF_CODE, 0x1234u);             /* LEGACY: unknown status code */
#endif

    int d = vsnd_dec_pcm_info_resp(resp, used, cfg.streams, VSND_PINFO_SIZE, info);
    if (d != VSND_OK) return fail(SND_EPROTO, vsnd_why(d));
    *resp_used = used;
    return SND_OK;
}

static void report(uint32_t resp_used)
{
    console_puts("[snd] virtio-sound: "); console_dec(cfg.streams); console_puts(" streams (jacks "); console_dec(cfg.jacks);
    console_puts(", chmaps "); console_dec(cfg.chmaps); console_puts(")");
    for (uint32_t i = 0; i < cfg.streams; i++) {
        console_puts(i ? "; #" : ": #"); console_dec(i);
        console_puts(info[i].direction == VIRTIO_SND_D_OUTPUT ? " out " : " in ");
        console_dec(info[i].channels_min); console_puts("-"); console_dec(info[i].channels_max);
        console_puts("ch formats "); puthex(info[i].formats); console_puts(" rates "); puthex(info[i].rates);
        console_puts(" features "); puthex(info[i].features);
    }
    console_puts("; PCM_INFO used length "); console_dec(resp_used); console_puts("\n");
}

int vsnd_init(const void *fdt)
{
    static struct fdt_virtio nodes[32];
    if (inited) return last_rc;
    inited = 1;
    int n = fdt ? virtio_map_window(fdt, nodes, 32) : 0;
    if (n < 0) { console_puts("[snd] no sound device (virtio-mmio window unusable)\n"); return last_rc = SND_ENODEV; }
    int found = -1, legacy = 0;
    for (int i = 0; i < n; i++) {
        volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)nodes[i].base;
        if (r[VIO_R_MAGIC / 4] != VIO_MAGIC || r[VIO_R_DEVID / 4] != VIRTIO_SND_DEVICE_ID) continue;
        if (r[VIO_R_VERSION / 4] != 2) { legacy = 1; continue; }
        found = i; break;
    }
    if (found < 0) {
        if (legacy) {
            console_puts("[snd] a virtio-sound device uses the LEGACY transport; only modern (version 2) is supported: add -global virtio-mmio.force-legacy=false\n");
            return last_rc = SND_ELEGACY;
        }
        console_puts("[snd] no sound device\n");
        return last_rc = SND_ENODEV;
    }
    regs = (volatile uint8_t *)(uintptr_t)nodes[found].base;
    uint32_t used = 0;
    int rc = bring_up(&used);
    if (rc == SND_OK) report(used);
    return last_rc = rc;
}

int vsnd_present(void) { return live; }
uint32_t vsnd_streams(void) { return live ? cfg.streams : 0; }

/* ---- PCM playback implementation ---- */

/* Validate stream is an output stream and PCM_INFO supports the requested format/rate/channels.
 * Encodes and submits SET_PARAMS via the control queue. */
int vsnd_set_params(uint32_t stream_id, uint32_t format, uint32_t rate, uint32_t channels,
                    uint32_t buffer_bytes, uint32_t period_bytes, uint32_t features)
{
    if (!live) return SND_ENODEV;

    /* Permission check: caller must have AUDIO_CAP_PLAY */
    uint32_t principal = proc_current_principal();
    if (!vsnd_check_audio_cap(principal, AUDIO_CAP_PLAY)) return VSND_EPERM;

    if (stream_id >= cfg.streams) return SND_EPROTO;
    if (info[stream_id].direction != VIRTIO_SND_D_OUTPUT) return SND_EPROTO;

    struct vsnd_pcm_params p = {
        .stream_id = stream_id,
        .buffer_bytes = buffer_bytes,
        .period_bytes = period_bytes,
        .features = features,
        .channels = (uint8_t)channels,
        .format = (uint8_t)format,
        .rate = (uint8_t)rate,
    };

    uint8_t *req = (uint8_t *)(ctrl_page + OFF_REQ);
    size_t written = vsnd_enc_pcm_set_params(req, VSND_SETP_SIZE, &p, &info[stream_id], cfg.streams);
    if (!written) return SND_EPROTO;

    uint32_t used = 0;
    int rc = control((uint32_t)written, VSND_HDR_SIZE, &used);
    if (rc != SND_OK) return rc;

    return vsnd_dec_resp_hdr((uint8_t *)(ctrl_page + OFF_RESP), used);
}

/* Submit START control command for the given output stream. */
int vsnd_start(uint32_t stream_id)
{
    if (!live) return SND_ENODEV;

    /* Permission check: caller must have AUDIO_CAP_PLAY */
    uint32_t principal = proc_current_principal();
    if (!vsnd_check_audio_cap(principal, AUDIO_CAP_PLAY)) return VSND_EPERM;

    if (stream_id >= cfg.streams) return SND_EPROTO;
    if (info[stream_id].direction != VIRTIO_SND_D_OUTPUT) return SND_EPROTO;

    uint8_t *req = (uint8_t *)(ctrl_page + OFF_REQ);
    size_t written = vsnd_enc_pcm_hdr(req, VSND_PCM_HDR_SIZE, VIRTIO_SND_R_PCM_START, stream_id, cfg.streams);
    if (!written) return SND_EPROTO;

    uint32_t used = 0;
    int rc = control((uint32_t)written, VSND_HDR_SIZE, &used);
    if (rc != SND_OK) return rc;

    return vsnd_dec_resp_hdr((uint8_t *)(ctrl_page + OFF_RESP), used);
}

/* Submit STOP control command for the given output stream. */
int vsnd_stop(uint32_t stream_id)
{
    if (!live) return SND_ENODEV;

    /* Permission check: caller must have AUDIO_CAP_PLAY */
    uint32_t principal = proc_current_principal();
    if (!vsnd_check_audio_cap(principal, AUDIO_CAP_PLAY)) return VSND_EPERM;

    if (stream_id >= cfg.streams) return SND_EPROTO;
    if (info[stream_id].direction != VIRTIO_SND_D_OUTPUT) return SND_EPROTO;

    uint8_t *req = (uint8_t *)(ctrl_page + OFF_REQ);
    size_t written = vsnd_enc_pcm_hdr(req, VSND_PCM_HDR_SIZE, VIRTIO_SND_R_PCM_STOP, stream_id, cfg.streams);
    if (!written) return SND_EPROTO;

    uint32_t used = 0;
    int rc = control((uint32_t)written, VSND_HDR_SIZE, &used);
    if (rc != SND_OK) return rc;

    return vsnd_dec_resp_hdr((uint8_t *)(ctrl_page + OFF_RESP), used);
}

/* Release a TX page after the doorbell may have been rung. If the device owns
 * the buffer, reset it first so it provably stops DMA before we free the page.
 * If reset is unconfirmed, leak the page (safe) rather than free memory the
 * device may still read. Mark live=0 so subsequent calls fail closed. */
static void release_tx_page(uintptr_t page, int device_owns_buf)
{
    if (device_owns_buf) {
        int stopped = reset_dev();
        if (stopped) pmm_free_page(page);
        /* else: leak on purpose - device may still be DMA-ing into this page */
        live = 0;
    } else {
        pmm_free_page(page);
    }
}

/* Generate a sine wave at the given frequency in a page and submit it on the TX queue for the given stream.
 * Waits for the completion (device writes status trailer). Duration is approximate based on buffer size.
 * Resilient to device failures: checks status, handles errors gracefully, always frees the page. */
int vsnd_play_tone(uint32_t stream_id, uint32_t freq_hz, uint32_t duration_ms)
{
    if (!live) return SND_ENODEV;

    /* Permission check: caller must have AUDIO_CAP_PLAY */
    uint32_t principal = proc_current_principal();
    if (!vsnd_check_audio_cap(principal, AUDIO_CAP_PLAY)) return VSND_EPERM;

    if (stream_id >= cfg.streams) return SND_EPROTO;
    if (info[stream_id].direction != VIRTIO_SND_D_OUTPUT) return SND_EPROTO;

    /* Allocate one page for the sine wave buffer (4 KB). */
    uintptr_t page = pmm_alloc_page();
    if (!page) return SND_ENOMEM;

    /* Generate sine wave at 48000 Hz, S16LE, 2 channels (interleaved L/R). */
    const uint32_t sample_rate = 48000;
    const uint32_t freq = freq_hz;
    const uint32_t channels = 2;
    const uint32_t bytes_per_sample = 2; /* S16 */
    const uint32_t frame_bytes = channels * bytes_per_sample;
    const uint32_t samples_per_page = PAGE_SIZE / frame_bytes;

    int16_t *buf = (int16_t *)page;
    const int32_t TWO_PI = 0x6487E;    /* 2π in Q16.16 */
    const int32_t step = (int32_t)(((int64_t)TWO_PI * freq) / sample_rate); /* angle increment per sample */
    int32_t angle = 0;
    for (uint32_t i = 0; i < samples_per_page; i++) {
        int32_t sin_fp = vsnd_sin_fp(angle);           /* Q16.16 [-1.0, 1.0] */
        int16_t sample = (int16_t)((sin_fp * 16384) >> 16); /* scale to ~50% amplitude */
        buf[i * 2] = sample;     /* Left */
        buf[i * 2 + 1] = sample; /* Right */
        angle += step;
        if (angle >= TWO_PI) angle -= TWO_PI;
    }

    /* Submit on TX queue (VIRTIO_SND_VQ_TX = 2). */
    struct sq *q = &Q[VIRTIO_SND_VQ_TX];

    /* Build descriptor chain: head=0 -> xfer header (device-read), data (device-read), status (device-write). */
    q->desc[0].addr = (uint64_t)(ctrl_page + OFF_REQ);  /* reuse ctrl_page for xfer header */
    q->desc[0].len = VSND_XFER_SIZE;
    q->desc[0].flags = DESC_NEXT;
    q->desc[0].next = 1;

    q->desc[1].addr = (uint64_t)page;
    q->desc[1].len = PAGE_SIZE;
    q->desc[1].flags = DESC_NEXT;
    q->desc[1].next = 2;

    q->desc[2].addr = (uint64_t)(ctrl_page + OFF_RESP); /* reuse ctrl_page for status */
    q->desc[2].len = VSND_PSTATUS_SIZE;
    q->desc[2].flags = DESC_WRITE;
    q->desc[2].next = 0;

    /* Encode xfer header. */
    uint8_t *xfer_hdr = (uint8_t *)(ctrl_page + OFF_REQ);
    if (!vsnd_enc_pcm_xfer(xfer_hdr, VSND_XFER_SIZE, stream_id, cfg.streams)) {
        pmm_free_page(page);
        return SND_EPROTO;
    }

    /* Clear status area. */
    uint8_t *status_area = (uint8_t *)(ctrl_page + OFF_RESP);
    for (uint32_t i = 0; i < VSND_PSTATUS_SIZE; i++) status_area[i] = 0xff;

    /* Submit the buffer to the device. If the doorbell is rung, the device owns
     * the page buffer until it completes; on timeout we must reset first. */
    int device_owns_buf = 0;
    /* Fault 1 (legacy AURORA_SND_FAULT=1): withhold the doorbell so the device never sees the buffer. */
#if defined(AURORA_SND_FAULT) && AURORA_SND_FAULT == 1
    /* LEGACY: never ring the doorbell */
#else
    mb();
    if (vq_submit(&q->vq, 0, VSND_PSTATUS_SIZE) != VQ_OK) {
        pmm_free_page(page);
        return SND_EPROTO;
    }
    wr(VIO_R_QNOTIFY, VIRTIO_SND_VQ_TX);
    device_owns_buf = 1;
#endif

    /* Wait for completion with a generous timeout (duration + some margin). */
    uint64_t t0 = timer_count();
    uint64_t limit = ms_ticks(duration_ms + 500); /* duration + 500ms margin */

    for (;;) {
        uint16_t id; uint32_t len;
        int r = vq_complete(&q->vq, &id, &len);
        if (r == VQ_OK) {
            if (id != 0) { release_tx_page(page, device_owns_buf); return SND_EPROTO; }
            /* Validate status trailer. */
            uint32_t latency = 0;
            int sr = vsnd_dec_pcm_status(status_area, len, &latency);
            pmm_free_page(page);
            return sr;
        }
        if (r != VQ_EEMPTY) { release_tx_page(page, device_owns_buf); return SND_EPROTO; }
        if (rd(VIO_R_STATUS) & (VIO_ST_NEEDS_RESET | VIO_ST_FAILED)) { release_tx_page(page, device_owns_buf); return SND_EPROTO; }
        if (timer_count() - t0 > limit) {
            /* Timeout: if the device owns the buffer, reset the device first so it
             * provably stops DMA before we free the page. If reset is unconfirmed,
             * leak the page (safe but wasteful) rather than free memory the device
             * may still be reading. */
            if (device_owns_buf) {
                int stopped = reset_dev();
                if (stopped) pmm_free_page(page);
                live = 0;
            } else {
                pmm_free_page(page);
            }
            return SND_ETIMEOUT;
        }
    }
}
