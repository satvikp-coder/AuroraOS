#ifndef AURORA_VSND_H
#define AURORA_VSND_H
#include <stdint.h>

/*
 * Kernel virtio-sound driver (kernel/drivers/virtio_snd.c): DEVICE BRING-UP ONLY.
 * Finds a modern virtio-mmio sound device (ID 25) through the device tree, negotiates VIRTIO_F_VERSION_1 only, sets up the four queues,
 * reads the configuration and asks the device for PCM_INFO of every stream. No playback, no capture, no EL0 access, no syscall.
 * Message layouts and decoders live in virtio_snd.h. Everything the device supplies is treated as hostile; a failing device is reset and
 * marked failed and boot continues.
 */
enum { SND_OK = 0, SND_ENODEV = -1, SND_ELEGACY = -2, SND_EPROTO = -3, SND_ETIMEOUT = -4, SND_ENOMEM = -5 };

/* Probe and bring up the first modern sound device. Prints exactly ONE "[snd] ..." line. Idempotent. */
int      vsnd_init(const void *fdt);
int      vsnd_present(void);               /* 1 once the device is live and its streams were described */
uint32_t vsnd_streams(void);               /* stream count from config space (0 if not present) */
#endif
