# Synthetic PCM capture fixture

The local host probe sends generated PCM through a private PulseAudio server. It verifies an input source for AuroraOS capture tests without opening a hardware audio device. The optional QEMU probe below verifies synthetic PCM in a guest RX queue.

## Run the host probe

On Linux with PulseAudio, its command-line clients, Python 3, and GNU coreutils installed:

```sh
./tests/tools/pulse-capture-fixture.sh
```

The script starts a separate PulseAudio process with a Unix socket in a private temporary directory. It loads a null sink and a named FIFO source, `aurora_fixture`, using 16 kHz signed 16-bit little-endian mono PCM. The probe writes a generated 4,800-frame pattern to the FIFO, reads from `aurora_fixture` through `parec`, and checks that the complete pattern arrives byte for byte on a sample boundary. Temporary files and owned processes are removed on exit. The probe is for local validation and is outside the required CI jobs.

[PulseAudio module documentation](https://www.freedesktop.org/projects/pulseaudio/Documentation/User/Modules/)

## Run the guest RX probe

Use the same Linux dependencies as the host probe, plus the AArch64 cross-toolchain and a QEMU build that includes the PulseAudio backend. The guest probe was verified on QEMU 11.1.0.

```sh
./tests/qemu/sound-capture-tests.sh
```

The script starts a private PulseAudio server and builds an opt-in capture-test kernel. It waits for the guest to start capture before writing alternating signed PCM markers at 16 kHz. QEMU uses the format negotiated by the guest with input mixing disabled. The guest checks record authorization before and after the test grant, drains the preposted RX periods before release, and verifies at least 64 samples of each exact marker. The script also checks that the guest completed boot. It prints the path to the serial, QEMU, and fixture writer logs under `build/sound-capture.*`.

The capture test runs with networking disabled and uses synthetic PCM exclusively. Application capture, microphone permission prompts, a visible recording indicator, and speech remain separate work. This probe is outside the required CI jobs. For playback regression coverage, run `./tests/qemu/sound-tests.sh`.

[QEMU backend options](https://www.qemu.org/docs/master/system/invocation.html)
