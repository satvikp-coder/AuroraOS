#!/bin/sh
# tests/qemu/review-tests.sh - the trusted review MODAL on the REAL kernel (Cycle 21): a test kernel (AURORA_REVIEW_TEST=1) opens fixed S1 reviews (requester = the shell,
# named "Aura (test)") up to four times, one second after the previous one ended; S1 EXECUTION IS DISABLED, so nothing a review does can change any setting. The check
# drives Enter-on-default-focus, Escape, an early and a valid Ctrl+Alt+Delete, Tab and Enter on Approve, and the 60 second expiry with real QMP key and pointer events,
# while a demo and a HOSTILE EL0 UI client (UI mask 64+256) are running. Evidence: screendump pixels (the secure strip, the scrim, nothing underneath changing) and the
# guest's own "shell: review ..." / "shell: focus=" lines. The host/QMP can inject the gesture: no claim of protection against a malicious host. TCG only.
# Usage: ./tests/qemu/review-tests.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT" || exit 1
OUT="$ROOT/build/review-tests"; rm -rf "$OUT"; mkdir -p "$OUT"
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
$CC $PF -o "$OUT/uiclient.elf" tests/programs/uiclient.c && ok "the EL0 UI client compiles" || { bad "uiclient failed to compile"; echo "review-tests: $PASSES passed, $FAILS failed"; exit 1; }
cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$OUT/mkafs" tests/tools/mkafs.c kernel/fs/afs.c && ok "mkafs compiled" || { bad "mkafs failed to compile"; exit 1; }
"$OUT/mkafs" "$OUT/disk.img" 2048 installer 0x7 uiclient.elf "$OUT/uiclient.elf" > "$OUT/mkafs.log" 2>&1 && ok "disk image provisioned with the installer-owned client" || bad "mkafs failed"
AURORA_REVIEW_TEST=1 AURORA_SHELL_CLIENTS=$((64 + 256)) BUILD_DIR="$OUT/k" ./build.sh >/dev/null 2>&1 && ok "test kernel builds (review script + demo and hostile UI clients)" || { bad "test kernel failed to build"; exit 1; }
sc=review
L="$OUT/$sc.log"; : > "$L"
qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$OUT/k/kernel.bin" \
    -display none -serial file:"$L" -monitor none -qmp unix:"$OUT/$sc.sock",server=on,wait=off \
    -device ramfb $LEG -drive file="$OUT/disk.img",if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0 \
    -device virtio-keyboard-device -device virtio-tablet-device &
QPID=$!
python3 "$SCRIPT_DIR/review_check.py" --qmp "$OUT/$sc.sock" --serial "$L" --tmp "$OUT" --scenario "$sc" --timeout 460 > "$OUT/$sc.check.log" 2>&1
rc=$?; cat "$OUT/$sc.check.log"
if [ "$rc" -eq 0 ]; then ok "$sc: all checks passed"; else bad "$sc: checks failed (see $OUT/$sc.check.log)"; fi
stop_qemu
echo "review-tests: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
