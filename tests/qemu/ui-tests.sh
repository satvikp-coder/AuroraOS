#!/bin/sh
# tests/qemu/ui-tests.sh - QEMU tests for the native Tier 0 Clear demo (issue #7, stage G6): input -> focus/hit testing -> scene
# -> framebuffer. The kernel is built with AURORA_UI_DEMO=N; tests/qemu/ui_check.py injects real keyboard/pointer events over QMP and
# verifies the guest serial log AND QEMU screendump pixels (focus ring, active item, scrim, containment, no click-through, restore).
# Fallbacks: no input device and no display must boot and run the demo loop without a crash.
# Disposable files only, under build/ui-tests/; QEMU is started/stopped only by this script, by its own PID. TCG only: no statement
# about real hardware or performance.
#
# Usage: ./tests/qemu/ui-tests.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/ui-tests"
rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
expect()     { if grep -Eq -- "$2" "$1"; then ok "$3"; else bad "$3 (pattern: $2)"; fi; }
expect_not() { if grep -Eq -- "$2" "$1"; then bad "$3 (found: $2)"; else ok "$3"; fi; }

QPID=""
stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT
trap 'stop_qemu; exit 130' INT TERM
start_qemu() {
    k="$1"; l="$2"; sock="$3"; shift 3
    : > "$l"; rm -f "$sock"
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$k" \
        -display none -serial file:"$l" -monitor none -qmp unix:"$sock",server=on,wait=off "$@" &
    QPID=$!
}
wait_for() {   # log pattern seconds
    i=0; n=$(( $3 * 2 ))
    while [ "$i" -lt "$n" ]; do
        grep -qE -- "$2" "$1" 2>/dev/null && return 0
        grep -qE "KERNEL PANIC|SELFTEST FAILED" "$1" 2>/dev/null && return 1
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.5; i=$((i + 1))
    done
    return 1
}
LEG="-global virtio-mmio.force-legacy=false"

echo "== building =="
AURORA_UI_DEMO=150 BUILD_DIR="$OUT/demo" ./build.sh >/dev/null 2>&1 || { bad "UI demo kernel build failed"; echo "ui-tests: $PASSES passed, $FAILS failed"; exit 1; }
AURORA_UI_DEMO=5 BUILD_DIR="$OUT/short" ./build.sh >/dev/null 2>&1 || { bad "short UI demo kernel build failed"; echo "ui-tests: $PASSES passed, $FAILS failed"; exit 1; }
ok "UI demo kernels build"

echo "== interactive: keyboard + tablet + ramfb =="
L="$OUT/ui.log"
start_qemu "$OUT/demo/kernel.bin" "$L" "$OUT/ui.sock" -device ramfb $LEG -device virtio-keyboard-device -device virtio-tablet-device
python3 "$SCRIPT_DIR/ui_check.py" --qmp "$OUT/ui.sock" --serial "$L" --tmp "$OUT" --timeout 200 > "$OUT/ui.check.log" 2>&1
rc=$?
cat "$OUT/ui.check.log"
if [ "$rc" -eq 0 ]; then ok "interactive UI: all checks passed"; else bad "interactive UI: checks failed (see $OUT/ui.check.log)"; fi
stop_qemu

echo "== fallback: display but no input devices =="
L="$OUT/noinput.log"
start_qemu "$OUT/short/kernel.bin" "$L" "$OUT/noinput.sock" -device ramfb $LEG
wait_for "$L" "ui: demo done" 60; r=$?
stop_qemu
if [ "$r" -eq 0 ]; then ok "no input: the UI demo runs to completion"; else bad "no input: demo did not finish"; fi
expect "$L" "^ui: devices=0 keyboards=0 pointers=0" "no input: zero devices reported"
expect_not "$L" "KERNEL PANIC|SELFTEST FAILED" "no input: no panic"

echo "== fallback: input devices but no display =="
L="$OUT/nodisplay.log"
start_qemu "$OUT/short/kernel.bin" "$L" "$OUT/nodisplay.sock" $LEG -device virtio-keyboard-device -device virtio-tablet-device
wait_for "$L" "ui: demo done" 60; r=$?
stop_qemu
if [ "$r" -eq 0 ]; then ok "no display: the UI demo loop runs to completion"; else bad "no display: demo did not finish"; fi
expect "$L" "^gfx: disabled" "no display: graphics reported disabled"
expect_not "$L" "KERNEL PANIC|SELFTEST FAILED" "no display: no panic"

echo "ui-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
