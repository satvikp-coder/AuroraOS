#!/bin/sh
# tests/qemu/e2e-tests.sh — issue #6 end to end: storage + program loader + settings registry TOGETHER, on one kernel and one disposable image.
#
#   One test kernel (AURORA_LOAD_TEST=1 AURORA_SET_TEST=1) and one AuroraFS image provisioned by the host (tests/tools/mkafs). Three QEMU processes
#   run on the SAME image. In every boot the kernel launches EL0 programs FROM THE DISK (alpha, beta, the faulting programs, the settings client), those
#   programs write files and settings through system calls, and everything persists to the next emulator process. The individual suites
#   (loader-tests.sh, settings-tests.sh) prove each subsystem; this script proves they work on the same disk at the same time and nothing in one
#   disturbs the other: files written by loaded programs survive next to the settings record, the settings client launches from a disk that already
#   holds program data, and the host finds both the programs' files and the settings record in the image.
#
# Usage: ./tests/qemu/e2e-tests.sh      Requires aarch64-linux-gnu-gcc, qemu-system-aarch64 and a host C compiler.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1

EXPECTED_LOAD=23
EXPECTED_SET=18
EXPECTED_PROC_TESTS=79

OUT="$ROOT/build/e2e-tests"
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
    while [ "$i" -lt 480 ]; do
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
disk_args() { echo "-global virtio-mmio.force-legacy=false -drive file=$1,if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0"; }

echo "== building =="
AURORA_LOAD_TEST=1 AURORA_SET_TEST=1 BUILD_DIR="$OUT/kernel" ./build.sh >/dev/null 2>&1 || { bad "kernel build failed"; echo "e2e-tests: $PASSES passed, $FAILS failed"; exit 1; }
KERNEL="$OUT/kernel/kernel.bin"
CC="${CROSS_COMPILE:-aarch64-linux-gnu-}gcc"
PF="-O1 -ffreestanding -fno-builtin -nostdlib -static -no-pie -fno-pic -fno-pie -mgeneral-regs-only -fno-stack-protector -Wall -Wextra -Werror -Wl,-T,tests/programs/user.ld -Wl,-z,noexecstack -Wl,-z,max-page-size=4096 -Wl,--build-id=none"
P="$OUT/programs"; mkdir -p "$P"
$CC $PF -DAPP_ALPHA -o "$P/alpha.elf" tests/programs/app.c && $CC $PF -DAPP_BETA -o "$P/beta.elf" tests/programs/app.c && $CC $PF -o "$P/fault.elf" tests/programs/fault.c && $CC $PF -o "$P/setapp.elf" tests/programs/setapp.c && $CC $PF -o "$P/init.elf" tests/programs/init.c \
    && ok "programs compiled (alpha, beta, fault, setapp, init)" || { bad "programs failed to compile"; echo "e2e-tests: $PASSES passed, $FAILS failed"; exit 1; }
cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs compiled" || { bad "mkafs failed to compile"; echo "e2e-tests: $PASSES passed, $FAILS failed"; exit 1; }
head -c 700 /dev/zero | tr '\0' 'x' > "$P/corrupt.bin"; printf 'CORRUPT-ME-MARKER' | dd of="$P/corrupt.bin" conv=notrunc 2>/dev/null
printf 'THIS IS TEXT, NOT AN EXECUTABLE. It has a valid AuroraFS checksum but no ELF header at all, so the loader must refuse it.\n' > "$P/bad.bin"
if grep -aq "AURORA-PROGRAM-\|SETAPP" "$KERNEL" "$OUT/kernel/kernel.elf"; then bad "a program marker string is inside the kernel binary"; else ok "no program or client string is inside the kernel binary"; fi

img="$OUT/disk.img"
"$OUT/mkafs" "$img" 4096 \
    installer 0x7 alpha.elf "$P/alpha.elf"   installer 0x7 beta.elf "$P/beta.elf"   installer 0x7 fault.elf "$P/fault.elf"   installer 0x7 setapp.elf "$P/setapp.elf"   installer 0x7 init.elf "$P/init.elf" \
    installer 0xF loose.elf "$P/alpha.elf"   installer 0x7 corrupt.elf "$P/corrupt.bin"   installer 0x7 bad.elf "$P/bad.bin" \
    105 0x7 shadow.elf "$P/alpha.elf" > "$OUT/mkafs.log" 2>&1 && ok "image provisioned: 8 installer-owned files (incl. init.elf) + 1 application-owned look-alike" || { bad "mkafs failed"; cat "$OUT/mkafs.log"; }
off="$(grep -abo "CORRUPT-ME-MARKER" "$img" | head -1 | cut -d: -f1)"
if [ -n "$off" ]; then printf '\377' | dd of="$img" bs=1 seek=$((off + 100)) conv=notrunc 2>/dev/null; ok "host damaged one data byte of corrupt.elf"; else bad "could not find corrupt.elf in the image"; fi

echo "== three boots on one image =="
prev=100
for n in 1 2 3; do
    log="$OUT/boot$n.log"; run_qemu "$KERNEL" "$log" $(disk_args "$img"); finished "boot$n" "$log"
    expect "$log" "\[fs\] mount: AuroraFS v1 mounted" "boot$n: mounted WITHOUT formatting"
    expect "$log" "AURORA-PROC-SELFTEST: $EXPECTED_PROC_TESTS passed, 0 failed" "boot$n: userspace suite still passes"
    expect "$log" "AURORA-LOAD-SELFTEST: $EXPECTED_LOAD passed, 0 failed" "boot$n: all $EXPECTED_LOAD loader checks pass"
    expect "$log" "AURORA-SET-SELFTEST: $EXPECTED_SET passed, 0 failed" "boot$n: all $EXPECTED_SET settings checks pass (on a disk that also holds program data)"
    expect_not "$log" "\[(load|set)-test\].*: FAIL" "boot$n: no failing loader or settings check"
    expect "$log" "\[load\] alpha exit code 0, beta exit code 0" "boot$n: alpha.elf and beta.elf, loaded from the disk, ran concurrently and exited 0"
    expect "$log" "\[user p[0-9]\] SETAPP writer done" "boot$n: the settings client, loaded from the same disk, saved settings through SYS_SETTINGS"
    expect "$log" "\[init\] started init.elf as pid [0-9]+, identity 1" "boot$n: the kernel started the installed init.elf as the first user process (identity 1)"
    expect "$log" "\[user p[0-9]\] AURORA-INIT started by the kernel" "boot$n: init, loaded from the disk, ran at EL0"
    expect "$log" "\[user p[0-9]\] INIT boot count $n" "boot$n: init kept its boot counter in its own file ($n)"
    expect "$log" "\[init\] init exited with code 0" "boot$n: init exited 0 and the boot went on"
    next=$((prev + 10))
    expect "$log" "\[set\] final saved text_scale=$next" "boot$n: settings state $next left on the disk"
    if [ "$n" -gt 1 ]; then expect "$log" "\[set\] boot: status=0 \(loaded from disk\) theme=2 text_scale=$prev reduced_motion=1 focus=2 aura=2 notifications=1" "boot$n: a new QEMU process loaded the settings saved by the previous one, with program files on the same disk"; fi
    prev=$next
done
expect "$OUT/boot1.log" "alpha counter before=0 after=1 beta counter before=0 after=1" "boot1: program-written counter files created (0 -> 1)"
expect "$OUT/boot2.log" "alpha counter before=3 after=4 beta counter before=2 after=3" "boot2: program-written counters survived the restart and advanced"
expect "$OUT/boot3.log" "alpha counter before=6 after=7 beta counter before=4 after=5" "boot3: and again"
expect "$OUT/boot3.log" "\[fs\] mount: AuroraFS v1 mounted.*14 files" "after boots 1-2 the image holds 14 files: 9 provisioned, alpha.dat, beta.dat, the clone's alpha.dat, settings.v1, init.log"
echo "== a PRODUCTION kernel (no test code) on the same image starts init too =="
AURORA_PRODUCTION=1 BUILD_DIR="$OUT/prod" ./build.sh >/dev/null 2>&1 && ok "production kernel built" || bad "production kernel build failed"
log="$OUT/prod.log"; run_qemu "$OUT/prod/kernel.bin" "$log" $(disk_args "$img"); finished prod "$log"
expect "$log" "\[init\] started init.elf as pid [0-9]+, identity 1" "production: the kernel started init.elf from the disk"
expect "$log" "\[user p[0-9]\] INIT boot count 4" "production: init's counter file written by the three earlier test boots read back and advanced (3 -> 4)"
expect "$log" "\[init\] init exited with code 0" "production: init exited 0"
expect "$log" "Boot complete" "production: the boot finished"
expect_not "$log" "\[(load|set|fs|fsproc)-test\]|AURORA-(LOAD|SET|FS)" "production: no loader, settings or filesystem test code ran"
# host-side proof of every layer
for s in "AURORAFS" "AURORA-PROGRAM-ALPHA" "AURORA-PROGRAM-BETA" "ASET" "ALPHA" "BETA" "INIT"; do
    if grep -aq "$s" "$img"; then ok "host: '$s' is physically inside the image file"; else bad "host: '$s' not found in the image"; fi
done

echo "e2e-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
