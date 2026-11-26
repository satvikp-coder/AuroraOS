#ifndef AURORA_VSND_H
#define AURORA_VSND_H
#include <stdint.h>
#include <virtio_snd.h>   /* for AUDIO_CAP_PLAY, AUDIO_CAP_RECORD */

/*
 * Kernel virtio-sound driver (kernel/drivers/virtio_snd.c): DEVICE BRING-UP + PCM PLAYBACK.
 * Finds a modern virtio-mmio sound device (ID 25) through the device tree, negotiates VIRTIO_F_VERSION_1 only, sets up the four queues,
 * reads the configuration and asks the device for PCM_INFO of every stream.
 * PCM playback: SET_PARAMS / PREPARE / START / STOP / RELEASE on the control queue; sine-wave tone generation submitted on the TX queue.
 * Message layouts and decoders live in virtio_snd.h. Everything the device supplies is treated as hostile; a failing device is reset and
 * marked failed and boot continues.
 */
enum { SND_OK = 0, SND_ENODEV = -1, SND_ELEGACY = -2, SND_EPROTO = -3, SND_ETIMEOUT = -4, SND_ENOMEM = -5, SND_EPERM = -6 };

/* Probe and bring up the first modern sound device. Prints exactly ONE "[snd] ..." line. Idempotent. */
int      vsnd_init(const void *fdt);
int      vsnd_present(void);               /* 1 once the device is live and its streams were described */
uint32_t vsnd_streams(void);               /* stream count from config space (0 if not present) */

/* PCM playback API (valid only when vsnd_present() == 1). All parameters are validated against the stream's PCM_INFO. */
/* Configure stream: format (VIRTIO_SND_PCM_FMT_*), rate (VIRTIO_SND_PCM_RATE_*), channels, buffer/period bytes, features. */
int vsnd_set_params(uint32_t stream_id, uint32_t format, uint32_t rate, uint32_t channels,
                    uint32_t buffer_bytes, uint32_t period_bytes, uint32_t features);

/* Transition a configured playback stream. */
int vsnd_prepare(uint32_t stream_id);
int vsnd_release(uint32_t stream_id);
int vsnd_start(uint32_t stream_id);
int vsnd_stop(uint32_t stream_id);

/* Submit a 1â€“21 ms tone at 1â€“24000 Hz to a started stereo S16/48 kHz stream. The tone length must fit its configured period. */
int vsnd_play_tone(uint32_t stream_id, uint32_t freq_hz, uint32_t duration_ms);

/* Audio capability management (kernel code only). */
int vsnd_grant_audio(uint32_t principal, uint8_t caps);   /* grant audio capabilities to a principal; 0 on success, -1 on error */
int vsnd_revoke_audio(uint32_t principal);                 /* revoke all audio capabilities for a principal; returns count revoked */

#ifdef AURORA_SND_PLAY_TEST
int vsnd_playback_selftest(void);
#endif
#ifdef AURORA_SND_RX_TEST
int vsnd_capture_selftest(void);
#endif
#endif
