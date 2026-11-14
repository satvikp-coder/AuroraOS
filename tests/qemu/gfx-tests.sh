#!/bin/sh
# tests/qemu/gfx-tests.sh - QEMU regression tests for native graphics (issue #7, stage G2: fw_cfg + ramfb).
#
# What is proven, and how:
#   * The AuroraOS kernel itself finds fw_cfg in the device tree, finds etc/ramfb in the fw_cfg directory, reserves a contiguous
#     framebuffer from its page allocator, configures ramfb through a fw_cfg DMA write and draws a diagnostic pattern.
#   * QEMU's own `screendump` (over QMP, headless: -display none) is then parsed by tests/qemu/gfx_check.py: the image must be
#     1024x768 (not the 640x480 placeholder QEMU shows with no guest software) and ~55 exact pixels plus exact per-colour pixel
#     counts must match. A serial line alone is never accepted as proof.
#   * Missing display: a normal boot (no -device ramfb) must report graphics disabled and keep every other service working.
#   * RAM sizes 128 / 256 / 512 MiB: the framebuffer is the top 3 MiB of RAM each time and the screenshot still verifies.
# Everything uses disposable files under build/gfx-tests/. QEMU is started and stopped only by this script (by its own PID),
# on a private QMP socket. No physical device, host disk, network port or other process is touched.
#
# Environment: Linux with aarch64-linux-gnu-gcc, qemu-system-aarch64 (>= 8 for QMP screendump format=ppm) and python3.
# TCG does not model caches, so a pass here says nothing about real-hardware cache coherence (see docs/kernel/native-graphics-plan.md).
#
# Usage: ./tests/qemu/gfx-tests.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1

OUT="$ROOT/build/gfx-tests"
rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
expect()     { if grep -Eq -- "$2" "$1"; then ok "$3"; else bad "$3 (pattern: $2)"; fi; }
expect_not() { if grep -Eq -- "$2" "$1"; then bad "$3 (found: $2)"; else ok "$3"; fi; }

# reserved page count printed by the kernel: "  pages total/reserved/free: A/B/C"
reserved_pages() { sed -n 's/^  pages total\/reserved\/free: [0-9]*\/\([0-9]*\)\/.*/\1/p' "$1" | head -1 | tr -d '\r'; }

QPID=""
stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT
trap 'stop_qemu; exit 130' INT TERM

# start_qemu kernel serial_log qmp_socket ram [extra args...]    (leaves the VM running; stop_qemu ends it)
start_qemu() {
    k="$1"; l="$2"; sock="$3"; ram="$4"; shift 4
    : > "$l"; rm -f "$sock"
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m "$ram" -kernel "$k" \
        -display none -serial file:"$l" -monitor none -qmp unix:"$sock",server=on,wait=off "$@" &
    QPID=$!
}
# wait_boot serial_log seconds: wait for the end of boot, a panic or a halt. 0 = reached the end, 1 = timed out.
wait_boot() {
    i=0; n=$(( $2 * 2 ))
    while [ "$i" -lt "$n" ]; do
        if grep -qE "Boot complete|KERNEL PANIC|SELFTEST FAILED|Kernel halted" "$1" 2>/dev/null; then return 0; fi
        kill -0 "$QPID" 2>/dev/null || return 0
        sleep 0.5; i=$((i + 1))
    done
    return 1
}

echo "== building =="
BUILD_DIR="$OUT/normal" ./build.sh >/dev/null 2>&1 || { bad "kernel build failed"; echo "gfx-tests: $PASSES passed, $FAILS failed"; exit 1; }
ok "kernel builds"
AURORA_GFX_SHAPES=1 BUILD_DIR="$OUT/shapes" ./build.sh >/dev/null 2>&1 || { bad "shapes (G3) kernel build failed"; echo "gfx-tests: $PASSES passed, $FAILS failed"; exit 1; }
ok "G3 shapes test kernel builds"

run_display() {   # name ram expected_fb_base
    name="$1"; ram="$2"; fb="$3"
    echo "== ramfb display, $ram of RAM =="
    L="$OUT/$name.log"; S="$OUT/$name.sock"
    start_qemu "$OUT/normal/kernel.bin" "$L" "$S" "$ram" -device ramfb
    python3 "$SCRIPT_DIR/gfx_check.py" --qmp "$S" --serial "$L" --timeout 120 --keep "$OUT/$name-first.ppm" > "$OUT/$name.check.log" 2>&1
    rc=$?; sed 's/^/    /' "$OUT/$name.check.log" | grep -E "FAIL|note:|gfx_check:"
    if [ "$rc" -eq 0 ]; then ok "$name: screenshot verified (dimensions, pixels, boundaries, exact colour counts)"; else bad "$name: screenshot verification failed (see $OUT/$name.check.log)"; fi
    expect "$L" "^gfx: reserved framebuffer 0x0*$fb - " "$name: framebuffer is the top 3 MiB of RAM ($fb)"
    expect "$L" "^gfx: framebuffer 1024x768 XRGB8888 stride 4096 at 0x0*$fb" "$name: kernel reports 1024x768 XRGB8888, stride 4096"
    expect "$L" "^gfx: test pattern drawn" "$name: test pattern reported"
    wait_boot "$L" 240; wb=$?
    if [ "$wb" -eq 0 ]; then ok "$name: kernel finished booting"; else bad "$name: boot timed out"; fi
    expect_not "$L" "KERNEL PANIC|SELFTEST FAILED|Kernel halted" "$name: no panic and no failed self-test"
    expect "$L" "Memory management OK" "$name: memory management self-tests pass with the framebuffer reserved"
    expect "$L" "Userspace OK" "$name: EL0 process tests pass"
    # The userspace tests and every later boot step have now run: the display must be unchanged.
    python3 "$SCRIPT_DIR/gfx_check.py" --qmp "$S" --serial "$L" --timeout 30 > "$OUT/$name.check2.log" 2>&1
    if [ "$?" -eq 0 ]; then ok "$name: screenshot still identical after the whole boot (nothing overwrote the framebuffer)"; else bad "$name: screenshot changed after boot (see $OUT/$name.check2.log)"; fi
    stop_qemu
}
run_display ram256 256M 4fd00000
run_display ram128 128M 47d00000
run_display ram512 512M 5fd00000

echo "== G3: rounded rectangles and borders, byte-exact against an independent reference =="
L="$OUT/shapes.log"; S="$OUT/shapes.sock"
start_qemu "$OUT/shapes/kernel.bin" "$L" "$S" 256M -device ramfb
python3 "$SCRIPT_DIR/gfx_check.py" --pattern shapes --qmp "$S" --serial "$L" --timeout 120 --keep "$OUT/shapes-first.ppm" > "$OUT/shapes.check.log" 2>&1
rc=$?; sed 's/^/    /' "$OUT/shapes.check.log" | grep -E "FAIL|note:|gfx_check:"
if [ "$rc" -eq 0 ]; then ok "shapes: screenshot equals the independent reference image byte for byte"; else bad "shapes: screenshot verification failed (see $OUT/shapes.check.log)"; fi
expect "$L" "^gfx: shapes pattern drawn" "shapes: kernel reported the shapes pattern"
wait_boot "$L" 240
expect_not "$L" "KERNEL PANIC|SELFTEST FAILED|Kernel halted|gfx: pattern FAILED" "shapes: no panic, no failed self-test, no failed drawing"
python3 "$SCRIPT_DIR/gfx_check.py" --pattern shapes --qmp "$S" --serial "$L" --timeout 30 > "$OUT/shapes.check2.log" 2>&1
if [ "$?" -eq 0 ]; then ok "shapes: screenshot still identical after the whole boot"; else bad "shapes: screenshot changed after boot"; fi
stop_qemu

echo "== no display device: a normal boot =="
L="$OUT/nodisplay.log"
start_qemu "$OUT/normal/kernel.bin" "$L" "$OUT/nodisplay.sock" 256M
wait_boot "$L" 240; wb=$?
if [ "$wb" -eq 0 ]; then ok "nodisplay: finished without timing out"; else bad "nodisplay: boot timed out"; fi
stop_qemu
[ -s "$L" ] && ok "nodisplay: serial output produced" || bad "nodisplay: silent boot"
expect "$L" "^gfx: disabled \(no ramfb display: file not in the fw_cfg directory\)" "nodisplay: graphics reports disabled with the reason"
expect_not "$L" "^gfx: (reserved|framebuffer|test pattern)" "nodisplay: nothing reserved, configured or drawn"
expect_not "$L" "KERNEL PANIC|SELFTEST FAILED|Kernel halted" "nodisplay: no panic and no failed self-test"
expect "$L" "Memory management OK" "nodisplay: memory management OK"
expect "$L" "Interrupts and timer OK" "nodisplay: interrupts OK"
expect "$L" "Userspace OK" "nodisplay: userspace OK"
expect "$L" "Boot complete" "nodisplay: boot completes over the UART"
NODISP_RSV="$(reserved_pages "$L")"
RAM_RSV="$(reserved_pages "$OUT/ram256.log")"
if [ -n "$NODISP_RSV" ] && [ -n "$RAM_RSV" ] && [ $((RAM_RSV - NODISP_RSV)) -eq 768 ]; then ok "ramfb boot reserves exactly 768 more pages (3 MiB) than a boot without a display ($RAM_RSV vs $NODISP_RSV)"; else bad "reserved page delta is not 768 ($RAM_RSV vs $NODISP_RSV)"; fi

echo "== other display devices that the kernel does not drive =="
L="$OUT/vgpu.log"
start_qemu "$OUT/normal/kernel.bin" "$L" "$OUT/vgpu.sock" 256M -global virtio-mmio.force-legacy=false -device virtio-gpu-device
wait_boot "$L" 240; wb=$?
stop_qemu
if [ "$wb" -eq 0 ]; then ok "virtio-gpu only: finished booting"; else bad "virtio-gpu only: boot timed out"; fi
expect "$L" "^gfx: disabled \(no ramfb display" "virtio-gpu only: graphics disabled (no ramfb), no crash"
expect "$L" "Boot complete" "virtio-gpu only: boot completes"
expect_not "$L" "KERNEL PANIC|SELFTEST FAILED|Kernel halted" "virtio-gpu only: no panic"

echo "== ramfb and a virtio disk together (virtio slots and fw_cfg do not interfere) =="
L="$OUT/both.log"; IMG="$OUT/disk.img"; dd if=/dev/zero of="$IMG" bs=512 count=2048 2>/dev/null
start_qemu "$OUT/normal/kernel.bin" "$L" "$OUT/both.sock" 256M -device ramfb -global virtio-mmio.force-legacy=false -drive file="$IMG",if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0
python3 "$SCRIPT_DIR/gfx_check.py" --qmp "$OUT/both.sock" --serial "$L" --timeout 120 > "$OUT/both.check.log" 2>&1
if [ "$?" -eq 0 ]; then ok "ramfb + virtio-blk: screenshot verified"; else bad "ramfb + virtio-blk: screenshot failed (see $OUT/both.check.log)"; fi
wait_boot "$L" 240
stop_qemu
expect "$L" "Block device ready" "ramfb + virtio-blk: the block driver still works"
expect_not "$L" "KERNEL PANIC|SELFTEST FAILED|Kernel halted" "ramfb + virtio-blk: no panic"

echo "gfx-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
