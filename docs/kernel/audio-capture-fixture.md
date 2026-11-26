# Synthetic PCM capture fixture

The local host probe sends generated PCM through a private PulseAudio server. It verifies an input source for future AuroraOS capture tests without opening a hardware audio device. The current guest configures the virtio-sound RX queue during discovery and still needs a capture implementation.

## Run the host probe

On Linux with PulseAudio, its command-line clients, Python 3, and GNU coreutils installed:

```sh
./tests/tools/pulse-capture-fixture.sh
```

The script starts a separate PulseAudio process with a Unix socket in a private temporary directory. It loads a null sink and a named FIFO source, `aurora_fixture`, using 16 kHz signed 16-bit little-endian mono PCM. The probe writes a generated 4,800-frame pattern to the FIFO, reads from `aurora_fixture` through `parec`, and checks that the complete pattern arrives byte for byte on a sample boundary. Temporary files and owned processes are removed on exit. The probe is for local validation and is outside the required CI jobs.

[PulseAudio module documentation](https://www.freedesktop.org/projects/pulseaudio/Documentation/User/Modules/)

## Connect a future guest capture test

The same module setup can supply an audio source for a future end-to-end test on QEMU builds with the PulseAudio backend. Launch QEMU while that server is running and leave the source writer paused until the guest has started capture. Use the private server socket and named source and sink explicitly.

```sh
-audiodev pa,id=snd0,server=unix:/path/to/private/native,in.name=aurora_fixture,out.name=aurora_silence
-global virtio-mmio.force-legacy=false
-device virtio-sound-device,audiodev=snd0,streams=2
```

[QEMU backend options](https://www.qemu.org/docs/master/system/invocation.html)

Once RX submission is implemented, the guest test can select an input stream through `PCM_INFO`, queue writable buffers before starting capture, and feed the same known sample pattern from the host. The used length must include the eight-byte status trailer. Subtract the trailer to obtain the PCM byte count, reject counts beyond the buffer and counts that are not frame-aligned, and compare decoded frames with the expected pattern. Add cases for short completions, malformed status, and a timeout while the device owns the buffer.

The QEMU WAV backend writes guest playback to a host file. The host probe above verifies the synthetic input source. End-to-end guest capture awaits the RX implementation.
