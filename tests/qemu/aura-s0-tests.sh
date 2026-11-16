#!/bin/sh
# tests/qemu/aura-s0-tests.sh - authorised S0 Aura navigation through the Action Broker (Cycle 18). EL0 clients ask the broker with SYS_UI op 3; the policy
# decides from the kernel-attested identity, and only the shell executes. Three sequential boots of the REAL shell (kernel built with
# AURORA_SHELL_CLIENTS=<mask>, test-only; the navigate right is granted by kernel code):
#   allow  a client that holds the navigate right: Open Pages and Open Settings really change the screen; everything else it asks is refused
#   deny   a client that holds no right: every navigation is denied and the shell never moves
#   crash  a client that asks and faults at once: the queued request must not outlive it
# Real QMP key events are not needed here; evidence is guest "[user pN]" and "aura:" lines plus screendump pixels. TCG only.
# Usage: ./tests/qemu/aura-s0-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/aura-s0-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
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
$CC $PF -o "$OUT/uiclient.elf" tests/programs/uiclient.c && ok "the EL0 UI client compiles as a static AArch64 ET_EXEC" || { bad "uiclient failed to compile"; echo "aura-s0-tests: $PASSES passed, $FAILS failed"; exit 1; }
cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs compiled" || { bad "mkafs failed to compile"; exit 1; }
"$OUT/mkafs" "$OUT/disk.img" 2048 installer 0x7 uiclient.elf "$OUT/uiclient.elf" > "$OUT/mkafs.log" 2>&1 && ok "disk image provisioned with the installer-owned client" || bad "mkafs failed"

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
    python3 "$SCRIPT_DIR/aura_s0_check.py" --qmp "$OUT/$sc.sock" --serial "$L" --tmp "$OUT" --scenario "$sc" --timeout 480 > "$OUT/$sc.check.log" 2>&1
    rc=$?; cat "$OUT/$sc.check.log"
    if [ "$rc" -eq 0 ]; then ok "$sc: all checks passed"; else bad "$sc: checks failed (see $OUT/$sc.check.log)"; fi
    stop_qemu
}
want() { [ -z "$AURA_S0_ONLY" ] || case " $AURA_S0_ONLY " in *" $1 "*) return 0;; *) return 1;; esac; }
want allow && boot allow 2048
want deny && boot deny 4096
want crash && boot crash 8192
echo "aura-s0-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
