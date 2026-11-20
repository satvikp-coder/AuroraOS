#!/bin/sh
# tests/qemu/spaces-tests.sh - persistent Spaces on the SHIPPED kernel with the PACKAGED disk, real keys through QMP and the guest's own lines.
#   persist  ONE disk, THREE emulator boots: select Study/Work/Projects/Personal through Index and see each saved; restart: Work is restored; launch Calculator, select Projects; restart:
#            Projects is restored and the Calculator is NOT running (a Space is configuration, not a resumed process)
#   rodisk / nodisk / corrupt / badvalue  the failure cases are reported honestly and never claimed as saved
#   legacy   a settings record written before Spaces existed still loads (text size kept) and survives the layout change across a restart
# Usage: ./tests/qemu/spaces-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/spaces-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
QPID=""
stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT
trap 'stop_qemu; exit 130' INT TERM
LEG="-global virtio-mmio.force-legacy=false"
PKG_DIR="$OUT/pkg" ./scripts/package-qemu.sh > "$OUT/pkg.log" 2>&1 && ok "the package builds" || { bad "package failed (see $OUT/pkg.log)"; echo "spaces-tests: $PASSES passed, $FAILS failed"; exit 1; }
cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs compiled" || { bad "mkafs failed"; exit 1; }
python3 "$SCRIPT_DIR/spaces_files.py" "$OUT" || { bad "fixtures failed"; exit 1; }
K="$OUT/pkg/kernel.bin"; A="$OUT/pkg/apps"
APPS="installer 0x7 calc.elf $A/calc.elf installer 0x7 notes.elf $A/notes.elf"
SETOWN=0x7F000002
# shellcheck disable=SC2086
"$OUT/mkafs" "$OUT/legacy.img" 2048 $APPS $SETOWN 3 settings.v1 "$OUT/legacy.bin" > "$OUT/legacy.mk.log" 2>&1 || bad "mkafs legacy failed"
# shellcheck disable=SC2086
"$OUT/mkafs" "$OUT/corrupt.img" 2048 $APPS $SETOWN 3 settings.v1 "$OUT/corrupt.bin" > "$OUT/corrupt.mk.log" 2>&1 || bad "mkafs corrupt failed"
# shellcheck disable=SC2086
"$OUT/mkafs" "$OUT/badvalue.img" 2048 $APPS $SETOWN 3 settings.v1 "$OUT/badvalue.bin" > "$OUT/badvalue.mk.log" 2>&1 || bad "mkafs badvalue failed"
cp "$OUT/pkg/aurora-fresh.img" "$OUT/persist.img"; cp "$OUT/pkg/aurora-fresh.img" "$OUT/ro.img"

boot() {   # scenario image|none [drive options]
    sc="$1"; img="$2"; ro="$3"
    echo "== scenario: $sc =="
    L="$OUT/$sc.log"; : > "$L"; rm -f "$OUT/$sc.sock"
    if [ "$img" = none ]; then DRIVE=""; else DRIVE="-drive file=$OUT/$img,if=none,format=raw,id=d0$ro -device virtio-blk-device,drive=d0"; fi
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$K" \
        -display none -serial file:"$L" -monitor none -qmp unix:"$OUT/$sc.sock",server=on,wait=off \
        -device ramfb $LEG -device virtio-keyboard-device -device virtio-tablet-device $DRIVE &
    QPID=$!
    python3 "$SCRIPT_DIR/spaces_check.py" --qmp "$OUT/$sc.sock" --serial "$L" --tmp "$OUT" --scenario "$sc" --timeout 300 > "$OUT/$sc.check.log" 2>&1
    rc=$?; cat "$OUT/$sc.check.log"
    if [ "$rc" -eq 0 ]; then ok "$sc: all checks passed"; else bad "$sc: checks failed (see $OUT/$sc.check.log)"; fi
    stop_qemu
}
boot select persist.img ""
boot restore persist.img ""
boot norun persist.img ""
boot rodisk ro.img ",readonly=on"
boot nodisk none ""
boot legacy legacy.img ""
boot legacy2 legacy.img ""
boot corrupt corrupt.img ""
boot badvalue badvalue.img ""
echo "spaces-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
