#!/bin/sh
# tests/qemu/pages-tests.sh - the native Pages screen (Cycle 18): a read-only live view of the REAL process table, driven by real QMP key and
# pointer events and verified on screendump PIXELS plus the guest's own "[user pN]" and "shell: pages" lines. Two sequential boots:
#   live   EL0 clients running (two CPU-bound, a yielder, a blocked one; AURORA_SHELL_CLIENTS=7): Pages lists them with their real states,
#          the numbers change while it is open, Home/Index/pointer/Back navigation works, the clients keep running
#   empty  no clients: the honest empty state
# QEMU is started and stopped only by this script, by PID. TCG only: no claim about real hardware.
# Usage: ./tests/qemu/pages-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/pages-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
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
$CC $PF -o "$OUT/shclient.elf" tests/programs/shclient.c && ok "the EL0 test client compiles as a static AArch64 ET_EXEC" || { bad "shclient failed to compile"; echo "pages-tests: $PASSES passed, $FAILS failed"; exit 1; }
cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs compiled" || { bad "mkafs failed to compile"; exit 1; }
"$OUT/mkafs" "$OUT/disk.img" 2048 installer 0x7 shclient.elf "$OUT/shclient.elf" > "$OUT/mkafs.log" 2>&1 && ok "disk image provisioned with the installer-owned client" || bad "mkafs failed"

boot() {   # scenario mask [extra qemu args...]
    sc="$1"; mask="$2"; shift 2
    echo "== scenario: $sc (clients mask $mask) =="
    AURORA_SHELL_CLIENTS="$mask" BUILD_DIR="$OUT/k_$sc" ./build.sh >/dev/null 2>&1 || { bad "$sc: kernel build failed"; return; }
    cp "$OUT/disk.img" "$OUT/disk_$sc.img"
    L="$OUT/$sc.log"; : > "$L"; rm -f "$OUT/$sc.sock"
    qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$OUT/k_$sc/kernel.bin" \
        -display none -serial file:"$L" -monitor none -qmp unix:"$OUT/$sc.sock",server=on,wait=off \
        -device ramfb $LEG -drive file="$OUT/disk_$sc.img",if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0 "$@" &
    QPID=$!
    python3 "$SCRIPT_DIR/pages_check.py" --qmp "$OUT/$sc.sock" --serial "$L" --tmp "$OUT" --scenario "$sc" --timeout 480 > "$OUT/$sc.check.log" 2>&1
    rc=$?; cat "$OUT/$sc.check.log"
    if [ "$rc" -eq 0 ]; then ok "$sc: all checks passed"; else bad "$sc: checks failed (see $OUT/$sc.check.log)"; fi
    stop_qemu
}
KB="-device virtio-keyboard-device -device virtio-tablet-device"
# PAGES_ONLY="live" runs only the named scenarios (debugging); CI runs them all
want() { [ -z "$PAGES_ONLY" ] || case " $PAGES_ONLY " in *" $1 "*) return 0;; *) return 1;; esac; }
want live && boot live 7 $KB
want empty && boot empty 0 $KB
echo "pages-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
