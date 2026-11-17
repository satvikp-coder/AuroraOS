#!/bin/sh
# tests/qemu/clock-tests.sh - the native Clock screen (Cycle 19): the REAL shell (no test flags, no EL0 clients) on the REAL SYS_TIMER service.
# One boot: open Clock through Index by keyboard, choose a preset, Start, watch the countdown change on screen, Cancel, then start a 1 minute timer by
# pointer, leave for Home and let it finish off-screen; the completion toast must appear in the header, take no focus, and disappear. Real QMP key and
# pointer events and screendump pixels. QEMU is started and stopped only by this script, by PID. TCG only: no claim about real hardware.
# Usage: ./tests/qemu/clock-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/clock-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
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
BUILD_DIR="$OUT/k_$sc" ./build.sh >/dev/null 2>&1 || { bad "$sc: kernel build failed"; echo "clock-tests: $PASSES passed, $FAILS failed"; exit 1; }
L="$OUT/$sc.log"; : > "$L"; rm -f "$OUT/$sc.sock"
qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$OUT/k_$sc/kernel.bin" \
    -display none -serial file:"$L" -monitor none -qmp unix:"$OUT/$sc.sock",server=on,wait=off \
    -device ramfb $LEG -device virtio-keyboard-device -device virtio-tablet-device &
QPID=$!
python3 "$SCRIPT_DIR/clock_check.py" --qmp "$OUT/$sc.sock" --serial "$L" --tmp "$OUT" --scenario "$sc" --timeout 420 > "$OUT/$sc.check.log" 2>&1
rc=$?; cat "$OUT/$sc.check.log"
if [ "$rc" -eq 0 ]; then ok "$sc: all checks passed"; else bad "$sc: checks failed (see $OUT/$sc.check.log)"; fi
stop_qemu
echo "clock-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
