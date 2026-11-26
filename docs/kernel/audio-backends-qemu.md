# QEMU ARM64 virt Audio Backends — Research Notes (AUDIO-04)

QEMU ARM64 virt can expose an input stream through virtio-sound. The guest driver implements playback. A local host probe validates a synthetic input source for future RX tests.

---

## 1. QEMU virtio-sound device

QEMU's `virtio-sound-device` (virtio ID 25) implements PCM capture and playback inside the guest using the host's configured audio backend. It is exposed over the modern virtio-mmio transport (as used by AuroraOS) with `-global virtio-mmio.force-legacy=false`.

Sources: [QEMU VirtIO Sound documentation](https://www.qemu.org/docs/master/system/devices/virtio/virtio-snd.html) and [audio backend options](https://www.qemu.org/docs/master/system/invocation.html).

### Device properties

| Property | Meaning | Status in QEMU 11.1 |
|---|---|---|
| `jacks` | Number of physical jacks | **Unimplemented** |
| `streams` | Number of PCM streams | Supported. AuroraOS checks the directions returned by PCM_INFO. |
| `chmaps` | Number of channel maps | **Unimplemented** |

### Stream properties

- Use PCM_INFO as the source of direction. The QEMU 11.1.0 suite observes out/in for two streams and out/out/in/in for four streams.
- QEMU reports a fixed PCM_INFO per stream: channels 1-2, formats S8|U8|S16|U16|S32|U32|FLOAT, all 14 rates, no features (matching what `tests/qemu/sound-tests.sh` expects: `1-2ch formats 0xe0078 rates 0x3fff features 0x0`).

### AuroraOS current usage (`tests/qemu/sound-tests.sh`)

```
-audiodev wav,id=snd0,path=<file> \
-global virtio-mmio.force-legacy=false \
-device virtio-sound-device,audiodev=snd0[,streams=N]
```

The WAV backend writes played-back audio to a disposable file under `build/sound-tests/`. Scenario 6 verifies "bring-up plays nothing: the WAV file holds only its 44-byte header" — confirming no PCM frames are sent during bring-up.

---

## 2. QEMU audio backends compatible with virtio-sound

QEMU audio backends are selected with `-audiodev <backend>,id=<name>` or the legacy `-audio driver=<backend>,model=virtio`.

| Backend | `-audiodev` name | Capture (input) support | Playback (output) support | Notes |
|---|---|---|---|---|
| WAV file | `wav` | **No** (output-only; writes to file) | Yes | Used by AuroraOS sound-tests.sh. File receives played PCM frames. |
| ALSA | `alsa` | Yes (if host has ALSA capture device) | Yes | Linux host only. Requires `alsa` audio library at QEMU build time. |
| PulseAudio | `pa` | Yes (if host PulseAudio has source) | Yes | Linux/Unix host. `server=` option to specify PulseAudio socket. |
| PipeWire | `pipewire` | Yes (if host PipeWire has source) | Yes | Modern Linux host. Requires PipeWire at QEMU build time. |
| SDL | `sdl` | Limited | Yes | Cross-platform; capture depends on SDL audio backend. |
| CoreAudio | `coreaudio` | Yes (if host has input device) | Yes | macOS host only. |

**QEMU option reference:** `-audiodev help` lists backends available on the host QEMU build.

---

## 3. Does virtio-sound support the RX (capture) queue?

**Yes.** QEMU's virtio-sound implements both the TX (playback) and RX (capture) virtqueues:

- **TX queue** (VIRTIO_SND_VQ_TX = 2): guest submits PCM buffers for playback; QEMU's audio backend consumes them and renders to the host output device (or writes to a WAV file).
- **RX queue** (VIRTIO_SND_VQ_RX = 3): QEMU's audio backend captures audio from the host input device and writes it into guest-submitted buffers; the guest is notified via the RX used ring when a period of capture data is available.

[QEMU device implementation](https://github.com/qemu/qemu/blob/master/hw/audio/virtio-snd.c)

**AuroraOS driver:** The current `kernel/drivers/virtio_snd.c` sets up all four queues (controlq, eventq, txq, rxq) during bring-up, but **does not yet use the RX queue** — no capture path is implemented ([the host fixture](audio-capture-fixture.md) verifies the synthetic source).

---

## 4. Limitations

1. **WAV backend does not prove capture possible.** The WAV backend is output-only: it receives PCM frames from the guest (playback) and writes them to a file. It does not capture from any host input device. A successful playback test with `-audiodev wav` tells nothing about whether capture works.

2. **Capture requires an input-capable backend.** To test capture, the QEMU command line needs a backend that can read from a host audio source (alsa, pa, pipewire, coreaudio), e.g. `-audiodev alsa,id=snd0`. QEMU needs the audio backend at build time and a configured input source at runtime. A PulseAudio FIFO source can supply synthetic frames without a hardware device.

3. **No host microphone access in CI.** Per project constraints, tests must not access the owner's real microphone. Capture testing uses controlled synthetic audio fixtures only (see AUDIO-06).

4. **AuroraOS driver does not implement capture.** The RX queue is set up but no `vsnd_capture()` or RX-queue submission exists. Capture remains future work (the host probe exercises the PulseAudio source).

---

## 5. Recommendation

For AuroraOS testing on QEMU ARM64 "virt":
- **Playback testing:** `-audiodev wav,id=snd0,path=<file>` (already used by sound-tests.sh)
- **Capture testing (future):** feed a controlled synthetic source through an input-capable backend. CI should never open a host microphone.
- **Do not assume capture works just because the device reports an input stream** — stream direction is reported by the device, but actual capture depends on the backend and host hardware.
