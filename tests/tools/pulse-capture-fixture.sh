#!/bin/sh
set -eu
umask 077

for program in pulseaudio pactl parec mktemp mkfifo timeout python3; do
    command -v "$program" >/dev/null || { printf 'Missing dependency %s\n' "$program" >&2; exit 1; }
done
fixture=$(mktemp -d /tmp/aurora-pcm.XXXXXX)
pulse_pid=
reader_pid=
cleanup()
{
    if [ -n "$reader_pid" ]; then kill "$reader_pid" 2>/dev/null || :; wait "$reader_pid" 2>/dev/null || :; fi
    if [ -n "$pulse_pid" ]; then kill "$pulse_pid" 2>/dev/null || :; wait "$pulse_pid" 2>/dev/null || :; fi
    rm -rf -- "$fixture"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

export PULSE_RUNTIME_PATH="$fixture/runtime"
export PULSE_STATE_PATH="$fixture/state"
export PULSE_COOKIE="$fixture/cookie"
export PULSE_SERVER="unix:$fixture/native"
mkdir "$PULSE_RUNTIME_PATH" "$PULSE_STATE_PATH"
cat > "$fixture/fixture.pa" <<EOF
.fail
load-module module-native-protocol-unix socket=$fixture/native auth-cookie=$PULSE_COOKIE
load-module module-null-sink sink_name=aurora_silence
load-module module-pipe-source source_name=aurora_fixture file=$fixture/input.pcm format=s16le rate=16000 channels=1
EOF
pulseaudio -nF "$fixture/fixture.pa" --daemonize=no --use-pid-file=no --exit-idle-time=-1 --disable-shm=yes > "$fixture/pulse.log" 2>&1 &
pulse_pid=$!

waited=0
while ! pactl list short sources 2>/dev/null | awk '$2 == "aurora_fixture" { found = 1 } END { exit !found }'; do
    if [ "$waited" -ge 50 ] || ! kill -0 "$pulse_pid" 2>/dev/null; then
        cat "$fixture/pulse.log" >&2
        exit 1
    fi
    sleep 0.1
    waited=$((waited + 1))
done

parec --device=aurora_fixture --raw --format=s16le --rate=16000 --channels=1 --latency-msec=50 > "$fixture/recorded.pcm" &
reader_pid=$!
timeout 10 python3 - "$fixture" <<'PY'
from pathlib import Path
import struct
import sys
import time

fixture = Path(sys.argv[1])
pattern = b"".join(struct.pack("<h", ((i * 13) % 4096) - 2048) for i in range(4800))
with (fixture / "input.pcm").open("wb", buffering=0) as source:
    source.write(pattern)
for _ in range(80):
    recording = (fixture / "recorded.pcm").read_bytes()
    offset = recording.find(pattern)
    if offset >= 0 and offset % 2 == 0:
        print("PASS: isolated PulseAudio fixture delivered 4800 exact PCM frames")
        break
    time.sleep(0.1)
else:
    raise SystemExit("FAIL: PulseAudio fixture did not deliver the expected PCM frames")
PY
