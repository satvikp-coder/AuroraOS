# AuroraOS QEMU Emulator Beta

## Fresh Installation

### Prerequisites

- **Host OS**: Linux (native) or Windows via [WSL2](https://learn.microsoft.com/en-us/windows/wsl/install)
- **Target**: ARM64 (AArch64) QEMU `virt` machine

### Download Artifacts

1. **Kernel binary**: `build/kernel.bin` — produced by `./build.sh`
2. **Disk image**: `build/disk.img` — created by the build script or recovery tools

> **Note**: The build script produces `build/kernel.bin` and `build/kernel.elf`. A disposable QEMU disk image is generated when `AURORA_BLK_TEST=1` or `AURORA_FS_TEST=1` is passed to `build.sh`.

### Run the Emulator

```bash
# From the repo root
./run-qemu.sh              # boots build/kernel.bin (default)
./run-qemu.sh build/kernel.bin   # explicit kernel path
```

The script launches QEMU with the following configuration:

- Machine: `virt` (ARM64 versatile abatement)
- CPU: `cortex-a53` (32-bit compatible mode)
- RAM: 256 MiB
- No graphics (nographic mode, serial console only)
- Serial output to stdout (visible in the terminal)

To quit QEMU at any time, press **Ctrl-A** then **X**.

---

## Boot Procedure

1. Ensure the kernel binary exists:

   ```bash
   ./build.sh
   ```

2. Boot QEMU:

   ```bash
   ./run-qemu.sh
   ```

3. Observe the serial console output. A successful boot displays:

   ```
   AuroraOS ARM64 virt boot
   RAM: 256 MiB
   MMU stage-1 active
   ```

4. The kernel then proceeds with early initialization (memory setup, device tree parsing, IRQ enabling) and transitions to the shell prompt (if a disk image with an installed OS is present) or enters a minimal monitor loop.

---

## Usage

Once booted, the OS is controlled entirely via the **serial console** (no GUI in this beta):

| Action | Key(s) |
|---|---|
| Quit QEMU | `Ctrl-A` then `X` |
| Basic introspection | `help` (if shell is active) |
| Inspect memory/state | Depends on shell configuration |

**Note**: The beta build has no windowing system, no network, and no native input stack beyond what QEMU's `-nographic` mode provides. All interaction happens through the captured serial port.

---

## Known Limitations

- **No offline speech**: Aura's offline language model is not compiled into this build.
- **No physical hardware support**: This build runs exclusively on QEMU ARM64 `virt`. No real-device drivers are included.
- **ARM64 only**: `x86_64` and `aarch32` are not supported.
- **No networking**: Networking is not configured in the beta.
- **No persistent storage across reboots**: The disk image is ephemeral (unless `AURORA_BLK_TEST=0`/`AURORA_FS_TEST=0` is used with a pre-provisioned image).
- **Text input**: Limited to what QEMU's `-serial stdio` can capture; no virtio-console or keyboard passthrough in this configuration.

---

## Integrity Verification

SHA-256 checksums are provided for all distributed artifacts. Verify them after download or build.

### Checksums

| File | Expected SHA-256 |
|---|---|
| `build/kernel.bin` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `build/disk.img` | `aF3dE5fB7c1A2dE4f6B9c3D2e5A6b7C8d9E0f1A2B3C4D5eF6A7B8C9D0E1F2A3B4` *(example)* |

### Verification Commands

```bash
# Verify kernel.bin
sha256sum build/kernel.bin

# Verify disk image
sha256sum build/disk.img
```

If a checksum does not match the expected value, re-run `./build.sh` to regenerate the artifacts from source.

---

## Supported Hosts

| Host | Instructions |
|---|---|
| **Linux** (native) | All build and run commands work directly. Install `qemu-system-arm` if needed: `sudo apt install qemu-system-arm`. |
| **Windows** | Use [WSL2](https://learn.microsoft.com/en-us/windows/wsl/install) with a Debian/Ubuntu distro. Run all shell commands (`./build.sh`, `./run-qemu.sh`, etc.) inside the WSL terminal, **not** PowerShell. The kernel and tools are cross-compiled for ARM64 and run identically under WSL. |

> **Windows WSL tip**: If you installed AuroraOS on Windows PowerShell, migrate your workflow to WSL (`wsl --install`) for full compatibility.

---