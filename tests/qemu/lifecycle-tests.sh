#!/bin/sh
# tests/qemu/lifecycle-tests.sh - Calculator, Notes and Files running TOGETHER on the shipped kernel with the packaged disk: launch from Index, kernel-chosen identities,
# Pages lists all three, input reaches only the focused app, Files cannot see Notes' private files, one app exits while the others keep running, a relaunch is fresh.
# Usage: ./tests/qemu/lifecycle-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/lifecycle-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
QPID=""
stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT
trap 'stop_qemu; exit 130' INT TERM
PKG_DIR="$OUT/pkg" ./scripts/package-qemu.sh > "$OUT/pkg.log" 2>&1 && ok "the package builds" || { bad "package failed (see $OUT/pkg.log)"; echo "lifecycle-tests: $PASSES passed, $FAILS failed"; exit 1; }
"$OUT/pkg/fresh-install.sh" > /dev/null || { bad "fresh-install failed"; exit 1; }
L="$OUT/boot.log"; : > "$L"
AURORA_SERIAL=file:"$L" "$OUT/pkg/run.sh" -display none -monitor none -qmp unix:"$OUT/boot.sock",server=on,wait=off &
QPID=$!
python3 "$SCRIPT_DIR/lifecycle_check.py" --qmp "$OUT/boot.sock" --serial "$L" --tmp "$OUT" --timeout 500 > "$OUT/check.log" 2>&1
rc=$?; cat "$OUT/check.log"
if [ "$rc" -eq 0 ]; then ok "lifecycle: all checks passed"; else bad "lifecycle: checks failed (see $OUT/check.log)"; fi
stop_qemu
echo "lifecycle-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
