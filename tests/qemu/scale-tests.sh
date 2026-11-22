#!/bin/sh
# tests/qemu/scale-tests.sh - native text scaling and high contrast on the REAL shell under QEMU. Seven boots (sequential, one QEMU at a time):
# 200, 200 high contrast, 75, 150 high contrast, 125, 175 high contrast and 100 high contrast (all six scales, both contrast modes where listed). Each is driven by real QMP key and pointer events and checked on
# screendump pixels (scale_check.py). The appearance is selected by the test-only build flags AURORA_SHELL_TEXT_PCT / AURORA_SHELL_CONTRAST because
# no screen changes it yet; the shipped kernel starts at 100 percent, standard contrast. TCG only: no claim about real hardware.
# Usage: ./tests/qemu/scale-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/scale-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
QPID=""
stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT
trap 'stop_qemu; exit 130' INT TERM
LEG="-global virtio-mmio.force-legacy=false"
run_variant() {   # pct contrast
    pct="$1"; hc="$2"; name="v${pct}_${hc}"
    echo "== text scale $pct percent, high contrast $hc =="
    AURORA_SHELL_TEXT_PCT="$pct" AURORA_SHELL_CONTRAST="$hc" BUILD_DIR="$OUT/k_$name" ./build.sh >/dev/null 2>&1 || { bad "kernel build failed ($name)"; return; }
    L="$OUT/$name.log"; : > "$L"; rm -f "$OUT/$name.sock"
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$OUT/k_$name/kernel.bin" \
        -display none -serial file:"$L" -monitor none -qmp unix:"$OUT/$name.sock",server=on,wait=off \
        -device ramfb $LEG -device virtio-keyboard-device -device virtio-tablet-device &
    QPID=$!
    python3 "$SCRIPT_DIR/scale_check.py" --qmp "$OUT/$name.sock" --serial "$L" --tmp "$OUT" --pct "$pct" --contrast "$hc" --timeout 420 > "$OUT/$name.check.log" 2>&1
    rc=$?; cat "$OUT/$name.check.log"
    if [ "$rc" -eq 0 ]; then ok "$pct percent contrast $hc: all checks passed"; else bad "$pct percent contrast $hc: checks failed (see $OUT/$name.check.log)"; fi
    stop_qemu
}
run_variant 200 0
run_variant 200 1
run_variant 75 0
run_variant 150 1
run_variant 125 0
run_variant 175 1
run_variant 100 1
echo "scale-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
