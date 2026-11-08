#!/bin/sh
# tests/qemu/storage-tests.sh — QEMU regression tests for the virtio block driver (milestone A of issue #6).
#
# Every scenario uses a DISPOSABLE image file created under build/storage-tests/. No physical disk, host partition or
# existing file is ever opened. Scenarios:
#   1. no disk attached: a normal kernel boots, reports "no block device", every other suite still passes.
#   2. destructive driver tests on a fresh 1 MiB disk, then a SECOND, separate QEMU run on the SAME image: the marker
#      written by boot 1 is read back by boot 2, and the host independently finds the bytes in the image file.
#   3. a normal (non-test) kernel with a disk attached never writes to it (image checksum unchanged).
#   4. a read-only disk: reads work, writes are refused, the image is unchanged.
#   5. a legacy-transport device is reported and refused (no guessing), boot continues, image unchanged.
#   6. a tiny and a larger disk report their true capacity.
# Silent boots, timeouts, wrong counts or an unexpected panic are FAILURES.
#
# Usage: ./tests/qemu/storage-tests.sh      Requires the aarch64 toolchain and qemu-system-aarch64.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1

EXPECTED_BLK_TESTS=13      # keep in sync with kernel/blktest.c (read/write disk)
EXPECTED_BLK_TESTS_RO=3    # identify x2 + the read-only check
EXPECTED_PROC_TESTS=79

OUT="$ROOT/build/storage-tests"
rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
expect()     { if grep -Eq -- "$2" "$1"; then ok "$3"; else bad "$3 (pattern: $2)"; fi; }
expect_not() { if grep -Eq -- "$2" "$1"; then bad "$3 (found: $2)"; else ok "$3"; fi; }

TIMED_OUT=0
# run_qemu kernel log [extra qemu args...]
run_qemu() {
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
disk_args() { # image [extra drive options]
    echo "-global virtio-mmio.force-legacy=false -drive file=$1,if=none,format=raw,id=d0$2 -device virtio-blk-device,drive=d0"
}
mkimg() { dd if=/dev/zero of="$1" bs=512 count="$2" 2>/dev/null; }
sum() { md5sum "$1" | cut -d' ' -f1; }

echo "== building =="
BUILD_DIR="$OUT/normal" ./build.sh >/dev/null 2>&1 || { bad "normal build failed"; echo "storage-tests: $PASSES passed, $FAILS failed"; exit 1; }
AURORA_BLK_TEST=1 BUILD_DIR="$OUT/blktest" ./build.sh >/dev/null 2>&1 || { bad "block-test build failed"; echo "storage-tests: $PASSES passed, $FAILS failed"; exit 1; }
NORMAL="$OUT/normal/kernel.bin"; TESTK="$OUT/blktest/kernel.bin"

echo "== 1. no disk attached =="
log="$OUT/nodisk.log"; run_qemu "$NORMAL" "$log"; finished nodisk "$log"
expect "$log" "\[blk\] no block device attached" "nodisk: reported as a normal condition, not an error"
expect "$log" "AURORA-PROC-SELFTEST: $EXPECTED_PROC_TESTS passed, 0 failed" "nodisk: userspace suite still passes"
expect_not "$log" "unusable" "nodisk: not treated as a broken device"

echo "== 2. destructive driver tests + persistence across two QEMU runs =="
img="$OUT/disk1.img"; mkimg "$img" 2048
log1="$OUT/boot1.log"; run_qemu "$TESTK" "$log1" $(disk_args "$img"); finished boot1 "$log1"
expect "$log1" "device at 0x[0-9a-f]+, transport version 2, SPI interrupt ID [0-9]+ \(polled, unused\), features hi/lo 0x[0-9a-f]+, capacity 2048 sectors" "boot1: device found through the device tree; modern transport; capacity read from the device"
expect "$log1" "AURORA-BLK-SELFTEST: $EXPECTED_BLK_TESTS passed, 0 failed" "boot1: all $EXPECTED_BLK_TESTS block checks pass"
expect "$log1" "persistence: first boot on this disk, marker written and flushed: PASS" "boot1: marker written and flushed"
expect "$log1" "a request whose doorbell is lost times out.*: PASS" "boot1: lost doorbell gives a bounded timeout and the driver recovers"
expect "$log1" "bounds: .*: PASS" "boot1: out-of-range requests are refused before the device"
expect_not "$log1" "\[blk-test\].*FAIL" "boot1: no failing block check"
expect "$log1" "AURORA-PROC-SELFTEST: $EXPECTED_PROC_TESTS passed, 0 failed" "boot1: userspace suite unaffected by the driver"
# host-side proof: the marker really is in the image file, at sector 100 (offset 51200)
if [ "$(dd if="$img" bs=512 skip=100 count=1 2>/dev/null | head -c 8)" = "AURORA-P" ]; then ok "host: the file image contains the marker at sector 100"; else bad "host: marker not found in the image file"; fi
if [ "$(dd if="$img" bs=512 skip=2047 count=1 2>/dev/null | od -An -tx1 | head -1 | tr -d ' ')" != "0000000000000000" ]; then ok "host: the last sector of the image was written"; else bad "host: last sector still zero"; fi
log2="$OUT/boot2.log"; run_qemu "$TESTK" "$log2" $(disk_args "$img"); finished boot2 "$log2"
expect "$log2" "persistence: the marker written by an EARLIER boot is on the disk, all 512 bytes identical.*: PASS" "boot2: a NEW emulator process read back the exact marker written by boot 1"
expect "$log2" "AURORA-BLK-SELFTEST: $EXPECTED_BLK_TESTS passed, 0 failed" "boot2: all block checks pass again on the used disk"
expect_not "$log2" "first boot on this disk" "boot2: the disk was not reinitialised"

echo "== 3. a normal kernel never writes to an attached disk =="
img3="$OUT/disk3.img"; mkimg "$img3" 256; printf 'DO-NOT-TOUCH' | dd of="$img3" bs=1 seek=1000 conv=notrunc 2>/dev/null
before="$(sum "$img3")"
log="$OUT/normal-with-disk.log"; run_qemu "$NORMAL" "$log" $(disk_args "$img3"); finished normal-with-disk "$log"
expect "$log" "\[blk\] block device present \(256 sectors\); not touched by a normal boot" "normal-with-disk: device seen, capacity 256 sectors, not used for tests"
if [ "$(sum "$img3")" = "$before" ]; then ok "normal-with-disk: the image is byte-for-byte unchanged"; else bad "normal-with-disk: the image changed"; fi

echo "== 4. read-only disk =="
img4="$OUT/disk4.img"; mkimg "$img4" 2048; printf 'READONLY-DATA' | dd of="$img4" bs=1 seek=0 conv=notrunc 2>/dev/null
before="$(sum "$img4")"
log="$OUT/readonly.log"; run_qemu "$TESTK" "$log" $(disk_args "$img4" ,readonly=on); finished readonly "$log"
expect "$log" "READ-ONLY" "readonly: the device advertises read-only and the driver noticed"
expect "$log" "read-only disk: reads succeed, writes are refused with BLK_ERO.*: PASS" "readonly: reads work and writes are refused by the driver"
expect "$log" "AURORA-BLK-SELFTEST: $EXPECTED_BLK_TESTS_RO passed, 0 failed" "readonly: exactly the read-only checks ran (no destructive test)"
if [ "$(sum "$img4")" = "$before" ]; then ok "readonly: the image is byte-for-byte unchanged"; else bad "readonly: the image changed"; fi

echo "== 5. legacy transport is refused, not guessed =="
img5="$OUT/disk5.img"; mkimg "$img5" 256; before="$(sum "$img5")"
log="$OUT/legacy.log"; run_qemu "$TESTK" "$log" -global virtio-mmio.force-legacy=true -drive file="$img5",if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0
finished legacy "$log"
expect "$log" "LEGACY transport; only modern \(version 2\) is supported" "legacy: reported with the remedy"
expect "$log" "block device present but unusable" "legacy: boot continues and says the device is unusable"
expect "$log" "AURORA-PROC-SELFTEST: $EXPECTED_PROC_TESTS passed, 0 failed" "legacy: the rest of the system still boots and passes"
if [ "$(sum "$img5")" = "$before" ]; then ok "legacy: the image is byte-for-byte unchanged"; else bad "legacy: the image changed"; fi

echo "== 6. capacities =="
for sectors in 512 16384; do
    img6="$OUT/disk-$sectors.img"; mkimg "$img6" "$sectors"
    log="$OUT/cap-$sectors.log"; run_qemu "$TESTK" "$log" $(disk_args "$img6"); finished "cap-$sectors" "$log"
    expect "$log" "capacity $sectors sectors" "cap-$sectors: the reported capacity is the real one"
    expect "$log" "AURORA-BLK-SELFTEST: $EXPECTED_BLK_TESTS passed, 0 failed" "cap-$sectors: block checks pass"
done

echo "storage-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
