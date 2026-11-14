#!/bin/sh
# tests/qemu/settings-ui-tests.sh - the REAL kernel's native Settings screen with real QMP keyboard events, five boots (sequential):
#   nodisk   no block device: changes are session-only and say so; the Aura-mode setting is refused; high contrast is session-only;
#   save     a freshly formatted AuroraFS disk: values are saved through the typed registry as the shell's own principal; the registry refuses
#            the system-permission setting (Aura mode);
#   reload   a NEW QEMU process on the same disk: the saved text size is applied at boot; Reset is saved;
#   readonly the same disk attached read-only: the save fails honestly and nothing changes;
#   corrupt  the filesystem metadata destroyed: no store, session-only, no crash.
# Disposable images under build/settings-ui-tests/. QEMU is started and stopped only by this script, by PID. TCG only.
# Usage: ./tests/qemu/settings-ui-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/settings-ui-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
QPID=""
stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT
trap 'stop_qemu; exit 130' INT TERM
LEG="-global virtio-mmio.force-legacy=false"
echo "== building the real kernel =="
BUILD_DIR="$OUT/k" ./build.sh >/dev/null 2>&1 || { bad "kernel build failed"; echo "settings-ui-tests: $PASSES passed, $FAILS failed"; exit 1; }
ok "real kernel builds"
cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs compiled" || { bad "mkafs failed to compile"; exit 1; }
IMG="$OUT/disk.img"
"$OUT/mkafs" "$IMG" 2048 > "$OUT/mkafs.log" 2>&1 && ok "empty AuroraFS image formatted by the host tool" || bad "mkafs failed"
boot() {   # scenario [extra qemu args...]
    sc="$1"; shift
    echo "== scenario: $sc =="
    L="$OUT/$sc.log"; : > "$L"; rm -f "$OUT/$sc.sock"
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$OUT/k/kernel.bin" \
        -display none -serial file:"$L" -monitor none -qmp unix:"$OUT/$sc.sock",server=on,wait=off \
        -device ramfb $LEG -device virtio-keyboard-device -device virtio-tablet-device "$@" &
    QPID=$!
    python3 "$SCRIPT_DIR/settings_ui_check.py" --qmp "$OUT/$sc.sock" --serial "$L" --tmp "$OUT" --scenario "$sc" --timeout 420 > "$OUT/$sc.check.log" 2>&1
    rc=$?; cat "$OUT/$sc.check.log"
    if [ "$rc" -eq 0 ]; then ok "$sc: all checks passed"; else bad "$sc: checks failed (see $OUT/$sc.check.log)"; fi
    stop_qemu
}
DISK="-drive file=$IMG,if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0"
RODISK="-drive file=$IMG,if=none,format=raw,id=d0,readonly=on -device virtio-blk-device,drive=d0"
boot nodisk
boot save $DISK
boot reload $DISK
boot readonly $RODISK
dd if=/dev/zero of="$IMG" bs=512 count=16 conv=notrunc 2>/dev/null
boot corrupt $DISK
echo "settings-ui-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
