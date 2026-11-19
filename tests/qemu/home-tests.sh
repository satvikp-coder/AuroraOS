#!/bin/sh
# tests/qemu/home-tests.sh - the REAL kernel (no test flags): the native Aurora Glass Home, Dock, Spaces and About modal under QEMU.
#   * keyboard + tablet + ramfb: home_check.py injects real QMP events and verifies guest log lines AND screendump pixels;
#   * display without input devices: Home still appears and the shell loop idles; input without a display: UART boot still completes;
#   * neither: plain UART boot as before. Also 128 MiB and 512 MiB guests show Home.
# Disposable files under build/home-tests/; QEMU started/stopped only by this script by PID. TCG only: no claim about real hardware.
# Usage: ./tests/qemu/home-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/home-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
expect()     { if grep -Eq -- "$2" "$1"; then ok "$3"; else bad "$3 (pattern: $2)"; fi; }
expect_not() { if grep -Eq -- "$2" "$1"; then bad "$3 (found: $2)"; else ok "$3"; fi; }
QPID=""
stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT
trap 'stop_qemu; exit 130' INT TERM
start_qemu() {   # kernel log sock ram [extra...]
    k="$1"; l="$2"; sock="$3"; ram="$4"; shift 4
    : > "$l"; rm -f "$sock"
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m "$ram" -kernel "$k" \
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

echo "== building the real kernel =="
BUILD_DIR="$OUT/k" ./build.sh >/dev/null 2>&1 || { bad "kernel build failed"; echo "home-tests: $PASSES passed, $FAILS failed"; exit 1; }
ok "real kernel builds"

echo "== interactive: ramfb + keyboard + tablet =="
L="$OUT/home.log"
start_qemu "$OUT/k/kernel.bin" "$L" "$OUT/home.sock" 256M -device ramfb $LEG -device virtio-keyboard-device -device virtio-tablet-device
python3 "$SCRIPT_DIR/home_check.py" --qmp "$OUT/home.sock" --serial "$L" --tmp "$OUT" --timeout 420 > "$OUT/home.check.log" 2>&1
rc=$?; cat "$OUT/home.check.log"
if [ "$rc" -eq 0 ]; then ok "interactive Home: all checks passed"; else bad "interactive Home: checks failed (see $OUT/home.check.log)"; fi
stop_qemu

echo "== display, no input devices =="
L="$OUT/noinput.log"
start_qemu "$OUT/k/kernel.bin" "$L" "$OUT/noinput.sock" 256M -device ramfb $LEG
wait_for "$L" "shell: ready keyboards=0 pointers=0" 240; r=$?
if [ "$r" -eq 0 ]; then ok "no input: Home is up and the shell reports no keyboard and no pointer"; else bad "no input: shell never became ready"; fi
python3 - "$OUT/noinput.sock" "$OUT/noinput.ppm" > "$OUT/noinput.shot.txt" 2>&1 <<'PY'
import sys, time
sys.path.insert(0, "tests/qemu")
from gfx_check import Qmp, read_ppm
q = Qmp(sys.argv[1], time.time() + 30)
# the guest logs "ready" just before it presents the frame, and a loaded CI runner can be slow: poll for the expected pixels instead of
# trusting one screenshot after a fixed sleep (the assertions themselves are unchanged)
end = time.time() + 25
while True:
    q.cmd("screendump", filename=sys.argv[2], format="ppm")
    w, h, d = read_ppm(sys.argv[2])
    px = lambda x, y: tuple(d[(y * w + x) * 3:(y * w + x) * 3 + 3])
    ok = (w, h) == (1024, 768) and px(500, 30) == (255, 255, 255) and px(500, 65) == (82, 98, 122) and px(410, 704) == (42, 104, 232)
    cursor = px(506, 384) == (20, 33, 54)
    if (ok and not cursor) or time.time() > end: break
    time.sleep(0.5)
print("noinput-screen", "OK" if ok and not cursor else "BAD")
PY
stop_qemu
expect "$OUT/noinput.shot.txt" "^noinput-screen OK" "no input: screenshot shows Home (white header, muted Horizon rail (limited input), active Home dock item) and no pointer cursor"
expect "$OUT/noinput.log" "Boot complete" "no input: boot completed before the interactive loop"
echo "== input devices, no display: UART boot completes and idles =="
L="$OUT/nodisplay.log"
start_qemu "$OUT/k/kernel.bin" "$L" "$OUT/nodisplay.sock" 256M $LEG -device virtio-keyboard-device -device virtio-tablet-device
wait_for "$L" "Boot complete" 240; r=$?
stop_qemu
if [ "$r" -eq 0 ]; then ok "no display: boot completes"; else bad "no display: boot did not complete"; fi
expect "$L" "gfx: disabled" "no display: graphics reported disabled"
expect_not "$L" "shell: " "no display: the shell never starts (no framebuffer)"
expect_not "$L" "KERNEL PANIC|SELFTEST FAILED" "no display: no panic"

echo "== UART only =="
L="$OUT/uart.log"
start_qemu "$OUT/k/kernel.bin" "$L" "$OUT/uart.sock" 256M
wait_for "$L" "Boot complete" 240; r=$?
stop_qemu
if [ "$r" -eq 0 ]; then ok "UART only: boot completes"; else bad "UART only: boot did not complete"; fi
expect_not "$L" "shell: " "UART only: no shell output"

for ram in 128M 512M; do
    echo "== Home at $ram =="
    L="$OUT/ram$ram.log"
    start_qemu "$OUT/k/kernel.bin" "$L" "$OUT/ram$ram.sock" "$ram" -device ramfb
    wait_for "$L" "shell: ready" 240; r=$?
    stop_qemu
    if [ "$r" -eq 0 ]; then ok "$ram guest: Home is up and the shell is ready"; else bad "$ram guest: shell never became ready"; fi
    expect_not "$L" "KERNEL PANIC|SELFTEST FAILED" "$ram guest: no panic"
done

echo "home-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
