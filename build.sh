#!/bin/sh
# build.sh â€” Build the AuroraOS ARM64 kernel
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
#   AURORA_SND_FAULT=n BUILD_DIR=... ./build.sh     sound driver simulates hostile device answer n (tests/qemu/sound-tests.sh)
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
-Wall -Wextra -Werror -O2 -I kernel/include -I third_party/monocypher/src -I third_party/monocypher/src/optional"
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
if [ -n "$AURORA_SHELL_TEXT_PCT" ]; then
    CFLAGS="$CFLAGS -DAURORA_SHELL_TEXT_PCT=$AURORA_SHELL_TEXT_PCT -DAURORA_SHELL_LAYOUT_LOG=1"   # test kernel: the real shell started at this text scale; logs control geometry (tests/qemu/scale-tests.sh)
fi
if [ -n "$AURORA_REVIEW_TEST" ]; then
    CFLAGS="$CFLAGS -DAURORA_REVIEW_TEST=1"   # test kernel: opens fixed trusted reviews (S1 execution stays disabled) so tests/qemu/review-tests.sh can drive them
fi
if [ -n "$AURORA_LEDGER_TEST" ]; then
    CFLAGS="$CFLAGS -DAURORA_LEDGER_TEST=$AURORA_LEDGER_TEST"   # test kernel: runs a fixed durable-Passport script at boot (tests/qemu/ledger-tests.sh)
fi
if [ -n "$AURORA_SHELL_CLIENTS" ]; then
    CFLAGS="$CFLAGS -DAURORA_SHELL_CLIENTS=$AURORA_SHELL_CLIENTS"   # test kernel: EL0 clients from shclient.elf run while the shell is interactive (tests/qemu/shell-sched-tests.sh)
fi
if [ -n "$AURORA_SHELL_CONTRAST" ]; then
    CFLAGS="$CFLAGS -DAURORA_SHELL_CONTRAST=$AURORA_SHELL_CONTRAST -DAURORA_SHELL_LAYOUT_LOG=1"   # test kernel: the real shell started in high contrast
fi
if [ -n "$AURORA_BENCH" ]; then
    CFLAGS="$CFLAGS -DAURORA_BENCH=1"   # test kernel: prints the native renderer CPU-time baseline (tests/qemu/bench.sh)
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
if [ -n "$AURORA_MODEL_TEST" ]; then
    CFLAGS="$CFLAGS -DAURORA_MODEL_TEST=1"      # SYS_MODEL end-to-end tests: need a disk provisioned by tests/tools/mkafs with modelclient.elf (tests/qemu/model-tests.sh)
fi
if [ -n "$AURORA_INFER_TEST" ]; then
    CFLAGS="$CFLAGS -DAURORA_INFER_TEST=1"      # EL0 int8 matvec over a model-volume tensor: needs a disk provisioned by tests/tools/mkafs with inferclient.elf (tests/qemu/infer-tests.sh)
fi
if [ -n "$AURORA_SET_TEST" ]; then
    CFLAGS="$CFLAGS -DAURORA_SET_TEST=1"        # settings registry tests: need a disk provisioned by tests/tools/mkafs
fi
if [ -n "$AURORA_GFX_BASIC" ]; then
    CFLAGS="$CFLAGS -DAURORA_GFX_BASIC=1"       # draw the G2 pixel pattern instead of the native Home (tests/qemu/gfx-tests.sh)
fi
if [ -n "$AURORA_GFX_SHAPES" ]; then
    CFLAGS="$CFLAGS -DAURORA_GFX_SHAPES=1"      # draw the G3 shapes diagnostic image instead of the G2 pattern (needs a ramfb display)
fi
if [ -n "$AURORA_GFX_TEXT" ]; then
    CFLAGS="$CFLAGS -DAURORA_GFX_TEXT=1"        # draw the G4 text diagnostic image (needs a ramfb display)
fi
if [ "$AURORA_SND_PLAY_TEST" = 1 ]; then CFLAGS="$CFLAGS -DAURORA_SND_PLAY_TEST"; fi
if [ "$AURORA_SND_RX_TEST" = 1 ]; then CFLAGS="$CFLAGS -DAURORA_SND_RX_TEST"; fi
if [ -n "$AURORA_SND_FAULT" ]; then
    CFLAGS="$CFLAGS -DAURORA_SND_FAULT=$AURORA_SND_FAULT"   # test kernel: the sound driver SIMULATES a hostile device answer (tests/qemu/sound-tests.sh)
fi
if [ -n "$AURORA_FAULT_TEST" ]; then
    CFLAGS="$CFLAGS -DAURORA_FAULT_TEST=$AURORA_FAULT_TEST"
fi

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
OBJS=""

echo "[build] Assembling..."
for s in boot/aarch64/start.S kernel/arch/aarch64/vectors.S kernel/arch/aarch64/exc_selftest.S kernel/arch/aarch64/fpsimd.S kernel/proc/user_progs.S; do
    o="$BUILD_DIR/$(basename "$s" .S).o"
    $AS -march=armv8-a $ASFLAGS -o "$o" "$s"
    OBJS="$OBJS $o"
done

if [ -z "$AURORA_PRODUCTION" ]; then      # EL0 FP/SIMD context-switch programs (test kernels only)
    o="$BUILD_DIR/user_fp_progs.o"
    $AS -march=armv8-a $ASFLAGS -o "$o" kernel/proc/user_fp_progs.S
    OBJS="$OBJS $o"
    o="$BUILD_DIR/user_mem_progs.o"                # EL0 SYS_MEM (user heap) programs (test kernels only)
    $AS -march=armv8-a $ASFLAGS -o "$o" kernel/proc/user_mem_progs.S
    OBJS="$OBJS $o"
fi

if [ -n "$AURORA_FS_TEST" ]; then      # EL0 file-access boundary programs: test kernels only, never in a normal or production image
    o="$BUILD_DIR/user_test_progs.o"
    $AS -march=armv8-a $ASFLAGS -o "$o" kernel/proc/user_test_progs.S
    OBJS="$OBJS $o"
fi

echo "[build] Compiling kernel..."
for c in kernel/kernel.c kernel/console.c kernel/mm/fdt.c kernel/mm/pmm.c kernel/mm/vmm.c kernel/mm/mmpolicy.c kernel/mm/ro.c kernel/lib.c kernel/proc/aspace.c kernel/proc/proc.c kernel/proc/sched.c kernel/proc/memsys.c kernel/proc/modelsys.c kernel/proc/ipc.c kernel/proc/fssys.c kernel/proc/elf.c kernel/proc/launch.c kernel/proc/apps.c kernel/proc/procsnap.c kernel/proc/procsnap_k.c kernel/proc/rosys.c kernel/settings.c kernel/settingstest.c kernel/loadtest.c kernel/modeltest.c kernel/infertest.c kernel/proctest.c kernel/drivers/virtio_blk.c kernel/drivers/fwcfg.c kernel/drivers/fwcfg_proto.c kernel/drivers/virtio_mmio.c kernel/drivers/virtio_input.c kernel/drivers/virtqueue.c kernel/drivers/virtio_snd.c kernel/input/input_core.c kernel/input/ui_focus.c kernel/ui/ui_scene.c kernel/ui/ui_demo.c kernel/ui/gfx_bench.c kernel/ui/index_model.c kernel/ui/shell.c kernel/ui/hold.c kernel/ui/shell_run.c kernel/ui/ui_proto.c kernel/ui/ui_sys.c kernel/ui/aura_cmd.c kernel/ui/passport.c kernel/ui/trail.c kernel/ui/timersvc.c kernel/proc/timersys.c kernel/proc/audiosys.c kernel/ui/ledger_store.c kernel/ui/ledger_kernel.c kernel/ui/review.c kernel/ui/broker_policy.c kernel/ui/aura_exec.c kernel/ui/broker_kernel.c kernel/input/input_demo.c kernel/gfx/gfx.c kernel/gfx/gfx_draw.c kernel/gfx/gfx_region.c kernel/gfx/gfx_shape.c kernel/gfx/gfx_text.c kernel/gfx/font_noto_mono.c kernel/blktest.c kernel/fs/afs.c kernel/fs/modelstore.c kernel/fs/model_signed_volume.c kernel/fs/modelvol.c kernel/crypto/sha256.c third_party/monocypher/src/monocypher.c third_party/monocypher/src/optional/monocypher-ed25519.c kernel/fstest.c kernel/fsproctest.c kernel/irq.c kernel/irqtest.c kernel/arch/aarch64/gicv2.c kernel/arch/aarch64/timer.c \
         kernel/mm/selftest.c kernel/arch/aarch64/exceptions.c; do
    o="$BUILD_DIR/$(basename "$c" .c).o"
    $CC $CFLAGS -c "$c" -o "$o"
    OBJS="$OBJS $o"
done

echo "[build] Linking kernel..."
$LD -z max-page-size=4096 -T kernel/linker.ld -o "$KERNEL_ELF" $OBJS -Map "$MAP_FILE"

echo "[build] Creating raw binary..."
$OBJCOPY -O binary "$KERNEL_ELF" "$KERNEL_BIN"

echo "[build] Done: $KERNEL_ELF ($KERNEL_BIN)"
ls -la "$KERNEL_ELF" "$KERNEL_BIN"
