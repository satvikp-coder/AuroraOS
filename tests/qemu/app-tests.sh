#!/bin/sh
# tests/qemu/app-tests.sh - the native application launcher and the Calculator (Cycle 21), on the SHIPPED kernel (no test flags) with a real disk and a real keyboard/pointer.
# Index finds Calculator, Enter makes the KERNEL launcher start the installer-owned "calc.elf" through the existing trusted loader under a kernel-chosen identity; Pages lists the
# process; the Calculator draws its surface, takes real key presses (12 + 8 = 20), exits on Q (Esc and Tab belong to the compositor), is reaped (surface gone, instance forgotten) and a relaunch is a fresh instance with
# no stale state. Missing executable and missing disk: honestly unavailable. Real QMP key/pointer events, screendump pixels, the guest's own lines. TCG only.
#   launch   disk with calc.elf: find, launch, visible in Pages, 12+8=20, a second launch while open is refused, Q exits, cleanup, relaunch is fresh
#   noexe    a disk WITHOUT calc.elf: Calculator is listed "not available yet" and Enter launches nothing
#   nodisk   no disk: the same
# Usage: ./tests/qemu/app-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/app-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
QPID=""
stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT
trap 'stop_qemu; exit 130' INT TERM
LEG="-global virtio-mmio.force-legacy=false"
CC="${CROSS_COMPILE:-aarch64-linux-gnu-}gcc"
PF="-O1 -ffreestanding -fno-builtin -nostdlib -static -no-pie -fno-pic -fno-pie -mgeneral-regs-only -fno-stack-protector -Wall -Wextra -Werror -Wl,-T,tests/programs/user.ld -Wl,-z,noexecstack -Wl,-z,max-page-size=4096 -Wl,--build-id=none"
$CC $PF -o "$OUT/calc.elf" tests/programs/calc.c && ok "the Calculator compiles as a static AArch64 ET_EXEC" || { bad "calc.c failed to compile"; echo "app-tests: $PASSES passed, $FAILS failed"; exit 1; }
cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs compiled" || { bad "mkafs failed to compile"; exit 1; }
BUILD_DIR="$OUT/k" ./build.sh >/dev/null 2>&1 && ok "the shipped kernel builds (no test flags)" || { bad "kernel failed to build"; exit 1; }
"$OUT/mkafs" "$OUT/disk.img" 2048 installer 0x7 calc.elf "$OUT/calc.elf" > "$OUT/mkafs.log" 2>&1 && ok "disk provisioned with the installer-owned calc.elf" || bad "mkafs failed"
"$OUT/mkafs" "$OUT/empty.img" 2048 > "$OUT/mkafs2.log" 2>&1 && ok "disk without the Calculator formatted" || bad "mkafs (empty) failed"
boot() {   # scenario image|none
    sc="$1"; img="$2"
    echo "== scenario: $sc =="
    L="$OUT/$sc.log"; : > "$L"; rm -f "$OUT/$sc.sock"
    if [ "$img" = none ]; then DRIVE=""; else DRIVE="-drive file=$OUT/$img,if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0"; fi
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$OUT/k/kernel.bin" \
        -display none -serial file:"$L" -monitor none -qmp unix:"$OUT/$sc.sock",server=on,wait=off \
        -device ramfb $LEG -device virtio-keyboard-device -device virtio-tablet-device $DRIVE &
    QPID=$!
    python3 "$SCRIPT_DIR/app_check.py" --qmp "$OUT/$sc.sock" --serial "$L" --tmp "$OUT" --scenario "$sc" --timeout 400 > "$OUT/$sc.check.log" 2>&1
    rc=$?; cat "$OUT/$sc.check.log"
    if [ "$rc" -eq 0 ]; then ok "$sc: all checks passed"; else bad "$sc: checks failed (see $OUT/$sc.check.log)"; fi
    stop_qemu
}
boot launch disk.img
boot noexe empty.img
boot nodisk none
echo "app-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
