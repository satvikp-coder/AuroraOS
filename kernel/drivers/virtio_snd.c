/*
 * kernel/drivers/virtio_snd.c - virtio sound device (ID 25) over the modern virtio-mmio transport: DEVICE BRING-UP + PCM PLAYBACK.
 * Original code written from the OASIS VIRTIO 1.2 specification (2.1 status, 2.7 split virtqueues, 3.1.1 initialisation, 4.2 MMIO,
 * 5.14 sound device; extract in docs/kernel/virtio-sound-spec-notes.md). Wire layouts and hostile-input decoders: virtio_snd.h.
 *
 * Scope: discover, reset, negotiate VIRTIO_F_VERSION_1 only, set up controlq/eventq/txq/rxq (shared virtqueue.c bookkeeping), DRIVER_OK,
 * read config (jacks, streams, chmaps) with a generation-consistent read, then ONE polled PCM_INFO request for all streams.
 * PCM playback: SET_PARAMS / PREPARE / START / STOP / RELEASE on the control queue; sine-wave tone generation submitted on the TX queue.
 * Failure policy: every wait is bounded by the ARM generic timer; any protocol violation, timeout or refusal sets FAILED, resets the
 * device, frees its pages only once the reset is confirmed, and returns an error. Boot always continues.
 *
 * Deterministic compile-time fault injection (test builds only, tests/qemu/sound-tests.sh) via AURORA_SND_FAULT=<n>:
 *  1 withhold the PCM_INFO request (bounded timeout), 2 non-zero padding in PCM_INFO record 0,
 *  3 used length one byte short, 4 config reports VSND_MAX_STREAMS+1 streams, 5 unknown status code.
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
#include <capture_lifecycle.h>
#include <tts_synth.h>

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

static uint32_t capture_owner_principal;
static struct cap_table capture_sessions;

/* Grant audio capabilities to a principal. Kernel code only.
 * Returns 0 on success, -1 if principal is invalid (kernel, none, reserved) or table full. */
int vsnd_grant_audio(uint32_t principal, uint8_t caps)
{
    if (!principal || principal == 0xFFFFFFFFu) return -1;  /* kernel or none */
    if ((principal & 0xFF000000u) == 0x7F000000u && principal != PRINCIPAL_SHELL) return -1; /* reserved range */
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
 * The caller must supply its resolved identity. Explicit session owners
 * are checked verbatim, including when kernel code calls on their behalf. */
static int vsnd_check_audio_cap(uint32_t principal, uint8_t cap)
{
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
    if (capture_owner_principal == principal) {
        capture_owner_principal = 0;
    }
    cap_table_revoke_principal(&capture_sessions, principal);
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

int vsnd_capture_open(struct cap_owner owner, uint32_t stream_id, struct cap_handle *out)
{
    if (!vsnd_check_audio_cap(owner.principal, AUDIO_CAP_RECORD)) return CAP_E_CAP;
    if (live && stream_id >= cfg.streams) return CAP_E_BOUNDS;
    return cap_table_open(&capture_sessions, owner, AUDIO_CAP_RECORD, stream_id, out);
}

int vsnd_capture_prepare(struct cap_handle handle, struct cap_owner owner, uint32_t buffer_count)
{
    struct cap_session *session = cap_table_session(&capture_sessions, handle);
    if (!session) return CAP_E_BOUNDS;
    return cap_prepare(session, owner, buffer_count);
}

int vsnd_capture_start(struct cap_handle handle, struct cap_owner owner)
{
    struct cap_session *session = cap_table_session(&capture_sessions, handle);
    if (!session) return CAP_E_BOUNDS;
    if (!vsnd_check_audio_cap(owner.principal, AUDIO_CAP_RECORD)) return CAP_E_CAP;
    return cap_start(session, owner);
}

int vsnd_capture_stop(struct cap_handle handle, struct cap_owner owner)
{
    struct cap_session *session = cap_table_session(&capture_sessions, handle);
    if (!session) return CAP_E_BOUNDS;
    return cap_stop(session, owner);
}

int vsnd_capture_release(struct cap_handle handle, struct cap_owner owner)
{
    struct cap_session *session = cap_table_session(&capture_sessions, handle);
    if (!session) return CAP_E_BOUNDS;
    return cap_release(session, owner);
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
    /* Fault 1 keeps the request unpublished, including for devices that poll the ring. */
#if !(defined(AURORA_SND_FAULT) && AURORA_SND_FAULT == 1)
    if (vq_submit(&q->vq, 0, resp_len) != VQ_OK) return SND_EPROTO;
    wr(VIO_R_QNOTIFY, VIRTIO_SND_VQ_CONTROL);
#endif
    uint64_t t0 = timer_count(), limit = ms_ticks(CTRL_MS);
    for (;;) {
        uint16_t id; uint32_t len;
        int r = vq_complete(&q->vq, &id, &len);
        if (r == VQ_OK) { if (id != 0) return SND_EPROTO; *used_len = len; break; }
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

/* Release the PCM page after an error. If the doorbell has been rung the device may
 * still be DMA-ing the page: reset the device and only free it once the reset is
 * confirmed (leak it otherwise, safe but wasteful). Mark live=0 so subsequent calls
 * fail closed. If the doorbell was never rung the device never saw the page, so free
 * it immediately. */
static void release_pcm_page(uintptr_t page, int device_owns_buf)
{
    if (!device_owns_buf) { pmm_free_page(page); return; }
    int stopped = reset_dev();
    if (stopped) {
        pmm_free_page(page);
        free_pages();
    }
    live = 0;
}

/* ---- PCM playback implementation ---- */

/* Validate stream is an output stream and PCM_INFO supports the requested format/rate/channels.
 * Encodes and submits SET_PARAMS via the control queue. */
int vsnd_set_params(uint32_t stream_id, uint32_t format, uint32_t rate, uint32_t channels,
                    uint32_t buffer_bytes, uint32_t period_bytes, uint32_t features)
{
    if (!live) return SND_ENODEV;

    /* Permission check: caller must have AUDIO_CAP_PLAY */
    uint32_t principal = proc_current_principal_or(PRINCIPAL_SHELL);
    if (!vsnd_check_audio_cap(principal, AUDIO_CAP_PLAY)) return VSND_EPERM;

    if (stream_id >= cfg.streams) return SND_EPROTO;
    if (info[stream_id].direction != VIRTIO_SND_D_OUTPUT) return SND_EPROTO;

    if (format > VSND_PCM_FMT_MAX || rate > VSND_PCM_RATE_MAX || channels > UINT8_MAX) return VSND_EARG;

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
    if (rc != SND_OK) return fail(rc, "PCM SET_PARAMS queue failed");

    return vsnd_dec_resp_hdr((uint8_t *)(ctrl_page + OFF_RESP), used);
}

static int playback_ctl(uint32_t stream_id, uint32_t code)
{
    if (!live) return SND_ENODEV;
    if (!vsnd_check_audio_cap(proc_current_principal_or(PRINCIPAL_SHELL), AUDIO_CAP_PLAY)) return VSND_EPERM;
    if (stream_id >= cfg.streams || info[stream_id].direction != VIRTIO_SND_D_OUTPUT) return SND_EPROTO;

    uint8_t *req = (uint8_t *)(ctrl_page + OFF_REQ);
    size_t written = vsnd_enc_pcm_hdr(req, VSND_PCM_HDR_SIZE, code, stream_id, cfg.streams);
    if (!written) return SND_EPROTO;
    uint32_t used = 0;
    int rc = control((uint32_t)written, VSND_HDR_SIZE, &used);
    if (rc != SND_OK) return fail(rc, "PCM control queue failed");
    return vsnd_dec_resp_hdr((uint8_t *)(ctrl_page + OFF_RESP), used);
}

int vsnd_prepare(uint32_t stream_id) { return playback_ctl(stream_id, VIRTIO_SND_R_PCM_PREPARE); }
int vsnd_start(uint32_t stream_id) { return playback_ctl(stream_id, VIRTIO_SND_R_PCM_START); }
int vsnd_stop(uint32_t stream_id) { return playback_ctl(stream_id, VIRTIO_SND_R_PCM_STOP); }
int vsnd_release(uint32_t stream_id) { return playback_ctl(stream_id, VIRTIO_SND_R_PCM_RELEASE); }

/* Submit one DMA page of S16LE stereo PCM frames on TX queue and wait for completion.
 * Takes ownership of `page` and frees it only after device completion or confirmed reset. */
static int vsnd_submit_tx_page(uint32_t stream_id, uintptr_t page, uint32_t byte_len, uint32_t timeout_ms)
{
    struct sq *q = &Q[VIRTIO_SND_VQ_TX];

    q->desc[0].addr = (uint64_t)(ctrl_page + OFF_REQ);
    q->desc[0].len = VSND_XFER_SIZE;
    q->desc[0].flags = DESC_NEXT;
    q->desc[0].next = 1;

    q->desc[1].addr = (uint64_t)page;
    q->desc[1].len = byte_len;
    q->desc[1].flags = DESC_NEXT;
    q->desc[1].next = 2;

    q->desc[2].addr = (uint64_t)(ctrl_page + OFF_RESP);
    q->desc[2].len = VSND_PSTATUS_SIZE;
    q->desc[2].flags = DESC_WRITE;
    q->desc[2].next = 0;

    uint8_t *xfer_hdr = (uint8_t *)(ctrl_page + OFF_REQ);
    if (!vsnd_enc_pcm_xfer(xfer_hdr, VSND_XFER_SIZE, stream_id, cfg.streams)) {
        pmm_free_page(page);
        return SND_EPROTO;
    }

    uint8_t *status_area = (uint8_t *)(ctrl_page + OFF_RESP);
    for (uint32_t i = 0; i < VSND_PSTATUS_SIZE; i++) status_area[i] = 0xff;

    int device_owns_buf = 0;
#if defined(AURORA_SND_FAULT) && AURORA_SND_FAULT == 1
    /* LEGACY: never ring the doorbell (device never sees the buffer) */
#else
    mb();
    if (vq_submit(&q->vq, 0, VSND_PSTATUS_SIZE) != VQ_OK) {
        pmm_free_page(page);
        return SND_EPROTO;
    }
    wr(VIO_R_QNOTIFY, VIRTIO_SND_VQ_TX);
    device_owns_buf = 1;
#endif

    uint64_t t0 = timer_count();
    uint64_t limit = ms_ticks(timeout_ms);

    for (;;) {
        uint16_t id; uint32_t len;
        int r = vq_complete(&q->vq, &id, &len);
        if (r == VQ_OK) {
            if (id != 0) { release_pcm_page(page, device_owns_buf); return SND_EPROTO; }
            uint32_t latency = 0;
            int sr = vsnd_dec_pcm_status(status_area, len, &latency);
            pmm_free_page(page);
            return sr;
        }
        if (r != VQ_EEMPTY) { release_pcm_page(page, device_owns_buf); return SND_EPROTO; }
        if (rd(VIO_R_STATUS) & (VIO_ST_NEEDS_RESET | VIO_ST_FAILED)) { release_pcm_page(page, device_owns_buf); return SND_EPROTO; }
        if (timer_count() - t0 > limit) {
            release_pcm_page(page, device_owns_buf);
            return SND_ETIMEOUT;
        }
    }
}

/* Generate a sine wave at the given frequency in a page and submit it on the TX queue for the given stream.
 * Waits for the completion (device writes status trailer). Duration is approximate based on buffer size.
 * The page stays owned by the driver until completion or a confirmed reset. */
int vsnd_play_tone(uint32_t stream_id, uint32_t freq_hz, uint32_t duration_ms)
{
    if (!live) return SND_ENODEV;

    /* Permission check: caller must have AUDIO_CAP_PLAY */
    uint32_t principal = proc_current_principal_or(PRINCIPAL_SHELL);
    if (!vsnd_check_audio_cap(principal, AUDIO_CAP_PLAY)) return VSND_EPERM;

    if (stream_id >= cfg.streams) return SND_EPROTO;
    if (info[stream_id].direction != VIRTIO_SND_D_OUTPUT) return SND_EPROTO;

    if (freq_hz == 0 || freq_hz > 24000 || duration_ms == 0 || duration_ms > 21) return VSND_EARG;

    if (Q[VIRTIO_SND_VQ_TX].vq.qn < 3u) return SND_EPROTO;

    /* Allocate one page for the sine wave buffer (4 KB). */
    uintptr_t page = pmm_alloc_page();
    if (!page) return SND_ENOMEM;

    /* Generate sine wave at 48000 Hz, S16LE, 2 channels (interleaved L/R). */
    const uint32_t sample_rate = 48000;
    const uint32_t freq = freq_hz;
    const uint32_t channels = 2;
    const uint32_t bytes_per_sample = 2; /* S16 */
    const uint32_t frame_bytes = channels * bytes_per_sample;
    const uint32_t frames = duration_ms * (sample_rate / 1000u);

    int16_t *buf = (int16_t *)page;
    const int32_t TWO_PI = 0x6487E;    /* 2π in Q16.16 */
    const int32_t step = (int32_t)(((int64_t)TWO_PI * freq) / sample_rate); /* angle increment per sample */
    int32_t angle = 0;
    for (uint32_t i = 0; i < frames; i++) {
        int32_t sin_fp = vsnd_sin_fp(angle);           /* Q16.16 [-1.0, 1.0] */
        int16_t sample = (int16_t)((sin_fp * 16384) >> 16); /* scale to ~50% amplitude */
        buf[i * 2] = sample;     /* Left */
        buf[i * 2 + 1] = sample; /* Right */
        angle += step;
        if (angle >= TWO_PI) angle -= TWO_PI;
    }

    return vsnd_submit_tx_page(stream_id, page, frames * frame_bytes, duration_ms + 500u);
}

/* Synthesize a short ASCII cue (<= 6 chars, <= 960 stereo frames = 3840 bytes) via tts_synth.h
 * and submit it on the TX queue with AUDIO_CAP_PLAY enforcement. */
int vsnd_play_tts(uint32_t stream_id, const char *text, size_t len)
{
    if (!live) return SND_ENODEV;
    uint32_t principal = proc_current_principal_or(PRINCIPAL_SHELL);
    if (!vsnd_check_audio_cap(principal, AUDIO_CAP_PLAY)) return VSND_EPERM;
    if (stream_id >= cfg.streams || info[stream_id].direction != VIRTIO_SND_D_OUTPUT) return SND_EPROTO;
    if (!text || len == 0 || len > 6u) return VSND_EARG;
    if (Q[VIRTIO_SND_VQ_TX].vq.qn < 3u) return SND_EPROTO;

    int16_t mono[1024];
    size_t nsamp = 0;
    if (tts_synthesize(text, len, mono, 1024u, &nsamp) != TTS_OK || nsamp == 0 || nsamp > 1024u)
        return VSND_EARG;

    uintptr_t page = pmm_alloc_page();
    if (!page) return SND_ENOMEM;

    int16_t *buf = (int16_t *)page;
    for (size_t i = 0; i < nsamp; i++) {
        buf[i * 2] = mono[i];
        buf[i * 2 + 1] = mono[i];
    }

    return vsnd_submit_tx_page(stream_id, page, (uint32_t)(nsamp * 4u), 550u);
}

#ifdef AURORA_SND_PLAY_TEST
int vsnd_playback_selftest(void)
{
    const uint32_t period = 20u * 48u * 4u;
    if (vsnd_play_tone(0, 440, 20) != VSND_EPERM) return VSND_EPROTO;
    if (vsnd_grant_audio(PRINCIPAL_SHELL, AUDIO_CAP_PLAY) != 0) return VSND_EPERM;
    int rc = vsnd_set_params(0, VIRTIO_SND_PCM_FMT_S16, VIRTIO_SND_PCM_RATE_48000, 2, 2u * period, period, 0);
    if (rc == SND_OK) rc = vsnd_prepare(0);
    if (rc == SND_OK) rc = vsnd_start(0);
    if (rc == SND_OK) rc = vsnd_play_tone(0, 440, 20);
    if (rc == SND_OK) rc = vsnd_stop(0);
    if (rc == SND_OK) rc = vsnd_release(0);
    (void)vsnd_revoke_audio(PRINCIPAL_SHELL);
    return rc;
}
#endif

#ifdef AURORA_SND_RX_TEST
/* This fixed-format probe is callable only in the test image. It never exposes
 * sample bytes or a recording interface to applications. */
#define RX_PERIOD 640u                  /* 20 ms at 16 kHz, S16LE, mono */
#define RX_DATA_OFF 16u
#define RX_STATUS_OFF (RX_DATA_OFF + RX_PERIOD)
#define RX_SLOTS 4u
#define RX_SLOT_BYTES (RX_STATUS_OFF + VSND_PSTATUS_SIZE)
_Static_assert(RX_SLOTS * RX_SLOT_BYTES <= PAGE_SIZE, "RX periods fit in one DMA page");

static int capture_submit(struct sq *q, uintptr_t slot, uint16_t head)
{
    uint8_t *samples = (uint8_t *)(slot + RX_DATA_OFF);
    for (unsigned i = 0; i < RX_PERIOD; i++) samples[i] = 0;
    for (unsigned i = 0; i < VSND_PSTATUS_SIZE; i++) samples[RX_PERIOD + i] = 0xff;
    return vq_submit(&q->vq, head, RX_PERIOD + VSND_PSTATUS_SIZE);
}

static int capture_command(size_t request)
{
    uint32_t used = 0;
    if (!request) return SND_EPROTO;
    uint32_t code = vsnd_get_le32((uint8_t *)(ctrl_page + OFF_REQ));
    int rc = control((uint32_t)request, VSND_HDR_SIZE, &used);
    if (rc == SND_OK) rc = vsnd_dec_resp_hdr((uint8_t *)(ctrl_page + OFF_RESP), used);
    if (rc != SND_OK) {
        console_puts("sound capture control failed, opcode=");
        puthex(code);
        console_puts("\n");
    }
    return rc;
}

static int capture_probe(void)
{
    if (!vsnd_check_audio_cap(proc_current_principal_or(PRINCIPAL_SHELL), AUDIO_CAP_RECORD)) return VSND_EPERM;
    if (!live) return SND_ENODEV;
    struct sq *q = &Q[VIRTIO_SND_VQ_RX];
    if (q->vq.qn < 2u * RX_SLOTS) return SND_EPROTO;
    uint32_t stream = 0;
    while (stream < cfg.streams && info[stream].direction != VIRTIO_SND_D_INPUT) stream++;
    if (stream == cfg.streams) return VSND_ENODEV;
    struct vsnd_pcm_params params = {
        .stream_id = stream, .buffer_bytes = RX_SLOTS * RX_PERIOD, .period_bytes = RX_PERIOD,
        .channels = 1, .format = VIRTIO_SND_PCM_FMT_S16, .rate = VIRTIO_SND_PCM_RATE_16000,
    };
    uintptr_t page = pmm_alloc_page();
    if (!page) return SND_ENOMEM;
    uint8_t *req = (uint8_t *)(ctrl_page + OFF_REQ);
    int rc = capture_command(vsnd_enc_pcm_set_params(req, VSND_SETP_SIZE, &params, &info[stream], cfg.streams));
    if (rc != SND_OK) goto fail;
    rc = capture_command(vsnd_enc_pcm_hdr(req, VSND_PCM_HDR_SIZE, VIRTIO_SND_R_PCM_PREPARE, stream, cfg.streams));
    if (rc != SND_OK) goto fail;

    /* Keep multiple periods resident so the host callback can fill another
     * period while the guest processes and replenishes a completed chain. */
    for (unsigned i = 0; i < RX_SLOTS; i++) {
        uintptr_t slot = page + i * RX_SLOT_BYTES;
        uint16_t head = (uint16_t)(2u * i);
        if (!vsnd_enc_pcm_xfer((uint8_t *)slot, VSND_XFER_SIZE, stream, cfg.streams)) { rc = SND_EPROTO; goto fail; }
        q->desc[head] = (struct desc){ .addr = slot, .len = VSND_XFER_SIZE, .flags = DESC_NEXT, .next = head + 1u };
        q->desc[head + 1u] = (struct desc){ .addr = slot + RX_DATA_OFF, .len = RX_PERIOD + VSND_PSTATUS_SIZE,
                                         .flags = DESC_WRITE };
        mb();
        if (capture_submit(q, slot, head) != VQ_OK) { rc = SND_EPROTO; goto fail; }
    }
    wr(VIO_R_QNOTIFY, VIRTIO_SND_VQ_RX);
    rc = capture_command(vsnd_enc_pcm_hdr(req, VSND_PCM_HDR_SIZE, VIRTIO_SND_R_PCM_START, stream, cfg.streams));
    if (rc != SND_OK) goto fail;
    console_puts("sound capture input started\n");

    unsigned positive = 0, negative = 0, submitted = RX_SLOTS;
    int low = 0, high = 0;
    uint64_t capture_start = timer_count();
    for (unsigned period = 0; period < submitted; period++) {
        uint16_t id;
        uint32_t used;
        uint64_t t0 = timer_count();
        for (;;) {
            int result = vq_complete(&q->vq, &id, &used);
            if (result == VQ_OK) break;
            if (result != VQ_EEMPTY || (rd(VIO_R_STATUS) & (VIO_ST_NEEDS_RESET | VIO_ST_FAILED))) {
                rc = SND_EPROTO;
                goto fail;
            }
            if (timer_count() - t0 > ms_ticks(period == 0 ? 3000 : 750) ||
                timer_count() - capture_start > ms_ticks(6000)) {
                console_puts("sound capture RX timed out, period=");
                console_dec(period);
                console_puts("\n");
                rc = SND_ETIMEOUT;
                goto fail;
            }
        }
        if ((id & 1u) || id / 2u >= RX_SLOTS) {
            rc = SND_EPROTO;
            goto fail;
        }
        uintptr_t slot = page + (id / 2u) * RX_SLOT_BYTES;
        int16_t samples[RX_PERIOD / 2u];
        rc = vsnd_copy_rx_s16_period((const uint8_t *)(slot + RX_DATA_OFF),
                                     RX_PERIOD + VSND_PSTATUS_SIZE, used, samples, RX_PERIOD / 2u);
        if (rc != VSND_OK) goto fail;
        for (unsigned i = 0; i < RX_PERIOD / 2u; i++) {
            int sample = samples[i];
            if (sample < low) low = sample;
            if (sample > high) high = sample;
            positive += sample == 0x1234;
            negative += sample == -0x1234;
        }
        if ((positive < 64 || negative < 64) && submitted < 100u) {
            if (capture_submit(q, slot, id) != VQ_OK) { rc = SND_EPROTO; goto fail; }
            submitted++;
            wr(VIO_R_QNOTIFY, VIRTIO_SND_VQ_RX);
        }
    }

    rc = capture_command(vsnd_enc_pcm_hdr(req, VSND_PCM_HDR_SIZE, VIRTIO_SND_R_PCM_STOP, stream, cfg.streams));
    if (rc == SND_OK)
        rc = capture_command(vsnd_enc_pcm_hdr(req, VSND_PCM_HDR_SIZE, VIRTIO_SND_R_PCM_RELEASE, stream, cfg.streams));
    if (rc != SND_OK) goto fail;
    pmm_free_page(page);
    console_puts("sound capture signed range magnitude -"); console_dec((unsigned)-low);
    console_puts(" to "); console_dec((unsigned)high);
    console_puts("; expected sample counts "); console_dec(positive);
    console_puts("/"); console_dec(negative); console_puts("\n");
    return positive >= 64 && negative >= 64 ? SND_OK : VSND_EIO;

fail:
    /* A published RX descriptor may be visible even before its notification.
     * Reset and confirm that DMA has stopped before freeing any of its memory. */
    release_pcm_page(page, 1);
    return rc;
}

int vsnd_capture_selftest(void)
{
    if (capture_probe() != VSND_EPERM) return SND_EPROTO;
    if (vsnd_grant_audio(PRINCIPAL_SHELL, AUDIO_CAP_RECORD) != 0) return VSND_EPERM;
    int rc = capture_probe();
    (void)vsnd_revoke_audio(PRINCIPAL_SHELL);
    if (capture_probe() != VSND_EPERM) return SND_EPROTO;
    return rc;
}
#endif
