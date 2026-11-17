#!/bin/sh
# tests/qemu/aura-timer-tests.sh - Aura timer commands end to end (Cycle 19): the REAL shell, the REAL broker and the REAL SYS_TIMER service. One boot:
# open Aura through Home by keyboard, type "start a 1 minute timer", see the Clock count down, refuse a second start, cancel, start a 25 minute timer,
# refuse a bare number and an out-of-range duration, then let a real 1 minute timer finish while the Clock is not on screen (toast). Real QMP key events
# and screendump pixels plus the guest's own "aura: shell ran" and "shell: clock" lines. QEMU is started and stopped only by this script. TCG only.
# Usage: ./tests/qemu/aura-timer-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/aura-timer-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
QPID=""
stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT
trap 'stop_qemu; exit 130' INT TERM
LEG="-global virtio-mmio.force-legacy=false"

sc=run
echo "== scenario: $sc =="
BUILD_DIR="$OUT/k_$sc" ./build.sh >/dev/null 2>&1 || { bad "$sc: kernel build failed"; echo "aura-timer-tests: $PASSES passed, $FAILS failed"; exit 1; }
L="$OUT/$sc.log"; : > "$L"; rm -f "$OUT/$sc.sock"
qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$OUT/k_$sc/kernel.bin" \
    -display none -serial file:"$L" -monitor none -qmp unix:"$OUT/$sc.sock",server=on,wait=off \
    -device ramfb $LEG -device virtio-keyboard-device -device virtio-tablet-device &
QPID=$!
python3 "$SCRIPT_DIR/aura_timer_check.py" --qmp "$OUT/$sc.sock" --serial "$L" --tmp "$OUT" --scenario "$sc" --timeout 420 > "$OUT/$sc.check.log" 2>&1
rc=$?; cat "$OUT/$sc.check.log"
if [ "$rc" -eq 0 ]; then ok "$sc: all checks passed"; else bad "$sc: checks failed (see $OUT/$sc.check.log)"; fi
stop_qemu
echo "aura-timer-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
