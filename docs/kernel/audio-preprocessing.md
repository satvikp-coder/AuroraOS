# Audio Preprocessing Pipeline — Design (AUDIO-10)

Status: **Design documentation only.** No implementation. Describes a fixed-point audio preprocessing pipeline for EL0 keyword spotting, fitting within AuroraOS's memory limits (32 pages = 128 KiB heap, 56 pages = 224 KiB address space).

---

## 1. Design goals

- **Fixed-point only.** No floating-point or NEON in the kernel (`-mgeneral-regs-only`, no NEON kernels exist). All processing in integer/fixed-point arithmetic.
- **No heap allocation.** Static buffers only; no `malloc` in the audio path.
- **Bounded latency.** Fixed-size frames, fixed processing budget per frame.
- **EL0-compatible.** Runs in an EL0 process within the 128 KiB heap limit.
- **Deterministic.** Same input → same output, for auditability and testing.

## 2. Pipeline stages

```
PCM capture (16 kHz, S16, mono)
    │
    ▼
[1] Frame buffer (fixed-size ring, 20 ms frames = 320 samples)
    │
    ▼
[2] Pre-emphasis filter (y[n] = x[n] - 0.97 * x[n-1], fixed-point Q12)
    │
    ▼
[3] Windowing (Hamming, fixed-point Q15 coefficients)
    │
    ▼
[4] Filter bank / energy bands (fixed number of mel-like bands, Q12)
    │
    ▼
[5] Log energy (lookup table or fixed-point approximation)
    │
    ▼
[6] Feature vector (static struct, ~13 coefficients)
    │
    ▼
[7] Keyword decision (tiny decision tree or template match → intent enum)
    │
    ▼
struct aura_intent (validated by aura_validate, submitted to broker)
```

## 3. Memory budget

| Component | Size |
|---|---|
| PCM frame buffer (20 ms @ 16 kHz S16 mono) | 640 bytes |
| Window coefficients (static, 320 × int16) | 640 bytes |
| Filter bank energies (e.g., 13 bands × uint16) | 26 bytes |
| Feature vector (13 × int16) | 26 bytes |
| Pre-emphasis state (1 × int16) | 2 bytes |
| Decision tree / templates (static const) | ~1–4 KiB |
| **Total static** | **~2–6 KiB** |

This fits comfortably within the 128 KiB EL0 heap limit and 24 KiB AuroraFS file limit.

## 4. Sample rate and format

- **Input:** 16 kHz, S16LE, mono — matches the QEMU virtio-sound default stream info (1-2 channels, S16 offered, 16 kHz rate offered).
- **Processing:** All rates normalized to 16 kHz internally; if the device provides a different rate, a fixed-point sample-rate converter (linear interpolation) would be added (future work).
- **Buffer:** One page (4 KiB) = ~250 ms of 16 kHz mono S16 audio — enough for one processing cycle plus margin.

## 5. Keyword spotting (not STT)

Per `docs/kernel/offline-speech.md`, general speech-to-text is infeasible on AuroraOS EL0. Instead:

- **Closed command set:** ≤20 templates (e.g., "open home", "timer 5 minutes", "theme dark").
- **Mechanism:** Fixed-point MFCC-like features + tiny decision tree or DTW/template match → `struct aura_intent` directly.
- **Output:** Deterministic intent enum (`AK_OPEN_SCREEN`, `AK_TIMER`, etc.), **not free text**.
- **No tokenizer, no decoder, no beam search.**

## 6. Integration with the intent system

The preprocessing pipeline output feeds directly into AuroraOS's existing intent path:

```
features → keyword match → AK_* intent enum → aura_validate(&intent, &caps)
    → broker IPC → bp_decide() → BP_ALLOW_NOW / BP_REVIEW / BP_DENY
    → (if S1) trusted review → rv_verify() → execution
```

`bp_decide()` re-validates every intent (including KWS-produced ones) — there is no privileged path for audio. See `docs/kernel/offline-speech.md` §4.

## 7. Constraints honored

- **No real microphone access:** Pipeline is designed for synthetic/test audio; real capture requires virtio-sound RX (not yet implemented, AUDIO-06).
- **No S3 execution:** Audio-triggered intents cannot reach S3 (destructive actions require tactile hold confirmation per safety rules).
- **No untrusted model output:** KWS produces intent enums directly, not free text. `aura_validate()` enforces the intent schema.
- **No Qwen3 dependency:** Preprocessing is fixed-point DSP, not neural inference.

## 8. Future work

- Fixed-point sample-rate converter (for non-16 kHz device input).
- VAD (voice activity detection) to gate processing on sound presence.
- Energy-based trigger to reduce CPU usage when silent.
- Integration with `vsnd_revoke_audio` / `AUDIO_CAP_RECORD` / `AUDIO_CAP_MIC` when capture is implemented.
