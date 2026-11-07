# AuroraOS — Aurora Glass Design System

**Status:** UI concept and prototype design contract (not implemented in the native OS)  
**Version:** 1.2 · 2026-11-07  
**Primary tool:** OpenDesign, https://github.com/nexu-io/open-design  
**Development agent:** OpenCode  
**Engineering blueprint:** `AuroraOS_Final_Blueprint_OpenDesign.md`

## 1. Mission

Create a distinctive and attractive AI-native user experience that combines **Windows-inspired productivity, taskbar organization and window management** with **iOS-inspired soft, tactile glass surfaces**, without cloning vendor screens or using protected assets. Our own design is **Aurora Glass**. It must look polished, calm, useful and fast on phones. The complete OS functions with Aura off. A small custom ARM64 kernel will render the native version later; the OpenDesign HTML prototype is a **design-only host app**.

## 2. Visual mood

- **Keywords:** crisp, atmospheric, legible, layered, understated, productive, tactile, modern.
- **Avoid:** neon sci-fi grids, purple glowing AI orbs, massive blur layers, glass over text, uncontrolled animation, copied iOS Control Center, copied Windows Start menu.
- **Key signature:** Aurora Dock with integrated Ask/Search, Spaces as organized work contexts, a small Aurora Horizon progress rail, and a restrained Glass Corner inbox handle.
- **Use original assets:** icons, illustrations and wallpaper must be original or properly licensed. Never use Apple/Microsoft logos, icon libraries or branded illustrations without appropriate permission.

## 3. Design tokens

| Token | Light | Dark | Notes |
|---|---|---|---|
| `canvas` | `#EEF3F8` | `#0B1020` | Background and wallpaper base |
| `surface-opaque` | `#FFFFFF` | `#192338` | Critical, content-heavy, secure prompts |
| `surface-glass` | `rgba(255,255,255,0.74)` | `rgba(24,32,49,0.76)` | Noncritical chrome only |
| `dock-glass` | `rgba(240,246,255,0.80)` | `rgba(18,27,43,0.82)` | Primary bottom navigation |
| `ink` | `#142136` | `#F3F7FF` | Primary text |
| `ink-muted` | `#52627A` | `#B4C2D8` | Secondary text |
| `accent` | `#3478F6` | `#6AA3FF` | Selection, primary interaction |
| `aura` | `#00AEB8` | `#46D6DF` | Assistant/action progress |
| `focus` | `#175CD3` | `#A8D7FF` | Keyboard focus |

- **Font:** freely licensed clean sans font with multilingual fallbacks; no proprietary system fonts required.
- **Type sizes:** 14–17 px for routine text, 28–34 px for screen headlines; text reflows up to 200%.
- **Corner radii:** windows 16–24 px; controls 12–18 px.
- **Touch target:** minimum 48 × 48 logical px; spacing at least 8 px between adjacent critical actions.
- **Spacing scale:** 4, 8, 12, 16, 24, 32 px.
- **Motion:** 120–220 ms one-shot transitions, no continuous animations. Respect reduced motion.
- **Secure surfaces:** permission dialogs, deletion confirmation, PIN, emergency and recovery UI must use opaque/high-contrast panels.

## 4. Three appearance tiers

1. **Clear (Tier 0):** Opaque, well-designed panels. No alpha blending or blur. Default for native QEMU OS, low-end devices, high-contrast mode, low battery, or missing GPU drivers. Every action works.
2. **Soft Glass (Tier 1):** Alpha blending over **static pre-blurred wallpaper** only. Avoid live background blur. Enabled only after measured compositing budgets allow it.
3. **Dynamic Glass (Tier 2):** Optional accelerated live backdrop effects on verified devices; never required by navigation, content or safe actions.

OpenDesign's browser mockups should visually support Clear and Soft Glass. The native compositor implementation begins at Clear.

## 5. Responsive layout rules

**Phone 360–412 logical px:** one-column Home/Desk, bottom Dock, modal Plan sheet with room to review all steps, readable Pages list. One-handed controls near the bottom.  
**Tablet:** 2-column workspace and split Vault, Spaces sidebar when room allows.  
**Desktop:** Windows-inspired taskbar-style Dock with running indicators, snapping/tiling, resizable windows, keyboard-first focus. This is a future UI target, not evidence of x86 hardware support.

## 6. Components to design

- **Aurora Dock:** Back, Home, Spaces, Pages, Ask/Search microphone. On desktop becomes taskbar + tray.
- **Aurora Home/Desk:** Continue, pinned tools, current Space, contextual updates; classic app view optional.
- **Spaces:** Study, Work, Projects, Personal. Each holds content, pinned tools and permissioned settings.
- **Pages:** transparent about Running / Paused / Saved states; memory pressure state.
- **Glass Corner:** held inbox/notifications indicator, accessible by tap/keyboard.
- **Aurora Horizon:** progress/status rail; no perpetual movement.
- **Aura Plan:** voice transcript, numbered editable steps, risk markers, confirmation, running status, receipt, failure/undo; never a generic chat bubble or glowing orb.
- **Index:** local search over apps, files/objects, settings, system actions.
- **Vault:** user files, export, local index, recoverable items.
- **Passport:** permissions, access log, per-action approval.
- **Trail:** version history, recovery and local change receipts.
- **Quick Center:** brightness/volume/network/battery/mic indicators with honest unsupported states.
- **Core app archetypes:** Notes, Files, Calculator, Clock, Settings, Calendar; Camera and Phone are hardware-dependent mockups only.

## 7. Aura interaction rules

- Aura lives in the Ask/Search experience; optional, offline-first, and does not obstruct normal use.
- Quick Commands remain usable without the language model.
- For multi-step plans, show recognized request and numbered editable actions *before* execution.
- All app and system operations go through Action Broker / Passport; no arbitrary commands, kernel privileges or silent permission changes.
- S0 simple view/open/timer: one safe step may run immediately; S1 reversible settings: plan preview; S2 file modifications: explicit review; S3 delete/send/install/share: tactile hold confirmation with no voice-only approval.
- Show successful steps, partial failures, undo limitations and what was changed. Model memory, voice listening and data access are user-controlled.
- Prototype operations are simulated only; they never affect a real device.

## 8. Accessibility and internationalization

A visible equivalent for every gesture. Focus states, logical tab sequence, semantic labels, screen-reader tree, keyboard control, switch-input accessibility, 200% text reflow, high contrast, reduced motion, large touch targets. Use text/icon status alongside color. Language resources should be separated from UI layout to support English, Hindi and Gujarati in future. Do not claim offline speech availability until models have been verified.

## 9. Prototyping tasks in OpenDesign

Build one shared component system with stateful navigation and honest mock data. First 4 screens: Home/Desk, Spaces, Index, Aura Plan + confirmation/receipt. Then Pages, Quick Center, Vault, Passport, Trail, Settings, lock/onboarding, Notes/Files/Clock/Calculator.

**Deliverables:** HTML/CSS/JS source, design tokens, screenshots at 360/390/412 px, light/dark and Clear/Soft Glass, accessibility checklist, interaction notes, and native compositor mapping. Use local assets with attribution; don't require external network to demonstrate core navigation. All generated files must remain editable and stored in the project.

**Definition of done:** no dead-end buttons; all important actions reachable without AI; clear mode fully functional; keyboard-only passes; safe action confirmation flows; coherent typography/colors; no large text clipping; no fake working cellular/camera/network capability.

## 10. Source precedence

For visuals, this `DESIGN.md` wins over the original paper-style `AuroraOS Folio.md`. For OS feature semantics and Aura risk/permission behavior, retain the Folio and the engineering blueprint. For implementation feasibility, the engineering blueprint governs. Never turn this browser prototype into the OS kernel or claim it is native AuroraOS.
