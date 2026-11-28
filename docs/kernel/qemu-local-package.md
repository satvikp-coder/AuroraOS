# Build and run the local AuroraOS QEMU package

This package runs AuroraOS as an ARM64 guest with QEMU's `virt` machine and software emulation. It uses one Cortex-A53 virtual CPU and 256 MiB of guest RAM by default. The host also needs space for the cross compiler, QEMU, native build outputs and a separate copy of each writable guest disk.

## Set up the build

Use Linux or a WSL 2 distribution on Windows. Under WSL, run the commands in the Linux shell. Keep the checkout and bundle in an ordinary Linux filesystem directory with a path free of spaces and commas. The graphical launch uses QEMU's graphical frontend. On Windows, follow Microsoft's guide for WSL GUI support. [Install WSL](https://learn.microsoft.com/en-us/windows/wsl/install) · [Run Linux GUI apps](https://learn.microsoft.com/en-us/windows/wsl/tutorials/gui-apps)

On Debian-based distributions, install the host tools and ARM64 cross-toolchain, then clone the repository.

```sh
sudo apt-get update
sudo apt-get install git build-essential gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu qemu-system-arm
git clone https://github.com/satvikp-coder/AuroraOS.git
cd AuroraOS
chmod +x build.sh
sh scripts/package-qemu.sh
```

The default output is `build/pkg`. Each build needs a new output directory. To make another bundle without replacing a previous bundle or its installed disk, use an unused path such as `PKG_DIR=build/pkg-next sh scripts/package-qemu.sh`. If a build fails partway, use another new directory.

## Verify, install and launch

```sh
cd build/pkg
./verify-bundle.sh
./fresh-install.sh
./run.sh -nic none
```

The SHA-256 manifest detects damaged or changed bundle files. It is unsigned and cannot identify the publisher. For a trusted comparison, keep the manifest separately from the artifacts or compare it with an independently trusted copy. Successful verification covers the pristine bundle. Your writable state lives in `data/aurora.img`.

The Index opens installed Calculator, Notes, Files and Clock. Close the QEMU window to stop, then use `./run.sh -nic none` again on the same disk. For serial diagnostics on a host without a display, use `./run.sh -display none -monitor none -nic none` and stop with Ctrl+C. That mode does not show Aurora Glass.

## Keep and recover guest data

Stop QEMU before copying or replacing a disk image. Back up `data/aurora.img` to a separate, previously unused path. Store the source commit from `manifest.txt` with the backup.

If a guest disk fails to mount or packaged applications are missing, preserve the original disk and confirm that the pristine bundle verifies. Make a clean install in a new bundle directory and boot it independently to check the host and package. To restore from a known-good backup, use a copy in the same package's `data/aurora.img` path with QEMU stopped and the failed disk preserved elsewhere. Retain the corresponding package version for recovery. `fresh-install.sh --force` discards the current guest disk and should only be used for an intentional reset.

## Verify a candidate before distribution

From the checkout root, compare independent package builds from the same commit and toolchain. Run the package and recovery suites on disposable images. Run the broader repository CI before accepting a candidate.

```sh
chmod +x scripts/package-qemu.sh
sh tests/qemu/pkg-tests.sh
sh tests/qemu/recovery-tests.sh
```

Record the exact commit, tool versions, hashes, and actual results. The native interface currently uses Clear mode with printable ASCII text. The guest runs on QEMU only. The source package does not include a language model or a real-world offline speech recognizer. The default graphical package command leaves audio capture and playback device setup out. Test reports should distinguish synthetic fixtures from user-facing capabilities.

For distribution, include the AuroraOS Apache 2.0 license and the Noto Sans Mono source font's SIL Open Font License and copyright notices. The host compiler, QEMU and other tools are installed separately. Verify distribution permissions for anything added to the package and retain its license notices. [AuroraOS license](../../LICENSE) · [Noto Sans Mono license](../../third_party/fonts/NotoSansMono/OFL.txt)
