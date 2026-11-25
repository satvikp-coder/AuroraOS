#ifndef AURORA_VSND_H
#define AURORA_VSND_H
#include <stdint.h>
#include <virtio_snd.h>   /* for AUDIO_CAP_PLAY, AUDIO_CAP_RECORD */

/*
 * Kernel virtio-sound driver (kernel/drivers/virtio_snd.c): DEVICE BRING-UP + PCM PLAYBACK.
 * Finds a modern virtio-mmio sound device (ID 25) through the device tree, negotiates VIRTIO_F_VERSION_1 only, sets up the four queues,
 * reads the configuration and asks the device for PCM_INFO of every stream.
 * PCM playback: SET_PARAMS / START / STOP on the control queue; sine-wave tone generation submitted on the TX queue.
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

/* Start/stop playback on a configured stream. */
int vsnd_start(uint32_t stream_id);
int vsnd_stop(uint32_t stream_id);

/* Generate and play a sine wave tone at the given frequency (Hz) on an output stream for the given duration in milliseconds. */
int vsnd_play_tone(uint32_t stream_id, uint32_t freq_hz, uint32_t duration_ms);

/* Audio capability management (kernel code only). */
int vsnd_grant_audio(uint32_t principal, uint8_t caps);   /* grant audio capabilities to a principal; 0 on success, -1 on error */
int vsnd_revoke_audio(uint32_t principal);                 /* revoke all audio capabilities for a principal; returns count revoked */

/* ---- Fault injection for testing (test builds only) ---- */
/* Fault kinds:
 *   VSND_FAULT_MISSING_DEVICE   = 1  -- device not found / ENODEV
 *   VSND_FAULT_UNSUPPORTED_STREAM = 2  -- stream_id out of range or wrong direction
 *   VSND_FAULT_INVALID_FORMAT   = 3  -- format/rate/channels not supported by stream
 *   VSND_FAULT_MALFORMED_RESPONSE = 4  -- device returns malformed response
 *   VSND_FAULT_TIMEOUT          = 5  -- control/TX queue operation times out
 *   VSND_FAULT_OVERSIZED_COMPLETION = 6 -- device writes more than expected
 *   VSND_FAULT_QUEUE_CORRUPTION = 7  -- virtqueue used index / id corruption
 *   VSND_FAULT_DEVICE_RESET     = 8  -- device signals NEEDS_RESET/FAILED
 *
 * The `nth` parameter triggers the fault on the N-th call to the affected operation (1-based).
 * nth == 0 disables the fault. Only one fault kind can be active at a time.
 */
#define VSND_FAULT_MISSING_DEVICE       1
#define VSND_FAULT_UNSUPPORTED_STREAM   2
#define VSND_FAULT_INVALID_FORMAT       3
#define VSND_FAULT_MALFORMED_RESPONSE   4
#define VSND_FAULT_TIMEOUT              5
#define VSND_FAULT_OVERSIZED_COMPLETION 6
#define VSND_FAULT_QUEUE_CORRUPTION     7
#define VSND_FAULT_DEVICE_RESET         8

int vsnd_test_fault(int kind, uint32_t nth);
#endif
