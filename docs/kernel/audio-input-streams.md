# QEMU virtio-sound Input-Stream Availability — Investigation (AUDIO-05)

Status: **Read-only investigation.** No kernel changes. Documents whether QEMU's virtio-sound device on the ARM64 "virt" machine exposes input (capture) streams, and what that means for AuroraOS.

---

## 1. Does QEMU virtio-sound expose input streams?

The QEMU 11.1.0 regression suite observed these PCM_INFO directions. Read the reported direction instead of relying on stream position. [QEMU VirtIO Sound documentation](https://www.qemu.org/docs/master/system/devices/virtio/virtio-snd.html)

- `streams=1` → one output (playback) stream only
- `streams=2` → stream 0 = output (playback), stream 1 = input (capture)
- `streams=4` → streams 0 and 1 = output, streams 2 and 3 = input

This matches what AuroraOS's `tests/qemu/sound-tests.sh` verifies:

```
[snd] virtio-sound: 2 streams (jacks 0, chmaps 0): #0 out ...; #1 in ...; PCM_INFO used length 68
```

## 2. What the AuroraOS driver sees

The current driver (`kernel/drivers/virtio_snd.c`) reads PCM_INFO for all streams during bring-up and reports each stream's direction:

```c
console_puts(info[i].direction == VIRTIO_SND_D_OUTPUT ? " out " : " in ");
```

The message-layer header (`kernel/include/virtio_snd.h`) defines:
- `VIRTIO_SND_D_OUTPUT 0u` — playback stream
- `VIRTIO_SND_D_INPUT  1u` — capture/input stream

The PCM_INFO decoder (`vsnd_dec_pcm_info`) validates that direction is either OUTPUT or INPUT and rejects anything else.

## 3. Does an input stream mean capture actually works?

**Not necessarily.** The device reports stream directions in its config space/PCM_INFO, but actual capture depends on:

| Factor | Requirement |
|---|---|
| QEMU version | A virtio-sound build with an RX queue and an input-capable backend |
| QEMU audio backend | Must be input-capable (alsa, pa, pipewire, coreaudio). The `wav` backend is output-only. |
| Input source | An explicit host source or a synthetic source. Use a synthetic source in tests. |
| Guest driver | AuroraOS driver sets up the RX queue but does **not** yet submit capture buffers |

**Conclusion:** QEMU virtio-sound *can* expose input streams, and the AuroraOS driver can *detect* them, but capture is not yet functional in AuroraOS. The `wav` backend used by sound-tests.sh cannot capture.

## 4. Implications for AuroraOS

- **Playback (TX):** QEMU tests exercise SET_PARAMS through RELEASE and validate PCM samples in the output file.
- **Capture (RX):** Not implemented. The RX queue exists but no capture API exists. Synthetic fixture testing only (AUDIO-06).
- **Input-stream detection:** Works today — `vsnd_streams()` and the bring-up report show which streams are input vs output.
