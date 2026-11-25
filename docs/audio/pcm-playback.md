# virtio-sound PCM Playback Plan

## Overview
Planned virtio-sound backend for AuroraOS EL0 audio playback. Uses the
virtio-audio (or virtio-sound) device model in QEMU to route PCM streams
from user processes to the emulated audio hardware.

## Data Path
1. EL0 audio client opens `/dev/sound0` (virtio-sound front device)
2. Submits PCM frames (frame format: S16LE, 2 channels, 48 kHz) via virtio ring
3. Kernel virtio-sound driver moves data to ring buffer in shared memory
4. QEMU virtio-audio backend reads ring and drives `virtio-snd-card0`
5. Audio output to QEMU speaker emulation or host audio interface

## Plan Outline

### 1. Frontend (`kernel/drivers/virtio-sound.c`)
- virtio device initialization (split virtqueue setup)
- PCM format negotiation (S16LE, stereo, 48kHz)
- Ring buffer management (alloc, push, pop, drain)
- Open/close, prepare, commit, reset syscalls exposed via `SYS_SOUND`

### 2. Userspace API (`kernel/ui/sound-client.c` / `SYS_SOUND`)
- `audio_play_start(format)` — begin PCM playback
- `audio_play_frames(frames)` — write frames to ring
- `audio_play_stop()` — flush and disable
- `audio_get_status()` — return latency, available frames, sample rate

### 3. QEMU Integration
- Device tree: ` virtio,address` and ` virtio,irq` under `sound0`
- QEMU command: `-device virtio-sound-pci,device_index=1`
- Optional: `-audiodev wavetable,id=sound0 -device virtu so-sound-pci,id=sound`

### 4. Buffer & Latency
- Ring buffer size: 4096 frames (≈85 ms at 48kHz stereo)
- Period size: 1024 frames (≈21 ms)
- Interrupt triggered after each period write; kernel schedules next fill
- Under-run handling: return silence until next period

### 5. Safety & Isolation
- Per-process ring buffer ownership; hostile clients receive EACCES
- Buffer overflow detected via guard pages; fault contained to client process
- Sample rate mismatch between clients is not supported — mix at kernel level