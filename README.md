# AuroraOS

An independent, open-source ARM64 operating system with the **Aurora Glass** UI and an offline assistant, **Aura**.

**Not an Android ROM, not a Linux distribution, not a launcher.** AuroraOS has its own kernel; no Linux or Android code is used. It is early-stage: a small kernel that boots and manages memory on an emulator, plus a browser-based design prototype. It is not usable as an operating system yet.

## Status at a glance

| Area | State | Where |
|---|---|---|
| ARM64 kernel boots on QEMU `virt` | **Working, tested in CI** | `boot/`, `kernel/` |
| Physical memory manager, MMU (W^X, guard pages), physical-target mapping policy, device-tree parsing, exception diagnostics | **Working on QEMU, tested in CI** | `kernel/mm/`, `kernel/arch/`, `docs/kernel/` |
| Interrupts, timer, separate exception stack | **Working on QEMU, tested in CI** | `kernel/irq.c`, `kernel/arch/`, `docs/kernel/interrupts-timers.md` |
| Processes, EL0 userspace, preemptive scheduler, syscalls, bounded IPC | **Working on QEMU, tested in CI** (8 processes, isolated address spaces, W^X, fault containment) | `kernel/proc/`, `docs/kernel/userspace.md` |
| Storage and filesystem | **Working on QEMU, tested in CI**: virtio-blk driver; AuroraFS with per-owner names, file-id existence hiding, per-principal quotas, crash and uncertain-write handling; EL0 file access | `kernel/drivers/`, `kernel/fs/`, `docs/kernel/storage.md` |
| Program loader and init | **Working on QEMU, tested in CI**: a narrow static AArch64 ELF subset is loaded from AuroraFS into an isolated EL0 process under an identity chosen by the kernel; the kernel starts an installed `init.elf` as the first user process | `kernel/proc/elf.c`, `kernel/proc/launch.c`, `docs/kernel/loader.md` |
| Native settings registry | **Working on QEMU, tested in CI**: six typed preferences, capability-checked, persistent on AuroraFS (stored preferences; no hardware is driven) | `kernel/settings.c`, `docs/kernel/settings.md` |
| Native graphics and native Aurora Glass UI | Not started | issues #7, #8 |
| Aurora Glass **browser prototype** | **Prototype only** (runs in a host browser, controls nothing real) | `aurora-glass/prototype/` |
| Aura Action Broker | **Mock only**: a tested contract for typed, approved actions; not wired into the UI, not an OS service | `aurora-glass/prototype/broker.js` |
| Offline language model, speech | Not started | issues #12, #13 |
| Samsung Galaxy F04 | **Unverified research target.** Nothing here has run on hardware. | issue #15 |

## What works now

**Kernel (QEMU ARM64 `virt`, one CPU)**
- Boots from an arm64 Image header (so QEMU hands over the device tree), runs at EL1 (entered from EL2 if started there).
- Requires a validated device tree: RAM size and reservations come from it, and the kernel refuses to boot rather than guess. A fixed-RAM mode exists only for development builds with `AURORA_FIXED_RAM=1`.
- Bitmap page allocator with checked frees; stage-1 MMU with text read-only/executable, rodata read-only, data/stack read-write/no-execute, unmapped stack guard page, and a policy that refuses unsafe aliases (writable text/rodata, executable data, device-tree `no-map` memory, wrong memory type for MMIO).
- Exception vectors with decoded diagnostics over the serial console.
- Processes at EL0 with private address spaces, a preemptive scheduler, system calls and bounded IPC; faults are contained. Storage: a virtio block driver and AuroraFS (see the table). A program is loaded from the disk and run under a kernel-chosen identity (`docs/kernel/loader.md`); a native settings registry persists on AuroraFS (`docs/kernel/settings.md`).
- Tests (QEMU, all in CI): memory 128, interrupts 93, processes 154, storage 53, filesystem and EL0 file access 161, program loader 72, settings 74, and an end-to-end run of loader + storage + settings on one disk 50; host-side parser, policy and ELF-validator tests (with a 300,000-iteration fuzz) under ASan/UBSan. QEMU only: nothing here has run on hardware.

**Browser prototype (host only)**
- Home, Aurora Dock, Spaces, Index search, Aura Plan sheet, Pages (simulated multitasking and memory), Quick Center (simulated controls), Settings (one registry, simple and advanced views, searchable from Index; each entry is labelled live, stored-only or locked); Clear and Soft Glass tiers, light/dark, high contrast, 150%/200% text, reduced motion, keyboard support.
- Everything is simulated. Changing a toggle changes the prototype, never your computer or phone.

**Aura Action Broker (mock)**: typed actions, risk classes S0-S3, permissions that fail closed, approvals bound to the exact plan, undo from real receipts. It documents its own limits: it is JavaScript in one browser context, not a security boundary. The native broker must run in a privileged OS service.

## Not done yet

Drivers beyond the console and the virtio block device, native graphics and UI, native apps (Notes, Files, Calculator, Clock, Settings UI), Passport/Trail/Vault as native services, an installer service and signed applications (executables are authorised today by installer ownership of the file), the offline model and voice, and any physical-device port. Browser prototype screens are **not** native OS features.

## Run it from a fresh checkout

**Browser prototype and its tests** (any OS; Node 20+):
```
npm ci
npx playwright install chromium
node tests/unit/aurora-glass.test.js
node tests/unit/core.test.js
node tests/unit/broker.test.js
npx playwright test            # starts a local static server on :3000
npx http-server aurora-glass/prototype -p 3000   # to look at it yourself
```

**Kernel** (Linux, or Windows via WSL2): install `gcc-aarch64-linux-gnu`, `binutils-aarch64-linux-gnu`, `qemu-system-arm` (Debian/Ubuntu names) and a host C compiler.
```
./build.sh                         # build/kernel.elf and build/kernel.bin
./run-qemu.sh                      # boot it (Ctrl-A X quits)
./tests/qemu/boot-smoke-test.sh    # boots headless, checks for the boot banner
./tests/qemu/mm-tests.sh           # memory management, MMU, deliberate faults
./tests/kernel-host/run.sh         # device-tree parser, mapping policy and ELF validator on the host (ASan/UBSan)
./tests/qemu/process-tests.sh      # EL0 processes, scheduler, IPC
./tests/qemu/storage-tests.sh      # virtio block driver on disposable images
./tests/qemu/fs-tests.sh           # AuroraFS, EL0 file access, quotas, existence hiding
./tests/qemu/loader-tests.sh       # programs loaded from a host-provisioned disk (3 QEMU runs)
./tests/qemu/settings-tests.sh     # native settings registry (3 QEMU runs)
./tests/qemu/e2e-tests.sh          # loader + storage + settings together on one disk
```
Always boot `build/kernel.bin` (the ELF gets no device tree). On Windows, run the shell commands inside WSL, not PowerShell.

## Documents

- `DESIGN.md`: Aurora Glass visual contract. `AuroraOS_Final_Blueprint_OpenDesign.md`: architecture and plan.
- `docs/kernel/mmu-configuration.md`: reviewed MMU settings, mapping policy and known limits.
- `docs/kernel/userspace.md`, `storage.md`, `loader.md`, `settings.md`: processes and syscalls, the filesystem, the program loader and launch policy, the settings registry (each lists its limits).
- `aurora-glass/open-design-handoff/native-compositor-mapping.md`: browser-to-native component mapping.

## Safety

The Samsung Galaxy F04 is an experimental research target only. No physical-device operations are performed from this repository: no flashing, rooting, bootloader or firmware changes. All low-level work runs on QEMU ARM64 `virt`.

## License

Apache-2.0. See `LICENSE`.
