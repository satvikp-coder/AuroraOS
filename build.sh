#!/bin/sh
# build.sh — Build the AuroraOS ARM64 kernel
#
# Toolchain: aarch64-linux-gnu-gcc (freestanding; no libc is linked).
# Target: QEMU ARM64 "virt" machine.
#
# Usage: ./build.sh
#   BUILD_DIR=build/x ./build.sh                    build into another directory
#   AURORA_FIXED_RAM=1 ./build.sh                   DEV ONLY: allow a fixed-RAM fallback (see kernel/kernel.c)
#   AURORA_BLK_TEST=1 BUILD_DIR=... ./build.sh      build the DESTRUCTIVE block-device tests (disposable QEMU disk only)
#   AURORA_FS_TEST=1 BUILD_DIR=... ./build.sh       build the DESTRUCTIVE filesystem tests (disposable QEMU disk only)
#   AURORA_GFX_SHAPES=1 BUILD_DIR=... ./build.sh     build a kernel that draws the G3 rounded-rectangle test image (tests/qemu/gfx-tests.sh)
#   AURORA_FAULT_TEST=1 BUILD_DIR=... ./build.sh    build a deliberate-fault test kernel
#                                                   (see tests/qemu/mm-tests.sh)

set -e

CC=aarch64-linux-gnu-gcc
AS=aarch64-linux-gnu-as
LD=aarch64-linux-gnu-ld
OBJCOPY=aarch64-linux-gnu-objcopy

BUILD_DIR="${BUILD_DIR:-build}"
KERNEL_ELF="$BUILD_DIR/kernel.elf"
KERNEL_BIN="$BUILD_DIR/kernel.bin"
MAP_FILE="$BUILD_DIR/kernel.map"

# -mgeneral-regs-only: no FP/SIMD in kernel code. -mstrict-align: before the MMU
# is on all data accesses are Device memory, where unaligned access faults.
CFLAGS="-march=armv8-a -mgeneral-regs-only -mstrict-align -ffreestanding -nostdlib -fno-builtin \
-fno-stack-protector -fno-pic -fno-strict-aliasing -fno-tree-loop-distribute-patterns \
-Wall -Wextra -Werror -O2 -I kernel/include"
# AURORA_FIXED_RAM=1: DEV-ONLY fallback that assumes 256 MiB at 0x40000000 when no device tree is found.
# Normal builds refuse to boot without a validated device tree.
# AURORA_TEST_ENTRY=spsel0|el1t builds CONTROLLED boot-entry variants (tests only): spsel0 enters at EL1 with SPSel=0
# and junk stack pointers; el1t (use a machine with virtualization=on) ERETs from EL2 to EL1t. Both prove that
# start.S selects each stack pointer explicitly instead of assuming the entry state.
ASFLAGS=""
case "$AURORA_TEST_ENTRY" in
    "") ;;
    spsel0) ASFLAGS="--defsym AURORA_TEST_ENTRY_SPSEL0=1"; CFLAGS="$CFLAGS -DAURORA_TEST_ENTRY=1" ;;
    el1t)   ASFLAGS="--defsym AURORA_TEST_ENTRY_EL1T=1";   CFLAGS="$CFLAGS -DAURORA_TEST_ENTRY=2" ;;
    *) echo "unknown AURORA_TEST_ENTRY=$AURORA_TEST_ENTRY" >&2; exit 2 ;;
esac
# Test-only SVC hooks are compiled in unless AURORA_PRODUCTION=1.
if [ -z "$AURORA_PRODUCTION" ]; then
    CFLAGS="$CFLAGS -DAURORA_SELFTEST_HOOKS=1"
fi
if [ -n "$AURORA_INPUT_TEST" ]; then
    CFLAGS="$CFLAGS -DAURORA_INPUT_TEST=$AURORA_INPUT_TEST"   # test kernel: runs the input demo for N seconds (tests/qemu/input-tests.sh)
fi
if [ -n "$AURORA_UI_DEMO" ]; then
    CFLAGS="$CFLAGS -DAURORA_UI_DEMO=$AURORA_UI_DEMO"   # test kernel: native Tier 0 Clear demo for N seconds (tests/qemu/ui-tests.sh)
fi
if [ -n "$AURORA_FIXED_RAM" ]; then
    CFLAGS="$CFLAGS -DAURORA_FIXED_RAM=1"
fi
if [ -n "$AURORA_BLK_TEST" ]; then
    CFLAGS="$CFLAGS -DAURORA_BLK_TEST=1"      # DESTRUCTIVE block tests: only for a disposable QEMU disk image
fi
if [ -n "$AURORA_FS_TEST" ]; then
    CFLAGS="$CFLAGS -DAURORA_FS_TEST=1"        # DESTRUCTIVE filesystem tests: only for a disposable QEMU disk image
fi
if [ -n "$AURORA_LOAD_TEST" ]; then
    CFLAGS="$CFLAGS -DAURORA_LOAD_TEST=1"      # loader tests: need a disk provisioned by tests/tools/mkafs; they write only through the programs they launch
fi
if [ -n "$AURORA_SET_TEST" ]; then
    CFLAGS="$CFLAGS -DAURORA_SET_TEST=1"        # settings registry tests: need a disk provisioned by tests/tools/mkafs
fi
if [ -n "$AURORA_GFX_SHAPES" ]; then
    CFLAGS="$CFLAGS -DAURORA_GFX_SHAPES=1"      # draw the G3 shapes diagnostic image instead of the G2 pattern (needs a ramfb display)
fi
if [ -n "$AURORA_GFX_TEXT" ]; then
    CFLAGS="$CFLAGS -DAURORA_GFX_TEXT=1"        # draw the G4 text diagnostic image (needs a ramfb display)
fi
if [ -n "$AURORA_FAULT_TEST" ]; then
    CFLAGS="$CFLAGS -DAURORA_FAULT_TEST=$AURORA_FAULT_TEST"
fi

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
OBJS=""

echo "[build] Assembling..."
for s in boot/aarch64/start.S kernel/arch/aarch64/vectors.S kernel/arch/aarch64/exc_selftest.S kernel/proc/user_progs.S; do
    o="$BUILD_DIR/$(basename "$s" .S).o"
    $AS -march=armv8-a $ASFLAGS -o "$o" "$s"
    OBJS="$OBJS $o"
done

if [ -n "$AURORA_FS_TEST" ]; then      # EL0 file-access boundary programs: test kernels only, never in a normal or production image
    o="$BUILD_DIR/user_test_progs.o"
    $AS -march=armv8-a $ASFLAGS -o "$o" kernel/proc/user_test_progs.S
    OBJS="$OBJS $o"
fi

echo "[build] Compiling kernel..."
for c in kernel/kernel.c kernel/console.c kernel/mm/fdt.c kernel/mm/pmm.c kernel/mm/vmm.c kernel/mm/mmpolicy.c kernel/lib.c kernel/proc/aspace.c kernel/proc/proc.c kernel/proc/sched.c kernel/proc/ipc.c kernel/proc/fssys.c kernel/proc/elf.c kernel/proc/launch.c kernel/settings.c kernel/settingstest.c kernel/loadtest.c kernel/proctest.c kernel/drivers/virtio_blk.c kernel/drivers/fwcfg.c kernel/drivers/fwcfg_proto.c kernel/drivers/virtio_mmio.c kernel/drivers/virtio_input.c kernel/input/input_core.c kernel/input/ui_focus.c kernel/ui/ui_scene.c kernel/ui/ui_demo.c kernel/input/input_demo.c kernel/gfx/gfx.c kernel/gfx/gfx_draw.c kernel/gfx/gfx_region.c kernel/gfx/gfx_shape.c kernel/gfx/gfx_text.c kernel/gfx/font_noto_mono.c kernel/blktest.c kernel/fs/afs.c kernel/fstest.c kernel/fsproctest.c kernel/irq.c kernel/irqtest.c kernel/arch/aarch64/gicv2.c kernel/arch/aarch64/timer.c \
         kernel/mm/selftest.c kernel/arch/aarch64/exceptions.c; do
    o="$BUILD_DIR/$(basename "$c" .c).o"
    $CC $CFLAGS -c "$c" -o "$o"
    OBJS="$OBJS $o"
done

echo "[build] Linking kernel..."
$LD -T kernel/linker.ld -o "$KERNEL_ELF" $OBJS -Map "$MAP_FILE"

echo "[build] Creating raw binary..."
$OBJCOPY -O binary "$KERNEL_ELF" "$KERNEL_BIN"

echo "[build] Done: $KERNEL_ELF ($KERNEL_BIN)"
ls -la "$KERNEL_ELF" "$KERNEL_BIN"
