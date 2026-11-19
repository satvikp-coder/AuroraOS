#!/bin/sh
# scripts/package-qemu.sh - build the LOCAL QEMU development package (kernel + AuroraFS image with the native applications installed).
# Local only: nothing is published. Output directory (default build/pkg, override with PKG_DIR=...):
#   kernel.bin        the production kernel (no test flags)
#   aurora-fresh.img  a pristine AuroraFS image: installer-owned native apps, nothing else (the TEMPLATE; never booted directly)
#   apps/*.elf        the validated application executables
#   manifest.txt      SHA-256 of every artifact, installed file sizes/CRCs, tool versions
#   fresh-install.sh  creates data/aurora.img from the template; refuses to overwrite an existing disk unless --force
#   run.sh            boots the kernel on data/aurora.img (NEVER creates or resets it, so saved data survives restarts); extra args go to QEMU
# Requires: aarch64-linux-gnu-gcc/binutils, a host cc, sha256sum, qemu-system-aarch64 (only to RUN the package).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
PKG="${PKG_DIR:-$ROOT/build/pkg}"; SECTORS="${DISK_SECTORS:-2048}"
CROSS="${CROSS_COMPILE:-aarch64-linux-gnu-}"
rm -rf "$PKG"; mkdir -p "$PKG/apps" "$PKG/obj"

echo "package: building the production kernel"
BUILD_DIR="$PKG/obj/k" ./build.sh >/dev/null
cp "$PKG/obj/k/kernel.bin" "$PKG/kernel.bin"

PF="-O1 -ffreestanding -fno-builtin -nostdlib -static -no-pie -fno-pic -fno-pie -mgeneral-regs-only -fno-stack-protector -Wall -Wextra -Werror -Wl,-T,tests/programs/user.ld -Wl,-z,noexecstack -Wl,-z,max-page-size=4096 -Wl,--build-id=none"
ARGS=""; NAMES=""
while read -r name src; do
    case "$name" in ''|'#'*) continue;; esac
    echo "package: building $name from $src"
    "${CROSS}gcc" $PF -o "$PKG/apps/$name" "$src"
    ARGS="$ARGS $name=$PKG/apps/$name"; NAMES="$NAMES $name"
done < tools/pkg/apps.list

# the installed names and the kernel registry's executable names must be the same set, both ways
# (an app the kernel cannot launch, or a launcher entry with no executable, is a packaging bug)
REG=$(grep -o '"[A-Za-z0-9_.-]*\.elf"' kernel/proc/apps.c | tr -d '"' | sort -u | tr '\n' ' ')
INS=$(for n in $NAMES; do echo "$n"; done | sort -u | tr '\n' ' ')
[ "$REG" = "$INS" ] || { echo "package: FAIL: registry executables [$REG] differ from installed [$INS]" >&2; exit 1; }

cc -std=c11 -Wall -Wextra -O1 -I tests/tools/shim -I kernel/include -o "$PKG/obj/mkdisk" tools/pkg/mkdisk.c kernel/fs/afs.c kernel/proc/elf.c
# shellcheck disable=SC2086
"$PKG/obj/mkdisk" "$PKG/aurora-fresh.img" "$SECTORS" $ARGS > "$PKG/obj/installed.txt"

{
    echo "AuroraOS local QEMU development package (not a release)"
    echo "source commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "disk sectors: $SECTORS (512 bytes each)"
    echo "tools: $("${CROSS}gcc" --version | head -1); $(cc --version | head -1); $(qemu-system-aarch64 --version 2>/dev/null | head -1 || true)"
    echo; echo "sha256:"
    (cd "$PKG" && sha256sum kernel.bin aurora-fresh.img $(for n in $NAMES; do echo "apps/$n"; done))
    echo; echo "installed (installer-owned, read-only for everyone; name size crc32):"; cat "$PKG/obj/installed.txt"
} > "$PKG/manifest.txt"

cat > "$PKG/fresh-install.sh" <<'EOS'
#!/bin/sh
# Creates data/aurora.img from the pristine template. Refuses to overwrite an existing disk (it would erase saved data) unless --force is given.
D="$(cd "$(dirname "$0")" && pwd)"; mkdir -p "$D/data"
if [ -e "$D/data/aurora.img" ] && [ "$1" != "--force" ]; then echo "fresh-install: $D/data/aurora.img already exists; run with --force to ERASE it and start over" >&2; exit 1; fi
cp "$D/aurora-fresh.img" "$D/data/aurora.img" && echo "fresh-install: created $D/data/aurora.img"
EOS
cat > "$PKG/run.sh" <<'EOS'
#!/bin/sh
# Boots AuroraOS on data/aurora.img. It never creates, resets or reformats the disk: run fresh-install.sh once first.
# Extra arguments are passed to QEMU (for example -display none); AURORA_SERIAL=file:log.txt redirects the serial console (default: stdio). Close the window or press Ctrl+C to stop.
D="$(cd "$(dirname "$0")" && pwd)"
[ -f "$D/data/aurora.img" ] || { echo "run: no disk at $D/data/aurora.img; run ./fresh-install.sh first" >&2; exit 1; }
exec qemu-system-aarch64 -machine virt,highmem=off -cpu cortex-a53 -smp 1 -m 256M -kernel "$D/kernel.bin" \
    -global virtio-mmio.force-legacy=false -device ramfb -device virtio-keyboard-device -device virtio-tablet-device \
    -drive file="$D/data/aurora.img",if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0 -serial "${AURORA_SERIAL:-stdio}" "$@"
EOS
chmod +x "$PKG/fresh-install.sh" "$PKG/run.sh"
rm -rf "$PKG/obj"
echo "package: OK -> $PKG"; cat "$PKG/manifest.txt"
