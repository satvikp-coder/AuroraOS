#!/bin/sh
# tests/qemu/recovery-tests.sh - the PACKAGED production kernel against disks in every condition a user can end up with: fresh, blank, corrupt superblock,
# unsupported version, corrupt inode, truncated image, read-only disk, no disk at all, and an unexpected power loss (QEMU killed with SIGKILL) at boot.
# For each: the kernel must reach the interactive shell (never panic or hang), report the condition honestly, and NEVER modify a disk it could not mount
# (the image checksum is compared with the checksum taken BEFORE the boot). Also: the bundle verifier accepts the package and rejects every tamper.
# Every image is a disposable copy under build/recovery-tests; nothing outside it is opened. QEMU is started and stopped only by this script. TCG only.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"; cd "$ROOT" || exit 1
OUT="$ROOT/build/recovery-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
QPID=""; stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT; trap 'stop_qemu; exit 130' INT TERM
has()  { grep -aEq -- "$2" "$1"; }
sum()  { sha256sum "$1" | cut -d' ' -f1; }

PKG="$OUT/pkg"
PKG_DIR="$PKG" ./scripts/package-qemu.sh > "$OUT/pkg.log" 2>&1 && ok "the package builds" || { bad "package build failed (see $OUT/pkg.log)"; tail -5 "$OUT/pkg.log"; echo "recovery-tests: $PASSES passed, $FAILS failed"; exit 1; }
FRESH="$PKG/aurora-fresh.img"

# boot IMG LOG [ro|nodisk]: start the packaged kernel on IMG and wait (polling the serial log, no fixed sleep) until the shell is ready or 240 s pass
QEMU_COMMON="-machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel $PKG/kernel.bin -global virtio-mmio.force-legacy=false -device ramfb -device virtio-keyboard-device -device virtio-tablet-device -display none -monitor none"
start_boot() {   # $1 image, $2 log, $3 mode
    : > "$2"
    case "$3" in
        nodisk) qemu-system-aarch64 $QEMU_COMMON -serial file:"$2" & ;;
        ro)     qemu-system-aarch64 $QEMU_COMMON -drive file="$1",if=none,format=raw,id=d0,readonly=on -device virtio-blk-device,drive=d0 -serial file:"$2" & ;;
        *)      qemu-system-aarch64 $QEMU_COMMON -drive file="$1",if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0 -serial file:"$2" & ;;
    esac
    QPID=$!
}
wait_ready() {   # $1 log
    i=0; while [ "$i" -lt 240 ]; do has "$1" "shell: ready keyboards=" && return 0; kill -0 "$QPID" 2>/dev/null || return 1; sleep 1; i=$((i + 1)); done; return 1
}
boot() {         # $1 name, $2 image, $3 mode ; sets L
    L="$OUT/$1.log"; start_boot "$2" "$L" "$3"
    if wait_ready "$L"; then ok "$1: the kernel reached the interactive shell"; else bad "$1: the shell was never reached (see $L)"; fi
    stop_qemu
    if has "$L" "KERNEL PANIC|SELFTEST FAILED"; then bad "$1: the kernel panicked or a self-test failed"; else ok "$1: no panic and no failed self-test"; fi
}
variant() {      # $1 name: a fresh disposable copy of the pristine image
    cp "$FRESH" "$OUT/$1.img"
}

# 1. fresh install
variant fresh; B=$(sum "$OUT/fresh.img"); boot fresh "$OUT/fresh.img"
has "$L" "\[fs\] mount: AuroraFS v1 mounted" && ok "fresh: AuroraFS mounted with the installer-owned applications" || bad "fresh: the filesystem did not mount"
has "$L" "shell: settings store=none" && bad "fresh: the shell said there was no filesystem" || ok "fresh: the shell saw the filesystem"
# A boot on a MOUNTABLE disk may create the kernel's own state files (the Trail and its anchor); it must never touch the superblock, the installer-owned
# application inodes, or the applications' data sectors. Those are the first 1280 bytes (superblock + inodes 0..2) and the data sectors 17.. (one per 512 bytes of each app).
APPSECT=0; for f in "$PKG"/apps/*.elf; do APPSECT=$((APPSECT + ($(wc -c < "$f") + 511) / 512)); done
cmp -s -n 1280 "$FRESH" "$OUT/fresh.img" && ok "fresh: the superblock and the installer-owned application inodes are byte-for-byte unchanged by a boot" || bad "fresh: a boot changed the superblock or an application inode"
cmp -s -n $((APPSECT * 512)) -i $((17 * 512)) "$FRESH" "$OUT/fresh.img" && ok "fresh: the $APPSECT data sectors holding the installed applications are unchanged by a boot" || bad "fresh: a boot changed application data"
[ "$B" != "$(sum "$OUT/fresh.img")" ] && ok "fresh: the boot did create its own state (the Trail), so this suite is not comparing a disk the kernel never wrote" || bad "fresh: the boot wrote nothing, so the persistence check below proves nothing"
# persistence: the SAME disk, booted again, must show the state the first boot wrote
AFTER1=$(sum "$OUT/fresh.img"); boot persist "$OUT/fresh.img"
has "$L" "\[fs\] mount: AuroraFS v1 mounted read-only-safe, 5 files" && ok "persist: the second boot mounts the disk with the files the first boot created (3 installed + 2 system)" || bad "persist: the file set did not survive the restart"
has "$L" "ledger: trail state=ok records=[1-9]" && ok "persist: the Trail written by the first boot was read back and is valid" || bad "persist: the Trail did not persist"
cmp -s -n 1280 "$FRESH" "$OUT/fresh.img" && ok "persist: the installer-owned applications are still unchanged after a second boot" || bad "persist: an application inode changed"

# 2. damaged or foreign disks: refuse to mount, say so, boot anyway, never write
damage() {       # $1 name, $2 description, $3 expected serial pattern, shell command that damages $OUT/$1.img in $4
    variant "$1"; sh -c "$4" || { bad "$1: could not prepare the damaged image"; return; }
    B=$(sum "$OUT/$1.img"); boot "$1" "$OUT/$1.img"
    has "$L" "$3" && ok "$1: $2 is reported honestly" || bad "$1: expected the message /$3/"
    has "$L" "AuroraFS v1 mounted" && bad "$1: a damaged disk was mounted" || ok "$1: the damaged disk was not mounted"
    has "$L" "shell: settings store=none" && ok "$1: the shell started without a store and said so (settings last for this session only)" || bad "$1: the shell did not report the missing store"
    [ "$B" = "$(sum "$OUT/$1.img")" ] && ok "$1: the disk is byte-for-byte unchanged (nothing was formatted or repaired behind the user's back)" || bad "$1: the kernel modified a disk it could not mount"
}
damage sbcorrupt  "a corrupt superblock (checksum mismatch)"  "filesystem is CORRUPT: refusing to mount" "printf '\\377' | dd of=$OUT/sbcorrupt.img bs=1 seek=16 count=1 conv=notrunc 2>/dev/null"
damage inocorrupt "a corrupt inode (checksum mismatch)"        "filesystem is CORRUPT: refusing to mount" "printf '\\377' | dd of=$OUT/inocorrupt.img bs=1 seek=$((512 + 8)) count=1 conv=notrunc 2>/dev/null"
damage badversion "an unsupported filesystem version"          "unsupported filesystem version"           "printf '\\011' | dd of=$OUT/badversion.img bs=1 seek=8 count=1 conv=notrunc 2>/dev/null"
damage blank      "a blank disk (no filesystem)"               "no filesystem on the disk"                "dd if=/dev/zero of=$OUT/blank.img bs=512 count=2048 conv=notrunc 2>/dev/null"
damage truncated  "an image shorter than its filesystem"      "filesystem is CORRUPT: refusing to mount" "head -c 8192 $FRESH > $OUT/truncated.img"
damage foreign    "a disk holding someone else's data"         "no filesystem on the disk"                "head -c 1048576 /dev/urandom > $OUT/foreign.img; printf 'NOTAFS!!' | dd of=$OUT/foreign.img bs=1 conv=notrunc 2>/dev/null"

# 3. read-only disk: mounts and runs, nothing can be written
variant ro; B=$(sum "$OUT/ro.img"); boot ro "$OUT/ro.img" ro
has "$L" "\[fs\] mount: AuroraFS v1 mounted" && ok "ro: a read-only disk mounts and its files are available" || bad "ro: a read-only disk did not mount"
[ "$B" = "$(sum "$OUT/ro.img")" ] && ok "ro: the read-only image is unchanged" || bad "ro: the read-only image changed"

# 4. no disk at all
L="$OUT/nodisk.log"; start_boot "" "$L" nodisk
if wait_ready "$L"; then ok "nodisk: the kernel reached the shell with no disk attached"; else bad "nodisk: the shell was never reached"; fi
stop_qemu
has "$L" "\[blk\] no block device attached" && ok "nodisk: the missing disk is reported" || bad "nodisk: the missing disk was not reported"
has "$L" "KERNEL PANIC" && bad "nodisk: panic" || ok "nodisk: no panic"

# 5. unexpected power loss: SIGKILL at a few points during boot, then a normal boot of the SAME disk
variant crash; B=$(sum "$OUT/crash.img")
EARLY=0
for n in 1 2 3; do
    start_boot "$OUT/crash.img" "$OUT/crash$n.log" disk
    i=0; while [ "$i" -lt 120 ]; do if [ "$(wc -c < "$OUT/crash$n.log")" -gt $((n * 700)) ]; then break; fi; sleep 1; i=$((i + 1)); done    # kill after the kernel has printed a little more each time (a different boot stage)
    has "$OUT/crash$n.log" "shell: ready keyboards=" || EARLY=$((EARLY + 1))
    kill -9 "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""
done
[ "$EARLY" -ge 2 ] && ok "crash: at least two of the three kills landed BEFORE the shell was ready (mid-boot, not after it)" || bad "crash: the kills did not land mid-boot ($EARLY of 3)"
[ "$B" = "$(sum "$OUT/crash.img")" ] && ok "crash: three boots killed mid-way left the disk byte-for-byte unchanged (a boot does no writes)" || bad "crash: the disk changed after killed boots"
boot crashrecover "$OUT/crash.img"
has "$L" "\[fs\] mount: AuroraFS v1 mounted" && ok "crash: after the killed boots the same disk mounts normally" || bad "crash: the disk no longer mounts after killed boots"

# 6. the bundle verifier
cp -r "$PKG" "$OUT/bundle"; rm -rf "$OUT/bundle/data"
if (cd "$OUT/bundle" && ./verify-bundle.sh > "$OUT/verify-ok.log" 2>&1); then ok "verify: the untouched bundle verifies"; else bad "verify: the untouched bundle was rejected (see $OUT/verify-ok.log)"; fi
tamper() {       # $1 name, $2 shell command run inside a fresh copy of the bundle
    rm -rf "$OUT/t"; cp -r "$OUT/bundle" "$OUT/t"
    (cd "$OUT/t" && sh -c "$2") 2>/dev/null
    if (cd "$OUT/t" && ./verify-bundle.sh > "$OUT/verify-$1.log" 2>&1); then bad "verify: $1 was NOT detected"; else ok "verify: $1 is detected"; fi
}
tamper "a modified kernel"        "printf x >> kernel.bin"
tamper "a modified disk template" "printf x >> aurora-fresh.img"
tamper "a modified application"   "printf x >> apps/calc.elf"
tamper "a deleted application"    "rm apps/calc.elf"
tamper "an unlisted extra file in apps/" "printf x > apps/extra.elf"
# KNOWN LIMIT, asserted so it cannot be forgotten: the manifest is a list of SHA-256 digests, not a signature. Whoever can change the kernel AND the manifest
# together produces a bundle that verifies. Checksums detect accidents and partial tampering; authenticity needs a signature checked against a key held elsewhere.
rm -rf "$OUT/t"; cp -r "$OUT/bundle" "$OUT/t"
printf x >> "$OUT/t/kernel.bin"; NEWH=$(sha256sum "$OUT/t/kernel.bin" | cut -d' ' -f1)
sed "s/^[0-9a-f]\{64\}  kernel.bin\$/$NEWH  kernel.bin/" "$OUT/t/manifest.txt" > "$OUT/t/m.new" && mv "$OUT/t/m.new" "$OUT/t/manifest.txt"
if (cd "$OUT/t" && ./verify-bundle.sh > "$OUT/verify-rewrite.log" 2>&1); then ok "verify: LIMIT confirmed: a kernel and manifest rewritten together still verify (a checksum is not authentication)"; else bad "verify: the rewrite case behaved unexpectedly (see $OUT/verify-rewrite.log)"; fi
grep -q "not authentication" "$OUT/verify-rewrite.log" && ok "verify: its output states that limit" || bad "verify: its output does not state the limit"
rm -rf "$OUT/t"

echo "recovery-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
