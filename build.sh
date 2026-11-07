#!/bin/sh
# build.sh — Build the AuroraOS ARM64 kernel
#
# Toolchain: aarch64-linux-gnu-gcc (cross-compiler for AArch64 Linux)
# Target: QEMU ARM64 "virt" machine
#
# Usage: ./build.sh

set -e

CC=aarch64-linux-gnu-gcc
AS=aarch64-linux-gnu-as
LD=aarch64-linux-gnu-ld
OBJCOPY=aarch64-linux-gnu-objcopy

KERNEL_ELF="build/kernel.elf"
KERNEL_BIN="build/kernel.bin"
MAP_FILE="build/kernel.map"

mkdir -p build

echo "[build] Assembling boot entry..."
$AS -march=armv8-a -I boot/aarch64 -o build/start.o boot/aarch64/start.S

echo "[build] Compiling kernel..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -c kernel/kernel.c -o build/kernel.o

echo "[build] Linking kernel..."
$LD -T kernel/linker.ld \
    -o "$KERNEL_ELF" \
    build/start.o build/kernel.o \
    -Map "$MAP_FILE"

echo "[build] Creating raw binary..."
$OBJCOPY -O binary "$KERNEL_ELF" "$KERNEL_BIN"

echo "[build] Done: $KERNEL_ELF ($KERNEL_BIN)"
ls -la "$KERNEL_ELF" "$KERNEL_BIN"
