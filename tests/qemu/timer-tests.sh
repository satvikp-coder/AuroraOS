#!/bin/sh
# tests/qemu/timer-tests.sh - SYS_TIMER (42), the native timer service, against the REAL shell with EL0 clients (kernel built with AURORA_SHELL_CLIENTS,
# test-only; the timer capability is granted by kernel code in the test client table).
#   basic    a client with the capability (timers, quota, cancel, completion event) next to a client WITHOUT it
#   hostile  bad durations / ops / pointers / ids and a 20,000-iteration flood, while Tab keeps changing the screen
#   owners   a holder, a spy on foreign ids, a client that exits and one that faults holding timers, and a late client that must get its full quota
#   starved  the basic client while two CPU-bound spinners run: completion is still prompt
# Real QMP key events + screendump pixels prove the shell stays interactive; "[user pN] tm: ..." lines are what the EL0 clients saw.
# QEMU is started and stopped only by this script. TCG only.
# Usage: ./tests/qemu/timer-tests.sh        (TIMER_ONLY="basic owners" runs a subset)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/timer-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
FAILS=0; PASSES=0
ok()  { PASSES=$((PASSES + 1)); echo "  PASS: $1"; }
bad() { FAILS=$((FAILS + 1));   echo "  FAIL: $1"; }
QPID=""
stop_qemu() { if [ -n "$QPID" ]; then kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""; fi; }
trap stop_qemu EXIT
trap 'stop_qemu; exit 130' INT TERM
LEG="-global virtio-mmio.force-legacy=false"
CC="${CROSS_COMPILE:-aarch64-linux-gnu-}gcc"
PF="-O1 -ffreestanding -fno-builtin -nostdlib -static -no-pie -fno-pic -fno-pie -mgeneral-regs-only -fno-stack-protector -Wall -Wextra -Werror -Wl,-T,tests/programs/user.ld -Wl,-z,noexecstack -Wl,-z,max-page-size=4096 -Wl,--build-id=none"
$CC $PF -o "$OUT/timerclient.elf" tests/programs/timerclient.c && ok "the EL0 timer client compiles as a static AArch64 ET_EXEC" || { bad "timerclient failed to compile"; echo "timer-tests: $PASSES passed, $FAILS failed"; exit 1; }
$CC $PF -o "$OUT/shclient.elf" tests/programs/shclient.c && ok "the CPU-bound spinner client compiles" || { bad "shclient failed to compile"; exit 1; }
cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs compiled" || { bad "mkafs failed to compile"; exit 1; }
"$OUT/mkafs" "$OUT/disk.img" 2048 installer 0x7 timerclient.elf "$OUT/timerclient.elf" installer 0x7 shclient.elf "$OUT/shclient.elf" > "$OUT/mkafs.log" 2>&1 && ok "disk image provisioned with the installer-owned clients" || { bad "mkafs failed"; cat "$OUT/mkafs.log"; }

boot() {   # scenario mask
    sc="$1"; mask="$2"
    echo "== scenario: $sc (clients mask $mask) =="
    AURORA_SHELL_CLIENTS="$mask" BUILD_DIR="$OUT/k_$sc" ./build.sh >/dev/null 2>&1 || { bad "$sc: kernel build failed"; return; }
    cp "$OUT/disk.img" "$OUT/disk_$sc.img"
    L="$OUT/$sc.log"; : > "$L"; rm -f "$OUT/$sc.sock"
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$OUT/k_$sc/kernel.bin" \
        -display none -serial file:"$L" -monitor none -qmp unix:"$OUT/$sc.sock",server=on,wait=off \
        -device ramfb $LEG -drive file="$OUT/disk_$sc.img",if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0 \
        -device virtio-keyboard-device -device virtio-tablet-device &
    QPID=$!
    python3 "$SCRIPT_DIR/timer_check.py" --qmp "$OUT/$sc.sock" --serial "$L" --tmp "$OUT" --scenario "$sc" --timeout 420 > "$OUT/$sc.check.log" 2>&1
    rc=$?; cat "$OUT/$sc.check.log"
    if [ "$rc" -eq 0 ]; then ok "$sc: all checks passed"; else bad "$sc: checks failed (see $OUT/$sc.check.log)"; fi
    stop_qemu
}
want() { [ -z "$TIMER_ONLY" ] || case " $TIMER_ONLY " in *" $1 "*) return 0;; *) return 1;; esac; }
want basic && boot basic $((32768 + 131072))
want hostile && boot hostile 65536
want owners && boot owners $((262144 + 524288 + 1048576 + 2097152 + 4194304))
want starved && boot starved $((32768 + 1))
echo "timer-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
