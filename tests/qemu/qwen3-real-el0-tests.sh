#!/bin/sh
# Opt-in native test. Supply an existing converted Qwen3-0.6B model.
set -eu
[ "$#" -eq 1 ] || { echo "usage: $0 local-model.aurq3" >&2; exit 2; }
model=$(realpath "$1")
[ -f "$model" ] || exit 2
root=$(CDPATH='' cd "$(dirname "$0")/../.." && pwd)
cd "$root"
work=$(mktemp -d /tmp/aurora-qwen-real.XXXXXX)
echo "Native model test artifacts: $work"

aarch64-linux-gnu-gcc -O2 -DQ3F_SINGLE_POSITION -ffreestanding -fno-builtin -fno-math-errno \
    -nostdlib -static -no-pie -Wall -Wextra -Werror -Wl,-T,tests/programs/user.ld \
    -Wl,-z,max-page-size=4096 -Wl,--build-id=none -I kernel/include \
    -o "$work/inferclient.elf" tests/programs/qwen3-real-el0.c
cc -O2 -I tests/tools/shim -I kernel/include -o "$work/mkafs" tests/tools/mkafs.c kernel/fs/afs.c
cc -O2 -I kernel/include -o "$work/mkmodel" tests/tools/mkmodel.c kernel/fs/modelstore.c kernel/crypto/sha256.c
cc -O2 -I kernel/include -o "$work/host" tests/kernel-host/qwen3_full_test.c -lm

"$work/host" "$model" 151643 >"$work/host.log"
expected=$(sed -n 's/^qwen3_full: prefill 1 tokens .* first generated token=\([0-9][0-9]*\)$/\1/p' "$work/host.log")
[ -n "$expected" ] || { cat "$work/host.log"; exit 1; }

"$work/mkafs" "$work/system.img" 2048 installer 0x7 inferclient.elf "$work/inferclient.elf"
"$work/mkmodel" "$work/model.img" "qwen3:1=$model"
AURORA_INFER_TEST=1 BUILD_DIR="$work/kernel" sh ./build.sh >"$work/build.log" 2>&1

log="$work/serial.log"
: >"$log"
start=$(date +%s)
qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M \
    -kernel "$work/kernel/kernel.bin" -display none -nic none -monitor none \
    -serial "file:$log" -global virtio-mmio.force-legacy=false \
    -drive "file=$work/system.img,if=none,format=raw,id=sys" -device virtio-blk-device,drive=sys \
    -drive "file=$work/model.img,if=none,format=raw,readonly=on,id=model" -device virtio-blk-device,drive=model \
    >"$work/qemu.log" 2>&1 &
pid=$!
cleanup() { kill "$pid" 2>/dev/null || :; wait "$pid" 2>/dev/null || :; }
trap cleanup EXIT
trap 'exit 130' INT TERM

deadline=$((start + 360))
while kill -0 "$pid" 2>/dev/null && [ "$(date +%s)" -lt "$deadline" ]; do
    if grep -Eq 'Boot complete|KERNEL PANIC|SELFTEST FAILED' "$log"; then break; fi
    sleep 1
done
tail -25 "$log"
grep -q 'Boot complete' "$log"
grep -q 'qwen-real: verified 28-layer model index' "$log"
grep -Eq "qwen-real: next token $expected[[:space:]]*$" "$log"
python3 - "$work/host.log" "$log" <<'PY_LOGIT'
import math
from pathlib import Path
import re
import struct
import sys

host = float(re.search(r"first logit=([^\s]+)", Path(sys.argv[1]).read_text()).group(1))
bits = int(re.search(r"qwen-real: logit bits (\d+)", Path(sys.argv[2]).read_text()).group(1))
guest, = struct.unpack("<f", struct.pack("<I", bits))
print(f"Selected-token logit: host {host:.8g}, EL0 {guest:.8g}, absolute difference {abs(host - guest):.3g}")
if not math.isclose(host, guest, rel_tol=1e-4, abs_tol=1e-4):
    raise SystemExit("EL0 logit differs from the host runtime")
PY_LOGIT
grep -Eq 'object 0 as PRINCIPAL_INFER: terminated with exit 0: PASS' "$log"
grep -Eq 'no leaks: every page is back and no process remains: PASS' "$log"
section=$(sed -n '/\[infer-test\] setup/,$p' "$log")
if printf '%s\n' "$section" | grep -Eq 'FAULT at EL0|KERNEL PANIC|SELFTEST FAILED'; then exit 1; fi
printf 'PASS: real model EL0 next token %s matched the full host runtime; QEMU boot and inference %ss\n' \
    "$expected" "$(($(date +%s) - start))"
