#!/bin/sh
# tests/qemu/s1-tests.sh - REAL S1 settings execution (Cycle 21): the shipped kernel (no test flags) with a real AuroraFS disk and a real keyboard. A typed Aura command opens the
# trusted review; Cancel and Escape change nothing; a real approval (Ctrl+Alt+Delete, Tab to Approve, Enter) runs the chain parse -> broker -> review -> health -> PRE record ->
# registry -> read-back -> apply -> POST record, and the text size really changes; after a guest RESTART the setting and both Trail records are still there. Read-only disk and
# no disk: the command is refused honestly and nothing changes. Real QMP key events, screendump pixels, the guest's own lines. The host/QMP can inject the gesture: no claim
# of protection against a malicious host. TCG only.
#   approve   fresh disk: cancel, escape, then approve "make text bigger" (100 -> 125)
#   persist   the same disk again: 125 is the saved appearance, both records are on the disk, "make text smaller" is reviewed against the REAL value 125
#   rodisk    the same disk read-only: refused, honestly
#   nodisk    no disk: refused, honestly
#   undo      the writable disk again: the typed word "undo" reverses the earlier receipt (125 -> 100) through a NEW trusted review and a second PRE/POST pair
# Usage: ./tests/qemu/s1-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/s1-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
QPID=""
stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT
trap 'stop_qemu; exit 130' INT TERM
LEG="-global virtio-mmio.force-legacy=false"
cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs compiled" || { bad "mkafs failed to compile"; exit 1; }
BUILD_DIR="$OUT/k" ./build.sh >/dev/null 2>&1 && ok "the shipped kernel builds (no test flags)" || { bad "kernel failed to build"; exit 1; }
"$OUT/mkafs" "$OUT/disk.img" 2048 > "$OUT/mkafs.log" 2>&1 && ok "empty AuroraFS image formatted" || bad "mkafs failed"
boot() {   # scenario [ro|none]
    sc="$1"; mode="$2"
    echo "== scenario: $sc =="
    L="$OUT/$sc.log"; : > "$L"; rm -f "$OUT/$sc.sock"
    if [ "$mode" = none ]; then DRIVE=""; elif [ "$mode" = ro ]; then DRIVE="-drive file=$OUT/disk.img,if=none,format=raw,id=d0,readonly=on -device virtio-blk-device,drive=d0"; else DRIVE="-drive file=$OUT/disk.img,if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0"; fi
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$OUT/k/kernel.bin" \
        -display none -serial file:"$L" -monitor none -qmp unix:"$OUT/$sc.sock",server=on,wait=off \
        -device ramfb $LEG -device virtio-keyboard-device -device virtio-tablet-device $DRIVE &
    QPID=$!
    python3 "$SCRIPT_DIR/s1_check.py" --qmp "$OUT/$sc.sock" --serial "$L" --tmp "$OUT" --scenario "$sc" --timeout 300 > "$OUT/$sc.check.log" 2>&1
    rc=$?; cat "$OUT/$sc.check.log"
    if [ "$rc" -eq 0 ]; then ok "$sc: all checks passed"; else bad "$sc: checks failed (see $OUT/$sc.check.log)"; fi
    stop_qemu
}
boot approve
boot persist
boot rodisk ro
boot nodisk none
boot undo
echo "s1-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
