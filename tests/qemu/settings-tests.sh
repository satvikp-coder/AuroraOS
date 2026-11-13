#!/bin/sh
# tests/qemu/settings-tests.sh — native settings registry (issue #6, milestone E). Disposable images under build/settings-tests/ only.
#
#   The EL0 client (tests/programs/setapp.c) is compiled on the host and written into an AuroraFS image by tests/tools/mkafs, owned by the installer
#   principal. Three QEMU processes use the SAME image: boot 1 finds no settings file (typed defaults), the kernel launches the client, which saves
#   settings through SYS_SETTINGS; boots 2 and 3 are new emulator processes that must load exactly what the previous boot saved, then change it again.
#   Also: the kernel's own matrix (authorisation, validation, corrupt / newer-version / missing records, injected storage failures, quota).
#   A kernel with no disk must run no settings test and not fail.
#
# Usage: ./tests/qemu/settings-tests.sh     Requires aarch64-linux-gnu-gcc, qemu-system-aarch64 and a host C compiler.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1

EXPECTED_SET=18
EXPECTED_PROC_TESTS=79

OUT="$ROOT/build/settings-tests"
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
AURORA_SET_TEST=1 BUILD_DIR="$OUT/kernel" ./build.sh >/dev/null 2>&1 || { bad "settings-test kernel build failed"; echo "settings-tests: $PASSES passed, $FAILS failed"; exit 1; }
KERNEL="$OUT/kernel/kernel.bin"
CC="${CROSS_COMPILE:-aarch64-linux-gnu-}gcc"
PF="-O1 -ffreestanding -fno-builtin -nostdlib -static -no-pie -fno-pic -fno-pie -mgeneral-regs-only -fno-stack-protector -Wall -Wextra -Werror -Wl,-T,tests/programs/user.ld -Wl,-z,noexecstack -Wl,-z,max-page-size=4096 -Wl,--build-id=none"
P="$OUT/programs"; mkdir -p "$P"
$CC $PF -o "$P/setapp.elf" tests/programs/setapp.c && ok "settings client compiled as a static AArch64 ET_EXEC" || { bad "setapp failed to compile"; echo "settings-tests: $PASSES passed, $FAILS failed"; exit 1; }
cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs compiled" || { bad "mkafs failed to compile"; echo "settings-tests: $PASSES passed, $FAILS failed"; exit 1; }
img="$OUT/disk.img"
"$OUT/mkafs" "$img" 2048 installer 0x7 setapp.elf "$P/setapp.elf" > "$OUT/mkafs.log" 2>&1 && ok "image provisioned with the installer-owned client (no settings file on it)" || bad "mkafs failed"
if grep -aq "ASET" "$img"; then bad "the fresh image already contains a settings record"; else ok "the fresh image contains no settings record"; fi

echo "== 1. three boots on one image =="
prev=100
for n in 1 2 3; do
    log="$OUT/boot$n.log"; run_qemu "$KERNEL" "$log" $(disk_args "$img"); finished "boot$n" "$log"
    expect "$log" "\[fs\] mount: AuroraFS v1 mounted" "boot$n: mounted WITHOUT formatting"
    expect "$log" "AURORA-PROC-SELFTEST: $EXPECTED_PROC_TESTS passed, 0 failed" "boot$n: userspace suite still passes"
    expect "$log" "AURORA-SET-SELFTEST: $EXPECTED_SET passed, 0 failed" "boot$n: all $EXPECTED_SET settings checks pass"
    expect_not "$log" "\[set-test\].*: FAIL" "boot$n: no failing settings check"
    expect "$log" "\[user p[0-9]\] SETAPP writer done" "boot$n: the EL0 writer, loaded from the disk, finished all its checks"
    expect "$log" "\[user p[0-9]\] SETAPP guest done" "boot$n: the EL0 guest (no write capability) was refused everything it tried"
    expect "$log" "\[user p[0-9]\] SETAPP system-ui done" "boot$n: the EL0 system UI wrote only the system setting"
    expect "$log" "\[user p[0-9]\] SETAPP reader done" "boot$n: the EL0 reader read back exactly the saved values"
    next=$((prev + 10))
    expect "$log" "\[set\] saved text_scale=$next" "boot$n: the EL0 writer saved text scale $next"
    expect "$log" "\[set\] re-read from the disk: text_scale=$next" "boot$n: re-reading the record from the disk gives $next"
    expect "$log" "\[set\] final saved text_scale=$next" "boot$n: final state $next is on the disk for the next boot"
    if [ "$n" -gt 1 ]; then expect "$log" "\[set\] boot: status=0 \(loaded from disk\) theme=2 text_scale=$prev reduced_motion=1 focus=2 aura=2 notifications=1" "boot$n: the new QEMU process loaded EXACTLY what the previous boot saved (text scale $prev, theme dark, motion reduced, focus rest, aura 2, notifications priority)"; fi
    prev=$next
done
expect "$OUT/boot1.log" "\[set\] boot: status=1 \(no saved settings: defaults\) theme=0 text_scale=100 reduced_motion=0 focus=0 aura=1 notifications=0" "boot1: no settings file existed: typed defaults"
for t in "capabilities:" "authorisation and validation:" "EL0 writer" "saved before it took effect" "EL0 guest" "EL0 system UI" "EL0 reader" "the saved record \(40 bytes" "corrupt record" "version mismatch" "missing file" "storage failure A" "storage failure B" "storage failure C" "quota: with the settings principal" "the final state is on the disk" "no leaks"; do
    expect "$OUT/boot3.log" "\[set-test\] $t.*: PASS" "boot3: $t"
done
# host-side proof
if grep -aq "ASET" "$img"; then ok "host: the settings record ('ASET') is physically inside the image"; else bad "host: no settings record in the image"; fi
if grep -aq "AURORA-PROGRAM" "$KERNEL"; then bad "host: unexpected program marker in the kernel"; else ok "host: the kernel binary does not contain the client's code or text"; fi
if grep -aq "SETAPP writer done" "$KERNEL" "$OUT/kernel/kernel.elf"; then bad "host: the client's strings are inside the kernel (it must come from the disk)"; else ok "host: the client's strings are not inside the kernel binary"; fi

echo "== 2. no disk =="
log="$OUT/nodisk.log"; run_qemu "$KERNEL" "$log"; finished nodisk "$log"
expect "$log" "no mounted filesystem: settings tests not run" "nodisk: the settings tests are skipped, not faked"
expect "$log" "AURORA-PROC-SELFTEST: $EXPECTED_PROC_TESTS passed, 0 failed" "nodisk: userspace suite passes"

echo "settings-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
