#ifndef AURORA_VIRTIO_SND_H
#define AURORA_VIRTIO_SND_H
#include <stddef.h>
#include <stdint.h>

/*
 * virtio-snd (Sound Device, ID 25) message layer: freestanding, header-only, host-testable.
 * Source of truth: docs/kernel/virtio-sound-spec-notes.md (virtio 1.2 section 5.14). Only layouts quoted there are encoded here.
 *
 * Every message is built or parsed byte by byte in explicit little-endian, so nothing depends on host endianness, struct packing or alignment.
 * Everything the DEVICE writes (config space, response headers, PCM_INFO records, PCM status, events, used lengths) is hostile:
 * the decoders refuse short buffers, unknown codes, undefined bits, non-zero padding and counts above our bounds with VSND_EPROTO.
 * Nothing here touches MMIO or virtqueues; a driver feeds it buffers. Not linked into the kernel yet.
 */

/* ---- result codes ---- */
#define VSND_OK        0
#define VSND_EARG     -1                 /* caller passed bad arguments or a request the spec forbids */
#define VSND_EPROTO   -2                 /* the device sent something malformed or out of range */
#define VSND_EBADMSG  -3                 /* device answered VIRTIO_SND_S_BAD_MSG */
#define VSND_ENOTSUPP -4                 /* device answered VIRTIO_SND_S_NOT_SUPP */
#define VSND_EIO      -5                 /* device answered VIRTIO_SND_S_IO_ERR */

/* ---- our bounds on device-supplied counts (policy, not spec) ---- */
#ifndef VSND_MAX_JACKS
#define VSND_MAX_JACKS   32u
#endif
#ifndef VSND_MAX_STREAMS
#define VSND_MAX_STREAMS 32u
#endif
#ifndef VSND_MAX_CHMAPS
#define VSND_MAX_CHMAPS  32u
#endif

/* ---- device id, queues (5.14.1, 5.14.2) ---- */
#define VIRTIO_SND_DEVICE_ID 25u
#define VIRTIO_SND_VQ_CONTROL 0u
#define VIRTIO_SND_VQ_EVENT   1u
#define VIRTIO_SND_VQ_TX      2u
#define VIRTIO_SND_VQ_RX      3u

/* ---- request, event and status codes (5.14.6) ---- */
#define VIRTIO_SND_R_JACK_INFO        0x0001u
#define VIRTIO_SND_R_JACK_REMAP       0x0002u
#define VIRTIO_SND_R_PCM_INFO         0x0100u
#define VIRTIO_SND_R_PCM_SET_PARAMS   0x0101u
#define VIRTIO_SND_R_PCM_PREPARE      0x0102u
#define VIRTIO_SND_R_PCM_RELEASE      0x0103u
#define VIRTIO_SND_R_PCM_START        0x0104u
#define VIRTIO_SND_R_PCM_STOP         0x0105u
#define VIRTIO_SND_R_CHMAP_INFO       0x0200u
#define VIRTIO_SND_EVT_JACK_CONNECTED     0x1000u
#define VIRTIO_SND_EVT_JACK_DISCONNECTED  0x1001u
#define VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED 0x1100u
#define VIRTIO_SND_EVT_PCM_XRUN           0x1101u
#define VIRTIO_SND_S_OK       0x8000u
#define VIRTIO_SND_S_BAD_MSG  0x8001u
#define VIRTIO_SND_S_NOT_SUPP 0x8002u
#define VIRTIO_SND_S_IO_ERR   0x8003u

#define VIRTIO_SND_D_OUTPUT 0u
#define VIRTIO_SND_D_INPUT  1u

/* PCM stream features (bit numbers, 5.14.6.6.2) */
#define VIRTIO_SND_PCM_F_SHMEM_HOST        0u
#define VIRTIO_SND_PCM_F_SHMEM_GUEST       1u
#define VIRTIO_SND_PCM_F_MSG_POLLING       2u
#define VIRTIO_SND_PCM_F_EVT_SHMEM_PERIODS 3u
#define VIRTIO_SND_PCM_F_EVT_XRUNS         4u
#define VSND_PCM_F_DEFINED_MASK ((1u << 5) - 1u)
#define VSND_PCM_F_SHMEM_MASK   ((1u << VIRTIO_SND_PCM_F_SHMEM_HOST) | (1u << VIRTIO_SND_PCM_F_SHMEM_GUEST))

/* PCM formats: enum indexes 0..24 (5.14.6.6.2) */
#define VIRTIO_SND_PCM_FMT_IMA_ADPCM 0u
#define VIRTIO_SND_PCM_FMT_MU_LAW    1u
#define VIRTIO_SND_PCM_FMT_A_LAW     2u
#define VIRTIO_SND_PCM_FMT_S8        3u
#define VIRTIO_SND_PCM_FMT_U8        4u
#define VIRTIO_SND_PCM_FMT_S16       5u
#define VIRTIO_SND_PCM_FMT_U16       6u
#define VIRTIO_SND_PCM_FMT_S18_3     7u
#define VIRTIO_SND_PCM_FMT_U18_3     8u
#define VIRTIO_SND_PCM_FMT_S20_3     9u
#define VIRTIO_SND_PCM_FMT_U20_3    10u
#define VIRTIO_SND_PCM_FMT_S24_3    11u
#define VIRTIO_SND_PCM_FMT_U24_3    12u
#define VIRTIO_SND_PCM_FMT_S20      13u
#define VIRTIO_SND_PCM_FMT_U20      14u
#define VIRTIO_SND_PCM_FMT_S24      15u
#define VIRTIO_SND_PCM_FMT_U24      16u
#define VIRTIO_SND_PCM_FMT_S32      17u
#define VIRTIO_SND_PCM_FMT_U32      18u
#define VIRTIO_SND_PCM_FMT_FLOAT    19u
#define VIRTIO_SND_PCM_FMT_FLOAT64  20u
#define VIRTIO_SND_PCM_FMT_DSD_U8   21u
#define VIRTIO_SND_PCM_FMT_DSD_U16  22u
#define VIRTIO_SND_PCM_FMT_DSD_U32  23u
#define VIRTIO_SND_PCM_FMT_IEC958_SUBFRAME 24u
#define VSND_PCM_FMT_MAX 24u

/* PCM rates: enum indexes 0..13 (5.14.6.6.2) */
#define VIRTIO_SND_PCM_RATE_5512    0u
#define VIRTIO_SND_PCM_RATE_8000    1u
#define VIRTIO_SND_PCM_RATE_11025   2u
#define VIRTIO_SND_PCM_RATE_16000   3u
#define VIRTIO_SND_PCM_RATE_22050   4u
#define VIRTIO_SND_PCM_RATE_32000   5u
#define VIRTIO_SND_PCM_RATE_44100   6u
#define VIRTIO_SND_PCM_RATE_48000   7u
#define VIRTIO_SND_PCM_RATE_64000   8u
#define VIRTIO_SND_PCM_RATE_88200   9u
#define VIRTIO_SND_PCM_RATE_96000  10u
#define VIRTIO_SND_PCM_RATE_176400 11u
#define VIRTIO_SND_PCM_RATE_192000 12u
#define VIRTIO_SND_PCM_RATE_384000 13u
#define VSND_PCM_RATE_MAX 13u

#define VIRTIO_SND_CHMAP_MAX_SIZE 18u

/* ---- wire layouts: byte offsets and sizes (all fields little-endian) ---- */
/* struct virtio_snd_config { le32 jacks; le32 streams; le32 chmaps; }  (v1.2; v1.3 'controls' at 0xC is not read) */
#define VSND_CFG_OFF_JACKS   0u
#define VSND_CFG_OFF_STREAMS 4u
#define VSND_CFG_OFF_CHMAPS  8u
#define VSND_CFG_SIZE       12u
/* struct virtio_snd_hdr { le32 code; } */
#define VSND_HDR_OFF_CODE 0u
#define VSND_HDR_SIZE     4u
/* struct virtio_snd_event { hdr; le32 data; } */
#define VSND_EVENT_OFF_DATA 4u
#define VSND_EVENT_SIZE     8u
/* struct virtio_snd_query_info { hdr; le32 start_id; le32 count; le32 size; } */
#define VSND_QINFO_OFF_START_ID 4u
#define VSND_QINFO_OFF_COUNT    8u
#define VSND_QINFO_OFF_SIZE    12u
#define VSND_QINFO_SIZE        16u
/* struct virtio_snd_pcm_hdr { hdr; le32 stream_id; } */
#define VSND_PCM_HDR_OFF_STREAM_ID 4u
#define VSND_PCM_HDR_SIZE          8u
/* struct virtio_snd_pcm_set_params { pcm_hdr; le32 buffer_bytes; le32 period_bytes; le32 features; u8 channels, format, rate, padding; } */
#define VSND_SETP_OFF_BUFFER_BYTES  8u
#define VSND_SETP_OFF_PERIOD_BYTES 12u
#define VSND_SETP_OFF_FEATURES     16u
#define VSND_SETP_OFF_CHANNELS     20u
#define VSND_SETP_OFF_FORMAT       21u
#define VSND_SETP_OFF_RATE         22u
#define VSND_SETP_OFF_PADDING      23u
#define VSND_SETP_SIZE             24u
/* struct virtio_snd_pcm_info { le32 hda_fn_nid; le32 features; le64 formats; le64 rates; u8 direction, channels_min, channels_max; u8 padding[5]; } */
#define VSND_PINFO_OFF_HDA_FN_NID    0u
#define VSND_PINFO_OFF_FEATURES      4u
#define VSND_PINFO_OFF_FORMATS       8u
#define VSND_PINFO_OFF_RATES        16u
#define VSND_PINFO_OFF_DIRECTION    24u
#define VSND_PINFO_OFF_CHANNELS_MIN 25u
#define VSND_PINFO_OFF_CHANNELS_MAX 26u
#define VSND_PINFO_OFF_PADDING      27u
#define VSND_PINFO_PADDING_LEN       5u
#define VSND_PINFO_SIZE             32u
/* struct virtio_snd_pcm_xfer { le32 stream_id; } */
#define VSND_XFER_OFF_STREAM_ID 0u
#define VSND_XFER_SIZE          4u
/* struct virtio_snd_pcm_status { le32 status; le32 latency_bytes; } */
#define VSND_PSTATUS_OFF_STATUS  0u
#define VSND_PSTATUS_OFF_LATENCY 4u
#define VSND_PSTATUS_SIZE        8u

_Static_assert(VSND_CFG_OFF_CHMAPS + 4u == VSND_CFG_SIZE, "virtio_snd_config is 12 bytes");
_Static_assert(VSND_HDR_SIZE == 4u, "virtio_snd_hdr is 4 bytes");
_Static_assert(VSND_EVENT_OFF_DATA + 4u == VSND_EVENT_SIZE && VSND_EVENT_SIZE == 8u, "virtio_snd_event is 8 bytes");
_Static_assert(VSND_QINFO_OFF_START_ID == VSND_HDR_SIZE && VSND_QINFO_OFF_SIZE + 4u == VSND_QINFO_SIZE && VSND_QINFO_SIZE == 16u, "virtio_snd_query_info is 16 bytes");
_Static_assert(VSND_PCM_HDR_OFF_STREAM_ID == VSND_HDR_SIZE && VSND_PCM_HDR_SIZE == 8u, "virtio_snd_pcm_hdr is 8 bytes");
_Static_assert(VSND_SETP_OFF_BUFFER_BYTES == VSND_PCM_HDR_SIZE && VSND_SETP_OFF_PADDING + 1u == VSND_SETP_SIZE && VSND_SETP_SIZE == 24u, "virtio_snd_pcm_set_params is 24 bytes");
_Static_assert(VSND_PINFO_OFF_PADDING + VSND_PINFO_PADDING_LEN == VSND_PINFO_SIZE && VSND_PINFO_SIZE == 32u, "virtio_snd_pcm_info is 32 bytes");
_Static_assert(VSND_XFER_SIZE == 4u, "virtio_snd_pcm_xfer is 4 bytes");
_Static_assert(VSND_PSTATUS_OFF_LATENCY + 4u == VSND_PSTATUS_SIZE && VSND_PSTATUS_SIZE == 8u, "virtio_snd_pcm_status is 8 bytes");

/* Mirror structs, used ONLY to cross-check the offsets above against the compiler's natural layout (every field is naturally aligned, so no padding is
 * inserted). Never cast device memory to them: encode/decode below go byte by byte. */
struct vsnd_layout_config { uint32_t jacks, streams, chmaps; };
struct vsnd_layout_query_info { uint32_t code, start_id, count, size; };
struct vsnd_layout_pcm_hdr { uint32_t code, stream_id; };
struct vsnd_layout_set_params { uint32_t code, stream_id, buffer_bytes, period_bytes, features; uint8_t channels, format, rate, padding; };
struct vsnd_layout_pcm_info { uint32_t hda_fn_nid, features; uint64_t formats, rates; uint8_t direction, channels_min, channels_max, padding[5]; };
struct vsnd_layout_pcm_status { uint32_t status, latency_bytes; };
_Static_assert(sizeof(struct vsnd_layout_config) == VSND_CFG_SIZE && offsetof(struct vsnd_layout_config, chmaps) == VSND_CFG_OFF_CHMAPS, "config layout");
_Static_assert(sizeof(struct vsnd_layout_query_info) == VSND_QINFO_SIZE && offsetof(struct vsnd_layout_query_info, size) == VSND_QINFO_OFF_SIZE, "query_info layout");
_Static_assert(sizeof(struct vsnd_layout_pcm_hdr) == VSND_PCM_HDR_SIZE && offsetof(struct vsnd_layout_pcm_hdr, stream_id) == VSND_PCM_HDR_OFF_STREAM_ID, "pcm_hdr layout");
_Static_assert(sizeof(struct vsnd_layout_set_params) == VSND_SETP_SIZE && offsetof(struct vsnd_layout_set_params, features) == VSND_SETP_OFF_FEATURES
               && offsetof(struct vsnd_layout_set_params, channels) == VSND_SETP_OFF_CHANNELS && offsetof(struct vsnd_layout_set_params, rate) == VSND_SETP_OFF_RATE, "set_params layout");
_Static_assert(sizeof(struct vsnd_layout_pcm_info) == VSND_PINFO_SIZE && offsetof(struct vsnd_layout_pcm_info, formats) == VSND_PINFO_OFF_FORMATS
               && offsetof(struct vsnd_layout_pcm_info, rates) == VSND_PINFO_OFF_RATES && offsetof(struct vsnd_layout_pcm_info, direction) == VSND_PINFO_OFF_DIRECTION
               && offsetof(struct vsnd_layout_pcm_info, padding) == VSND_PINFO_OFF_PADDING, "pcm_info layout");
_Static_assert(sizeof(struct vsnd_layout_pcm_status) == VSND_PSTATUS_SIZE, "pcm_status layout");

/* ---- decoded forms ---- */
struct vsnd_config { uint32_t jacks, streams, chmaps; };
struct vsnd_pcm_info {
    uint32_t hda_fn_nid, features;
    uint64_t formats, rates;
    uint8_t direction, channels_min, channels_max;
};
struct vsnd_pcm_params {
    uint32_t stream_id, buffer_bytes, period_bytes, features;
    uint8_t channels, format, rate;
};
struct vsnd_event { uint32_t type, data; };

/* ---- little-endian primitives ---- */
static inline void vsnd_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t vsnd_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t vsnd_get_le64(const uint8_t *p)
{
    return (uint64_t)vsnd_get_le32(p) | ((uint64_t)vsnd_get_le32(p + 4) << 32);
}

/* ---- encoders (driver -> device). Return bytes written, or 0 if refused. ---- */

/* Common request header alone. */
static inline size_t vsnd_enc_hdr(uint8_t *buf, size_t cap, uint32_t code)
{
    if (!buf || cap < VSND_HDR_SIZE) return 0;
    vsnd_put_le32(buf + VSND_HDR_OFF_CODE, code);
    return VSND_HDR_SIZE;
}

/* Bytes of the device-writable response for a query of `count` items of `size` bytes: hdr + count*size. 0 on overflow or zero count/size. */
static inline size_t vsnd_query_resp_bytes(uint32_t count, uint32_t size)
{
    if (count == 0 || size == 0) return 0;
    uint64_t n = (uint64_t)count * size + VSND_HDR_SIZE;
    if (n > (uint64_t)UINT32_MAX || n > (uint64_t)SIZE_MAX) return 0;
    return (size_t)n;
}

/* JACK_INFO / PCM_INFO / CHMAP_INFO. Enforces start_id + count <= total (the spec's driver rule), without overflow. */
static inline size_t vsnd_enc_query_info(uint8_t *buf, size_t cap, uint32_t code, uint32_t start_id, uint32_t count, uint32_t size, uint32_t total)
{
    if (!buf || cap < VSND_QINFO_SIZE) return 0;
    if (code != VIRTIO_SND_R_JACK_INFO && code != VIRTIO_SND_R_PCM_INFO && code != VIRTIO_SND_R_CHMAP_INFO) return 0;
    if (count == 0 || start_id > total || count > total - start_id) return 0;
    if (!vsnd_query_resp_bytes(count, size)) return 0;
    vsnd_put_le32(buf + VSND_HDR_OFF_CODE, code);
    vsnd_put_le32(buf + VSND_QINFO_OFF_START_ID, start_id);
    vsnd_put_le32(buf + VSND_QINFO_OFF_COUNT, count);
    vsnd_put_le32(buf + VSND_QINFO_OFF_SIZE, size);
    return VSND_QINFO_SIZE;
}

/* PREPARE / RELEASE / START / STOP: a bare pcm_hdr. stream_id must be < streams. */
static inline size_t vsnd_enc_pcm_hdr(uint8_t *buf, size_t cap, uint32_t code, uint32_t stream_id, uint32_t streams)
{
    if (!buf || cap < VSND_PCM_HDR_SIZE || stream_id >= streams) return 0;
    if (code != VIRTIO_SND_R_PCM_PREPARE && code != VIRTIO_SND_R_PCM_RELEASE && code != VIRTIO_SND_R_PCM_START && code != VIRTIO_SND_R_PCM_STOP) return 0;
    vsnd_put_le32(buf + VSND_HDR_OFF_CODE, code);
    vsnd_put_le32(buf + VSND_PCM_HDR_OFF_STREAM_ID, stream_id);
    return VSND_PCM_HDR_SIZE;
}

/*
 * Validate SET_PARAMS against the driver rules of 5.14.6.6.3.2. `info` (the stream's decoded PCM_INFO) may be NULL to check only the
 * stream-independent rules. Returns VSND_OK or VSND_EARG.
 */
static inline int vsnd_check_set_params(const struct vsnd_pcm_params *p, const struct vsnd_pcm_info *info, uint32_t streams)
{
    if (!p) return VSND_EARG;
    if (p->stream_id >= streams) return VSND_EARG;
    if (p->period_bytes == 0 || p->buffer_bytes == 0) return VSND_EARG;
    if (p->buffer_bytes % p->period_bytes != 0) return VSND_EARG;
    if (p->format > VSND_PCM_FMT_MAX || p->rate > VSND_PCM_RATE_MAX) return VSND_EARG;
    if (p->features & ~VSND_PCM_F_DEFINED_MASK) return VSND_EARG;
    if (p->features & VSND_PCM_F_SHMEM_MASK) return VSND_EARG;    /* placeholders: "MUST NOT be selected at the moment" */
    if (p->channels == 0) return VSND_EARG;
    if (info) {
        if (!(info->formats & ((uint64_t)1 << p->format))) return VSND_EARG;
        if (!(info->rates & ((uint64_t)1 << p->rate))) return VSND_EARG;
        if (p->features & ~info->features) return VSND_EARG;
        if (p->channels < info->channels_min || p->channels > info->channels_max) return VSND_EARG;
    }
    return VSND_OK;
}

/* SET_PARAMS. Refuses anything vsnd_check_set_params rejects; padding is written as 0. */
static inline size_t vsnd_enc_pcm_set_params(uint8_t *buf, size_t cap, const struct vsnd_pcm_params *p, const struct vsnd_pcm_info *info, uint32_t streams)
{
    if (!buf || cap < VSND_SETP_SIZE || vsnd_check_set_params(p, info, streams) != VSND_OK) return 0;
    vsnd_put_le32(buf + VSND_HDR_OFF_CODE, VIRTIO_SND_R_PCM_SET_PARAMS);
    vsnd_put_le32(buf + VSND_PCM_HDR_OFF_STREAM_ID, p->stream_id);
    vsnd_put_le32(buf + VSND_SETP_OFF_BUFFER_BYTES, p->buffer_bytes);
    vsnd_put_le32(buf + VSND_SETP_OFF_PERIOD_BYTES, p->period_bytes);
    vsnd_put_le32(buf + VSND_SETP_OFF_FEATURES, p->features);
    buf[VSND_SETP_OFF_CHANNELS] = p->channels;
    buf[VSND_SETP_OFF_FORMAT] = p->format;
    buf[VSND_SETP_OFF_RATE] = p->rate;
    buf[VSND_SETP_OFF_PADDING] = 0;
    return VSND_SETP_SIZE;
}

/* I/O message header for txq / rxq. */
static inline size_t vsnd_enc_pcm_xfer(uint8_t *buf, size_t cap, uint32_t stream_id, uint32_t streams)
{
    if (!buf || cap < VSND_XFER_SIZE || stream_id >= streams) return 0;
    vsnd_put_le32(buf + VSND_XFER_OFF_STREAM_ID, stream_id);
    return VSND_XFER_SIZE;
}

/* ---- decoders (device -> driver): hostile input ---- */

/* Device config space. Counts above our bounds are refused. */
static inline int vsnd_dec_config(const uint8_t *buf, size_t len, struct vsnd_config *out)
{
    if (!buf || !out) return VSND_EARG;
    if (len < VSND_CFG_SIZE) return VSND_EPROTO;
    uint32_t j = vsnd_get_le32(buf + VSND_CFG_OFF_JACKS);
    uint32_t s = vsnd_get_le32(buf + VSND_CFG_OFF_STREAMS);
    uint32_t c = vsnd_get_le32(buf + VSND_CFG_OFF_CHMAPS);
    if (j > VSND_MAX_JACKS || s > VSND_MAX_STREAMS || c > VSND_MAX_CHMAPS) return VSND_EPROTO;
    out->jacks = j; out->streams = s; out->chmaps = c;
    return VSND_OK;
}

/* Map a VIRTIO_SND_S_* code to a result; unknown codes are a protocol error. */
static inline int vsnd_status_result(uint32_t code)
{
    switch (code) {
    case VIRTIO_SND_S_OK:       return VSND_OK;
    case VIRTIO_SND_S_BAD_MSG:  return VSND_EBADMSG;
    case VIRTIO_SND_S_NOT_SUPP: return VSND_ENOTSUPP;
    case VIRTIO_SND_S_IO_ERR:   return VSND_EIO;
    default:                    return VSND_EPROTO;
    }
}

/* Control response header. `len` = bytes the device reported writing (used length), which must cover the header. */
static inline int vsnd_dec_resp_hdr(const uint8_t *buf, size_t len)
{
    if (!buf) return VSND_EARG;
    if (len < VSND_HDR_SIZE) return VSND_EPROTO;
    return vsnd_status_result(vsnd_get_le32(buf + VSND_HDR_OFF_CODE));
}

/* One PCM_INFO record (`len` bytes available for it, >= 32). Undefined bits, bad direction, bad channel range or non-zero padding: VSND_EPROTO. */
static inline int vsnd_dec_pcm_info(const uint8_t *buf, size_t len, struct vsnd_pcm_info *out)
{
    if (!buf || !out) return VSND_EARG;
    if (len < VSND_PINFO_SIZE) return VSND_EPROTO;
    uint32_t feat = vsnd_get_le32(buf + VSND_PINFO_OFF_FEATURES);
    uint64_t fmts = vsnd_get_le64(buf + VSND_PINFO_OFF_FORMATS);
    uint64_t rates = vsnd_get_le64(buf + VSND_PINFO_OFF_RATES);
    uint8_t dir = buf[VSND_PINFO_OFF_DIRECTION], cmin = buf[VSND_PINFO_OFF_CHANNELS_MIN], cmax = buf[VSND_PINFO_OFF_CHANNELS_MAX];
    if (feat & ~VSND_PCM_F_DEFINED_MASK) return VSND_EPROTO;
    if (fmts == 0 || (fmts >> (VSND_PCM_FMT_MAX + 1u)) != 0) return VSND_EPROTO;
    if (rates == 0 || (rates >> (VSND_PCM_RATE_MAX + 1u)) != 0) return VSND_EPROTO;
    if (dir != VIRTIO_SND_D_OUTPUT && dir != VIRTIO_SND_D_INPUT) return VSND_EPROTO;
    if (cmin == 0 || cmin > cmax) return VSND_EPROTO;
    for (unsigned i = 0; i < VSND_PINFO_PADDING_LEN; i++)
        if (buf[VSND_PINFO_OFF_PADDING + i] != 0) return VSND_EPROTO;
    out->hda_fn_nid = vsnd_get_le32(buf + VSND_PINFO_OFF_HDA_FN_NID);
    out->features = feat; out->formats = fmts; out->rates = rates;
    out->direction = dir; out->channels_min = cmin; out->channels_max = cmax;
    return VSND_OK;
}

/*
 * A full PCM_INFO response (hdr + count records of `size` bytes each, size >= 32) as the device wrote it, `used_len` bytes.
 * Fails unless the status is OK, the device wrote exactly hdr + count*size, and every record decodes. Fills out[0..count-1].
 */
static inline int vsnd_dec_pcm_info_resp(const uint8_t *buf, size_t used_len, uint32_t count, uint32_t size, struct vsnd_pcm_info *out)
{
    if (!buf || !out || size < VSND_PINFO_SIZE) return VSND_EARG;
    size_t need = vsnd_query_resp_bytes(count, size);
    if (!need) return VSND_EARG;
    int r = vsnd_dec_resp_hdr(buf, used_len);
    if (r != VSND_OK) return r;
    if (used_len != need) return VSND_EPROTO;
    for (uint32_t i = 0; i < count; i++) {
        r = vsnd_dec_pcm_info(buf + VSND_HDR_SIZE + (size_t)i * size, size, &out[i]);
        if (r != VSND_OK) return r;
    }
    return VSND_OK;
}

/* PCM I/O status trailer. Per 5.14.6.8 only S_OK or S_IO_ERR are legal here; anything else is VSND_EPROTO. */
static inline int vsnd_dec_pcm_status(const uint8_t *buf, size_t len, uint32_t *latency_bytes)
{
    if (!buf) return VSND_EARG;
    if (len < VSND_PSTATUS_SIZE) return VSND_EPROTO;
    uint32_t st = vsnd_get_le32(buf + VSND_PSTATUS_OFF_STATUS);
    if (st != VIRTIO_SND_S_OK && st != VIRTIO_SND_S_IO_ERR) return VSND_EPROTO;
    if (latency_bytes) *latency_bytes = vsnd_get_le32(buf + VSND_PSTATUS_OFF_LATENCY);
    return st == VIRTIO_SND_S_OK ? VSND_OK : VSND_EIO;
}

/*
 * rxq completion: the used length covers the recorded frames plus the 8-byte status. Returns the frame byte count in *frames,
 * refusing a length below the status size or one claiming more frames than the buffer we posted (`frames_cap`).
 */
static inline int vsnd_rx_frames(uint32_t used_len, uint32_t frames_cap, uint32_t *frames)
{
    if (!frames) return VSND_EARG;
    if (used_len < VSND_PSTATUS_SIZE) return VSND_EPROTO;
    if (used_len - VSND_PSTATUS_SIZE > frames_cap) return VSND_EPROTO;
    *frames = used_len - VSND_PSTATUS_SIZE;
    return VSND_OK;
}

/* eventq entry: known type, and data (jack or stream id) within the configured count. */
static inline int vsnd_dec_event(const uint8_t *buf, size_t len, const struct vsnd_config *cfg, struct vsnd_event *out)
{
    if (!buf || !cfg || !out) return VSND_EARG;
    if (len < VSND_EVENT_SIZE) return VSND_EPROTO;
    uint32_t type = vsnd_get_le32(buf + VSND_HDR_OFF_CODE), data = vsnd_get_le32(buf + VSND_EVENT_OFF_DATA);
    switch (type) {
    case VIRTIO_SND_EVT_JACK_CONNECTED:
    case VIRTIO_SND_EVT_JACK_DISCONNECTED:
        if (data >= cfg->jacks) return VSND_EPROTO;
        break;
    case VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED:
    case VIRTIO_SND_EVT_PCM_XRUN:
        if (data >= cfg->streams) return VSND_EPROTO;
        break;
    default:
        return VSND_EPROTO;
    }
    out->type = type; out->data = data;
    return VSND_OK;
}
#endif
