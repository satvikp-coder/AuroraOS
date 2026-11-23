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
| Native graphics and shell | **A native Aurora Glass shell on QEMU, tested in CI** (issues #7 and #8): the kernel draws a ramfb framebuffer (fills, rounded rectangles, borders, monospaced bitmap text from Noto Sans Mono, SIL OFL 1.1, **printable ASCII only**), reads a virtio keyboard and tablet, and boots into a native Home with an Aurora Dock, Spaces, Index search, an About dialog, a Glass Corner sheet and an Aurora Horizon rail. QEMU screenshots and injected keyboard and pointer events are verified. QEMU/TCG only, Tier 0 Clear (opaque, no blur), US keyboard layout, no Hindi or Gujarati, state in memory only. Native screens now include Pages (a live read-only view of the process table), Aura (typed commands decided by a kernel policy; the model is NOT involved), Settings (text size and high contrast work; the other settings are stored preferences only), Clock (a kernel timer service), Trail (a read-only view of the audit ledger) and a hold-to-confirm demonstration. No real notifications. Nothing on real hardware | issues #7 and #8 (both open), `docs/kernel/native-graphics-plan.md` |
| EL0 UI service and native applications | **Working on QEMU, tested in CI**: EL0 programs describe surfaces through `SYS_UI` and the kernel-side compositor validates and draws them (hostile and unprivileged clients are refused); installed native Calculator, Notes (persistent, two-slot, verified read-back) and Files (read-only viewer), launched by the kernel under kernel-chosen identities | `kernel/ui/`, `kernel/proc/apps.c`, `tests/programs/`, `docs/kernel/apps.md` |
| Passport, Trail, trusted review | **Working on QEMU, tested in CI**: durable permission grants and a chained audit ledger (each record carries a 64-bit chain value over its predecessor) on AuroraFS (damaged or rolled-back ledgers deny everything), trusted S1 settings approvals with Undo, bound to the exact request. A checksum chain detects accidental damage; it is not tamper-proofing | `kernel/ui/passport.c`, `kernel/ui/trail.c`, `kernel/ui/review.c` |
| Memory safety and compute | **Working on QEMU, tested in CI**: bounded `SYS_MEM` user heap with guard pages, per-process and per-principal budgets and a kernel reserve; per-process FP/SIMD state and a native NEON matrix-vector test checked against a scalar reference | `kernel/proc/memsys.c`, `docs/kernel/` |
| Packaging and recovery | **Working on QEMU, tested in CI**: reproducible `kernel.bin` + fresh disk image bundle with a SHA-256 manifest (integrity, **not authentication**); recovery tests for corrupt, blank, truncated, read-only, missing and crash-interrupted disks, and for damaged installed executables | `scripts/package-qemu.sh`, `tests/qemu/recovery-tests.sh` |
| Shared virtqueue module, model store format | Host-tested foundations: one split-virtqueue bookkeeping module (used by both the virtio-input and virtio-blk drivers) and the `AURMODEL` read-only model volume format with SHA-256 (integrity only; no device attachment, installer, signature or runtime yet) | `kernel/drivers/virtqueue.c`, `kernel/fs/modelstore.c`, `docs/kernel/model-asset-store.md` |
| Aurora Glass **browser prototype** | **Prototype only** (runs in a host browser, controls nothing real) | `aurora-glass/prototype/` |
| Aura Action Broker | **Mock only**: a tested contract for typed, approved actions. The browser prototype's Aura, Vault, Passport, Trail and Clock go through it, but it is JavaScript in one browser context: not an OS service and not a security boundary | `aurora-glass/prototype/broker.js` |
| Offline language model, speech, audio | **Not started** (research notes only: `docs/kernel/native-inference-prerequisites.md`, `docs/kernel/audio-and-drivers-research.md`). No model runs in AuroraOS; Aura's native commands are a fixed typed grammar | issues #12, #13 |
| Samsung Galaxy F04 | **Unverified research target.** Nothing here has run on hardware. | issue #15 |

## What works now

**Kernel (QEMU ARM64 `virt`, one CPU)**
- Boots from an arm64 Image header (so QEMU hands over the device tree), runs at EL1 (entered from EL2 if started there).
- Requires a validated device tree: RAM size and reservations come from it, and the kernel refuses to boot rather than guess. A fixed-RAM mode exists only for development builds with `AURORA_FIXED_RAM=1`.
- Bitmap page allocator with checked frees; stage-1 MMU with text read-only/executable, rodata read-only, data/stack read-write/no-execute, unmapped stack guard page, and a policy that refuses unsafe aliases (writable text/rodata, executable data, device-tree `no-map` memory, wrong memory type for MMIO).
- Exception vectors with decoded diagnostics over the serial console.
- Processes at EL0 with private address spaces, a preemptive scheduler, system calls and bounded IPC; faults are contained. Storage: a virtio block driver and AuroraFS (see the table). A program is loaded from the disk and run under a kernel-chosen identity (`docs/kernel/loader.md`); a native settings registry persists on AuroraFS (`docs/kernel/settings.md`).
- Tests: the suites under `tests/qemu/` (memory, interrupts, processes, storage, filesystem, loader, settings, graphics, input, UI, apps, ledger, recovery, packaging and more) run in CI on QEMU; the per-suite check counts change often, so read them from the suite output rather than from this file. Host-side parser, policy and ELF-validator tests (with a 300,000-iteration fuzz) under ASan/UBSan. QEMU only: nothing here has run on hardware.

**Browser prototype (host only)**
- Shell: Home, Aurora Dock, Spaces, Pages (simulated multitasking and memory), Quick Center (simulated controls), Settings (one registry, simple and advanced views; each entry is labelled live, stored-only or locked), Index search (ranked results, recent use as a tie-break, and cautious typo suggestions for app names that never run on Enter), the Aura Plan sheet with review and hold-to-confirm, a first-run welcome, a keyboard shortcut sheet (press `?`), shared context menus for Vault files and Pages, and a desktop window card at 1024 px and wider.
- Apps: Notes (saved in this browser only), Calculator, Clock (clock, stopwatch, timer), Vault/Files (sample files, moves reviewed, deletes need a hold), Passport (permissions), Trail (receipts and undo).
- Looks and access: Clear and Soft Glass tiers, light/dark, high contrast, 125/150/200% text, reduced motion, keyboard support. English is the only verified language; Hindi and Gujarati are empty placeholders that show English.
- Everything is simulated. Changing a toggle changes the prototype, never your computer or phone.

**Aura Action Broker (mock)**: typed actions, risk classes S0-S3, permissions that fail closed, approvals bound to the exact plan, undo from real receipts. It documents its own limits: it is JavaScript in one browser context, not a security boundary. The native broker must run in a privileged OS service.

## Not done yet

Drivers beyond the console, virtio block, input and ramfb (no audio, no networking, no USB), an installer service and signed applications (executables are authorised today by installer ownership of the file; the bundle manifest is not a signature), non-ASCII text (no Hindi or Gujarati natively), Soft Glass, real notifications, the offline model, speech and any inference runtime, and any physical-device port (the Galaxy F04 is a research target only and is NOT ready). Everything runs on QEMU under TCG. Browser prototype screens are **not** native OS features.

## Run it from a fresh checkout

**Browser prototype and its tests** (any OS; Node 20+):
```
npm ci
npx playwright install chromium
for f in tests/unit/*.test.js; do node "$f" || break; done   # all unit tests (CI lists each one in .github/workflows/ci.yml)
npx playwright test --workers=1   # real-browser tests; starts a local static server on :3000
node tests/perf/profile.js --runs 7   # optional: measure the prototype (not a test; see docs/agents/AURORA_UI_PERF.md)
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
./tests/qemu/home-tests.sh         # the native shell (Home, Dock, Spaces, Index, About, Glass Corner) with real injected input
./tests/qemu/scale-tests.sh        # native text scaling and high contrast (4 boots)
./tests/qemu/settings-ui-tests.sh  # native Settings screen: no disk, save, restart, read-only, destroyed filesystem
```
Always boot `build/kernel.bin` (the ELF gets no device tree). On Windows, run the shell commands inside WSL, not PowerShell.

## Documents

- `DESIGN.md`: Aurora Glass visual contract. `AuroraOS_Final_Blueprint_OpenDesign.md`: architecture and plan.
- `docs/kernel/mmu-configuration.md`: reviewed MMU settings, mapping policy and known limits.
- `docs/kernel/native-graphics-plan.md`: native graphics on QEMU (issue #7): the G1 research (what the kernel has, what QEMU offers, the options compared, risks), the G2 record (fw_cfg + ramfb, framebuffer reservation, verified protocol facts, evidence, cache-coherence limits) the G3 record (rounded rectangles and borders, the exact coverage rule) and the G4 record (font source and licence, reproducible atlas generation, the text engine). QEMU only; ASCII text only; no input or windows yet.
- `docs/kernel/userspace.md`, `storage.md`, `loader.md`, `settings.md`: processes and syscalls, the filesystem, the program loader and launch policy, the settings registry (each lists its limits).
- `docs/kernel/el0-ui-service-design.md` (design only, nothing implemented) and `docs/aura/qwen3-local-feasibility.md` (analysis only: no model has been run on AuroraOS or benchmarked).
- `aurora-glass/open-design-handoff/native-compositor-mapping.md`: what a native compositor and UI toolkit must provide, component by component (a requirements reference; rows marked "QEMU, Tier 0" exist in the kernel-side native shell, the rest do not). Its names, colours and numbers are checked against the prototype by `tests/unit/handoff-docs.test.js`.
- `docs/agents/AURORA_UI_HANDOFF.md` (current state and next steps), `AURORA_UI_TASKS.md` (milestone log), `AURORA_TESTING.md` (how to test), `AURORA_UI_PERF.md` (measured prototype performance and its limits).

## Safety

The Samsung Galaxy F04 is an experimental research target only. No physical-device operations are performed from this repository: no flashing, rooting, bootloader or firmware changes. All low-level work runs on QEMU ARM64 `virt`.

## License

Apache-2.0. See `LICENSE`.
