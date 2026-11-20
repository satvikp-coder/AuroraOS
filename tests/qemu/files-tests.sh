#!/bin/sh
# tests/qemu/files-tests.sh - the native Files viewer (EL0, principal 603) on the SHIPPED kernel with the PACKAGED applications, real keys/pointer through QMP, screendump pixels and the guest's own lines.
#   list    a disk with files of several principals: the viewer lists ONLY what AuroraFS lets it read (private files of another principal and Notes' own private note do not exist for it),
#           previews text exactly, refuses to show binary or empty content, selection bounds hold, refresh, quit, fresh relaunch
#   many    more readable files than one SYS_FS LIST returns (16): the view says it shows only the first 16
#   nodisk  no disk: Files is unavailable and launches nothing
# Usage: ./tests/qemu/files-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/files-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
QPID=""
stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT
trap 'stop_qemu; exit 130' INT TERM
LEG="-global virtio-mmio.force-legacy=false"

PKG_DIR="$OUT/pkg" ./scripts/package-qemu.sh > "$OUT/pkg.log" 2>&1 && ok "the package builds (kernel + calc.elf, notes.elf, files.elf)" || { bad "package failed (see $OUT/pkg.log)"; tail -5 "$OUT/pkg.log"; echo "files-tests: $PASSES passed, $FAILS failed"; exit 1; }
cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs compiled" || { bad "mkafs failed"; exit 1; }
K="$OUT/pkg/kernel.bin"; A="$OUT/pkg/apps"
printf 'Hello from the Files viewer test.\nSecond line' > "$OUT/readme.txt"
head -c 40 /dev/zero > "$OUT/blob.bin"
: > "$OUT/empty.txt"
printf 'top secret of principal 700' > "$OUT/secret.txt"
printf 'x' > "$OUT/tiny.txt"
APPS="installer 0x7 calc.elf $A/calc.elf installer 0x7 notes.elf $A/notes.elf installer 0x7 files.elf $A/files.elf"
# shellcheck disable=SC2086
"$OUT/mkafs" "$OUT/list.img" 2048 $APPS 700 7 readme.txt "$OUT/readme.txt" 700 7 blob.bin "$OUT/blob.bin" 700 7 empty.txt "$OUT/empty.txt" 701 7 abcdefghijklmnopqrstuvwxyz01234 "$OUT/tiny.txt" 700 3 secret.txt "$OUT/secret.txt" 602 3 note.a "$OUT/tiny.txt" > "$OUT/list.mkafs.log" 2>&1 && ok "disk with files of several principals provisioned" || bad "mkafs list.img failed"
MANY=""; for o in 700 701 702; do for i in 1 2 3 4 5 6; do MANY="$MANY $o 7 f$o-$i.txt $OUT/tiny.txt"; done; done
# shellcheck disable=SC2086
"$OUT/mkafs" "$OUT/many.img" 2048 $APPS $MANY > "$OUT/many.mkafs.log" 2>&1 && ok "disk with 21 readable files provisioned" || bad "mkafs many.img failed"

boot() {   # scenario image|none
    sc="$1"; img="$2"
    echo "== scenario: $sc =="
    L="$OUT/$sc.log"; : > "$L"; rm -f "$OUT/$sc.sock"
    if [ "$img" = none ]; then DRIVE=""; else DRIVE="-drive file=$OUT/$img,if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0"; fi
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$K" \
        -display none -serial file:"$L" -monitor none -qmp unix:"$OUT/$sc.sock",server=on,wait=off \
        -device ramfb $LEG -device virtio-keyboard-device -device virtio-tablet-device $DRIVE &
    QPID=$!
    python3 "$SCRIPT_DIR/files_check.py" --qmp "$OUT/$sc.sock" --serial "$L" --tmp "$OUT" --scenario "$sc" --timeout 400 > "$OUT/$sc.check.log" 2>&1
    rc=$?; cat "$OUT/$sc.check.log"
    if [ "$rc" -eq 0 ]; then ok "$sc: all checks passed"; else bad "$sc: checks failed (see $OUT/$sc.check.log)"; fi
    stop_qemu
}
boot list list.img
boot many many.img
boot nodisk none
echo "files-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
