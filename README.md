# AuroraOS

An independent, open-source ARM64 operating system with the Aurora Glass UI and offline Aura AI assistant.

## Status

**Design prototype only.** This repository currently contains the Aurora Glass visual design system and an interactive browser-based prototype. The independent ARM64 kernel and native OS are planned but not yet implemented.

**Not an Android ROM, not a Linux distribution, not a launcher.** AuroraOS is a ground-up ARM64 operating system with its own kernel, system calls, UI toolkit, and offline assistant (Aura).

## What exists now

- `DESIGN.md` — Canonical Aurora Glass visual token and component contract (v1.2)
- `AuroraOS_Final_Blueprint_OpenDesign.md` — Architecture and build plan
- `AuroraOS_OpenCode_OpenDesign_Prompt.md` — OpenCode task prompt for OpenDesign integration
- `design-systems/aurora-glass/` — Portable OpenDesign design-system package (manifest, DESIGN.md, tokens.css, USAGE.md)
- `aurora-glass/prototype/` — Interactive browser-only HTML/CSS/JS prototype (host only, not an OS runtime)
  - Home / Desk, Aurora Dock, Spaces selector, Index search
  - Aura Plan sheet with numbered editable steps, confirmation, failure, and receipt
  - Clear (Tier 0 opaque) and Soft Glass (Tier 1 translucent) variants
  - Light and dark themes, 360/390/412px mobile previews
  - Keyboard navigation, 200% text reflow, high contrast, reduced motion
- `aurora-glass/tokens/tokens.json` — Machine-readable design token export
- `aurora-glass/open-design-handoff/native-compositor-mapping.md` — Component-to-native-compositor mapping
- `tests/ui-golden/aurora-glass.test.js` — 81 automated jsdom validation tests (all passing)

## What is planned

- Custom ARM64 kernel (QEMU ARM64 `virt` first target)
- Native graphics compositor (Tier 0 Clear, then optional Soft/Dynamic Glass)
- Native Aurora Glass UI ported from the browser prototype
- Core apps: Notes, Files, Calculator, Clock, Settings
- Aura offline AI assistant with Quick Commands and safe action plans (S0–S3)
- Passport permissions, Trail audit, Vault file management

## Safety

The Samsung Galaxy F04 is an experimental research target only. No physical device operations are performed in this repository. All low-level development targets QEMU ARM64 `virt`.

## License

Apache-2.0. See `LICENSE`.
