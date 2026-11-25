# Audio and driver research (desk and local-tool research; no audio driver exists)

Status: **research only.** No audio code was written, no hardware was touched. Every statement says where it comes from. Items marked **UNVERIFIED** must be checked against the primary source before anyone relies on them.

## 1. What was verified on this machine (QEMU 11.1.0, WSL)
- `qemu-system-aarch64 -device help` lists **`virtio-sound-device` (bus virtio-bus)** and `virtio-sound-pci`. The `-device` list also shows `usb-audio`, Intel HDA, AC97 and ES1370 (PCI/USB devices AuroraOS has no bus driver for). The `virtio-bus` variant sits on the same virtio-mmio transport the kernel already drives for block and input.
- `qemu-system-aarch64 -audiodev help` lists the backends `none`, `wav`, `alsa`, `dbus`, `jack`, `oss`, `pa`, `pipewire`, `sdl`, `spice`. The **`wav` backend writes what the guest plays to a file** and **`none` discards it**. That means a QEMU-only test needs no speakers and no host sound server: play a known tone in the guest, then check the samples in the resulting WAV on the host.
- **Verified by booting AuroraOS (QEMU 11.1.0, `-machine virt,highmem=off`, driver `kernel/drivers/virtio_snd.c`, suite `tests/qemu/sound-tests.sh`):**
  - Working command line: `-audiodev wav,id=snd0,path=<file> -global virtio-mmio.force-legacy=false -device virtio-sound-device,audiodev=snd0[,streams=N][,jacks=J][,chmaps=C]`. The `virt` machine's device-tree virtio-mmio slots carry it; device ID reads 25, transport version 2. Without `force-legacy=false` the slot is version 1 (legacy) and the driver refuses it with a clear line.
  - `-device virtio-sound-device,help` lists `audiodev`, `streams` (default 2), `jacks` (default 0), `chmaps` (default 0) plus the generic virtio properties. Omitting `audiodev` still realises the device (default backend).
  - The device offers VIRTIO_F_VERSION_1; accepting VERSION_1 alone is accepted (FEATURES_OK sticks). QueueNumMax is at least 8 on all four queues (the driver uses 8).
  - Config space: `streams` = the property; `jacks` and `chmaps` also reflect the properties (2 and 1 when set), even though QEMU documents jacks/chmaps as unimplemented (JACK_INFO/CHMAP_INFO were not sent).
  - PCM_INFO for all streams answers S_OK with **used length exactly 4 + 32*streams** (36, 68, 132 for 1, 2, 4 streams). Every stream: hda_fn_nid 0, features 0, channels 1-2, formats `0xe0078` (S8, U8, S16, U16, S32, U32, FLOAT), rates `0x3fff` (all 14), padding 0. Directions: streams=2 -> out, in; streams=4 -> out, out, in, in (first half output, as the QEMU source reads).
  - With the `wav` backend QEMU prints `audio: Can not open 'virtio-sound.in' (no host audio driver)` on stderr (wav is output-only); harmless for bring-up. After a bring-up-only boot the WAV file is its 44-byte header: no frames were sent. **Playback has not been attempted; audio output is NOT verified.**
- Still not run: SET_PARAMS/PREPARE/START, tx/rx transfers, events, JACK_INFO/CHMAP_INFO, and QEMU's reset internals.

## 2. What the sources say
- QEMU documents the device in `docs/system/devices/virtio-snd.rst` ([github.com/qemu/qemu](https://github.com/qemu/qemu/blob/master/docs/system/devices/virtio-snd.rst)); a typical invocation pairs `-audiodev <backend>,id=X` with `-device virtio-sound-pci,audiodev=X`. The device was added in 2023 as an implementation of the virtio sound spec 1.2 ([patch series cover letter](https://patchwork.kernel.org/project/qemu-devel/cover/cover.1686238728.git.manos.pitsidianakis@linaro.org/mbox)). The documented properties include `streams`; `jacks` was listed as not implemented in the snapshots read. Re-read the current page before implementing.
- The virtio 1.2 specification (OASIS) defines the sound device with four virtqueues (control, event, tx, rx), a configuration space (jacks, streams, channel maps) and PCM stream control (set parameters, prepare, start, stop, release). The device ID and exact message layouts are **UNVERIFIED here**: they were not copied from the spec text in this session and must be read from it.

## 3. What AuroraOS has, and the cost of a sound driver
- Block (`kernel/drivers/virtio_blk.c`, 336 lines) and input (`virtio_input.c`, 181 lines) each carry **their own split-virtqueue ring code** (descriptor table, avail ring, used ring, `last_used`, `avail_idx`, device-supplied ids range-checked). `virtio_mmio.c` shares only the window discovery and mapping. A sound driver needs four queues; writing it as a third private copy would triple the number of places a ring-handling mistake can live.
- Recommendation: **extract one shared virtqueue module first**, pure and host-testable against a simulated device (hostile used-ring ids, wrapped indices, device lying about lengths), migrate block and input onto it with their existing QEMU suites as the regression net, and only then write the sound driver on top.
- DMA/buffer ownership: the kernel owns every buffer the device can touch; EL0 never receives a device-visible address.

## 4. Safety and privacy rules (fixed before any code)
- **Capture is a privacy capability.** Reading the microphone queue needs an explicit capability granted by kernel code, never by an application to itself, and a visible indicator that cannot be drawn over by a client (compositor-owned, like the Review modal). Playback needs a lesser capability and a volume ceiling set by the kernel.
- All device-supplied data (lengths, statuses, stream counts, jack counts, channel maps) is untrusted and bounded. A device that reports 2^32 streams gets refused, not trusted.
- Applications get a bounded, kernel-mediated PCM ring (a syscall or mapped ring with kernel-chosen size), never MMIO, never a raw queue.
- Audio output is attention-grabbing: it must respect the same Do-not-disturb / attention rules as visual attention, and must never be the only signal for anything (accessibility: no information is audio-only).

## 5. Test design (QEMU `wav` backend)
1. Driver boot test: device present, negotiated features logged, one playback stream configured; no device → boot continues silently (as with block and input).
2. Tone test: guest plays a generated 440 Hz tone for a fixed number of frames; the host reads the WAV and checks frame count, sample format and that the dominant frequency and amplitude are what was sent (a deterministic oracle, no listening).
3. Underrun and stop: stopping mid-stream releases the stream; the WAV ends at the last played frame.
4. Hostile device: a simulated device (host test of the shared virtqueue) returns bad ids/lengths; the driver refuses and keeps running.
5. Capability tests: an application without the capability gets a refusal and no queue activity; capture without the indicator path is refused.
Nothing in this plan claims audio quality, latency or real-hardware behaviour.

## 6. Real hardware (Galaxy F04, MT6765): not researched beyond the existing readiness note
`galaxy-f04-readiness.md` records NOT READY. Audio on that phone would go through the SoC's audio front-end and a codec/PMIC whose identity, registers and licensing are **UNVERIFIED** and unavailable without hardware access, which is out of bounds. No claim is made. The virtio-sound path is the only audio target with any evidence behind it.

## 7. Order of work
1. **Step 1a done:** `kernel/include/virtqueue.h` + `kernel/drivers/virtqueue.c` (index arithmetic and hostile used-ring checks: id range, id in flight, repeated id, length over the cap, used index ahead, 16-bit wrap) with `tests/kernel-host/virtqueue_test.c` (57 checks, 8 of 9 mutants killed, the 9th is an equivalent redundant bound). Not linked into the kernel yet. **Step 1b not done:** migrate block + input onto it. Original step: Shared virtqueue module with host tests and migration of block + input (no behaviour change; existing suites must pass byte-for-byte).
2. (Spec extract now in `docs/kernel/virtio-sound-spec-notes.md`; still to do: header + host test.) Read the virtio-snd section of the spec; write the message layouts into a header with a host test of the encoders/decoders.
3. Sound driver. **Bring-up done:** `kernel/drivers/virtio_snd.c` + `kernel/include/vsnd.h` (discover, VERSION_1-only negotiation, 4 queues, config, one polled PCM_INFO, one `[snd]` line; hostile answers reset the device and boot continues), `tests/qemu/sound-tests.sh` (simulated faults are injected inside the driver by `AURORA_SND_FAULT` builds, not by the QEMU device). Still to do: playback only, `wav` backend test (section 5, items 1-3).
4. Capability and indicator design review; only then capture.

## 8. Offline speech and microphone state (issue #13)

QEMU ARM64 `virt` exposes an input stream through `virtio-sound-device` when configured with two streams. The existing boot test verifies its direction through `PCM_INFO`. The WAV backend used by that test is output-only, and the current guest driver stops after stream discovery. Capturing samples therefore requires a capture-capable QEMU backend or a deterministic test source and an AuroraOS receive path.

Before an application can receive samples, the guest needs kernel-mediated capture authorization, receive-buffer ownership and a compositor-owned indicator tied to the active capture state. Verify that denial leaves capture inactive and that stop, process exit and device failure all clear the indicator. Speech recognition then needs a bounded offline engine and a repeatable input fixture. Speech synthesis can be tested separately with PCM playback and the existing WAV test strategy.
