#!/bin/sh
# run-qemu.sh — Launch AuroraOS in QEMU ARM64 virt
#
# Usage: ./run-qemu.sh [kernel-image]
#
# Defaults to build/kernel.bin if no argument given.

set -e

KERNEL="${1:-build/kernel.bin}"

if [ ! -f "$KERNEL" ]; then
    echo "error: kernel image not found: $KERNEL" >&2
    echo "Run ./build.sh first." >&2
    exit 1
fi

echo "[qemu] Booting AuroraOS on ARM64 virt..."
echo "[qemu] Kernel: $KERNEL"
echo "[qemu] Press Ctrl-A then X to quit QEMU."
echo ""

qemu-system-aarch64 \
    -machine virt,highmem=off \
    -cpu cortex-a53 \
    -smp 1 \
    -m 256M \
    -kernel "$KERNEL" \
    -nographic \
    -serial stdio \
    -monitor none
