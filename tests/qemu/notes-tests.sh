#!/bin/sh
# tests/qemu/notes-tests.sh - the native Notes application (EL0) on the SHIPPED kernel with the PACKAGED disk (scripts/package-qemu.sh + fresh-install.sh), real keyboard/pointer via QMP,
# screendump pixels and the guest's own lines. TCG only.
#   persist  ONE disk, FOUR separate QEMU boots: (1) launch from Index, type "AuroraOS is my independent operating system.", Ctrl+S, quit, reopen -> same text;
#            (2) RESTART the emulator on the same disk: the exact text loads; delete 7 characters, save shorter text; (3) restart: the shorter text loads; save longer text; (4) restart: longer text loads
#   rodisk   read-only disk: saving is refused honestly
#   nodisk   no disk: Notes is unavailable and launches nothing
#   corrupt / newer / foreign  hostile files on the disk: a damaged or newer-version note is never opened or overwritten; another principal's world-readable file is invisible
# Usage: ./tests/qemu/notes-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/notes-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
QPID=""
stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT
trap 'stop_qemu; exit 130' INT TERM
LEG="-global virtio-mmio.force-legacy=false"

PKG_DIR="$OUT/pkg" ./scripts/package-qemu.sh > "$OUT/pkg.log" 2>&1 && ok "the package builds (kernel + disk with calc.elf and notes.elf)" || { bad "package failed (see $OUT/pkg.log)"; tail -5 "$OUT/pkg.log"; echo "notes-tests: $PASSES passed, $FAILS failed"; exit 1; }
cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs compiled" || { bad "mkafs failed"; exit 1; }
python3 "$SCRIPT_DIR/notes_files.py" "$OUT" || { bad "could not generate hostile files"; exit 1; }
K="$OUT/pkg/kernel.bin"
A="$OUT/pkg/apps"
mk() { img="$1"; shift; "$OUT/mkafs" "$OUT/$img" 2048 installer 0x7 calc.elf "$A/calc.elf" installer 0x7 notes.elf "$A/notes.elf" "$@" > "$OUT/$img.mkafs.log" 2>&1 || bad "mkafs $img failed"; }
mk corrupt.img 602 3 note.a "$OUT/corrupt.bin"
mk newer.img 602 3 note.a "$OUT/newer.bin"
mk foreign.img 601 7 note.a "$OUT/foreign.bin"
mk truncated.img 602 3 note.a "$OUT/truncated.bin"
mk short.img 602 3 note.a "$OUT/short.bin"
mk full.img 701 7 f0 "$OUT/corrupt.bin" 701 7 f1 "$OUT/corrupt.bin" 701 7 f2 "$OUT/corrupt.bin" 701 7 f3 "$OUT/corrupt.bin" 701 7 f4 "$OUT/corrupt.bin" 701 7 f5 "$OUT/corrupt.bin" 701 7 f6 "$OUT/corrupt.bin" 701 7 f7 "$OUT/corrupt.bin" 702 7 f8 "$OUT/corrupt.bin" 702 7 f9 "$OUT/corrupt.bin" 702 7 f10 "$OUT/corrupt.bin" 702 7 f11 "$OUT/corrupt.bin" 702 7 f12 "$OUT/corrupt.bin" 702 7 f13 "$OUT/corrupt.bin" 702 7 f14 "$OUT/corrupt.bin" 702 7 f15 "$OUT/corrupt.bin" 703 7 f16 "$OUT/corrupt.bin" 703 7 f17 "$OUT/corrupt.bin" 703 7 f18 "$OUT/corrupt.bin" 703 7 f19 "$OUT/corrupt.bin" 703 7 f20 "$OUT/corrupt.bin" 703 7 f21 "$OUT/corrupt.bin" 703 7 f22 "$OUT/corrupt.bin" 703 7 f23 "$OUT/corrupt.bin" 704 7 f24 "$OUT/corrupt.bin" 704 7 f25 "$OUT/corrupt.bin" 704 7 f26 "$OUT/corrupt.bin" 704 7 f27 "$OUT/corrupt.bin" 704 7 f28 "$OUT/corrupt.bin" 704 7 f29 "$OUT/corrupt.bin"      # 30 small files from four other principals + calc.elf + notes.elf = all 32 file slots used
mk ro.img
cp "$OUT/pkg/aurora-fresh.img" "$OUT/persist.img"
python3 -c "
import struct, zlib, sys
body = b'kept'; mid = struct.pack('<BBHI', 1, 0, len(body), 1)
open(sys.argv[1], 'wb').write(b'ANTE' + mid + struct.pack('<I', zlib.crc32(body, zlib.crc32(mid))) + body)" "$OUT/kept.bin"
python3 "$SCRIPT_DIR/fill_image.py" "$OUT/mkafs" "$OUT/spacefull.img" 256 4 installer 0x7 calc.elf "$A/calc.elf" installer 0x7 notes.elf "$A/notes.elf" 602 3 note.a "$OUT/kept.bin" > "$OUT/spacefull.mkafs.log" 2>&1 && ok "$(tail -1 "$OUT/spacefull.mkafs.log")" || { bad "could not build the full-disk image"; cat "$OUT/spacefull.mkafs.log"; }

boot() {   # scenario image|none [extra drive options]
    sc="$1"; img="$2"; ro="$3"; log="$4"
    echo "== scenario: $sc (${log:-$sc}) =="
    L="$OUT/${log:-$sc}.log"; : > "$L"; rm -f "$OUT/${log:-$sc}.sock"
    if [ "$img" = none ]; then DRIVE=""; else DRIVE="-drive file=$OUT/$img,if=none,format=raw,id=d0$ro -device virtio-blk-device,drive=d0"; fi
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$K" \
        -display none -serial file:"$L" -monitor none -qmp unix:"$OUT/${log:-$sc}.sock",server=on,wait=off \
        -device ramfb $LEG -device virtio-keyboard-device -device virtio-tablet-device $DRIVE &
    QPID=$!
    python3 "$SCRIPT_DIR/notes_check.py" --qmp "$OUT/${log:-$sc}.sock" --serial "$L" --tmp "$OUT" --scenario "$sc" --timeout 500 > "$OUT/${log:-$sc}.check.log" 2>&1
    rc=$?; cat "$OUT/${log:-$sc}.check.log"
    if [ "$rc" -eq 0 ]; then ok "$sc: all checks passed"; else bad "$sc: checks failed (see $OUT/${log:-$sc}.check.log)"; fi
    stop_qemu
}
boot first persist.img "" first
boot second persist.img "" second
boot third persist.img "" third
boot fourth persist.img "" fourth
boot restored persist.img "" restored
boot rodisk ro.img ",readonly=on" rodisk
boot nodisk none "" nodisk
boot corrupt corrupt.img "" corrupt
boot newer newer.img "" newer
boot foreign foreign.img "" foreign
boot truncated truncated.img "" truncated
boot short short.img "" short
boot full full.img "" full
NOTE_BEFORE=$(sha256sum "$OUT/spacefull.img" | cut -d' ' -f1)
boot spacefull spacefull.img "" spacefull
boot spacefull2 spacefull.img "" spacefull2
python3 -c "
import sys
img = open(sys.argv[1], 'rb').read(); pat = open(sys.argv[2], 'rb').read()
sys.exit(0 if img.count(pat) == 1 else 1)" "$OUT/spacefull.img" "$OUT/kept.bin" && ok "spacefull: the previously saved note is still on the disk, byte for byte, exactly once" || bad "spacefull: the saved note changed or was duplicated"
echo "notes-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
