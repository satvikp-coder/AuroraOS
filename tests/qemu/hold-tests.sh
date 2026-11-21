#!/bin/sh
# tests/qemu/hold-tests.sh - the hold-to-confirm DEMO (Cycle 22): the REAL shell. One boot: open the Hold screen through Index by keyboard, then hold Enter and the
# pointer button for real time. Early release, a 1.5 s hold, key/pointer variants, leaving the button, Tab and Escape are driven by real QMP events; the four
# segments are checked as screendump pixels and the confirmation count from the guest log. The demo confirms nothing real (S3 stays disabled, hold_input stays 0).
# Built with AURORA_SHELL_TEXT_PCT=100 only to get the control-geometry log (100 percent is the shipped layout). QEMU is started and stopped by this script, by PID.
# Usage: ./tests/qemu/hold-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/hold-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
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
AURORA_SHELL_TEXT_PCT=100 BUILD_DIR="$OUT/k_$sc" ./build.sh >/dev/null 2>&1 || { bad "$sc: kernel build failed"; echo "hold-tests: $PASSES passed, $FAILS failed"; exit 1; }
L="$OUT/$sc.log"; : > "$L"; rm -f "$OUT/$sc.sock"
qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$OUT/k_$sc/kernel.bin" \
    -display none -serial file:"$L" -monitor none -qmp unix:"$OUT/$sc.sock",server=on,wait=off \
    -device ramfb $LEG -device virtio-keyboard-device -device virtio-tablet-device &
QPID=$!
python3 "$SCRIPT_DIR/hold_check.py" --qmp "$OUT/$sc.sock" --serial "$L" --tmp "$OUT" --timeout 420 > "$OUT/$sc.check.log" 2>&1
rc=$?; cat "$OUT/$sc.check.log"
if [ "$rc" -eq 0 ]; then ok "$sc: all checks passed"; else bad "$sc: checks failed (see $OUT/$sc.check.log)"; fi
stop_qemu
echo "hold-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
