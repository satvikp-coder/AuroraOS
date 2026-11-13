#!/bin/sh
# tests/qemu/loader-tests.sh — program loader and trusted launch (issue #6, milestone D). Disposable images under build/loader-tests/ only.
#
#   The executables are compiled from tests/programs/*.c on the host and written into an AuroraFS image by tests/tools/mkafs (which links the REAL
#   kernel/fs/afs.c), owned by PRINCIPAL_INSTALLER. The kernel under test therefore does not contain them: the script proves that with a search of
#   the kernel binary for their marker strings and of the image for the same strings.
#   1. three QEMU runs on the SAME image: the kernel mounts it WITHOUT formatting, launches programs by name from AuroraFS and runs them at EL0.
#      Each program keeps a boot counter in a file it creates itself, so the counters must read 0->1, 1->2, 2->3 across the three emulator processes.
#   2. the same kernel with no disk and with an empty (unformatted) disk: the in-RAM loader tests still run, the end-to-end part is skipped, no panic.
#
# Usage: ./tests/qemu/loader-tests.sh      Requires aarch64-linux-gnu-gcc, qemu-system-aarch64 and a host C compiler.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1

EXPECTED_FULL=23         # 8 in-RAM loader checks + 15 end-to-end checks
EXPECTED_NODISK=8
EXPECTED_PROC_TESTS=79

OUT="$ROOT/build/loader-tests"
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
    while [ "$i" -lt 360 ]; do
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
AURORA_LOAD_TEST=1 BUILD_DIR="$OUT/kernel" ./build.sh >/dev/null 2>&1 || { bad "load-test kernel build failed"; echo "loader-tests: $PASSES passed, $FAILS failed"; exit 1; }
KERNEL="$OUT/kernel/kernel.bin"
CC="${CROSS_COMPILE:-aarch64-linux-gnu-}gcc"
PF="-O1 -ffreestanding -fno-builtin -nostdlib -static -no-pie -fno-pic -fno-pie -mgeneral-regs-only -fno-stack-protector -Wall -Wextra -Werror -Wl,-T,tests/programs/user.ld -Wl,-z,noexecstack -Wl,-z,max-page-size=4096 -Wl,--build-id=none"
P="$OUT/programs"; mkdir -p "$P"
$CC $PF -DAPP_ALPHA -o "$P/alpha.elf" tests/programs/app.c && $CC $PF -DAPP_BETA -o "$P/beta.elf" tests/programs/app.c && $CC $PF -o "$P/fault.elf" tests/programs/fault.c \
    && ok "test programs compiled (alpha, beta, fault) as static AArch64 ET_EXEC" || { bad "test programs failed to compile"; echo "loader-tests: $PASSES passed, $FAILS failed"; exit 1; }
cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs (the real afs.c over a file) compiled" || { bad "mkafs failed to compile"; echo "loader-tests: $PASSES passed, $FAILS failed"; exit 1; }

# files that are not programs
head -c 700 /dev/zero | tr '\0' 'x' > "$P/corrupt.bin"; printf 'CORRUPT-ME-MARKER' | dd of="$P/corrupt.bin" conv=notrunc 2>/dev/null
printf 'THIS IS TEXT, NOT AN EXECUTABLE. It has a valid AuroraFS checksum but no ELF header at all, so the loader must refuse it.\n' > "$P/bad.bin"

# ---- kernel does not contain the programs ----
if grep -aq "AURORA-PROGRAM-" "$KERNEL" "$OUT/kernel/kernel.elf"; then bad "the kernel binary contains a program marker string (programs must not be in the kernel)"; else ok "the kernel binary and ELF contain no program marker string"; fi

echo "== provisioning (host tool, installer-owned executables) =="
img="$OUT/disk.img"
"$OUT/mkafs" "$img" 2048 \
    installer 0x7 alpha.elf "$P/alpha.elf"   installer 0x7 beta.elf "$P/beta.elf"   installer 0x7 fault.elf "$P/fault.elf" \
    installer 0xF loose.elf "$P/alpha.elf"   installer 0x7 corrupt.elf "$P/corrupt.bin"   installer 0x7 bad.elf "$P/bad.bin" \
    105 0x7 shadow.elf "$P/alpha.elf" > "$OUT/mkafs.log" 2>&1 && ok "image provisioned: 6 installer-owned files + 1 application-owned look-alike" || { bad "mkafs failed"; cat "$OUT/mkafs.log"; }
if grep -aq "AURORA-PROGRAM-ALPHA" "$img" && grep -aq "AURORA-PROGRAM-BETA" "$img"; then ok "the programs' marker strings are physically inside the disk image"; else bad "program markers not found in the image"; fi
# damage ONE data byte of corrupt.elf behind its checksum
off="$(grep -abo "CORRUPT-ME-MARKER" "$img" | head -1 | cut -d: -f1)"
if [ -n "$off" ]; then printf '\377' | dd of="$img" bs=1 seek=$((off + 100)) conv=notrunc 2>/dev/null; ok "host flipped a byte inside corrupt.elf's data block (offset $((off + 100)))"; else bad "could not find corrupt.elf in the image"; fi

echo "== 1. three boots on one image =="
for n in 1 2 3; do
    log="$OUT/boot$n.log"; run_qemu "$KERNEL" "$log" $(disk_args "$img"); finished "boot$n" "$log"
    expect "$log" "\[fs\] mount: AuroraFS v1 mounted" "boot$n: mounted WITHOUT formatting"
    expect "$log" "AURORA-PROC-SELFTEST: $EXPECTED_PROC_TESTS passed, 0 failed" "boot$n: userspace suite still passes"
    expect "$log" "AURORA-LOAD-SELFTEST: $EXPECTED_FULL passed, 0 failed" "boot$n: all $EXPECTED_FULL loader checks pass"
    expect_not "$log" "\[load-test\].*: FAIL" "boot$n: no failing loader check"
    expect "$log" "\[user p[0-9]\] AURORA-PROGRAM-ALPHA running from AuroraFS" "boot$n: alpha.elf, loaded from the disk, printed its banner from EL0"
    expect "$log" "\[user p[0-9]\] AURORA-PROGRAM-BETA running from AuroraFS" "boot$n: beta.elf, loaded from the disk, printed its banner from EL0"
    expect "$log" "\[load\] alpha exit code 0, beta exit code 0" "boot$n: both programs exited 0 (every in-program check held)"
    expect "$log" "\[user p[0-9]\] AURORA-PROGRAM-FAULT about to misbehave" "boot$n: the fault program ran before it misbehaved"
done
expect "$OUT/boot1.log" "alpha counter before=0 after=1 beta counter before=0 after=1" "boot1: both counter files created by the programs (0 -> 1)"
# Every boot runs alpha three times (the main run, the post-fault sanity run, the pid-reuse run) and beta twice, so the counters advance by 3 and 2 per boot.
expect "$OUT/boot2.log" "alpha counter before=3 after=4 beta counter before=2 after=3" "boot2: counters written by the programs in the previous QEMU process are read back and bumped (alpha 3 -> 4, beta 2 -> 3)"
expect "$OUT/boot3.log" "alpha counter before=6 after=7 beta counter before=4 after=5" "boot3: alpha 6 -> 7, beta 4 -> 5 (state keeps changing across emulator runs)"
for t in "registry: applications are registered" "launch refused: an unregistered application name" "launch refused: a registered application whose executable does not exist" "launch refused: a file of the right name owned by the APPLICATION" "launch refused: an installer-owned executable that OTHERS may write" "launch refused: an installer-owned executable with a damaged data block" "launch refused: valid file, valid checksum, but not an executable" "programs: alpha.elf and beta.elf are launched BY NAME" "persistence: each program's counter file" "identity follows the registry, not the code" "faults: a loaded program that writes its own code" "faults: after five faulting programs" "PID reuse: beta started in the pid slot alpha just vacated" "no leaks: after every disk-launched scenario"; do
    expect "$OUT/boot3.log" "\[load-test\] $t.*: PASS" "boot3: $t"
done
for t in "loader: an image built in RAM" "loader: 20 hostile executables" "loader: with exactly 0..10 free pages" "loader: all 8 process slots" "loader: 300 mixed failing"; do
    expect "$OUT/boot1.log" "\[load-test\] $t.*: PASS" "boot1: $t"
done
expect "$OUT/boot3.log" "\[fs\] mount: AuroraFS v1 mounted.*10 files" "after three boots the image holds 10 files (7 provisioned + alpha/beta counters + the clone's own alpha.dat)"

echo "== 2. no disk / empty disk =="
log="$OUT/nodisk.log"; run_qemu "$KERNEL" "$log"; finished nodisk "$log"
expect "$log" "AURORA-LOAD-SELFTEST: $EXPECTED_NODISK passed, 0 failed" "nodisk: the $EXPECTED_NODISK in-RAM loader checks pass"
expect "$log" "end-to-end tests not run" "nodisk: end-to-end part skipped, not faked"
empty="$OUT/empty.img"; dd if=/dev/zero of="$empty" bs=512 count=2048 2>/dev/null; before="$(md5sum "$empty" | cut -d' ' -f1)"
log="$OUT/empty.log"; run_qemu "$KERNEL" "$log" $(disk_args "$empty"); finished empty "$log"
expect "$log" "AURORA-LOAD-SELFTEST: $EXPECTED_NODISK passed, 0 failed" "empty disk: only the in-RAM loader checks run"
if [ "$(md5sum "$empty" | cut -d' ' -f1)" = "$before" ]; then ok "empty disk: untouched (the loader tests never format a disk)"; else bad "empty disk: image was modified"; fi

echo "loader-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
