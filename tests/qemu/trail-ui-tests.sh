#!/bin/sh
# tests/qemu/trail-ui-tests.sh - the native Trail screen (Cycle 20) on the REAL kernel and the REAL ledgers: opened from Index by keyboard, driven by real QMP key
# and pointer events, verified on screendump pixels plus the guest's own "shell: say" / "shell: trail view" lines. Scenarios (one boot each):
#   records   a test kernel writes an audited boot marker and three grants to a fresh disk; the screen shows them, newest first, and the read-only guarantee holds
#   big       the same at 200 percent text
#   nodisk    no disk: "Trail: off. No disk, nothing is saved." and "No records yet."
#   readonly  the disk from "records" attached read-only: the records are visible, the health line says read-only
#   rollback  a hand-built disk whose trail.v1 is behind its anchor: "rollback found", nothing is claimed healthy
# QEMU is started and stopped only by this script. TCG only: no claim about real hardware.
# Usage: ./tests/qemu/trail-ui-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/trail-ui-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
QPID=""
stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT
trap 'stop_qemu; exit 130' INT TERM
LEG="-global virtio-mmio.force-legacy=false"
PASSPORT=2130706436
TRAIL=2130706437

cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs compiled" || { bad "mkafs failed to compile"; exit 1; }
python3 "$SCRIPT_DIR/ledger_gen.py" "$OUT/gen" && ok "hand-built ledger files generated" || { bad "ledger_gen failed"; exit 1; }
AURORA_LEDGER_TEST=1 BUILD_DIR="$OUT/k_rec" ./build.sh >/dev/null 2>&1 && ok "test kernel (ledger script 1) builds" || { bad "test kernel failed to build"; exit 1; }
AURORA_LEDGER_TEST=1 AURORA_SHELL_TEXT_PCT=200 BUILD_DIR="$OUT/k_big" ./build.sh >/dev/null 2>&1 && ok "test kernel at 200 percent builds" || { bad "200 percent kernel failed to build"; exit 1; }
BUILD_DIR="$OUT/k_plain" ./build.sh >/dev/null 2>&1 && ok "the plain kernel builds" || { bad "plain kernel failed to build"; exit 1; }

boot() {   # scenario kernel-dir image [ro]
    sc="$1"; kdir="$2"; img="$3"; mode="$4"
    echo "== scenario: $sc =="
    L="$OUT/$sc.log"; : > "$L"; rm -f "$OUT/$sc.sock"
    if [ -z "$img" ]; then DRIVE=""; elif [ "$mode" = ro ]; then DRIVE="-drive file=$img,if=none,format=raw,id=d0,readonly=on -device virtio-blk-device,drive=d0"; else DRIVE="-drive file=$img,if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0"; fi
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$OUT/$kdir/kernel.bin" \
        -display none -serial file:"$L" -monitor none -qmp unix:"$OUT/$sc.sock",server=on,wait=off \
        -device ramfb $LEG -device virtio-keyboard-device -device virtio-tablet-device $DRIVE &
    QPID=$!
    python3 "$SCRIPT_DIR/trail_ui_check.py" --qmp "$OUT/$sc.sock" --serial "$L" --tmp "$OUT" --scenario "$sc" --timeout 420 > "$OUT/$sc.check.log" 2>&1
    rc=$?; cat "$OUT/$sc.check.log"
    if [ "$rc" -eq 0 ]; then ok "$sc: all checks passed"; else bad "$sc: checks failed (see $OUT/$sc.check.log)"; fi
    stop_qemu
}
"$OUT/mkafs" "$OUT/main.img" 2048 > /dev/null 2>&1 && ok "empty AuroraFS image formatted" || bad "mkafs failed"
boot records k_rec "$OUT/main.img"
"$OUT/mkafs" "$OUT/big.img" 2048 > /dev/null 2>&1
boot big k_big "$OUT/big.img"
boot nodisk k_plain ""
boot readonly k_plain "$OUT/main.img" ro
"$OUT/mkafs" "$OUT/rollback.img" 2048 $PASSPORT 3 passport.v1 "$OUT/gen/p_good2.bin" $PASSPORT 3 anchor.v1 "$OUT/gen/at_behind.bin" $TRAIL 3 trail.v1 "$OUT/gen/t_good3.bin" > "$OUT/mkafs_rb.log" 2>&1 && ok "rollback disk built" || bad "mkafs (rollback) failed"
boot rollback k_plain "$OUT/rollback.img"
echo "trail-ui-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
