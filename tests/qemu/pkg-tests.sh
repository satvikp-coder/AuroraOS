#!/bin/sh
# tests/qemu/pkg-tests.sh - the local QEMU package: it builds deterministically, refuses unsafe inputs, and the PACKAGED kernel + disk (made by
# fresh-install.sh, booted by run.sh, nothing provisioned by hand) launches the native Calculator from Index. Also: run.sh never creates or resets
# a disk and fresh-install.sh never overwrites one.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"; cd "$ROOT" || exit 1
OUT="$ROOT/build/pkg-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
QPID=""; stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT; trap 'stop_qemu; exit 130' INT TERM

PKG_DIR="$OUT/p1" ./scripts/package-qemu.sh > "$OUT/p1.log" 2>&1 && ok "the package builds" || { bad "package build failed (see $OUT/p1.log)"; tail -5 "$OUT/p1.log"; echo "pkg-tests: $PASSES passed, $FAILS failed"; exit 1; }
AURORA_BLK_TEST=1 AURORA_FS_TEST=1 AURORA_TEST_ENTRY=el1t PKG_DIR="$OUT/p2" \
    ./scripts/package-qemu.sh > "$OUT/p2.log" 2>&1 || bad "second package build failed"
AURORA_PRODUCTION=1 BUILD_DIR="$OUT/production" sh ./build.sh > "$OUT/production.log" 2>&1 &&
    cmp -s "$OUT/p1/kernel.bin" "$OUT/production/kernel.bin" &&
    ok "the package contains the production kernel" || bad "the packaged kernel differs from a production build"
H1=$(cd "$OUT/p1" && sha256sum kernel.bin aurora-fresh.img apps/*.elf); H2=$(cd "$OUT/p2" && sha256sum kernel.bin aurora-fresh.img apps/*.elf)
if [ "$H1" = "$H2" ]; then ok "package builds are byte-identical even with test switches in the caller environment"; else bad "the builds differ"; echo "$H1"; echo "$H2"; fi
grep -q "calc.elf" "$OUT/p1/manifest.txt" && grep -q "kernel.bin" "$OUT/p1/manifest.txt" && ok "the manifest lists hashes and the installed files" || bad "manifest incomplete"
file "$OUT/p1/apps/calc.elf" | grep -q "ELF 64-bit LSB executable, ARM aarch64" && ok "calc.elf is a 64-bit AArch64 ET_EXEC" || bad "calc.elf has the wrong format"
SZ=$(wc -c < "$OUT/p1/apps/calc.elf"); if [ "$SZ" -le 24576 ]; then ok "calc.elf ($SZ bytes) is within the loader's 24 KiB limit"; else bad "calc.elf too large"; fi

# the tool refuses what the loader would refuse
cc -std=c11 -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkdisk" tools/pkg/mkdisk.c kernel/fs/afs.c kernel/proc/elf.c || { bad "mkdisk failed to build"; exit 1; }
head -c 100 /dev/zero > "$OUT/junk.elf"
"$OUT/mkdisk" "$OUT/x.img" 2048 junk.elf="$OUT/junk.elf" > /dev/null 2>&1 && bad "a non-ELF file was installed" || ok "a file that is not a valid ELF is refused"
head -c 30000 /dev/zero > "$OUT/big.elf"
"$OUT/mkdisk" "$OUT/x.img" 2048 big.elf="$OUT/big.elf" > /dev/null 2>&1 && bad "an oversize file was installed" || ok "a file over the loader's size limit is refused"
"$OUT/mkdisk" "$OUT/x.img" 2048 a.elf="$OUT/p1/apps/calc.elf" a.elf="$OUT/p1/apps/calc.elf" > /dev/null 2>&1 && bad "duplicate names accepted" || ok "duplicate names are refused"
"$OUT/mkdisk" "$OUT/x.img" 2048 "bad/name=$OUT/p1/apps/calc.elf" > /dev/null 2>&1 && bad "an invalid name was accepted" || ok "an invalid AuroraFS name is refused"
ARGS=""; for i in 1 2 3 4 5 6 7 8 9; do ARGS="$ARGS c$i.elf=$OUT/p1/apps/calc.elf"; done
"$OUT/mkdisk" "$OUT/x.img" 2048 $ARGS > /dev/null 2>&1 && bad "9 files exceeded the installer quota but were accepted" || ok "more files than the installer quota are refused"

# fresh-install / run semantics
P="$OUT/p1"
if "$P/run.sh" -display none > "$OUT/norun.log" 2>&1; then bad "run.sh booted without a disk"; else grep -q "run: no disk" "$OUT/norun.log" && ok "run.sh refuses to run without a disk" || bad "run.sh failed for the wrong reason"; fi
[ ! -e "$P/data/aurora.img" ] && ok "run.sh created no disk" || bad "run.sh created a disk"
"$P/fresh-install.sh" > /dev/null && [ -f "$P/data/aurora.img" ] && ok "fresh-install.sh creates data/aurora.img" || bad "fresh-install failed"
echo marker >> "$P/data/aurora.img"; S1=$(sha256sum "$P/data/aurora.img")
"$P/fresh-install.sh" > /dev/null 2>&1 && bad "fresh-install overwrote an existing disk" || ok "fresh-install.sh refuses to overwrite an existing disk"
[ "$S1" = "$(sha256sum "$P/data/aurora.img")" ] && ok "the existing disk is untouched" || bad "disk changed"
I="$OUT/install-race"; mkdir -p "$I"
cp "$P/fresh-install.sh" "$P/aurora-fresh.img" "$I/"
"$I/fresh-install.sh" > "$OUT/install-first.log" 2>&1 & first=$!
"$I/fresh-install.sh" > "$OUT/install-second.log" 2>&1 & second=$!
rc1=0; wait "$first" || rc1=$?
rc2=0; wait "$second" || rc2=$?
if [ "$((rc1 + rc2))" -eq 1 ] && cmp -s "$I/aurora-fresh.img" "$I/data/aurora.img"; then
    ok "concurrent installs publish exactly one complete disk"
else
    bad "concurrent installs failed to publish exactly one complete disk"
fi
mv "$I/data/aurora.img" "$I/saved.img"
ln -s "$I/missing.img" "$I/data/aurora.img"
if "$I/fresh-install.sh" > "$OUT/install-link.log" 2>&1 || [ -e "$I/missing.img" ]; then
    bad "fresh-install followed a disk symlink"
elif "$I/fresh-install.sh" --force > "$OUT/install-force.log" 2>&1 &&
     [ ! -L "$I/data/aurora.img" ] && [ ! -e "$I/missing.img" ] &&
     cmp -s "$I/aurora-fresh.img" "$I/data/aurora.img"; then
    ok "fresh-install replaces a disk symlink only with explicit force and leaves its target alone"
else
    bad "forced fresh-install did not replace the disk symlink safely"
fi
rm "$I/data/aurora.img" "$I/aurora-fresh.img"
if "$I/fresh-install.sh" > "$OUT/install-missing.log" 2>&1 || [ -n "$(ls -A "$I/data")" ]; then
    bad "failed copy left a published disk or staging file"
else
    ok "failed copy leaves no disk or staging file"
fi


# boot the PACKAGED artifacts and launch Calculator from Index (real keys, real pixels)
"$P/fresh-install.sh" --force > /dev/null
L="$OUT/boot.log"; : > "$L"
AURORA_SERIAL=file:"$L" "$P/run.sh" -display none -monitor none -qmp unix:"$OUT/boot.sock",server=on,wait=off &
QPID=$!
python3 "$SCRIPT_DIR/app_check.py" --qmp "$OUT/boot.sock" --serial "$L" --tmp "$OUT" --scenario launch --check-aura-notes --timeout 400 > "$OUT/boot.check.log" 2>&1
rc=$?; tail -4 "$OUT/boot.check.log"
if [ "$rc" -eq 0 ]; then ok "the packaged kernel and disk boot, run Calculator from Index and run Notes from Aura"; else bad "packaged boot checks failed (see $OUT/boot.check.log)"; fi
stop_qemu
L="$OUT/clock-restart.log"; : > "$L"
AURORA_SERIAL=file:"$L" "$P/run.sh" -display none -monitor none -qmp unix:"$OUT/restart.sock",server=on,wait=off &
QPID=$!
if python3 "$SCRIPT_DIR/app_check.py" --qmp "$OUT/restart.sock" --serial "$L" --tmp "$OUT" --scenario clock-restart > "$OUT/clock-restart.check.log" 2>&1; then
    ok "Clock restores the selected preset after reboot"
else
    cat "$OUT/clock-restart.check.log"
    bad "Clock preset restart checks failed"
fi
stop_qemu
echo "pkg-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
