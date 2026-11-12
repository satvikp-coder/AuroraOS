#!/bin/sh
# tests/qemu/fs-tests.sh — QEMU tests for AuroraFS (issue #6, milestone B). Disposable images under build/fs-tests/ only.
#
#   1. fresh zero image, THREE separate QEMU runs on the same file: boot 1 formats and runs the functional / authorization /
#      full-disk / crash-injection suite; boots 2 and 3 mount WITHOUT formatting, verify the files byte for byte, bump a counter
#      and run the corruption suite (damage injected and restored). The host independently finds the file bytes in the image.
#   2. host-side damage to the image between runs: flipped inode bit, wrong version, foreign content, truncated-to-zero
#      superblock: a NORMAL kernel reports it precisely, does not panic, boots on, and the image checksum is unchanged
#      (no automatic repair or reformat).
#   3. read-only disk holding a filesystem: reads verified, every modification refused, image unchanged.
#   4. normal kernel + no disk: nothing mounted, no panic.
#
# Usage: ./tests/qemu/fs-tests.sh      Requires the aarch64 toolchain and qemu-system-aarch64.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1

EXPECTED_FS_BOOT1=57
EXPECTED_FS_BOOTN=21
EXPECTED_FS_RO=2
EXPECTED_FSPROC_BOOT1=16
EXPECTED_FSPROC_BOOTN=14
EXPECTED_PROC_TESTS=79

OUT="$ROOT/build/fs-tests"
rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
expect()     { if grep -Eq -- "$2" "$1"; then ok "$3"; else bad "$3 (pattern: $2)"; fi; }
expect_not() { if grep -Eq -- "$2" "$1"; then bad "$3 (found: $2)"; else ok "$3"; fi; }

TIMED_OUT=0
run_qemu() { # kernel log extra-args...
    k="$1"; l="$2"; shift 2
    : > "$l"; TIMED_OUT=1
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$k" -nographic -serial file:"$l" -monitor none "$@" &
    pid=$!; i=0
    while [ "$i" -lt 240 ]; do
        if grep -qE "Boot complete|KERNEL PANIC|SELFTEST FAILED|Kernel halted" "$l" 2>/dev/null; then TIMED_OUT=0; break; fi
        kill -0 "$pid" 2>/dev/null || { TIMED_OUT=0; break; }
        sleep 0.5; i=$((i + 1))
    done
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
}
finished() {
    if [ "$TIMED_OUT" -eq 1 ]; then bad "$1: timed out"; else ok "$1: finished without timing out"; fi
    if [ -s "$2" ]; then ok "$1: serial output produced"; else bad "$1: silent boot failure"; fi
    expect_not "$2" "KERNEL PANIC|SELFTEST FAILED" "$1: no panic and no failed self-test"
}
disk_args() { echo "-global virtio-mmio.force-legacy=false -drive file=$1,if=none,format=raw,id=d0$2 -device virtio-blk-device,drive=d0"; }
mkimg() { dd if=/dev/zero of="$1" bs=512 count="$2" 2>/dev/null; }
sum() { md5sum "$1" | cut -d' ' -f1; }

echo "== building =="
BUILD_DIR="$OUT/normal" ./build.sh >/dev/null 2>&1 || { bad "normal build failed"; echo "fs-tests: $PASSES passed, $FAILS failed"; exit 1; }
AURORA_FS_TEST=1 BUILD_DIR="$OUT/fstest" ./build.sh >/dev/null 2>&1 || { bad "fs-test build failed"; echo "fs-tests: $PASSES passed, $FAILS failed"; exit 1; }
NORMAL="$OUT/normal/kernel.bin"; TESTK="$OUT/fstest/kernel.bin"

echo "== 1. three boots on one image =="
img="$OUT/disk.img"; mkimg "$img" 2048
for n in 1 2 3; do
    log="$OUT/boot$n.log"; run_qemu "$TESTK" "$log" $(disk_args "$img"); finished "boot$n" "$log"
    expect "$log" "AURORA-PROC-SELFTEST: $EXPECTED_PROC_TESTS passed, 0 failed" "boot$n: userspace suite still passes"
    expect_not "$log" "\[fs-test\].*: FAIL" "boot$n: no failing filesystem check"
done
expect "$OUT/boot1.log" "\[fs\] initial mount: no filesystem" "boot1: empty disk detected, not formatted automatically"
expect "$OUT/boot1.log" "AURORA-FS-SELFTEST: $EXPECTED_FS_BOOT1 passed, 0 failed" "boot1: all $EXPECTED_FS_BOOT1 checks pass"
for t in "unmounted filesystem fail with FS_EMOUNT" "oversized: writing 24 KiB" "invalid ids" "owner-only file: another principal gets FS_EACCES" "GUESSES the exact id" "full disk: a write that does not fit is FS_ENOSPC" "100 create/write/read/delete cycles" "crash injection \(write k" "crash injection for create and delete" "boot 1: filesystem formatted and persistent files written" "inject: an inode write rejected before reaching the device is a DEFINITE failure" "inject: an inode write that fails with unknown outcome returns FS_EUNCERTAIN, UNMOUNTS" "inject: an inode write that LANDED but was reported as failed" "inject: after an uncertain commit a second write is refused" "inject: after remount both files are exact" "inject: a failed flush AFTER the inode write is uncertain too" "inject: a data-block write failure" "inject: a failed data flush" "inject create" "inject delete" "inject chmod"; do
    expect "$OUT/boot1.log" "\[fs-test\] .*$t.*: PASS" "boot1: $t"
done
expect "$OUT/boot2.log" "\[fs\] initial mount: ok" "boot2: mounted without formatting"
expect "$OUT/boot2.log" "persist.txt: all 3023 bytes written by an EARLIER boot read back identical.*: PASS" "boot2: file from boot 1 read back identical in a new emulator process"
expect "$OUT/boot2.log" "boot counter was 1, now 2" "boot2: counter 1 -> 2"
expect "$OUT/boot3.log" "boot counter was 2, now 3" "boot3: counter 2 -> 3 (state keeps changing across runs)"
for n in 2 3; do
    expect "$OUT/boot$n.log" "AURORA-FS-SELFTEST: $EXPECTED_FS_BOOTN passed, 0 failed" "boot$n: all $EXPECTED_FS_BOOTN checks pass (incl. corruption suite)"
    expect "$OUT/boot$n.log" "authorization after the restart.*: PASS" "boot$n: authorization still enforced after restart"
    expect "$OUT/boot$n.log" "silent data corruption.*: PASS" "boot$n: flipped data bit is detected, never returned"
    expect "$OUT/boot$n.log" "version mismatch.*FS_EVERSION.*: PASS" "boot$n: version mismatch refused"
done
# EL0 file access (real user programs through SYS_FS)
expect "$OUT/boot1.log" "AURORA-FSPROC-SELFTEST: $EXPECTED_FSPROC_BOOT1 passed, 0 failed" "boot1: all $EXPECTED_FSPROC_BOOT1 EL0 file-access checks pass"
expect "$OUT/boot1.log" "program A \(EL0, identity 21\): creates file.*: PASS" "boot1: EL0 program A created, wrote and read back a file"
for n in 1 2 3; do
    expect "$OUT/boot$n.log" "program B \(EL0, identity 22\) with a GUESSED exact id.*: PASS" "boot$n: EL0 program B refused on the private file even with the exact id"
    expect "$OUT/boot$n.log" "hostile arguments from EL0.*: PASS" "boot$n: hostile EL0 arguments (kernel pointers, overflow) rejected safely"
    expect "$OUT/boot$n.log" "PID reuse: a new process created in the pid slot.*: PASS" "boot$n: a reused pid inherits no file identity"
done
for n in 2 3; do
    expect "$OUT/boot$n.log" "AURORA-FSPROC-SELFTEST: $EXPECTED_FSPROC_BOOTN passed, 0 failed" "boot$n: all $EXPECTED_FSPROC_BOOTN EL0 file-access checks pass"
    expect "$OUT/boot$n.log" "after the RESTART: a new process with identity 21 finds 'note' written by an earlier boot.*: PASS" "boot$n: a new EL0 process with the same application identity reads the file persisted by an earlier QEMU run"
done
# host-side proof
if [ "$(dd if="$img" bs=512 count=1 2>/dev/null | head -c 8)" = "AURORAFS" ]; then ok "host: sector 0 of the image carries the AuroraFS magic"; else bad "host: no AuroraFS magic in the image"; fi
if grep -c "AURORA-FILE-CONTENT-v1" "$img" >/dev/null 2>&1; then ok "host: the file content 'AURORA-FILE-CONTENT-v1' is physically inside the image file"; else bad "host: file content not found in the image"; fi

echo "== 2. host-side damage: reported precisely, never repaired =="
damage() { # name description  (uses $OUT/disk.img copy)
    cp "$img" "$OUT/$1.img"
}
# (a) one bit flipped in inode 0 (sector 1, byte 40)
damage inode; printf '\377' | dd of="$OUT/inode.img" bs=1 seek=$((512 + 40)) conv=notrunc 2>/dev/null
# (b) wrong version in the superblock
damage version; printf '\002' | dd of="$OUT/version.img" bs=1 seek=8 conv=notrunc 2>/dev/null
# (c) foreign content: sector 0 overwritten with text
damage foreign; printf 'THIS IS NOT AN AURORAFS DISK' | dd of="$OUT/foreign.img" bs=1 seek=0 conv=notrunc 2>/dev/null
# (d) superblock flipped bit
damage super; printf '\377' | dd of="$OUT/super.img" bs=1 seek=17 conv=notrunc 2>/dev/null
for c in "inode:CORRUPT" "version:unsupported filesystem version" "foreign:no filesystem on the disk" "super:CORRUPT"; do
    name="${c%%:*}"; want="${c#*:}"; dimg="$OUT/$name.img"; before="$(sum "$dimg")"
    log="$OUT/dmg-$name.log"; run_qemu "$NORMAL" "$log" $(disk_args "$dimg"); finished "damage-$name" "$log"
    expect "$log" "\[fs\] mount: .*$want" "damage-$name: reported as '$want'"
    expect "$log" "AURORA-PROC-SELFTEST: $EXPECTED_PROC_TESTS passed, 0 failed" "damage-$name: system still boots and passes its suites"
    if [ "$(sum "$dimg")" = "$before" ]; then ok "damage-$name: image byte-for-byte unchanged (no repair, no reformat)"; else bad "damage-$name: the image was modified"; fi
done
# a healthy filesystem mounts in a normal kernel, read-only in effect
before="$(sum "$img")"; log="$OUT/normal-good.log"; run_qemu "$NORMAL" "$log" $(disk_args "$img"); finished normal-good "$log"
expect "$log" "\[fs\] mount: AuroraFS v1 mounted.*5 files" "normal kernel: mounts the healthy filesystem (5 files: 3 from the kernel tests, 2 created by EL0)"
if [ "$(sum "$img")" = "$before" ]; then ok "normal kernel: mounting wrote nothing"; else bad "normal kernel: image changed by a normal boot"; fi

echo "== 3. read-only disk with a filesystem =="
before="$(sum "$img")"; log="$OUT/ro.log"; run_qemu "$TESTK" "$log" $(disk_args "$img" ,readonly=on); finished readonly "$log"
expect "$log" "AURORA-FS-SELFTEST: $EXPECTED_FS_RO passed, 0 failed" "readonly: $EXPECTED_FS_RO checks pass"
expect "$log" "read-only disk: write, create, delete, set_perm and format are all refused with FS_ERO.*: PASS" "readonly: every modification refused with FS_ERO"
if [ "$(sum "$img")" = "$before" ]; then ok "readonly: image unchanged"; else bad "readonly: image changed"; fi

echo "== 4. no disk =="
log="$OUT/nodisk.log"; run_qemu "$NORMAL" "$log"; finished nodisk "$log"
expect_not "$log" "\[fs\] mount" "nodisk: no filesystem activity"
expect "$log" "AURORA-PROC-SELFTEST: $EXPECTED_PROC_TESTS passed, 0 failed" "nodisk: userspace suite passes"

echo "fs-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
