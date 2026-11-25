# Offline Speech (STT/TTS) on AuroraOS — Design Notes

**Status:** Blocked on hardware (no microphone in QEMU ARM64 virt). Documenting the architecture so work can resume when hardware is available.

---

## 1. Why Native STT/TTS Is Not Feasible on AuroraOS EL0

### 1.1 Memory Constraints

AuroraOS EL0 processes are limited to **224 KiB of page-table backed memory** (see `kernel/include/arch.h` and `kernel/mm.c`). This cap is a hard architectural limit: the EL0 page table has a fixed number of entries, and the kernel refuses to map more.

| Model | Approx. Size (INT8 quantized) | Fits in 224 KiB? |
|-------|-------------------------------|------------------|
| Qwen3-0.6B | ~500 MB | ❌ No (2,200× over budget) |
| Whisper tiny | ~75 MB | ❌ No (330× over budget) |
| Vosk small (EN) | ~40 MB | ❌ No (180× over budget) |
| Keyword spotter (custom, 5–10 words) | ~50–100 KiB | ✅ Yes |

**Conclusion:** Full STT (speech-to-text) and TTS (text-to-speech) models are impossible in EL0. Even the smallest production models exceed the page cap by two orders of magnitude.

### 1.2 Compute Constraints

AuroraOS runs on ARM64 without NEON/SVE guarantees in the base QEMU `virt` machine. EL0 has no access to GPU/NPU acceleration. Real-time inference on audio (16 kHz, 16-bit mono = 32 KB/s) would saturate the CPU and starve the shell, scheduler, and broker.

### 1.3 Audio Capture Hardware

The QEMU `virt` machine exposes **no microphone** (`virtio-sound` input streams). The virtio-snd driver (`kernel/drivers/virtio_snd.c`) discovers streams via `PCM_INFO`; `direction == VIRTIO_SND_D_INPUT` (value 1) indicates capture capability, but the emulated device reports zero input streams.

```c
// virtio_snd.c:225 — log shows direction
console_puts(info[i].direction == VIRTIO_SND_D_OUTPUT ? " out " : " in ");
```

Until a microphone is wired into the VM (or real hardware is used), **audio capture cannot be tested end-to-end**.

---

## 2. What CAN Be Done Within AuroraOS Constraints

### 2.1 Keyword Spotting (KWS) — Feasible

A tiny, fixed-vocabulary keyword spotter (5–10 commands: "home", "back", "open settings", "louder", "quieter") can fit in **< 100 KiB** using:
- Quantized depthwise-separable CNN or tiny RNN (e.g., 2-layer GRU, 32 units)
- MFCC frontend (13 coeffs × 30 frames = 390 inputs)
- INT8 inference, no dynamic allocation

**Integration path:**
1. Virtio-snd RX queue (`VIRTIO_SND_VQ_RX`, index 3) receives PCM frames from the device.
2. A **kernel-space** capture buffer (not EL0) accumulates 30-frame windows (≈ 300 ms at 100 fps frame rate).
3. KWS runs in the kernel (EL1) on a work queue or dedicated core — **never in EL0**.
4. On keyword match, the kernel emits a **typed intent** directly into the Action Broker (`kernel/broker_kernel.h`), bypassing EL0 entirely.

> **Why kernel-space?** EL0 cannot hold the model weights. The kernel already owns the audio device and the broker; keeping the hot path in EL1 avoids copy overhead and respects the page cap.

### 2.2 Fixed-Command Recognition — Feasible

Instead of open-ended STT, define a **closed grammar** of ~20 shell commands:
- Navigation: `home`, `back`, `open <app>`, `close`
- Volume: `volume up`, `volume down`, `mute`
- Timer: `timer <n> minutes`, `cancel timer`
- Settings: `open settings`, `increase text size`, `high contrast`

Recognition is a **template match** on MFCC dtw or a tiny classifier — same footprint as KWS.

### 2.3 Audio Capture via virtio-sound — Already Implemented

The driver (`virtio_snd.c`) sets up:
- `VIRTIO_SND_VQ_CONTROL` (0) — parameter negotiation (`PCM_SET_PARAMS`, `PCM_PREPARE`, `START`, `STOP`)
- `VIRTIO_SND_VQ_EVENT` (1) — jack connect/disconnect, period elapsed, xrun
- `VIRTIO_SND_VQ_TX` (2) — **playback** (working, tested)
- `VIRTIO_SND_VQ_RX` (3) — **capture** (code path exists, untested for lack of device)

To enable capture:
1. Device must report an input stream (`direction == VIRTIO_SND_D_INPUT`).
2. Driver sends `PCM_SET_PARAMS` with `channels=1`, `rate=16000` (VIRTIO_SND_PCM_RATE_16000), `format=S16_LE` (VIRTIO_SND_PCM_FMT_S16).
3. Driver sends `PCM_PREPARE`, then `PCM_START`.
4. Device writes frames into guest buffers on RX queue; each completion includes an 8-byte `virtio_snd_pcm_status` trailer (see `virtio_snd.h:390`).
5. Kernel copies frames into a ring buffer for the KWS worker.

### 2.4 TTS — Host-Side Only

TTS requires even larger models. The practical path:
- Shell sends a **text string** to the host (via a virtio-console or vsock channel).
- Host runs a full TTS engine (Piper, Coqui, system TTS) and plays audio locally.
- No AuroraOS kernel or EL0 changes needed.

---

## 3. Integration with the Action Broker

### 3.1 Current Broker Flow (Typed Commands)

The shell's Aura screen parses typed text → `aura_parse()` → intent → policy check → S1 validation → trusted review → narrow S0 executor.

Key files:
- `kernel/ui/shell_run.c:150` — `aura_glue()` runs the broker for typed input.
- `kernel/broker_kernel.h` — `broker_env()`, `broker_replay()`, `broker_drain()`.

### 3.2 Proposed Voice Path

```
[Microphone] → [virtio-snd RX] → [Kernel ring buffer] → [KWS worker (EL1)]
                                                    │
                                                    ▼
                                          [Keyword match?]
                                             │         │
                                            Yes        No
                                             │         │
                                             ▼         ▼
                              [Emit typed intent]  [Discard frame]
                              into broker queue
                                             │
                                             ▼
                              [Broker validates] → [Review] → [Execute]
```

**Critical property:** The intent emitted by the KWS worker is **identical in structure** to a typed command parsed by `aura_parse()`. It carries:
- `intent.kind` (e.g., `BP_NAVIGATE`, `BP_SETTINGS`, `BP_TIMER`)
- `intent.arg` (validated, bounded)
- `requester = BP_SHELL_PID` (kernel shell, pid 0)
- `nonce` (monotonic, from `shell_nonce`)

Because the intent is **constructed in the kernel** from a fixed enum (not free text), there is **no parsing ambiguity** and **no injection surface**.

### 3.3 Security Property: Intent Cannot Be Modified Between Validation and Execution

The Action Broker already enforces this for typed commands (see `kernel/broker_kernel.c` and `aura_s1.c`):

1. **Validation** (`aura_parse` + policy) produces an immutable `bp_intent`.
2. **Review** presents the *exact* intent to the user (Trusted Review screen).
3. **Execution** (`bp_exec`) re-reads the intent from the broker's replay log (`broker_replay()`) — the **same bytes** that were reviewed.

For voice:
- KWS emits a **pre-validated intent** (from a fixed table).
- The review screen shows: "Run *volume up*?" — same as typed.
- Execution reads from replay log — **identical code path**.

No EL0 process touches the intent. No string parsing occurs after review. The property holds.

---

## 4. Alternative: Host-Side STT with Typed Command Fallback

When a microphone is available on the host (but not passed to the VM):

1. Host runs STT (Whisper.cpp, Vosk, platform API).
2. Host sends recognized text → AuroraOS via virtio-console / vsock.
3. AuroraOS shell feeds text into **existing Aura parser** (`aura_glue()`).
4. Flow continues: policy → review → execute.

**Advantages:**
- Unlimited model size on host.
- Zero kernel changes.
- Same security properties (review sees parsed intent, not raw text).

**Disadvantage:** Requires host daemon; not "offline" from the user's perspective.

---

## 5. Implementation Checklist (When Hardware Arrives)

| Task | File | Notes |
|------|------|-------|
| Add RX queue submit/complete in `virtio_snd.c` | `kernel/drivers/virtio_snd.c` | Mirror TX path; use `vsnd_enc_pcm_xfer` + `vsnd_rx_frames` |
| Expose capture stream config (rate, format) | `kernel/include/virtio_snd.h` | Already defined; just unused |
| Kernel ring buffer for PCM frames | New: `kernel/audio/capture_ring.c` | Lockless SPSC; KWS worker consumes |
| KWS model (INT8, < 100 KiB) | New: `kernel/audio/kws_model.h` | Generated offline; embedded as `const uint8_t[]` |
| KWS inference (EL1 work queue) | New: `kernel/audio/kws_worker.c` | Runs on timer tick or dedicated core |
| Emit intent to broker | `kernel/broker_kernel.c` | Add `broker_emit_voice_intent(kind, arg)` |
| Review screen string table for voice intents | `kernel/ui/shell.c` | Reuse existing `aura_kind_name()` |

---

## 6. References

- `kernel/drivers/virtio_snd.c` — PCM playback (TX) bring-up; RX path stubbed.
- `kernel/include/virtio_snd.h` — Wire layouts, `VIRTIO_SND_D_INPUT`, `vsnd_rx_frames()`.
- `kernel/include/passport.h` — Principal/capability model (shell = `PRINCIPAL_SHELL`).
- `kernel/ui/shell_run.c` — `aura_glue()`, `shell_nonce`, broker integration, trusted review.
- `docs/kernel/virtio-sound-spec-notes.md` — Virtio 1.2 §5.14 reference.
- Issue #13 — "Local STT/TTS engine".

---

*Generated for AuroraOS `feature/offline-speech-stt` branch. No secrets or credentials included.*