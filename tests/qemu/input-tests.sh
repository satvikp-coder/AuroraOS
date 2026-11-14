#!/bin/sh
# tests/qemu/input-tests.sh - QEMU regression tests for native input (issue #7, stage G5: virtio keyboard and tablet).
#
# Proven here, with real guest execution:
#   * the kernel finds virtio-input devices by probing the device tree's virtio-mmio slots (never a hard-coded slot) and reads
#     their capabilities (keyboard bitmap, tablet axis ranges) from device configuration space;
#   * events injected with QMP input-send-event reach the guest (the guest's own serial log names each one) AND change the
#     framebuffer (QEMU screendump pixels are asserted); a QMP "return" alone is never accepted;
#   * keyboard only, tablet only, and no input device at all are all safe; a normal (non-test) kernel with input devices and a
#     disk still boots and the block driver still works (the virtio window mapping is shared).
# Disposable files only, under build/input-tests/. QEMU is started/stopped only by this script, by its own PID, on a private QMP socket.
# TCG delivers events instantly and has no caches: nothing here is a statement about real hardware.
#
# Usage: ./tests/qemu/input-tests.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/input-tests"
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

start_qemu() {   # kernel serial_log qmp_socket [extra args...]
    k="$1"; l="$2"; sock="$3"; shift 3
    : > "$l"; rm -f "$sock"
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$k" \
        -display none -serial file:"$l" -monitor none -qmp unix:"$sock",server=on,wait=off "$@" &
    QPID=$!
}
wait_boot() {
    i=0; n=$(( $2 * 2 ))
    while [ "$i" -lt "$n" ]; do
        if grep -qE "Boot complete|KERNEL PANIC|SELFTEST FAILED|Kernel halted" "$1" 2>/dev/null; then return 0; fi
        kill -0 "$QPID" 2>/dev/null || return 0
        sleep 0.5; i=$((i + 1))
    done
    return 1
}
LEG="-global virtio-mmio.force-legacy=false"

echo "== building =="
BUILD_DIR="$OUT/normal" ./build.sh >/dev/null 2>&1 || { bad "normal kernel build failed"; echo "input-tests: $PASSES passed, $FAILS failed"; exit 1; }
AURORA_INPUT_TEST=90 BUILD_DIR="$OUT/demo" ./build.sh >/dev/null 2>&1 || { bad "input demo kernel build failed"; echo "input-tests: $PASSES passed, $FAILS failed"; exit 1; }
AURORA_INPUT_TEST=6 BUILD_DIR="$OUT/short" ./build.sh >/dev/null 2>&1 || { bad "short input demo kernel build failed"; echo "input-tests: $PASSES passed, $FAILS failed"; exit 1; }
ok "normal kernel and both input-test kernels build"

run_mode() {   # name mode kernel timeout [qemu args...]
    name="$1"; mode="$2"; k="$3"; to="$4"; shift 4
    echo "== $name =="
    L="$OUT/$name.log"
    start_qemu "$k" "$L" "$OUT/$name.sock" "$@"
    python3 "$SCRIPT_DIR/input_check.py" --qmp "$OUT/$name.sock" --serial "$L" --tmp "$OUT" --mode "$mode" --timeout "$to" > "$OUT/$name.check.log" 2>&1
    rc=$?
    cat "$OUT/$name.check.log"
    if [ "$rc" -eq 0 ]; then ok "$name: all input checks passed"; else bad "$name: input checks failed (see $OUT/$name.check.log)"; fi
    stop_qemu
}

run_mode keyboard-and-tablet full "$OUT/demo/kernel.bin" 150 -device ramfb $LEG -device virtio-keyboard-device -device virtio-tablet-device
run_mode keyboard-only keyboard "$OUT/demo/kernel.bin" 150 -device ramfb $LEG -device virtio-keyboard-device
run_mode tablet-only tablet "$OUT/demo/kernel.bin" 150 -device ramfb $LEG -device virtio-tablet-device
run_mode no-input-device none "$OUT/short/kernel.bin" 100 -device ramfb $LEG
run_mode no-input-no-display none "$OUT/short/kernel.bin" 100

echo "== normal kernel: keyboard + tablet + ramfb + virtio disk all present =="
L="$OUT/normal-all.log"; IMG="$OUT/disk.img"; dd if=/dev/zero of="$IMG" bs=512 count=2048 2>/dev/null
start_qemu "$OUT/normal/kernel.bin" "$L" "$OUT/normal-all.sock" -device ramfb $LEG -drive file="$IMG",if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0 -device virtio-keyboard-device -device virtio-tablet-device
wait_boot "$L" 240; wb=$?
stop_qemu
if [ "$wb" -eq 0 ]; then ok "normal kernel with all devices: finished booting"; else bad "normal kernel with all devices: boot timed out"; fi
expect "$L" "^input: devices=2 keyboards=1 pointers=1" "normal kernel: finds the keyboard and the tablet"
expect "$L" "Block device ready" "normal kernel: the block driver still works beside the input driver"
expect "$L" "Boot complete" "normal kernel: boot completes"
expect_not "$L" "KERNEL PANIC|SELFTEST FAILED|Kernel halted" "normal kernel: no panic"

echo "== normal kernel: no input devices, no disk =="
L="$OUT/normal-none.log"
start_qemu "$OUT/normal/kernel.bin" "$L" "$OUT/normal-none.sock"
wait_boot "$L" 240; wb=$?
stop_qemu
if [ "$wb" -eq 0 ]; then ok "UART-only boot: finished booting"; else bad "UART-only boot: timed out"; fi
expect "$L" "^input: devices=0 keyboards=0 pointers=0" "UART-only boot: zero input devices is a normal result"
expect "$L" "Boot complete" "UART-only boot: boot completes"

echo "== normal kernel: legacy-transport input device is refused, not driven =="
L="$OUT/legacy.log"
start_qemu "$OUT/normal/kernel.bin" "$L" "$OUT/legacy.sock" -global virtio-mmio.force-legacy=true -device virtio-keyboard-device
wait_boot "$L" 240; wb=$?
stop_qemu
expect "$L" "LEGACY transport" "legacy virtio input device: reported"
expect "$L" "^input: devices=0" "legacy virtio input device: not used"
expect "$L" "Boot complete" "legacy virtio input device: boot completes"

echo "input-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
