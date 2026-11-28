#!/bin/sh
# Dedicated QEMU virt ELF; the production build never compiles these sources.
# --build-only generates a fresh fixture, runs the host test and cross-links the probe.
set -eu
case "${1-}" in ""|--build-only) ;; *) echo "usage: $0 [--build-only]" >&2; exit 2;; esac
cd "$(dirname "$0")/../.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
probe=tests/qemu/signed-volume
vendor=third_party/monocypher/src
python3 -B "$probe/fixture.py" "$tmp"
# The complete source list is deliberately local to this fixture.
mode=${1-}
set -- "$probe/probe.c" kernel/fs/model_signed_volume.c kernel/fs/modelstore.c \
    kernel/crypto/sha256.c "$vendor/monocypher.c" "$vendor/optional/monocypher-ed25519.c"
cc -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -I"$tmp" -Ikernel/include -I"$vendor" -I"$vendor/optional" \
    "$@" -o "$tmp/probe-host"
"$tmp/probe-host"
aarch64-linux-gnu-gcc -std=c11 -Os -Wall -Wextra -Werror -ffreestanding -fno-builtin \
    -fno-stack-protector -fno-tree-loop-distribute-patterns -mstrict-align -mgeneral-regs-only \
    -ffunction-sections -fdata-sections -nostdlib -static -Wl,--gc-sections,--build-id=none \
    -I"$tmp" -Ikernel/include -I"$vendor" -I"$vendor/optional" -T "$probe/probe.ld" \
    "$probe/start.S" "$@" kernel/lib.c -o "$tmp/probe.elf"
if [ "$mode" = --build-only ]; then
    echo 'signed volume: host checks and AArch64 link passed'
    exit 0
fi
timeout 30s qemu-system-aarch64 -machine virt -cpu cortex-a53 -m 64M -nographic \
    -monitor none -serial none -semihosting-config enable=on,target=native -kernel "$tmp/probe.elf" >"$tmp/guest.log" 2>&1 || { cat "$tmp/guest.log" >&2; exit 1; }
grep -Fxq "AURORA-SIGNED-PROBE-PASS" "$tmp/guest.log" || { cat "$tmp/guest.log" >&2; exit 1; }
echo 'signed volume: AArch64 publisher, geometry and payload checks passed'
