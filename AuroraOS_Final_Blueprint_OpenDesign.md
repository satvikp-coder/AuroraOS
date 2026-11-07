# AuroraOS — Final Blueprint & Build Architecture

**Edition:** 1.2 · OpenDesign-integrated Aurora Glass UI · 7 November 2026  
**Interface source of truth:** `DESIGN.md` is the canonical *visual token and component contract*, while this document specifies architecture and screen behavior. Both supersede the old paper-style Folio visuals in `AuroraOS Folio.md`, which remains a reference for the *functions* of Desk, Spaces, Pages, Index, Vault, Passport, Trail and Aura.  
**Status:** Design and planning only. Nothing has been implemented, built for F04, or flashed.

## 1. The promise, in simple language

AuroraOS is planned as a **new independent ARM64 operating system**, not an Android ROM, Linux skin, or launcher. It has its own kernel, system calls, system services, UI toolkit, native app platform, and offline assistant called **Aura**. The first build target is **QEMU ARM64 (`virt`)**. The first experimental phone target is the **Samsung Galaxy F04**, which remains **unverified** until hardware boot and recovery feasibility are established.

Success has two separate definitions:

1. **OS success:** Someone can download a tested QEMU image from GitHub and use AuroraOS in a virtual machine, with useful native apps and, eventually, offline AI.
2. **Phone success:** The OS also boots on an explicitly supported physical phone with working drivers, security, safe installation and recovery. The F04 is a research target, **not a guaranteed success**.

The UI must work with Aura turned **off**.

## 2. Selected visual design: Aurora Glass (Windows structure + iOS-inspired glassmorphism)

**Change in this edition: only the UI design and the graphics work needed to render it.** All core product goals stay the same: custom ARM64 kernel, independent native apps, offline Aura, capability-based permissions, QEMU-first, and experimental Galaxy F04 port. No work has been implemented or flashed.

**Visual direction:** Combine the *organized, useful desktop/workspace layout* commonly associated with Windows with the *soft translucent, layered, tactile glass appearance* associated with modern iOS. Build our own original visual assets, icons, type system, layouts and animation language; **do not copy Apple/Microsoft UI assets or trade dress**. This design is called **Aurora Glass**.

### What users will see

- **Aurora Desktop / Desk:** a clean, personalized home canvas, not an endless app grid. A large welcome/search area, a compact **Continue** section with active Spaces, a **Pinned** tool cluster, useful small widgets, and one unobtrusive Aura suggestion. Users may select **Classic mode** for a familiar icon/app layout.
- **Aurora Dock (formerly Spine):** a persistent *frosted* bottom control strip on phones, with **Back · Spaces · Home · Pages** and the combined **Ask Aura / Search** entry. On wider screens it becomes a Windows-like bottom taskbar with running-app indicators and a system tray.
- **Spaces:** workspace tabs/panels for Study, Work, Personal and Projects. Each Space has its own accent color, wallpaper option, pinned apps, recent objects and notification preferences.
- **Pages:** live, paused and saved activities shown as overlapping glass window previews with clearly labeled states. On tablets/desktops, offer a snap-layout grid; on phones use a readable stacked view.
- **Glass Corner (replaces the Fold visual):** a small translucent, always discoverable notification handle in the top-right. It preserves Folio's held-notification behavior without the old cut-paper corner.
- **Aurora Horizon:** a thin, readable activity/progress rail along the bottom of the content. Aura uses it for listening/transcription and numbered action progress, but not continuous decorative glow.
- **Aura Panel:** a glass command surface that emerges from the Dock or microphone key. It shows transcription, a *numbered editable plan*, permission checks, progress, receipts and undo when supported. Aura remains optional and no avatar or glowing orb is required.
- **Quick Center:** Windows-like quick tiles and sliders inside a layered glass surface: Wi-Fi, Bluetooth, brightness, volume, battery saver, privacy indicators and Aura mode. Unsupported hardware controls are clearly labeled.
- **Vault, Passport, Trail, Settings and native apps:** share the same window style, navigation behavior and accessible controls; their underlying functions remain unchanged.

### Visual system — design tokens

| Token | Light mode | Dark mode | Purpose |
|---|---|---|---|
| Canvas | `#EEF3F8` | `#0B1020` | Main desktop/home background |
| Window surface | `rgba(255,255,255,0.74)` | `rgba(24,32,49,0.76)` | Primary glass card/window |
| Dock surface | `rgba(240,246,255,0.80)` | `rgba(18,27,43,0.82)` | Persistent navigation |
| Strong surface | `#FFFFFF` | `#192338` | Text-heavy or safety-critical opaque areas |
| Primary text | `#142136` | `#F3F7FF` | Headings and body |
| Secondary text | `#52627A` | `#B4C2D8` | Metadata |
| Aurora blue | `#3478F6` | `#6AA3FF` | Main interaction accent |
| Aurora cyan | `#00AEB8` | `#46D6DF` | AI/status/selection accent |
| Focus outline | `#175CD3` | `#A8D7FF` | Keyboard and accessibility focus |
| Warning / critical | Named color + icon + text | Named color + icon + text | Never communicate status by color only |

**Typography:** use an openly licensed sans-serif family with a familiar Windows-like hierarchy and readable iOS-like spacing; system text approximately 14–17 logical px, prominent home heading 28–34 px, minimum 48 × 48 logical-pixel interactive targets. Final font choice depends on language coverage, licensing and benchmarked rasterization.

**Geometry:** 16–24 px rounded windows, 12–18 px controls, 1 px borders, mild layered elevation, generous spacing and crisp iconography. Avoid excessive gradients, floating ornaments or unreadable backgrounds. Use a restrained static wallpaper with gentle blue/cyan/purple light fields, not the old neon sci-fi aesthetic.

**Motion:** quick 120–220 ms transforms and fades for opening windows, docking and switching Spaces. No compulsory perpetual animations. Respect reduced motion and battery saver.

### Glass rendering must be progressive, not required for usability

The independent OS may initially have only a basic framebuffer and a CPU renderer. We therefore specify three graphics tiers:

| Tier | Appearance | Used when |
|---|---|---|
| **Tier 0 — Clear** | Fully opaque windows, same layout, rounded corners, border and accents; no blur | First QEMU compositor, low-power mode, accessibility, GPU unavailable |
| **Tier 1 — Soft Glass** | Alpha-blended translucent panels over a *cached/static* blurred wallpaper; no expensive live backdrop blur | CPU compositor proves it can meet frame-time and memory targets |
| **Tier 2 — Dynamic Glass** | Hardware-accelerated *live* backdrop blur and richer transitions, bounded to small surfaces | A verified GPU/compositor path supports it with acceptable power consumption |

Tier 0 is a **complete, supported interface**, not a broken-looking fallback. The phone or emulator should choose the highest measured stable tier automatically, and users can force Clear mode. Never rely on true blur for legibility: text content, permission prompts, recovery tools and critical AI confirmations use **opaque or nearly opaque surfaces**. High contrast disables decorative transparency; reduced motion disables nonessential effects.

### Screen-specific acceptance criteria

| Screen | Expected visual behavior | Functionality that does **not** change |
|---|---|---|
| Lock | Large readable clock, privacy-safe translucent notification slips, solid PIN sheet | PIN, lockout rules, emergency access where hardware supports it |
| Home | Structured Start/Continue/Pinned layout, dock, Space tabs, optional widgets | Resume objects and tools without AI |
| Index | Searchable app/object/setting/action launcher | Local search and keyboard navigation |
| Spaces | Workspace selector and cards with independent accents | Persist supported workspace state |
| Pages | Glass previews and running/paused/saved labels; snap layout on large screens | Truthful lifecycle state and memory controls |
| Aura | Glass input panel + clear solid plan approval sheet | Action Broker, Passport, Trail, S0–S3 authorization |
| Quick Center | Glass tile grid and sliders | Existing setting registry and permissions |
| Vault | Split-view file explorer on desktop, stacked file browser on phone | User-controlled files, search and export |
| Passport / Trail | High-contrast data tables/lists within glass window chrome | Access denial, revocation, recovery and audit records |
| Settings | Windows-style category sidebar on large screens; simple stacked mobile settings | One settings registry, simple and advanced views |

**Adaptive sizes:** phones at 360–412 logical px; tablet/desktop layouts expand into multiple columns, movable windows, taskbar and snap grouping. The Galaxy F04 must remain responsive with **Clear** mode if glass effects are too costly.

### Navigation (same capabilities, refreshed controls)

Every essential action has a visible button: Back, Home, Spaces, Pages, Search/Ask, notifications and Quick Center. Swipes are shortcuts only. Keyboard, screen reader, switch controls, at least 200% text scaling and high contrast must operate in all three rendering tiers.

### Design source precedence

`AuroraOS Folio.md` remains the historical source for its *functionality and safety behavior*. Where it specifies **opaque paper sheets, the Fold visual, the 118 px Spine, no blur, flat Folio-only colors, or paper-style mockups**, those visual details are **superseded by this section**. Keep named functionality, permission semantics, Aura action plans, and accessibility goals; do not silently blend conflicting visual rules.

## 2A. OpenDesign integration — official GitHub project

**Selected external design workspace:** [OpenDesign (nexu-io/open-design)](https://github.com/nexu-io/open-design). It is an open-source AI design and prototyping workspace that supports **OpenCode** and portable `DESIGN.md` systems. It is **NOT** an operating-system kernel, a compositor, a driver library, or a runtime component that will be embedded in AuroraOS. We use it to produce, inspect, and iterate on the *visual design and interactive prototype* while preserving our independent OS architecture.

**What OpenDesign is responsible for:**

1. Read our version-controlled `DESIGN.md` as the Aurora Glass design contract.
2. Produce an interactive **HTML/CSS/JS prototype** for phone (360–412 logical px), tablet, and desktop sizes, including light/dark, Clear/Soft Glass, and reduced-motion modes.
3. Create consistent UI component specs, tokens, mock data, and reference screenshots for the Aurora Dock, Home/Desk, Spaces, Pages, Index, Glass Corner, Aura Plan sheet, Quick Center, Vault, Passport, Trail, Settings, onboarding, lock screen and native-app examples.
4. Support design iteration with OpenCode while preserving functional semantics from `AuroraOS Folio.md`: risk classes, confirmations, receipts, access grants, reversibility limitations, and accessibility.
5. Export static assets, screenshots, the structured design system, and behavior notes to be used as *references* for our later native AuroraOS compositor/toolkit implementation.

**OpenDesign must NOT be mistaken for the final OS:** its web prototype runs on a host browser or desktop environment. React/HTML/CSS components and browser blur do not automatically execute inside our custom kernel. OpenCode must reimplement or port the approved design using our native UI toolkit. The QEMU OS starts with **Tier 0 Clear** and adds translucency only after measurements permit it.

### Safest Windows + OpenCode workflow

- **Recommended:** install a verified **Windows x64** OpenDesign desktop release from the official [GitHub Releases](https://github.com/nexu-io/open-design/releases) or the official project website. Check release assets and verify publisher/integrity before running it.
- **Connect to OpenCode:** OpenDesign's README documents `od mcp install opencode` after the OpenDesign `od` command is installed. If the command isn't available, use the app's MCP-server settings/snippet rather than guessing command paths. Review the configuration before applying it. This is an integration into the **development agent only**, not into AuroraOS itself.
- **Alternative source route:** follow the upstream [QUICKSTART.md](https://github.com/nexu-io/open-design/blob/main/QUICKSTART.md): Node.js 24, Corepack, the repo-pinned pnpm version, and Windows/WSL2 setup guidance. Example source commands (run **outside** the AuroraOS repo):

  ```powershell
  git clone https://github.com/nexu-io/open-design.git
  cd open-design
  corepack enable
  pnpm install
  pnpm tools-dev run web
  ```

  The repo currently pins pnpm 10.33.2; check its actual `package.json` and QUICKSTART when installing as these can change.
- **Keep repositories separate:** `AuroraOS/` owns OS source, design files and tests; `open-design/` is a separate upstream tool checkout. Do not import its entire codebase into our kernel or add large generated dependencies to the OS source.
- **Version-pin and review:** record the OpenDesign release/commit used, back up `DESIGN.md`, inspect generated code, and verify external package/asset licenses. Do not run unknown scripts from generated mockups with OS-level privileges.

### OpenDesign design deliverable acceptance gates

| Gate | Acceptance test |
|---|---|
| Fidelity | All named screens share `DESIGN.md` tokens and consistent Aurora Glass components, not a random collection of mockups. |
| Responsive | Phone 360/390/412 px and tablet/desktop layouts are readable, with no essential clipped content. |
| Functional prototype | Visible controls navigate; Space switching, Index search, Aura plan editing, confirmation, receipts and settings toggles have believable simulated state. |
| Accessible | Keyboard navigation, visible focus, screen-reader labels, 200% text reflow, reduced motion, high contrast and no gesture-only essentials. |
| Glass fallback | Tier 0 Clear mode preserves the same navigation and actions with **zero backdrop blur**. |
| Safety UX | Aura S2/S3 actions clearly require confirmation and do **not** auto-run in the prototype. |
| Engineering handoff | Source files, token export, screenshots, asset licenses, and a native implementation mapping are provided. |

**First OpenDesign/OpenCode UI task:** build only the Home/Desk, Dock/Spine, Spaces selector and one Aura Plan interaction from `DESIGN.md`. Review in the browser before expanding to every screen. Don't create or flash a firmware image as part of UI prototyping.

## 3. Architecture: from hardware to user experience

```text
    USER: touch · keyboard · voice
                  |
     +------------v----------------------------+
     | Aurora Glass Shell + Native Apps            |
     | Desk, Dock, Spaces, Pages, Vault        |
     +------------+----------------------------+
                  | system APIs / intents
     +------------v----------------------------+
     | Aura service (optional, user-space)     |
     | Speech -> parser/model -> typed plan    |
     +------------+----------------------------+
                  | proposed actions only
     +------------v----------------------------+
     | Action Broker + Passport + Trail        |
     | Validation, permission, confirmation    |
     | Execution receipts and supported undo   |
     +------------+----------------------------+
                  | authorized service calls
     +------------v----------------------------+
     | Core user-space services                |
     | Init, Apps, Files, Spaces, Search,      |
     | Settings, Graphics, Input, Audio, Time  |
     +------------+----------------------------+
                  | system calls and IPC
     +------------v----------------------------+
     | Custom ARM64 Aurora Kernel              |
     | Memory, scheduler, processes, syscall,  |
     | interrupts, protection, driver interface|
     +------------+----------------------------+
                  | hardware-facing drivers
     +------------v----------------------------+
     | QEMU ARM64 virt / verified real device  |
     +-----------------------------------------+
```

**A clarification:** the interface, Aura, Passport, Trail and most system services belong in **user space**. The kernel should remain small and stable. A library being open-source or ARM64-compatible does not mean it already runs on AuroraOS; it may need a C/C++ runtime, threads, files, memory allocation and OS-specific porting.

### Kernel (the foundation)

- ARM64 boot entry and serial output.
- Exceptions, interrupts, timers and CPU modes.
- Physical allocator, virtual memory, MMU and address spaces.
- Processes, threads and a basic scheduler.
- System call boundary, IPC and safe user-memory copying.
- Storage and device interfaces with isolation and diagnostics.
- Later: stronger driver isolation, suspend/resume and hardware-specific control.

**Done means:** The kernel runs repeatably in QEMU, does not secretly use a Linux/Android kernel, and proves memory/process isolation in automated tests.

### Services (the system's workers)

- **Init:** launches system processes and recovers from crashes.
- **Storage/FS:** file persistence and recovery (start with an explicitly tested filesystem design/driver).
- **Process/app manager:** launch, stop, memory-pressure handling, app state.
- **Graphics/input:** framebuffer, compositor, font rendering, keyboard, mouse/touch.
- **Spaces/Page service:** associates content and page restore metadata with each Space.
- **Settings:** one registry feeding Simple and Advanced views.
- **Index/Vault:** local search of apps, objects, files and actions.
- **Passport:** permissions and capability grants enforced by the services.
- **Trail:** file/note versions, reversals, audit records with quotas and retention.
- **Audio/time/network:** added as driver and system API support is built and proven.

### Graphics architecture (updated for Aurora Glass)

The compositor uses **Background -> Windows -> Overlays -> Secure Prompts** with damage tracking. First deliver the **Tier 0 Clear** pipeline using a CPU-rendered framebuffer: fill, text, rounded rect, border, clipping, pointer/keyboard focus and window composition. Add Tier 1 alpha blending and *cached wallpaper blur* only after benchmarking. Tier 2 real-time backdrop blur is an **optional** GPU-accelerated improvement, never a prerequisite for native apps, OS boot, Aura, or accessibility. Recompute only damaged regions, cap layer count, and allocate fixed cache budgets. Sensitive lock/authorization surfaces remain opaque and must never be exposed to untrusted app screenshots or window enumeration.

Initial graphics support requires an explicitly implemented QEMU display driver; QEMU providing virtual GPU options does **not** mean that driver is already written. A safe fallback is text-only serial output until a validated framebuffer path exists.

### Application architecture

Provide a native executable format and stable versioned system APIs. Early apps: Terminal, Notes, Calculator, Clock/Timer, Files, Settings. Later: calendar, gallery, package installer, camera, calling and messaging only once device drivers are real.

App compatibility with Android APKs is **not** automatic. The `.aur` package format, signatures, SDK, install/update/remove flow and application sandbox need their own implementation and testing.

## 4. Offline Aura AI design

Aura is **an alternative input method**, never a required OS dependency and never privileged with direct kernel access.

```text
Voice -> offline speech-to-text ------+
                                      v
Typed text -> Quick Commands -> optional small local language model
                                      |
                         typed and validated action plan
                                      |
                         Action Broker + Passport
                                      |
                      preview / user confirmation
                                      |
                     execute system API requests
                                      |
                  results + Trail + optional local TTS
```

**Three modes from the Folio:**

- **Off:** Entire OS works through touch/keyboard, search remains available.
- **Quick Commands:** Small deterministic command interpreter, e.g. `timer 25`, `open Notes`, `brightness down`. This should be built before the AI model.
- **Aura Lite:** Local small language model can propose multi-step actions. It unloads when memory is low. These memory figures are estimates only.

**Candidate technology:** Evaluate Qwen3-0.6B as a starting model (Apache-2.0 model card), `llama.cpp`-style quantized inference, and `sherpa-onnx`-style offline speech. **First benchmark on the development computer**, then port or replace runtimes once AuroraOS has the required APIs. Fine-tune on a suitable computer/cloud GPU and deploy the quantized model; do not attempt significant model training on the F04.

**Safety model (based on the Folio):**

| Risk | Actions | Approval |
|---|---|---|
| S0 | Open, view, timer | A single safe step may run immediately; log result |
| S1 | Reversible settings | Preview if in a plan; supported undo |
| S2 | Edit, rename, move files | Show affected objects/diff and require tap |
| S3 | Delete, send, install, share | Show precise effect; require deliberate hold, never voice-only approval |

The model may output only a list of known **typed intents** such as `open_object`, `switch_space`, `start_timer`, `set_brightness`. It cannot execute arbitrary shell commands, directly write to devices, alter Passport permissions or approve its own steps. Every intent is checked again when executed. Revoke access so subsequent requests fail closed. Do not promise that everything is undoable: sent messages and externally transmitted data cannot be retrieved.

**Real-world scenario:** “Aura, prepare my phone for studying.” The system can propose `switch Space Study -> quiet notifications -> open Notes -> timer 25`, display the plan, allow editing steps, perform approved actions, and show a receipt. If a step fails, stop and offer repair, skip, stop, or safe rollback.

## 5. Build stages in the correct dependency order

The original Folio supplies an excellent **UI-01 ... UI-19** functional backlog. Keep that functional scope, but replace its visual acceptance tests with the Aurora Glass rules in Section 2. Its graphics assumptions (no alpha/blur) are superseded. The UI backlog still assumes a bootable OS/renderer test harness, so use **two early parallel tracks** and an integration gate.

### Track A — Visual prototype (runs on an existing computer OS)

**A0. Save design source.** Preserve `AuroraOS Folio.md` as the functional/historical UX reference. Put the supplied `DESIGN.md` into the AuroraOS project root as the visual source of truth; keep this blueprint as the architecture source. Pin the official OpenDesign tool release/commit. *Pass:* no conflicting visual token sets and a documented UI precedence rule.

**A1. OpenDesign Aurora Glass UI kit.** Use [OpenDesign](https://github.com/nexu-io/open-design) with OpenCode to generate and refine prototype window surfaces, Aurora Dock, Glass Corner, Horizon, responsive 360/390/412 px layouts, and desktop snap-layout behavior using mock data and `DESIGN.md`. Implement Clear/Soft Glass variants and test high-contrast mode. *Pass:* accessible interactive prototype, reference screenshots, 200% text reflow, keyboard navigation, consistent geometry and tokens.

**A2. OpenDesign end-to-end prototype.** Home -> Index -> Space -> Page -> Vault -> Passport -> Aura Plan sheet -> receipt. All sample data is marked mock. Export HTML/CSS/JS source, screenshots and native handoff notes. *Pass:* no dead controls; usable without AI; Clear mode provides equivalent access to every action, and output can be re-opened without OpenDesign cloud services.

The Track A prototype is **not** the real OS; it is the visual reference we will implement inside our own operating system.

### Track B — Independent ARM64 operating system

**B0. Toolchain and CI.** Linux/WSL2 or Linux build machine, ARM64 cross compiler, linker, build system, QEMU, debugger, automated smoke tests. *Pass:* clean repeatable builds from source.

**B1. Boot.** Minimal custom ARM64 entry, stack, serial UART, panic output. *Pass:* QEMU repeatedly displays `AuroraOS booting` and can exit deterministically.

**B2. Kernel basics.** Exception vectors, timers, memory map/device tree understanding, allocators and MMU. *Pass:* CPU/memory tests, exceptions and boundary checks.

**B3. Processes.** Userspace, scheduler, syscalls, IPC, address-space separation. *Pass:* at least two isolated userspace tasks run, invalid access fails safely.

**B4. Storage and system services.** Init, storage driver, filesystem, program loader, settings, time. *Pass:* a file written by one program survives reboot and is accessible only with authorization.

**B5. Graphics and input.** Implement actual QEMU display/input drivers, Tier 0 Clear renderer/compositor, window shell, keyboard and pointer. *Pass:* the genuine AuroraOS kernel renders an interactive Aurora Glass-layout screen in QEMU, without depending on blur or alpha.

### Merge A + B

**C0. Aurora Glass on the real OS.** Port the UI toolkit and tokens without relying on a host OS. *Pass:* Home/Desk, Glass Corner, Horizon, Aurora Dock, Index and Spaces work within our QEMU OS in Tier 0 Clear mode. After measuring latency and memory, optionally enable Tier 1 Soft Glass.

**C1. Workspace, apps and memory management.** Implement Pages, app lifecycle, state serialization, Notes, Calculator, Clock, Files, Settings. *Pass:* Space switching and supported saved state survive restart. Do not promise snapshot restoration of arbitrary app RAM without implementation.

**C2. Privacy and recovery.** Passport grants, access logs, Trail, recovery, local vault index, PIN/lock. *Pass:* revoked permissions are enforced by services; tested file restoration; onboarding works without internet.

**C3. Aura basic.** Typed Quick Commands, typed intents, Plan sheet and Action Broker. *Pass:* safe multi-step study routine completes/undoes where supported; no AI model needed.

**C4. Aura Lite.** Evaluate and fine-tune model separately, quantize, build a supported inference runtime or port one, evaluate commands and load shedding. *Pass:* model action output is schema-validated, held-out command tests pass and low-memory fallback works.

**C5. Offline voice.** Add QEMU-compatible audio path, local speech-to-text and text-to-speech, visible mic state, optional wake-word later. *Pass:* tested phone commands run with networking disabled and explicit permission prompts.

**C6. Developer platform.** Versioned SDK, signed `.aur` packages, package manager, sandbox and an example third-party app. *Pass:* install, run, revoke, update and uninstall without privilege bypasses.

**C7. Public QEMU beta.** Tested release artifacts for supported host platforms, checksums, clear install/run docs, reproducible CI and transparent known limitations. *Pass:* a new user runs AuroraOS from GitHub without replacing their current OS.

### Separate hardware track — Samsung Galaxy F04

**D0. Feasibility research only.** Verify exact device model/variant, documented boot chain, bootloader unlocking, available firmware/recovery procedures, kernel source/device tree, display/touch/storage/power components, licenses and testability. *Pass:* written **GO / CONDITIONAL GO / NO-GO** decision; no phone changes.

**D1. Physical boot work** (conditional on D0 approval). Work in a separate hardware-port branch with independent review, recovery plan and explicit authorization before any flash. *Pass:* controlled boot with robust recovery, then display/touch/storage/audio/power testing.

**D2. Feature expansion.** Battery/power charging safety, Wi-Fi, Bluetooth, cameras, radio, mobile data, calls, SMS etc. These require board-specific drivers, firmware and sometimes proprietary components. They are not promised by the QEMU release.

## 6. Converting the source Folio's UI issue backlog

Once kernel graphics are available, follow the **functional scope** of the original **UI-01 to UI-19** backlog, with these visual changes:

- **UI-01–05:** Aurora Glass color/spacing/type tokens; Tier 0 rectangles/text/windows; layout and focus; accessible compositor; Tier 1 alpha/cache work **separate and optional**.
- **UI-06–10:** Aurora Dock + Horizon + Glass Corner; Spaces, Aurora Home, Index and Pages with desktop snap-layout option.
- **UI-11–14:** settings registry, glass quick center, lock/onboarding, native apps, Passport and Trail. Permission prompts keep opaque content.
- **UI-15–16:** Quick Commands, Aura Plan sheet with legible confirmations, model and speech integration.
- **UI-17–18:** Vault search/storage, installer/update/recovery UI, adapted to Glass window components.
- **UI-19:** contrast in translucent and opaque modes, 200% scaling, accessible focus, reduced motion, screen readers and switch input.
- **UI-20 (new, optional):** Tier 1 cached/static glass effect benchmarking and dynamic-style safeguards. **Done when** no major lag, text contrast failures or significant idle redraws.
- **UI-21 (new, optional):** Tier 2 live blur on a verified accelerated renderer only, with instant fallback to Tier 0.

**Important:** These two additional UI tasks do not block kernel milestones, basic apps, Aura or GitHub emulator releases.

**The issue numbers and their detailed acceptance tests are preserved in the original Folio.** These are UI work items, not a substitute for kernel/system-service engineering.

## 7. Proposed repository layout

```text
AuroraOS/
  README.md
  LICENSE                  # license to select and approve
  docs/
    AuroraOS_Folio.md      # historical functionality and UX reference
    aurora-glass-spec.md   # current visual source of truth (from §2)
    blueprint.md
    architecture/
    device-compatibility.md
    safety-and-recovery.md
    decisions/
  boot/aarch64/
  kernel/
    arch/aarch64/
    mm/
    irq/
    sched/
    process/
    syscall/
    ipc/
  platforms/
    qemu-virt/
    samsung-f04-research/
  drivers/
    uart/
    timer/
    block/
    display/
    input/
    audio/
    network/
    power/
  userspace/
    init/
    services/
      app-manager/
      space-manager/
      settings/
      vault/
      passport/
      trail/
      action-broker/
  graphics/
    renderer/
    compositor/
    toolkit/
    a11y/
  DESIGN.md                # Aurora Glass tokens and UI contract used by OpenDesign
  aurora-glass/
    tokens/
    shell/
    prototype/             # generated with OpenDesign, runs on host only
    screenshots/
    open-design-handoff/   # exported components/assets/behavior notes
    performance-profiles/
  aura/
    commands/
    planner/
    runtime/
    speech/
    action-schemas/
    datasets/
    eval/
  apps/
    terminal/
    notes/
    files/
    clock/
    calculator/
    settings/
  sdk/
  packages/
  tools/
  tests/
    unit/
    integration/
    qemu/
    security/
    ui-golden/
    offline-ai/
  .github/workflows/
```

The **device research** folder is documentation only until a safe hardware-port plan exists. Keep datasets and model weights out of regular Git history unless distribution licenses and Git LFS/storage implications have been reviewed.

## 8. Development environment and responsibilities

- **Windows laptop:** OpenCode, Git client, terminal, editor, **OpenDesign** (as a separate design tool), the visual prototype, and lightweight tests.
- **Linux environment:** WSL2/Ubuntu or a dedicated Linux machine for ARM64 cross-compilation and QEMU.
- **Cloud development machine, if needed:** longer builds, CI validation, AI-model fine-tuning. Not required for the first serial boot.
- **GitHub:** source control, issues, PRs, CI, releases, build provenance, licensing, contributor documentation.
- **OpenCode:** implements one well-scoped issue at a time; runs targeted tests; opens a PR; reports assumptions and exact proof.
- **Independent reviewer:** reviews failures/security and validates tests before merging, especially hardware-affecting changes.

## 9. Repeatable issue workflow

1. Identify the next unblocked issue; record dependencies and acceptance criteria.
2. Give OpenCode exactly one implementation scope (do not ask it to build everything at once).
3. OpenCode edits code/tests/docs and runs local tests.
4. Save commit, PR, CI links and evidence; do not claim success from just compiling.
5. Review correctness, licensing, risks and untouched unrelated components.
6. Merge only after tests pass and review is satisfactory.
7. Run regression smoke tests on the main branch, then pick the next issue.
8. Keep experimental hardware changes in a separate track; no flashing without an explicit, reviewed decision.

## 10. Security and reliability gates

- All user/kernel memory crossings validated; no direct app access to arbitrary memory or devices.
- Per-app identities and least-privilege file handles/capabilities; deny by default.
- Crash isolation; service restart; storage consistency tests; recoverable system updates.
- Apps cannot bypass Passport by calling a lower-level service directly.
- Aura plans are typed, schema-validated, bounded and logged. High-impact actions require touch confirmation.
- Audit logs and Trail have retention controls; sensitive content is minimized/encrypted as support matures.
- Release builds carry known limitations, versioned compatibility, integrity hashes and signatures where supported.
- Accessibility tests: 200% font reflow, contrast over translucent surfaces, complete Clear-mode functionality, full keyboard operation, Name Lens and semantic controls.
- Explicit proof that all advertised **offline** tasks work with networking disabled.

## 11. Practical milestone checklist

| Milestone | Visible result | Gate |
|---|---|---|
| 0 | Design locked, architecture and development tools | Source reviewed; no phone modification |
| 1 | Real custom kernel prints welcome message in QEMU | Repeatable boot tests |
| 2 | Processes, isolated apps and persistent files | Restart/isolation tests |
| 3 | Aurora Glass home appears in Tier 0 Clear inside our real OS | Visual/keyboard/contrast tests |
| 4 | Spaces, Index, Pages and native core apps | State + low-memory tests |
| 5 | Passport, Vault and Trail | Access/recovery tests |
| 6 | Typed Aura commands and plan approvals | Unsafe actions blocked |
| 7 | Quantized Aura Lite runs locally | Accuracy, RAM, latency, fallback tests |
| 8 | Offline speech commands | No-network voice regression |
| 9 | App packaging, updates, GitHub emulator beta | Fresh user run succeeds |
| 10 | Experimental F04 device research/port | Only if proven feasible and recoverable |

## 12. Critical risks and honest assumptions

- **F04 boot compatibility:** Unverified; even an independent kernel that boots in QEMU may not boot the Samsung device.
- **Proprietary firmware/drivers:** Modem, GPU, power, display and audio could prevent a complete phone port.
- **AI runtime:** Qwen3-0.6B and offline speech are **candidates**, not tested AuroraOS software.
- **4 GB RAM numbers:** Original Folio estimates (including 120 MB graphics and 500–600 MB model) are **not measurements**. Glass UI textures and cached blur consume additional memory; profile working sets and cache eviction separately.
- **QEMU performance:** Software emulation speed on a Windows laptop does not reliably predict execution speed on F04. Dynamic glass is conditional on driver support and battery/thermal measurements; **Clear mode** always remains available.
- **Save/resume:** Apps must serialize state; raw process snapshotting is not promised.
- **Rewind:** Only local supported changes can be undone, not externally transmitted effects.
- **Voice model accuracy:** Different languages need separate measurement and appropriate speech model licensing.
- **Release scope:** 'Anyone can use it' means anyone on **supported systems**, starting with a QEMU package; not universal phone flashing.

## 13. First OpenCode instruction (planning only; OpenDesign included)

> You are the AuroraOS engineering lead. Treat `DESIGN.md` as the canonical Aurora Glass token/component contract, and `AuroraOS_Final_Blueprint_OpenDesign.md` Section 2/2A as the current UX implementation and design-tool plan. Read `AuroraOS Folio.md` for retained functionality, including Aura's plan/permission safety semantics; its old paper styling is superseded. Research integration with the official `https://github.com/nexu-io/open-design` project and inspect the pinned OpenDesign release's current installation instructions. Use **OpenDesign with OpenCode for host-side UI prototyping only**, never as an OS runtime dependency. Our kernel remains independent ARM64; first actual OS target is QEMU `virt`, and Galaxy F04 remains unverified. Work on **Phase 0 only**: inspect the local repository, verify prerequisites, outline kernel/graphics boundaries, establish the separate design prototype track, audit OpenDesign compatibility/install feasibility and generate a dependency-ordered backlog with acceptance criteria. Do not write kernel or UI implementation code in this audit. Do not root, flash, unlock or alter any phone. Do not push or merge without confirming repo identity and authorization. Return ONE next unblocked task.

## 13A. Dedicated OpenDesign prototype instruction (later, when Phase 0 passes)

> Using the **official** OpenDesign (`nexu-io/open-design`) with my existing OpenCode agent, read the local AuroraOS `DESIGN.md`, the updated blueprint, and the historical Folio functionality spec. Build an **interactive host-browser prototype only**: Home/Desk, Dock, Spaces, Index and the Aura command Plan/confirmation/receipt flow. Adopt the Windows-inspired organization and iOS-inspired glass depth **without copying protected UI assets**. Provide Clear/Soft Glass, light/dark, mobile 360/390/412 px, a 200%-text accessible layout, keyboard operation and reduced motion. Source everything from reusable tokens/components. Simulate OS operations with mock state: never imply actual control of phone hardware. Export source, design tokens, screenshots and a mapping to native AuroraOS primitives. Verify every visible control works and leave actual OS source, kernels, boot images and connected devices unchanged.

## 14. Final development principle

**Prototype Aurora Glass with OpenDesign while building the independent QEMU kernel. Then make the kernel run programs, port the approved layout to the native Clear-mode compositor, add essential apps and safe Aura actions, and enable richer glass effects only after benchmarking. Finally research compatible real phones.**

### Technical references (checked 7 Nov 2026)

- OpenDesign canonical GitHub repository: https://github.com/nexu-io/open-design
- OpenDesign README/OpenCode MCP integration: https://github.com/nexu-io/open-design/blob/main/README.md
- OpenDesign source quickstart: https://github.com/nexu-io/open-design/blob/main/QUICKSTART.md

- QEMU ARM `virt` board: https://www.qemu.org/docs/master/system/arm/virt
- QEMU ARM system emulation: https://www.qemu.org/docs/master/system/target-arm.html
- Qwen3-0.6B model: https://huggingface.co/Qwen/Qwen3-0.6B
- `llama.cpp`: https://github.com/ggml-org/llama.cpp
- `sherpa-onnx`: https://github.com/k2-fsa/sherpa-onnx
