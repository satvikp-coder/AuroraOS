/* Host test of the virtio-snd message layer: exact wire bytes for every request, and a hostile device for every decoder. */
#include <stdio.h>
#include <string.h>
#include <virtio_snd.h>

static int fails, checks;
#define CHECK(c, m) do { checks++; if (!(c)) { fails++; printf("  FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

static void le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void le64(uint8_t *p, uint64_t v) { le32(p, (uint32_t)v); le32(p + 4, (uint32_t)(v >> 32)); }

/* an honest QEMU-like stream: S8|U8|S16|U16|S32|U32|FLOAT, all 14 rates, 1..2 channels */
#define QEMU_FMTS ((1ull << 3) | (1ull << 4) | (1ull << 5) | (1ull << 6) | (1ull << 17) | (1ull << 18) | (1ull << 19))
static void put_info(uint8_t *p, uint32_t feat, uint64_t fmts, uint64_t rates, uint8_t dir, uint8_t cmin, uint8_t cmax)
{
    memset(p, 0, VSND_PINFO_SIZE);
    le32(p + 0, 0); le32(p + 4, feat); le64(p + 8, fmts); le64(p + 16, rates);
    p[24] = dir; p[25] = cmin; p[26] = cmax;
}

static struct vsnd_pcm_params good_params(void)
{
    struct vsnd_pcm_params p = { .stream_id = 0, .buffer_bytes = 4 * 1024, .period_bytes = 1024, .features = 0,
                                 .channels = 2, .format = VIRTIO_SND_PCM_FMT_S16, .rate = VIRTIO_SND_PCM_RATE_48000 };
    return p;
}

int main(void)
{
    uint8_t b[256];
    struct vsnd_config cfg;
    struct vsnd_pcm_info info, infos[4];

    /* ---- primitives ---- */
    vsnd_put_le32(b, 0x11223344u);
    CHECK(b[0] == 0x44 && b[1] == 0x33 && b[2] == 0x22 && b[3] == 0x11, "le32 byte order");
    le64(b, 0x0102030405060708ull);
    CHECK(vsnd_get_le64(b) == 0x0102030405060708ull && vsnd_get_le32(b) == 0x05060708u, "le64/le32 round trip");

    /* ---- config space ---- */
    memset(b, 0, sizeof b); le32(b, 0); le32(b + 4, 2); le32(b + 8, 0);
    CHECK(vsnd_dec_config(b, VSND_CFG_SIZE, &cfg) == VSND_OK && cfg.jacks == 0 && cfg.streams == 2 && cfg.chmaps == 0, "QEMU default config");
    CHECK(vsnd_dec_config(b, VSND_CFG_SIZE - 1, &cfg) == VSND_EPROTO, "short config refused");
    CHECK(vsnd_dec_config(0, VSND_CFG_SIZE, &cfg) == VSND_EARG && vsnd_dec_config(b, VSND_CFG_SIZE, 0) == VSND_EARG, "null config args");
    cfg.streams = 77;
    le32(b + 4, VSND_MAX_STREAMS + 1);
    CHECK(vsnd_dec_config(b, VSND_CFG_SIZE, &cfg) == VSND_EPROTO && cfg.streams == 77, "stream count over bound refused, out untouched");
    le32(b + 4, 0xFFFFFFFFu);
    CHECK(vsnd_dec_config(b, VSND_CFG_SIZE, &cfg) == VSND_EPROTO, "stream count 0xFFFFFFFF refused");
    le32(b + 4, 2); le32(b, VSND_MAX_JACKS + 1);
    CHECK(vsnd_dec_config(b, VSND_CFG_SIZE, &cfg) == VSND_EPROTO, "jack count over bound refused");
    le32(b, VSND_MAX_JACKS); le32(b + 8, VSND_MAX_CHMAPS + 1);
    CHECK(vsnd_dec_config(b, VSND_CFG_SIZE, &cfg) == VSND_EPROTO, "chmap count over bound refused");
    le32(b + 8, VSND_MAX_CHMAPS); le32(b + 4, VSND_MAX_STREAMS);
    CHECK(vsnd_dec_config(b, VSND_CFG_SIZE, &cfg) == VSND_OK && cfg.jacks == VSND_MAX_JACKS && cfg.chmaps == VSND_MAX_CHMAPS, "counts at bound accepted");

    /* ---- encoders: exact bytes ---- */
    memset(b, 0xAA, sizeof b);
    CHECK(vsnd_enc_hdr(b, sizeof b, VIRTIO_SND_R_PCM_INFO) == 4 && b[0] == 0x00 && b[1] == 0x01 && b[2] == 0 && b[3] == 0 && b[4] == 0xAA, "hdr bytes");
    CHECK(vsnd_enc_hdr(b, 3, VIRTIO_SND_R_PCM_INFO) == 0, "hdr needs 4 bytes");

    memset(b, 0xAA, sizeof b);
    CHECK(vsnd_enc_query_info(b, sizeof b, VIRTIO_SND_R_PCM_INFO, 0, 2, VSND_PINFO_SIZE, 2) == VSND_QINFO_SIZE, "query_info encodes");
    {
        static const uint8_t want[16] = { 0x00, 0x01, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 32, 0, 0, 0 };
        CHECK(memcmp(b, want, 16) == 0 && b[16] == 0xAA, "query_info bytes");
    }
    CHECK(vsnd_enc_query_info(b, 15, VIRTIO_SND_R_PCM_INFO, 0, 1, 32, 2) == 0, "query_info short buffer");
    CHECK(vsnd_enc_query_info(b, sizeof b, VIRTIO_SND_R_PCM_START, 0, 1, 32, 2) == 0, "query_info wrong code");
    CHECK(vsnd_enc_query_info(b, sizeof b, VIRTIO_SND_R_PCM_INFO, 1, 2, 32, 2) == 0, "start_id + count > total refused");
    CHECK(vsnd_enc_query_info(b, sizeof b, VIRTIO_SND_R_PCM_INFO, 0xFFFFFFFFu, 2, 32, 2) == 0, "start_id overflow refused");
    CHECK(vsnd_enc_query_info(b, sizeof b, VIRTIO_SND_R_PCM_INFO, 1, 0xFFFFFFFFu, 32, 0xFFFFFFFFu) == 0, "start_id + count wrap refused");
    CHECK(vsnd_enc_query_info(b, sizeof b, VIRTIO_SND_R_PCM_INFO, 0, 0, 32, 2) == 0, "zero count refused");
    CHECK(vsnd_enc_query_info(b, sizeof b, VIRTIO_SND_R_JACK_INFO, 0, 0x10000u, 0x10000u, 0x10000u) == 0, "response size overflow refused");
    CHECK(vsnd_query_resp_bytes(2, 32) == 68 && vsnd_query_resp_bytes(0, 32) == 0 && vsnd_query_resp_bytes(0xFFFFFFFFu, 0xFFFFFFFFu) == 0, "resp bytes");

    memset(b, 0xAA, sizeof b);
    CHECK(vsnd_enc_pcm_hdr(b, sizeof b, VIRTIO_SND_R_PCM_START, 1, 2) == VSND_PCM_HDR_SIZE, "pcm_hdr encodes");
    {
        static const uint8_t want[8] = { 0x04, 0x01, 0, 0, 1, 0, 0, 0 };
        CHECK(memcmp(b, want, 8) == 0 && b[8] == 0xAA, "pcm_hdr bytes");
    }
    CHECK(vsnd_enc_pcm_hdr(b, sizeof b, VIRTIO_SND_R_PCM_PREPARE, 0, 1) == 8 && vsnd_enc_pcm_hdr(b, sizeof b, VIRTIO_SND_R_PCM_RELEASE, 0, 1) == 8
          && vsnd_enc_pcm_hdr(b, sizeof b, VIRTIO_SND_R_PCM_STOP, 0, 1) == 8, "prepare/release/stop encode");
    CHECK(vsnd_enc_pcm_hdr(b, sizeof b, VIRTIO_SND_R_PCM_START, 2, 2) == 0, "pcm_hdr stream out of range");
    CHECK(vsnd_enc_pcm_hdr(b, sizeof b, VIRTIO_SND_R_PCM_SET_PARAMS, 0, 2) == 0, "pcm_hdr refuses set_params code");
    CHECK(vsnd_enc_pcm_hdr(b, 7, VIRTIO_SND_R_PCM_START, 0, 2) == 0, "pcm_hdr short buffer");

    memset(b, 0xAA, sizeof b);
    CHECK(vsnd_enc_pcm_xfer(b, sizeof b, 1, 2) == 4 && b[0] == 1 && b[1] == 0 && b[2] == 0 && b[3] == 0 && b[4] == 0xAA, "xfer bytes");
    CHECK(vsnd_enc_pcm_xfer(b, sizeof b, 2, 2) == 0 && vsnd_enc_pcm_xfer(b, 3, 0, 2) == 0, "xfer stream range / short buffer");

    /* ---- PCM_INFO decode ---- */
    put_info(b, 0, QEMU_FMTS, (1ull << 14) - 1, VIRTIO_SND_D_OUTPUT, 1, 2);
    CHECK(vsnd_dec_pcm_info(b, VSND_PINFO_SIZE, &info) == VSND_OK && info.formats == QEMU_FMTS && info.rates == 0x3FFFu
          && info.direction == 0 && info.channels_min == 1 && info.channels_max == 2, "QEMU pcm_info decodes");
    CHECK(vsnd_dec_pcm_info(b, VSND_PINFO_SIZE - 1, &info) == VSND_EPROTO, "short pcm_info refused");
    put_info(b, 1u << 5, QEMU_FMTS, 1, 0, 1, 2);
    CHECK(vsnd_dec_pcm_info(b, 32, &info) == VSND_EPROTO, "undefined feature bit refused");
    put_info(b, 0, 1ull << 25, 1, 0, 1, 2);
    CHECK(vsnd_dec_pcm_info(b, 32, &info) == VSND_EPROTO, "undefined format bit refused");
    put_info(b, 0, 1ull << 63, 1, 0, 1, 2);
    CHECK(vsnd_dec_pcm_info(b, 32, &info) == VSND_EPROTO, "format bit 63 refused");
    put_info(b, 0, 0, 1, 0, 1, 2);
    CHECK(vsnd_dec_pcm_info(b, 32, &info) == VSND_EPROTO, "empty format set refused");
    put_info(b, 0, QEMU_FMTS, 1ull << 14, 0, 1, 2);
    CHECK(vsnd_dec_pcm_info(b, 32, &info) == VSND_EPROTO, "undefined rate bit refused");
    put_info(b, 0, QEMU_FMTS, 0, 0, 1, 2);
    CHECK(vsnd_dec_pcm_info(b, 32, &info) == VSND_EPROTO, "empty rate set refused");
    put_info(b, 0, QEMU_FMTS, 1, 2, 1, 2);
    CHECK(vsnd_dec_pcm_info(b, 32, &info) == VSND_EPROTO, "direction 2 refused");
    put_info(b, 0, QEMU_FMTS, 1, 0, 0, 2);
    CHECK(vsnd_dec_pcm_info(b, 32, &info) == VSND_EPROTO, "channels_min 0 refused");
    put_info(b, 0, QEMU_FMTS, 1, 0, 3, 2);
    CHECK(vsnd_dec_pcm_info(b, 32, &info) == VSND_EPROTO, "channels_min > channels_max refused");
    for (int i = 0; i < 5; i++) {
        put_info(b, 0, QEMU_FMTS, 1, 0, 1, 2);
        b[27 + i] = 0x80;
        CHECK(vsnd_dec_pcm_info(b, 32, &info) == VSND_EPROTO, "non-zero padding byte refused");
    }

    /* full PCM_INFO response: hdr + 2 records */
    memset(b, 0, sizeof b);
    le32(b, VIRTIO_SND_S_OK);
    put_info(b + 4, 0, QEMU_FMTS, 0x3FFF, VIRTIO_SND_D_OUTPUT, 1, 2);
    put_info(b + 36, 0, QEMU_FMTS, 0x3FFF, VIRTIO_SND_D_INPUT, 1, 2);
    CHECK(vsnd_dec_pcm_info_resp(b, 68, 2, 32, infos) == VSND_OK && infos[0].direction == 0 && infos[1].direction == 1, "two-stream response");
    CHECK(vsnd_dec_pcm_info_resp(b, 67, 2, 32, infos) == VSND_EPROTO, "short used length refused");
    CHECK(vsnd_dec_pcm_info_resp(b, 69, 2, 32, infos) == VSND_EPROTO, "overlong used length refused");
    CHECK(vsnd_dec_pcm_info_resp(b, 3, 2, 32, infos) == VSND_EPROTO, "used length below header refused");
    CHECK(vsnd_dec_pcm_info_resp(b, 68, 2, 16, infos) == VSND_EARG, "record size below 32 refused");
    b[36 + 24] = 7;
    CHECK(vsnd_dec_pcm_info_resp(b, 68, 2, 32, infos) == VSND_EPROTO, "bad second record refused");
    le32(b, VIRTIO_SND_S_NOT_SUPP);
    CHECK(vsnd_dec_pcm_info_resp(b, 4, 2, 32, infos) == VSND_ENOTSUPP, "error status surfaces before length check");

    /* ---- response header / status codes ---- */
    le32(b, VIRTIO_SND_S_OK);       CHECK(vsnd_dec_resp_hdr(b, 4) == VSND_OK, "S_OK");
    le32(b, VIRTIO_SND_S_BAD_MSG);  CHECK(vsnd_dec_resp_hdr(b, 4) == VSND_EBADMSG, "S_BAD_MSG");
    le32(b, VIRTIO_SND_S_NOT_SUPP); CHECK(vsnd_dec_resp_hdr(b, 4) == VSND_ENOTSUPP, "S_NOT_SUPP");
    le32(b, VIRTIO_SND_S_IO_ERR);   CHECK(vsnd_dec_resp_hdr(b, 4) == VSND_EIO, "S_IO_ERR");
    le32(b, 0x8004u);               CHECK(vsnd_dec_resp_hdr(b, 4) == VSND_EPROTO, "unknown status 0x8004");
    le32(b, 0);                     CHECK(vsnd_dec_resp_hdr(b, 4) == VSND_EPROTO, "status 0");
    le32(b, VIRTIO_SND_R_PCM_INFO); CHECK(vsnd_dec_resp_hdr(b, 4) == VSND_EPROTO, "request code echoed as status");
    le32(b, 0x00008000u | 0xFFFF0000u); CHECK(vsnd_dec_resp_hdr(b, 4) == VSND_EPROTO, "high bits set on S_OK");
    le32(b, VIRTIO_SND_S_OK);       CHECK(vsnd_dec_resp_hdr(b, 3) == VSND_EPROTO && vsnd_dec_resp_hdr(0, 4) == VSND_EARG, "short / null header");

    /* ---- PCM I/O status ---- */
    uint32_t lat = 0;
    le32(b, VIRTIO_SND_S_OK); le32(b + 4, 4096);
    CHECK(vsnd_dec_pcm_status(b, 8, &lat) == VSND_OK && lat == 4096, "pcm_status OK with latency");
    le32(b, VIRTIO_SND_S_IO_ERR);
    CHECK(vsnd_dec_pcm_status(b, 8, &lat) == VSND_EIO, "pcm_status IO_ERR");
    le32(b, VIRTIO_SND_S_BAD_MSG);
    CHECK(vsnd_dec_pcm_status(b, 8, &lat) == VSND_EPROTO, "BAD_MSG is not legal in an I/O status");
    le32(b, VIRTIO_SND_S_NOT_SUPP);
    CHECK(vsnd_dec_pcm_status(b, 8, &lat) == VSND_EPROTO, "NOT_SUPP is not legal in an I/O status");
    le32(b, 0xDEADBEEFu);
    CHECK(vsnd_dec_pcm_status(b, 8, &lat) == VSND_EPROTO, "garbage I/O status");
    le32(b, VIRTIO_SND_S_OK);
    CHECK(vsnd_dec_pcm_status(b, 7, &lat) == VSND_EPROTO && vsnd_dec_pcm_status(b, 8, 0) == VSND_OK, "short status / null latency");

    /* ---- rx used length ---- */
    uint32_t fr = 99;
    CHECK(vsnd_rx_frames(8 + 1024, 1024, &fr) == VSND_OK && fr == 1024, "full rx period");
    CHECK(vsnd_rx_frames(8, 1024, &fr) == VSND_OK && fr == 0, "empty rx (end of stream)");
    fr = 99;
    CHECK(vsnd_rx_frames(8 + 1025, 1024, &fr) == VSND_EPROTO && fr == 99, "rx claims more than posted");
    CHECK(vsnd_rx_frames(7, 1024, &fr) == VSND_EPROTO && vsnd_rx_frames(0, 1024, &fr) == VSND_EPROTO, "rx below status size");
    CHECK(vsnd_rx_frames(0xFFFFFFFFu, 1024, &fr) == VSND_EPROTO, "rx huge length");

    /* ---- events ---- */
    struct vsnd_config ecfg = { .jacks = 1, .streams = 2, .chmaps = 0 };
    struct vsnd_event ev = { 0, 0 };
    le32(b, VIRTIO_SND_EVT_PCM_XRUN); le32(b + 4, 1);
    CHECK(vsnd_dec_event(b, 8, &ecfg, &ev) == VSND_OK && ev.type == VIRTIO_SND_EVT_PCM_XRUN && ev.data == 1, "xrun on stream 1");
    le32(b, VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED); le32(b + 4, 2);
    CHECK(vsnd_dec_event(b, 8, &ecfg, &ev) == VSND_EPROTO, "period elapsed on stream out of range");
    le32(b, VIRTIO_SND_EVT_JACK_CONNECTED); le32(b + 4, 0);
    CHECK(vsnd_dec_event(b, 8, &ecfg, &ev) == VSND_OK && ev.data == 0, "jack 0 connected");
    le32(b + 4, 1);
    CHECK(vsnd_dec_event(b, 8, &ecfg, &ev) == VSND_EPROTO, "jack id out of range");
    ecfg.jacks = 0; le32(b + 4, 0);
    CHECK(vsnd_dec_event(b, 8, &ecfg, &ev) == VSND_EPROTO, "jack event with no jacks");
    le32(b, VIRTIO_SND_S_OK);
    CHECK(vsnd_dec_event(b, 8, &ecfg, &ev) == VSND_EPROTO, "status code as event refused");
    le32(b, 0x1102u);
    CHECK(vsnd_dec_event(b, 8, &ecfg, &ev) == VSND_EPROTO, "unknown event type refused");
    le32(b, VIRTIO_SND_EVT_PCM_XRUN);
    CHECK(vsnd_dec_event(b, 7, &ecfg, &ev) == VSND_EPROTO, "short event refused");

    /* ---- SET_PARAMS ---- */
    put_info(b, (1u << VIRTIO_SND_PCM_F_EVT_XRUNS), QEMU_FMTS, 0x3FFF, 0, 1, 2);
    CHECK(vsnd_dec_pcm_info(b, 32, &info) == VSND_OK, "stream info for set_params");
    struct vsnd_pcm_params p = good_params();
    CHECK(vsnd_check_set_params(&p, &info, 2) == VSND_OK, "good params");
    memset(b, 0xAA, sizeof b);
    CHECK(vsnd_enc_pcm_set_params(b, sizeof b, &p, &info, 2) == VSND_SETP_SIZE, "set_params encodes");
    {
        static const uint8_t want[24] = { 0x01, 0x01, 0, 0, 0, 0, 0, 0, 0x00, 0x10, 0, 0, 0x00, 0x04, 0, 0,
                                          0, 0, 0, 0, 2, VIRTIO_SND_PCM_FMT_S16, VIRTIO_SND_PCM_RATE_48000, 0 };
        CHECK(memcmp(b, want, 24) == 0 && b[24] == 0xAA, "set_params bytes, padding 0");
    }
    CHECK(vsnd_enc_pcm_set_params(b, 23, &p, &info, 2) == 0, "set_params short buffer");
    p.features = 1u << VIRTIO_SND_PCM_F_EVT_XRUNS;
    CHECK(vsnd_check_set_params(&p, &info, 2) == VSND_OK, "offered feature accepted");

#define BAD(field, val, msg) do { p = good_params(); p.field = (val); CHECK(vsnd_check_set_params(&p, &info, 2) == VSND_EARG, msg); \
                                  CHECK(vsnd_enc_pcm_set_params(b, sizeof b, &p, &info, 2) == 0, msg " (encoder)"); } while (0)
    BAD(period_bytes, 0, "period 0");
    BAD(buffer_bytes, 0, "buffer 0");
    BAD(period_bytes, 1000, "buffer not a multiple of period");
    BAD(period_bytes, 8192, "period larger than buffer");
    BAD(format, VSND_PCM_FMT_MAX + 1, "format index 25");
    BAD(format, 0xFF, "format index 255");
    BAD(format, VIRTIO_SND_PCM_FMT_S24, "format not offered by stream");
    BAD(rate, VSND_PCM_RATE_MAX + 1, "rate index 14");
    BAD(rate, 0xFF, "rate index 255");
    BAD(channels, 0, "channels 0");
    BAD(channels, 3, "channels above channels_max");
    BAD(features, 1u << 5, "undefined feature");
    BAD(features, 1u << 31, "feature bit 31");
    BAD(features, 1u << VIRTIO_SND_PCM_F_SHMEM_HOST, "SHMEM_HOST placeholder");
    BAD(features, 1u << VIRTIO_SND_PCM_F_SHMEM_GUEST, "SHMEM_GUEST placeholder");
    BAD(features, VSND_PCM_F_SHMEM_MASK, "both SHMEM bits");
    BAD(features, 1u << VIRTIO_SND_PCM_F_MSG_POLLING, "feature not offered by stream");
    BAD(stream_id, 2, "stream id out of range");
    BAD(stream_id, 0xFFFFFFFFu, "stream id 0xFFFFFFFF");
#undef BAD
    p = good_params(); p.format = VIRTIO_SND_PCM_FMT_S24;
    CHECK(vsnd_check_set_params(&p, 0, 2) == VSND_OK, "without stream info only stream-independent rules apply");
    p.format = 25;
    CHECK(vsnd_check_set_params(&p, 0, 2) == VSND_EARG && vsnd_check_set_params(0, &info, 2) == VSND_EARG, "no info still checks ranges; null params");
    info.channels_min = 2; p = good_params(); p.channels = 1;
    CHECK(vsnd_check_set_params(&p, &info, 2) == VSND_EARG, "channels below channels_min");

    printf("virtio_snd_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
