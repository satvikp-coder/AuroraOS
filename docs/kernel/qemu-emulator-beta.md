# AuroraOS QEMU Emulator Beta

A development-focused QEMU distribution for running AuroraOS without physical ARM64 hardware. This beta package is designed for developers, testers, and early adopters who want to explore AuroraOS in an emulator environment.

## Fresh Installation

### Prerequisites

**Host Requirements:**
- **Operating System**: Linux (native) or Windows via [WSL2](https://learn.microsoft.com/en-us/windows/wsl/install)
- **QEMU**: `qemu-system-aarch64` version 7.0 or later
- **Cross-compiler**: `aarch64-linux-gnu-gcc` and related binutils
- **Architecture**: ARM64 (AArch64) QEMU `virt` machine

**Required Tools:**
| Tool | Version | Purpose |
|------|---------|---------|
| `aarch64-linux-gnu-gcc` | 11+ | Kernel cross-compilation |
| `aarch64-linux-gnu-ld` | 2.35+ | Kernel linking |
| `qemu-system-aarch64` | 7.0+ | Emulation (TCG mode) |
| `sha256sum` | coreutils | Integrity verification |
| `python3` | 3.8+ | Test automation |

### Installing Prerequisites

#### Linux (Debian/Ubuntu)
```bash
sudo apt update
sudo apt install -y \
    gcc-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    qemu-system-arm \
    python3 \
    coreutils
```

#### Linux (Fedora/RHEL)
```bash
sudo dnf install -y \
    gcc-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    qemu-system-aarch64 \
    python3 \
    coreutils
```

#### Windows WSL2 (Ubuntu/Debian)
```bash
# Inside WSL2
sudo apt update
sudo apt install -y \
    gcc-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    qemu-system-arm \
    python3 \
    coreutils
```

### Download Artifacts

The QEMU beta package is built from source. There are two approaches:

#### Option 1: Build from Source (Recommended for Development)

```bash
# Clone the repository
git clone https://github.com/your-org/AuroraOS.git
cd AuroraOS

# Build the native development package
AURORA_PRODUCTION=1 ./scripts/package-qemu.sh
```

This creates a complete package in `build/pkg/` containing:
- `kernel.bin` — Production kernel binary
- `aurora-fresh.img` — Pristine disk image template with installer apps
- `apps/*.elf` — Native applications (calc.elf, notes.elf, files.elf)
- `fresh-install.sh` — Creates your persistent data disk
- `run.sh` — Launches QEMU
- `manifest.txt` — SHA-256 checksums

#### Option 2: Use Pre-built Artifacts

For quick testing, download individual artifacts:

```bash
# Kernel binary (deterministically built)
wget https://example.com/auroraos/kernel.bin

# Disk image with native apps pre-installed
wget https://example.com/auroraos/disk.img
```

---

## Boot Procedure

### Step 1: Build the Kernel (if building from source)

```bash
# From the repository root
AURORA_PRODUCTION=1 ./build.sh
```

This produces:
- `build/kernel.elf` — ELF with symbols (for debugging)
- `build/kernel.bin` — Raw binary for QEMU boot

### Step 2: Create a Disk Image (if using package script)

```bash
# Creates data/aurora.img from the template
./build/pkg/fresh-install.sh
```

Or use the package directly:

```bash
# Launches QEMU with default configuration
./build/pkg/run.sh
```

### Step 3: Boot QEMU

**Using the package's run script:**
```bash
./build/pkg/run.sh
```

**Using the repo's run script with explicit kernel:**
```bash
./run-qemu.sh build/kernel.bin
```

### Expected Boot Output

A healthy boot displays:

```
[qemu] Booting AuroraOS on ARM64 virt...
[qemu] Kernel: build/kernel.bin
[qemu] Press Ctrl-A then X to quit QEMU.

AuroraOS ARM64 virt boot
RAM: 256 MiB
MMU stage-1 active
...
```

### Boot Flags Reference

| Flag | Value | Description |
|------|-------|-------------|
| `-machine` | `virt,highmem=off` | ARM64 versatile machine |
| `-cpu` | `cortex-a53` | 32-bit compatible CPU |
| `-smp` | `1` | Single CPU core |
| `-m` | `256M` | 256 MiB RAM |
| `-nographic` | — | No graphics, serial console |
| `-serial stdio` | — | Serial output to terminal |
| `-monitor none` | — | Disable QEMU monitor |

### Interactive Shell

Once booted, you'll see a shell prompt. Available commands include:

| Command | Description |
|---------|-------------|
| `help` | List available commands |
| `ls` | List files on AuroraFS |
| `cat <file>` | Read a file |
| `aura` | Launch Aura command interface |
| `+app-name` | Launch an installed app (e.g., `+calc`) |
| `Ctrl-A X` | Quit QEMU |

---

## Usage

### Serial Console Controls

Since the beta runs in serial console mode:

| Action | Key Sequence |
|--------|--------------|
| Quit QEMU | `Ctrl-A` then `X` |
| Break to QEMU monitor | `Ctrl-A` then `C` (but monitor is disabled) |
| Send break signal | `Ctrl-A` then `B` |
| Type text | Direct keyboard input |
| Edit commands | Standard line editing (backspace, arrows) |

### Running Applications

The fresh disk image comes pre-installed with native apps:

| App | Launch Command | Description |
|-----|----------------|-------------|
| Calculator | `+calc` | Basic calculator |
| Notes | `+notes` | Persistent note-taking |
| Files | `+files` | File browser/viewer |

### Using Aura (Offline Assistant)

```bash
# Launch Aura interface
aura

# Available native commands:
# - `time` — Show current time
# - `date` — Show date info
# - `help` — List all commands
```

### File Management

```bash
# List all files
ls /

# Read a file
cat /readme.txt

# App files are read-only (installed by kernel)
# User files persist across reboots in your data/aurora.img
```

### Settings

```bash
# View settings
settings list

# Available settings:
# - `text-size` (100-200%)
# - `high-contrast` (on/off)
# - `keyboard-layout` (us)
```

---

## Known Limitations

The QEMU Emulator Beta has the following known limitations:

### No Offline Speech
- **Impact**: Voice commands, dictation, and speech-to-text are **not available**
- **Workaround**: Text input via serial console or virtio keyboard
- **Status**: Research-only; see `docs/kernel/offline-speech.md` for roadmap

### No Physical Hardware Support
- **Supported**: QEMU `virt` machine (ARM64) only
- **Not Supported**:
  - Raspberry Pi
  - Apple Silicon
  - Physical ARM64 development boards
  - Real GPU acceleration
  - GPIO, I2C, SPI, UART (beyond PL011)
  - USB devices
  - WiFi, Bluetooth
- **Drivers**: Only `virtio-blk`, `virtio-input`, `virtio-snd`, `virtio-mmio`, `ramfb`, `fw_cfg`

### ARM64 Only
- **Architecture**: AArch64 (ARM64) only
- **Not Supported**: x86_64, AArch32, 32-bit ARM
- **CPU**: cortex-a53 emulation (no KVM acceleration)

### No Networking
- **Status**: Networking is **not implemented** in this beta
- **Impact**: No internet access, no OTA updates
- **Development**: Use file copy or git to transfer files

### No Native Graphics (Serial Mode)
- **Display**: Serial console only (`-nographic`)
- **No**: Graphical UI, windowing system
- **Alternative**: For graphical UI, use `scripts/package-qemu.sh` which includes `ramfb` support

### Disk Image Limitations
- **Size**: Fixed 1 MiB (2048 sectors × 512 bytes)
- **Max Files**: ~8 installer + ~8 user files
- **Max File Size**: ~24 KiB per file
- **No**: Dynamic resize, partitions, multiple disks

### Audio
- **Output**: Available via QEMU audio backend (pulse/pipewire/alsa)
- **Input**: Not implemented
- **Quality**: 48 kHz stereo PCM

### Persistence
- **Data Disk**: Created once by `fresh-install.sh`, survives reboots
- **Backup**: Copy `data/aurora.img` to preserve your work
- **No Encryption**: Disk image is plaintext

---

## Integrity Verification

### SHA-256 Checksums

Verify your artifacts after download or build:

```bash
# Verify kernel.bin
sha256sum build/kernel.bin

# Verify disk image
sha256sum build/pkg/aurora-fresh.img

# Verify all package files
cd build/pkg
sha256sum -c manifest.txt
```

### Expected Checksums (for verification)

| File | SHA-256 Checksum |
|------|-----------------|
| `kernel.bin` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` (example) |
| `aurora-fresh.img` | Computed during build |
| `apps/calc.elf` | Listed in manifest.txt |
| `apps/notes.elf` | Listed in manifest.txt |
| `apps/files.elf` | Listed in manifest.txt |

### Verification Script

Use the provided verification script:

```bash
./build/pkg/verify-bundle.sh
```

Output on success:
```
verify-bundle: OK: 8 files match manifest.txt
verify-bundle: this is an integrity check, not authentication: the manifest itself is not signed
```

### Reproducibility

Builds are deterministic. Verify by building twice:

```bash
# Build 1
PKG_DIR=/tmp/pkg1 ./scripts/package-qemu.sh

# Build 2
PKG_DIR=/tmp/pkg2 ./scripts/package-qemu.sh

# Compare
diff <(cd /tmp/pkg1 && sha256sum kernel.bin apps/*.elf) \
     <(cd /tmp/pkg2 && sha256sum kernel.bin apps/*.elf)
```

No output = byte-identical builds.

### Boot-Time Integrity

The kernel verifies disk integrity on every mount:

| Check | Algorithm |
|-------|-----------|
| Superblock | CRC-32 |
| Inodes | CRC-32 per inode |
| File data | CRC-32 per 512-byte sector |
| ELF validation | Magic, architecture, capabilities, size, W^X, kernel VA exclusion |

**On corruption**: Kernel refuses to mount, boots to shell with `settings store=none`.

---

## Supported Host Platforms

| Platform | Status | Notes |
|----------|--------|-------|
| **Linux (x86_64)** | ✅ Fully supported | Native QEMU TCG; best performance |
| **Windows 10/11 (WSL2)** | ✅ Fully supported | Ubuntu/Debian/Arch in WSL2; use `-display gtk` or `-display sdl` for GUI |
| **macOS (Apple Silicon)** | ⚠️ Untested | QEMU TCG works; `aarch64-linux-gnu` toolchain required (Homebrew) |
| **macOS (Intel)** | ⚠️ Untested | Same as Apple Silicon; Rosetta 2 may add overhead |
| **Linux (ARM64 native)** | ✅ Works | KVM available (`-enable-kvm -cpu host`) for near-native speed |

### Minimum Host Requirements

| Resource | Minimum | Recommended |
|----------|---------|-------------|
| CPU | 2 cores | 4+ cores |
| RAM | 2 GiB | 8+ GiB |
| Disk | 500 MiB | 2 GiB |
| OS | Ubuntu 22.04+, Debian 12+, Fedora 38+, Arch | Latest LTS |

### Host-Specific Instructions

#### Linux
```bash
# Install dependencies
sudo apt install gcc-aarch64-linux-gnu qemu-system-arm

# Build and run
./scripts/package-qemu.sh
./build/pkg/fresh-install.sh
./build/pkg/run.sh -display gtk  # Graphical mode
```

#### Windows WSL2
```powershell
# PowerShell - install WSL2
wsl --install -d Ubuntu

# Inside WSL2
sudo apt update && sudo apt install -y \
    gcc-aarch64-linux-gnu \
    qemu-system-arm \
    python3 \
    coreutils

# Build and run (same as Linux)
./scripts/package-qemu.sh
./build/pkg/fresh-install.sh
./build/pkg/run.sh -display sdl  # SDL works cross-platform
```

#### macOS (Requires Homebrew)
```bash
# Install toolchain
brew install aarch64-linux-gnu-gcc qemu

# Build (may need to set PATH)
PATH="/usr/local/opt/aarch64-elf/bin:$PATH" ./build.sh
```

---

## Quick Start Guide

### From Scratch (Linux/WSL2)

```bash
# 1. Clone repository
git clone https://github.com/your-org/AuroraOS.git
cd AuroraOS

# 2. Install prerequisites
sudo apt install gcc-aarch64-linux-gnu qemu-system-arm

# 3. Build the package
AURORA_PRODUCTION=1 ./scripts/package-qemu.sh

# 4. Create your data disk
./build/pkg/fresh-install.sh

# 5. Boot the OS
./build/pkg/run.sh

# 6. Quit with Ctrl-A X
```

### From Existing Build

```bash
# If kernel.bin and aurora-fresh.img exist:
./build/pkg/fresh-install.sh --force  # Recreate data disk
./build/pkg/run.sh -display sdl
```

---

## Troubleshooting

### Boot Hangs / No Output

```bash
# Save serial log to file
./build/pkg/run.sh -display none -serial file:boot.log
cat boot.log
```

Check for:
- Device tree issues
- Memory allocation failures
- Filesystem corruption

### "run.sh: no disk" Error

```bash
# You forgot to create the disk image
./build/pkg/fresh-install.sh
./build/pkg/run.sh
```

### "filesystem is CORRUPT" on Boot

- Your `data/aurora.img` is damaged
- Restore from backup or recreate:

```bash
# Backup first
cp data/aurora.img ~/aurora-backup.img

# Recreate (loses all data)
./build/pkg/fresh-install.sh --force
```

### verify-bundle.sh Fails

```bash
# Re-download or rebuild the package
# If you modified files intentionally, update manifest.txt:
cd build/pkg
sha256sum kernel.bin aurora-fresh.img apps/*.elf fresh-install.sh run.sh verify-bundle.sh > manifest.txt
```

### QEMU Not Found

```bash
# Debian/Ubuntu
sudo apt install qemu-system-arm

# Fedora
sudo dnf install qemu-system-aarch64

# Arch
sudo pacman -S qemu

# Check installation
which qemu-system-aarch64
```

### Audio Not Working

```bash
# Set audio driver
export QEMU_AUDIO_DRV=pa  # PulseAudio
# or
export QEMU_AUDIO_DRV=alsa

# Run with audio
./build/pkg/run.sh
```

---

## Security Considerations

⚠️ **This is beta software for development only.**

- **No authentication**: SHA-256 checksums detect corruption/tampering but do not verify origin
- **No encryption**: Disk images are plaintext; anyone with file access can read all data
- **No sandboxing**: EL0 apps share address space isolation only; no syscall filtering beyond capability checks
- **TCG only**: No hardware-enforced isolation; host kernel compromise = guest compromise

**Recommended use**: Development, testing, and evaluation only. **Not recommended** for sensitive data.

---

## Related Documentation

- `qemu-packaging.md` — Build process, package format, and reproducibility
- `loader.md` — Program loading, ELF validation
- `storage.md` — AuroraFS and filesystem
- `userspace.md` — Processes and syscalls
- `apps.md` — Native applications
- `native-graphics-plan.md` — Graphical UI on QEMU with ramfb

---

## Version Information

- **Document version**: 1.0 (Beta)
- **Compatible with**: AuroraOS commit `e24f8914d3689c28215aeb6bbd82435b1657a443`
- **Last updated**: 2026-11-25

---

*This documentation describes the QEMU Emulator Beta for development and testing purposes. For production use on physical hardware, consult the Hardware Porting Guide when available.*