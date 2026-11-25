#!/bin/sh
# tests/qemu/sound-tests.sh — QEMU regression tests for the virtio-sound driver: DEVICE BRING-UP ONLY (no playback, no capture).
#
# The device is QEMU's virtio-sound-device on the modern virtio-mmio transport with the `wav` audio backend writing to a disposable
# file under build/sound-tests/. Scenarios:
#   1. no sound device: exactly "[snd] no sound device", normal boot.
#   2. default device (streams=2): the exact one-line description QEMU 11.1 produces (verified by running it), PCM_INFO used length 68.
#   3. streams=1 and streams=4: stream count, directions (first half output) and used length follow the property.
#   4. jacks=2,chmaps=1: the config counts are read from the device.
#   5. legacy transport: reported with the remedy, never driven; boot continues.
#   6. bring-up plays nothing: the WAV file holds only its 44-byte header.
#   7. hostile device answers, SIMULATED inside the driver by AURORA_SND_FAULT test builds (the real QEMU device behaves correctly;
#      the driver corrupts what it received before validating or withholds the request): no request -> bounded timeout,
#      non-zero padding, short used length, too many streams in config, unknown status code. Each must give exactly one
#      "device failed" line, reset the device, and let boot complete with the userspace suite passing.
# Every scenario must print exactly ONE [snd] line. Silent boots, timeouts or a panic are FAILURES.
#
# Usage: ./tests/qemu/sound-tests.sh      Requires the aarch64 toolchain and qemu-system-aarch64 (verified with QEMU 11.1.0).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1

EXPECTED_PROC_TESTS=128

OUT="$ROOT/build/sound-tests"
rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
expect()     { if grep -Eq -- "$2" "$1"; then ok "$3"; else bad "$3 (pattern: $2)"; fi; }
expect_not() { if grep -Eq -- "$2" "$1"; then bad "$3 (found: $2)"; else ok "$3"; fi; }

TIMED_OUT=0
# run_qemu kernel log [extra qemu args...]
run_qemu() {
    k="$1"; l="$2"; shift 2
    : > "$l"; TIMED_OUT=1
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$k" -nographic -serial file:"$l" -monitor none "$@" 2>>"$l.stderr" &
    pid=$!; i=0
    while [ "$i" -lt 240 ]; do
        if grep -qE "Boot complete|KERNEL PANIC|SELFTEST FAILED|Kernel halted" "$l" 2>/dev/null; then TIMED_OUT=0; break; fi
        kill -0 "$pid" 2>/dev/null || { TIMED_OUT=0; break; }
        sleep 0.5; i=$((i + 1))
    done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
}
finished() {
    if [ "$TIMED_OUT" -eq 1 ]; then bad "$1: timed out"; else ok "$1: finished without timing out"; fi
    if [ -s "$2" ]; then ok "$1: serial output produced"; else bad "$1: silent boot failure"; fi
    expect_not "$2" "KERNEL PANIC|SELFTEST FAILED" "$1: no panic and no failed self-test"
    expect "$2" "Boot complete" "$1: boot completed"
    expect "$2" "AURORA-PROC-SELFTEST: $EXPECTED_PROC_TESTS passed, 0 failed" "$1: userspace suite still passes"
}
# expect_snd log expected-line description: the [snd] lines of the log (CRs stripped) must EQUAL the one expected line.
expect_snd() {
    got="$(tr -d '\r' < "$1" | grep '^\[snd\]')"
    if [ "$got" = "$2" ]; then ok "$3"; else bad "$3 (want '$2', got '$(echo "$got" | tr '\n' '|')')"; fi
}
snd_args() { # wav-file [extra device properties]
    echo "-audiodev wav,id=snd0,path=$1 -global virtio-mmio.force-legacy=false -device virtio-sound-device,audiodev=snd0$2"
}
# QEMU 11.1 describes every stream identically except for the direction: 1-2 channels, formats S8|U8|S16|U16|S32|U32|FLOAT, all 14 rates, no features.
S="1-2ch formats 0xe0078 rates 0x3fff features 0x0"

echo "== building =="
BUILD_DIR="$OUT/normal" ./build.sh >/dev/null 2>&1 || { bad "normal build failed"; echo "sound-tests: $PASSES passed, $FAILS failed"; exit 1; }
NORMAL="$OUT/normal/kernel.bin"
for f in 1 2 3 4 5; do
    AURORA_SND_FAULT=$f BUILD_DIR="$OUT/fault$f" ./build.sh >/dev/null 2>&1 || { bad "fault-$f build failed"; echo "sound-tests: $PASSES passed, $FAILS failed"; exit 1; }
done
ok "normal and 5 simulated-fault kernels built"

echo "== 1. no sound device =="
log="$OUT/none.log"; run_qemu "$NORMAL" "$log"; finished none "$log"
expect_snd "$log" "[snd] no sound device" "none: exactly one line: no sound device"

echo "== 2. default device (streams=2) =="
log="$OUT/default.log"; run_qemu "$NORMAL" "$log" $(snd_args "$OUT/default.wav"); finished default "$log"
expect_snd "$log" "[snd] virtio-sound: 2 streams (jacks 0, chmaps 0): #0 out $S; #1 in $S; PCM_INFO used length 68" \
    "default: exactly one line, 2 streams (output then input), used length = 4 + 2*32"

echo "== 3. streams property =="
log="$OUT/streams1.log"; run_qemu "$NORMAL" "$log" $(snd_args "$OUT/s1.wav" ,streams=1); finished streams1 "$log"
expect_snd "$log" "[snd] virtio-sound: 1 streams (jacks 0, chmaps 0): #0 out $S; PCM_INFO used length 36" "streams=1: one output stream, used length 36"
log="$OUT/streams4.log"; run_qemu "$NORMAL" "$log" $(snd_args "$OUT/s4.wav" ,streams=4); finished streams4 "$log"
expect_snd "$log" "[snd] virtio-sound: 4 streams (jacks 0, chmaps 0): #0 out $S; #1 out $S; #2 in $S; #3 in $S; PCM_INFO used length 132" \
    "streams=4: first half output, second half input, used length 132"

echo "== 4. config counts come from the device =="
log="$OUT/jacks.log"; run_qemu "$NORMAL" "$log" $(snd_args "$OUT/j.wav" ,jacks=2,chmaps=1); finished jacks "$log"
expect "$log" "^\[snd\] virtio-sound: 2 streams \(jacks 2, chmaps 1\)" "jacks=2,chmaps=1: counts read from config space"

echo "== 5. legacy transport is refused =="
log="$OUT/legacy.log"; run_qemu "$NORMAL" "$log" -audiodev wav,id=snd0,path="$OUT/l.wav" -global virtio-mmio.force-legacy=true -device virtio-sound-device,audiodev=snd0
finished legacy "$log"
expect_snd "$log" "[snd] a virtio-sound device uses the LEGACY transport; only modern (version 2) is supported: add -global virtio-mmio.force-legacy=false" \
    "legacy: exactly one line, with the remedy"

echo "== 6. bring-up plays nothing =="
if [ -f "$OUT/default.wav" ] && [ "$(wc -c < "$OUT/default.wav")" -eq 44 ]; then ok "default: the WAV file is only its 44-byte header (no frames sent)"; else bad "default: unexpected WAV size $(wc -c < "$OUT/default.wav" 2>/dev/null)"; fi

echo "== 7. hostile device answers (SIMULATED in the driver by AURORA_SND_FAULT builds) =="
fault() { # n description expected-reason
    log="$OUT/fault$1.log"; run_qemu "$OUT/fault$1/kernel.bin" "$log" $(snd_args "$OUT/f$1.wav"); finished "fault$1" "$log"
    expect_snd "$log" "[snd] virtio-sound: device failed: $3; device reset, boot continues without sound" "fault$1 ($2): exactly one failure line, device reset"
}
fault 1 "request withheld, bounded timeout" "PCM_INFO timed out"
fault 2 "non-zero padding in a PCM_INFO record" "malformed PCM_INFO response"
fault 3 "used length one byte short" "malformed PCM_INFO response"
fault 4 "config claims 33 streams" "config counts unstable or above driver bounds"
fault 5 "unknown status code" "malformed PCM_INFO response"

echo "sound-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
