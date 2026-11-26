#!/bin/sh
# Opt-in native RX probe. Requires a private PulseAudio process with the
# synthetic FIFO source and QEMU built with the PulseAudio backend.
set -eu
ROOT=$(CDPATH='' cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
if [ "$#" -eq 0 ]; then
    sh tests/tools/pulse-capture-fixture.sh sh "$ROOT/tests/qemu/sound-capture-tests.sh" private-fixture
    exit
fi
[ "$#" -eq 1 ] && [ "$1" = private-fixture ] && [ -p "${AURORA_PCM_FIFO:-}" ]
# An explicit private fixture is mandatory. Never select a default host source.
[ "$PULSE_SERVER" = "unix:${AURORA_PCM_FIFO%/*}/native" ]
mkdir -p "$ROOT/build"
out=$(mktemp -d "$ROOT/build/sound-capture.XXXXXX")
echo "Capture test artifacts: $out"
AURORA_SND_RX_TEST=1 BUILD_DIR="$out/kernel" sh ./build.sh >"$out/build.log" 2>&1 ||
    { tail -30 "$out/build.log"; exit 1; }
feed_pid=
qemu_pid=
cleanup()
{
    if [ -n "$feed_pid" ]; then kill "$feed_pid" 2>/dev/null || :; wait "$feed_pid" 2>/dev/null || :; fi
    if [ -n "$qemu_pid" ]; then kill "$qemu_pid" 2>/dev/null || :; wait "$qemu_pid" 2>/dev/null || :; fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# The test image looks for at least 64 samples of each polarity. Feed
# repeating 20 ms positive/negative blocks once QEMU opens the private source.
: >"$out/serial.log"
timeout 90 python3 - "$AURORA_PCM_FIFO" "$out/serial.log" <<'PY' >"$out/fixture-feed.log" 2>&1 &
from pathlib import Path
import struct
import subprocess
import sys
import time

# QEMU opens an input voice during device realization. Wait for the guest
# START acknowledgement so the fixture cannot drain during kernel boot.
serial = Path(sys.argv[2])
for _ in range(400):
    if b"sound capture input started" in serial.read_bytes() and subprocess.check_output(
        ["pactl", "list", "short", "source-outputs"]
    ).strip():
        break
    time.sleep(0.1)
else:
    raise SystemExit("Guest START or QEMU input connection did not appear")

print("Feeding the private source after guest START", flush=True)
periods = [struct.pack("<h", value) * 320 for value in (0x1234, -0x1234)]
try:
    with open(sys.argv[1], "wb", buffering=0) as pipe:
        for i in range(300):
            if pipe.write(periods[i % 2]) != len(periods[i % 2]):
                raise SystemExit("short fixture write")
            time.sleep(0.020)
except BrokenPipeError:
    pass
PY
feed_pid=$!

qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M \
    -kernel "$out/kernel/kernel.bin" -nodefaults -display none -monitor none -nic none \
    -serial "file:$out/serial.log" -global virtio-mmio.force-legacy=false \
    -audiodev "pa,id=fixture,server=$PULSE_SERVER,in.name=aurora_fixture,out.name=aurora_silence,in.mixing-engine=off,in.fixed-settings=off,in.latency=20000" \
    -device virtio-sound-device,audiodev=fixture,streams=2 >"$out/qemu.log" 2>&1 &
qemu_pid=$!
for i in $(seq 1 240); do
    if grep -q 'sound capture self-test passed' "$out/serial.log" &&
        grep -q 'Boot complete' "$out/serial.log"; then
        echo "PASS: AuroraOS RX transferred both exact synthetic PCM values"
        exit 0
    fi
    if grep -Eq 'SELFTEST FAILED|KERNEL PANIC' "$out/serial.log" ||
        ! kill -0 "$qemu_pid" 2>/dev/null; then break; fi
    sleep 0.25
done
tail -25 "$out/serial.log"
cat "$out/qemu.log" "$out/fixture-feed.log"
echo "FAIL: expected guest RX markers and completed boot" >&2
exit 1
