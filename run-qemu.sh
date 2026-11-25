#!/bin/sh
# run-qemu.sh — Launch AuroraOS in QEMU ARM64 virt
#
# Usage: ./run-qemu.sh [kernel-image] [memory-size]
#
# Defaults to build/kernel.bin and 256M if no arguments given.
# Memory size examples: 256M, 512M, 1G

set -e

KERNEL="${1:-build/kernel.bin}"
MEMORY="${2:-256M}"

if [ ! -f "$KERNEL" ]; then
    echo "error: kernel image not found: $KERNEL" >&2
    echo "Run ./build.sh first." >&2
    exit 1
fi

echo "[qemu] Booting AuroraOS on ARM64 virt..."
echo "[qemu] Kernel: $KERNEL"
echo "[qemu] Memory: $MEMORY"
echo "[qemu] Press Ctrl-A then X to quit QEMU."
echo ""

qemu-system-aarch64 \
    -machine virt,highmem=off \
    -cpu cortex-a53 \
    -smp 1 \
    -m "$MEMORY" \
    -kernel "$KERNEL" \
    -nographic \
    -serial stdio \
    -monitor none
