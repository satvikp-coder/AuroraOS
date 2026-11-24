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

echo "[build] Assembling exception vectors..."
$AS -march=armv8-a -I kernel/arch/aarch64 -o build/vectors.o kernel/arch/aarch64/vectors.S

echo "[build] Compiling kernel..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/kernel.c -o build/kernel.o

echo "[build] Compiling IRQ dispatch..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/irq.c -o build/irq.o

echo "[build] Compiling physical page allocator (pmm)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/mm/pmm.c -o build/pmm.o

echo "[build] Compiling virtual memory manager (vmm)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/mm/vmm.c -o build/vmm.o

echo "[build] Compiling memory mapping policy (mmpolicy)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/mm/mmpolicy.c -o build/mmpolicy.o

echo "[build] Compiling device tree parser (fdt)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/mm/fdt.c -o build/fdt.o

echo "[build] Compiling virtio block driver..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/drivers/virtio_blk.c -o build/virtio_blk.o

echo "[build] Compiling virtio input driver..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/drivers/virtio_input.c -o build/virtio_input.o

echo "[build] Compiling fwcfg driver..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/drivers/fwcfg.c -o build/fwcfg.o

echo "[build] Compiling fwcfg protocol helpers..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/drivers/fwcfg_proto.c -o build/fwcfg_proto.o

echo "[build] Compiling virtio-mmio transport..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/drivers/virtio_mmio.c -o build/virtio_mmio.o

echo "[build] Compiling virtqueue ring bookkeeping..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/drivers/virtqueue.c -o build/virtqueue.o

echo "[build] Compiling AuroraFS (afs)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/fs/afs.c -o build/afs.o

echo "[build] Compiling model store..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/fs/modelstore.c -o build/modelstore.o

echo "[build] Compiling model volume..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/fs/modelvol.c -o build/modelvol.o

echo "[build] Compiling settings..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/settings.c -o build/settings.o

echo "[build] Compiling input core logic..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/input/input_core.c -o build/input_core.o

echo "[build] Compiling graphics core (gfx)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/gfx/gfx.c -o build/gfx.o

echo "[build] Compiling graphics drawing (gfx_draw)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/gfx/gfx_draw.c -o build/gfx_draw.o

echo "[build] Compiling graphics region (gfx_region)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/gfx/gfx_region.c -o build/gfx_region.o

echo "[build] Compiling graphics shapes (gfx_shape)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/gfx/gfx_shape.c -o build/gfx_shape.o

echo "[build] Compiling graphics text (gfx_text)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/gfx/gfx_text.c -o build/gfx_text.o

echo "[build] Compiling Noto Mono font (font_noto_mono)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/gfx/font_noto_mono.c -o build/font_noto_mono.o

echo "[build] Compiling UI protocol (ui_proto)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/ui_proto.c -o build/ui_proto.o

echo "[build] Compiling UI system (ui_sys)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/ui_sys.c -o build/ui_sys.o

echo "[build] Compiling UI scene (ui_scene)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/ui_scene.c -o build/ui_scene.o

echo "[build] Compiling UI focus (ui_focus)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/ui_focus.c -o build/ui_focus.o

echo "[build] Compiling UI demo (ui_demo)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/ui_demo.c -o build/ui_demo.o

echo "[build] Compiling broker kernel (broker_kernel)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/broker_kernel.c -o build/broker_kernel.o

echo "[build] Compiling broker policy (broker_policy)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/broker_policy.c -o build/broker_policy.o

echo "[build] Compiling ledger store (ledger_store)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/ledger_store.c -o build/ledger_store.o

echo "[build] Compiling ledger kernel (ledger_kernel)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/ledger_kernel.c -o build/ledger_kernel.o

echo "[build] Compiling index model (index_model)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/index_model.c -o build/index_model.o

echo "[build] Compiling shell (shell)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/shell.c -o build/shell.o

echo "[build] Compiling shell run (shell_run)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/shell_run.c -o build/shell_run.o

echo "[build] Compiling aura command (aura_cmd)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/aura_cmd.c -o build/aura_cmd.o

echo "[build] Compiling aura exec (aura_exec)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/aura_exec.c -o build/aura_exec.o

echo "[build] Compiling passport..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/passport.c -o build/passport.o

echo "[build] Compiling trail..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/trail.c -o build/trail.o

echo "[build] Compiling hold..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/hold.c -o build/hold.o

echo "[build] Compiling review..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/review.c -o build/review.o

echo "[build] Compiling gfx bench (gfx_bench)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/gfx_bench.c -o build/gfx_bench.o

echo "[build] Compiling timersvc..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/ui/timersvc.c -o build/timersvc.o

echo "[build] Compiling exception handler..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/arch/aarch64/exceptions.c -o build/exceptions.o

echo "[build] Assembling FP/SIMD save/restore (fpsimd)..."
$AS -march=armv8-a -I kernel/arch/aarch64 -o build/fpsimd.o kernel/arch/aarch64/fpsimd.S

echo "[build] Assembling exception self-test (exc_selftest)..."
$AS -march=armv8-a -I kernel/arch/aarch64 -o build/exc_selftest.o kernel/arch/aarch64/exc_selftest.S

echo "[build] Compiling process address spaces (aspace)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/proc/aspace.c -o build/aspace.o

echo "[build] Compiling process table (proc)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/proc/proc.c -o build/proc.o

echo "[build] Compiling scheduler (sched)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/proc/sched.c -o build/sched.o

echo "[build] Compiling memory syscall (memsys)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/proc/memsys.c -o build/memsys.o

echo "[build] Compiling IPC (ipc)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/proc/ipc.c -o build/ipc.o

echo "[build] Compiling filesystem syscall (fssys)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/proc/fssys.c -o build/fssys.o

echo "[build] Compiling ELF validator (elf)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/proc/elf.c -o build/elf.o

echo "[build] Compiling launch registry (launch)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/proc/launch.c -o build/launch.o

echo "[build] Compiling application registry (apps)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/proc/apps.c -o build/apps.o

echo "[build] Compiling process snapshot (procsnap)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/proc/procsnap.c -o build/procsnap.o

echo "[build] Compiling process snapshot kernel binding (procsnap_k)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/proc/procsnap_k.c -o build/procsnap_k.o

echo "[build] Compiling timer syscall (timersys)..."
$CC -march=armv8-a -ffreestanding -nostdlib -fno-builtin \
    -Wall -Wextra -O2 -I kernel -I kernel/include -c kernel/proc/timersys.c -o build/timersys.o

echo "[build] Assembling user programs (user_progs)..."
$AS -march=armv8-a -I kernel/proc -o build/user_progs.o kernel/proc/user_progs.S

echo "[build] Assembling user FP programs (user_fp_progs)..."
$AS -march=armv8-a -I kernel/proc -o build/user_fp_progs.o kernel/proc/user_fp_progs.S

echo "[build] Assembling user memory programs (user_mem_progs)..."
$AS -march=armv8-a -I kernel/proc -o build/user_mem_progs.o kernel/proc/user_mem_progs.S

echo "[build] Assembling user test programs (user_test_progs)..."
$AS -march=armv8-a -I kernel/proc -o build/user_test_progs.o kernel/proc/user_test_progs.S

echo "[build] Linking kernel..."
$LD -T kernel/linker.ld \
    -o "$KERNEL_ELF" \
    build/start.o build/kernel.o \
    build/vectors.o build/irq.o build/fpsimd.o build/exc_selftest.o \
    build/pmm.o build/vmm.o build/mmpolicy.o build/fdt.o build/exceptions.o \
    build/virtio_blk.o build/virtio_input.o build/fwcfg.o build/fwcfg_proto.o build/virtio_mmio.o build/virtqueue.o \
    build/afs.o build/modelstore.o build/modelvol.o build/settings.o \
    build/input_core.o \
    build/gfx.o build/gfx_draw.o build/gfx_region.o build/gfx_shape.o build/gfx_text.o build/font_noto_mono.o \
    build/ui_proto.o build/ui_sys.o build/ui_scene.o build/ui_focus.o build/ui_demo.o \
    build/broker_kernel.o build/broker_policy.o \
    build/ledger_store.o build/ledger_kernel.o \
    build/index_model.o \
    build/shell.o build/shell_run.o \
    build/aura_cmd.o build/aura_exec.o \
    build/passport.o build/trail.o build/hold.o build/review.o \
    build/gfx_bench.o build/timersvc.o \
    build/aspace.o build/proc.o build/sched.o build/memsys.o build/ipc.o build/fssys.o \
    build/elf.o build/launch.o build/apps.o build/procsnap.o build/procsnap_k.o build/timersys.o \
    build/user_progs.o build/user_fp_progs.o build/user_mem_progs.o build/user_test_progs.o \
    -Map "$MAP_FILE"

echo "[build] Creating raw binary..."
$OBJCOPY -O binary "$KERNEL_ELF" "$KERNEL_BIN"

echo "[build] Done: $KERNEL_ELF ($KERNEL_BIN)"
ls -la "$KERNEL_ELF" "$KERNEL_BIN"