# Synthetic WAV Capture Fixture Test — Design and Limitations (AUDIO-06)

Status: **Design and fixture only.** No kernel capture implementation. Creates a controlled synthetic WAV fixture to exercise the capture path conceptually, and documents why real microphone capture is not tested.

---

## 1. Why a synthetic fixture?

Per project constraints:

- **Do NOT access owner's real microphone without authorization.**
- **Output-only WAV backend does NOT prove capture possible.**
- **Use controlled synthetic audio fixtures only.**
- **QEMU ARM64 virt: investigate actual capture capability, don't assume impossible.**

The QEMU `wav` backend used by `tests/qemu/sound-tests.sh` is output-only: it receives PCM frames from the guest (playback) and writes them to a file. It cannot capture from a host microphone. Therefore, a playback test with `-audiodev wav` proves nothing about capture.

To test the capture path without a real microphone, a synthetic WAV fixture provides known PCM data that the capture path would process — as if the device had delivered it on the RX queue.

## 2. Synthetic WAV fixture format

A minimal WAV file (44-byte header + PCM frames):

| Offset | Size | Field | Value (example) |
|---|---|---|---|
| 0 | 4 | RIFF marker | "RIFF" |
| 4 | 4 | File size - 8 | little-endian uint32 |
| 8 | 4 | Wave marker | "WAVE" |
| 12 | 4 | fmt marker | "fmt " |
| 16 | 4 | fmt chunk size | 16 |
| 20 | 2 | Audio format | 1 (PCM) |
| 22 | 2 | Channels | 1 (mono) or 2 (stereo) |
| 24 | 4 | Sample rate | 16000 |
| 28 | 4 | Byte rate | rate × channels × bytes_per_sample |
| 32 | 2 | Block align | channels × bytes_per_sample |
| 34 | 2 | Bits per sample | 16 |
| 36 | 4 | data marker | "data" |
| 40 | 4 | Data chunk size | bytes of PCM data |
| 44 | N | PCM data | interleaved samples |

**Fixture example:** 1 second of 16 kHz, 16-bit, mono sine wave at 440 Hz (A4) — 32,000 bytes of PCM data + 44-byte header = 32,044 bytes total.

## 3. How the fixture would be used

Since AuroraOS has no capture implementation yet, the fixture is a **design artifact**, not an executed test:

1. Generate `tests/fixtures/sine-440hz-16khz-mono-1s.wav` (synthetic, no microphone involved).
2. Document that a future capture test would feed this fixture's PCM data through the RX queue path (virtio-sound `VIRTIO_SND_VQ_RX`).
3. Verify the capture path correctly:
   - Accepts RX buffers on the RX queue
   - Decodes the PCM status trailer
   - Reports frame count via `vsnd_rx_frames()`
   - Rejects oversized used lengths (`used_len - 8 > frames_cap`)
   - Rejects used lengths below the 8-byte status size

## 4. Limitations

| Limitation | Explanation |
|---|---|
| No real capture | The fixture is synthetic data; no host microphone is involved. |
| No RX queue implementation | AuroraOS driver sets up rxq but never submits capture buffers. The fixture cannot be exercised end-to-end today. |
| WAV backend ≠ capture | Using `-audiodev wav` in QEMU does not provide capture data to the guest. A real capture test would need an input-capable backend (alsa, pa, pipewire) and host audio hardware. |
| Not a CI gate | Because no capture path exists, the fixture is documentation only and is not part of the automated test suite. |

## 5. Future capture test (when RX path is implemented)

When a capture API is added (e.g., `vsnd_capture()`), the test would:
1. Allocate a page for the capture buffer.
2. Submit it on the RX queue (VIRTIO_SND_VQ_RX).
3. Wait for the device to fill it with capture data and write the status trailer.
4. Validate `vsnd_rx_frames(used_len, buffer_bytes, &frames)` returns the expected frame count.
5. Free the page.

Until then, the synthetic WAV fixture remains a design artifact only.
