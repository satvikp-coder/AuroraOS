# Compositor-Owned Microphone Permission — Design (AUDIO-07)

Status: **Design documentation only.** No implementation. Describes how the compositor owns and mediates microphone access, following AuroraOS's existing capability/grant patterns.

---

## 1. Design goals

- The compositor (kernel UI, `kernel/ui/`) is the **sole owner** of microphone state. No EL0 process can access the microphone directly.
- Microphone access is a **grantable, revocable capability**, following the same pattern as `AUDIO_CAP_PLAY` / `AUDIO_CAP_RECORD` in `kernel/include/virtio_snd.h`.
- Grants are **bounded per-principal**, revoked on process exit, and validated against a reserved-principal rejection list.
- The microphone permission is **separate from** audio playback permission (`AUDIO_CAP_PLAY`) — holding playback permission does not imply capture permission.

## 2. Capability model

Following the existing `AUDIO_CAP_*` pattern (`kernel/include/virtio_snd.h` lines 29-31):

```c
#define AUDIO_CAP_PLAY   0x01u   /* playback: SET_PARAMS, START, STOP, play_tone */
#define AUDIO_CAP_RECORD 0x02u   /* capture: RX queue submission (future) */
#define AUDIO_CAP_MIC    0x04u   /* microphone: compositor-mediated mic access (this design) */
```

`AUDIO_CAP_MIC` is distinct from `AUDIO_CAP_RECORD`:
- `AUDIO_CAP_RECORD` = permission to use the RX queue (generic audio capture).
- `AUDIO_CAP_MIC` = compositor-owned permission to activate the microphone specifically (gate for the visible listening indicator).

A process needs **both** `AUDIO_CAP_RECORD` (to use the RX queue) and `AUDIO_CAP_MIC` (compositor approval to turn on the mic) to capture audio.

## 3. Grant/Revocation

The compositor (`kernel/ui/shell_run.c` or a dedicated `mic.c`) owns the grant table:

```c
int mic_grant(uint32_t principal);            /* grant AUDIO_CAP_MIC to a principal */
int mic_revoke(uint32_t principal);           /* revoke AUDIO_CAP_MIC for a principal */
int mic_check(uint32_t principal);            /* return 1 if principal holds AUDIO_CAP_MIC */
```

Rules (matching `vsnd_grant_audio` / `vsnd_revoke_audio`):
- Principal 0 (kernel) and `PRINCIPAL_NONE` (0xFFFFFFFF) are never grantable.
- Reserved range (`0x7F000000`–`0x7FFFFFFF`) is rejected.
- Grants are bounded (max 8 principals).
- **Revoked on process exit:** `proc_destroy()` and shell `reap()` call `mic_revoke(p->principal)`.
- **Revoked on compositor state change:** when the microphone is disabled (indicator off), all active mic grants are revoked.

## 4. Compositor mediation flow

1. EL0 process requests microphone access (via IPC or syscall).
2. Compositor checks if the process holds `AUDIO_CAP_RECORD` (audio capture permission).
3. If yes, compositor grants `AUDIO_CAP_MIC` and turns on the visible listening indicator (AUDIO-08).
4. When the process exits or releases the mic, the compositor revokes `AUDIO_CAP_MIC` and turns off the indicator.
5. The user can revoke at any time via the indicator UI, which revokes all `AUDIO_CAP_MIC` grants.

## 5. Why the compositor owns it

- The compositor controls the display and can draw the visible indicator (AUDIO-08).
- The compositor is the trusted UI boundary — it decides when the mic is active and ensures the indicator is visible whenever the mic is on.
- EL0 processes cannot bypass the compositor to activate the mic silently (no direct hardware access; all audio goes through virtio-sound RX queue which the compositor gates).

## 6. Integration with existing architecture

- Reuses `vsnd_check_audio_cap()`-style validation (kernel/include/virtio_snd.h).
- Follows the same grant/revoke lifecycle as `AUDIO_CAP_PLAY` (revoked in `proc_destroy()` and shell `reap()`).
- The microphone permission is checked at the compositor layer, not in the virtio-snd driver — the driver checks `AUDIO_CAP_RECORD` for RX queue access; the compositor checks `AUDIO_CAP_MIC` for mic activation.
