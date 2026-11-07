# Aurora Glass — Native Compositor Handoff Mapping

**Source:** DESIGN.md v1.2 (AuroraOS visual contract)  
**Prototype:** `aurora-glass/prototype/` (host-browser HTML/CSS/JS, design-only)  
**Target:** Future native AuroraOS compositor (Tier 0 Clear first, QEMU ARM64 virt)

> This mapping is a reference for porting the browser prototype to the native
> AuroraOS UI toolkit. It is NOT a runtime dependency: the prototype must never
> be embedded in the kernel or compositor.

## Rendering tiers (native mapping)

| Prototype tier | CSS | Native compositor equivalent | Priority |
|---|---|---|---|
| Clear (Tier 0) | `data-glass-tier="clear"` — opaque panels, no blur | CPU framebuffer pipeline: fill, rounded rect, border, text, clipping. No alpha blending, no backdrop blur. | **Mandatory — first graphics deliverable** |
| Soft Glass (Tier 1) | `data-glass-tier="soft"` — alpha over static blurred wallpaper | Alpha blending over cached/static pre-blurred wallpaper; no live backdrop blur. Only after frame-time/memory budget measured. | Optional, post-benchmark |
| Dynamic Glass (Tier 2) | (not required by prototype) | Hardware-accelerated live backdrop blur on verified GPU, bounded to small surfaces. Instant fallback to Tier 0. | Optional, never required |

## Component → native primitive mapping

| Prototype component | HTML/CSS element | Native compositor primitive | Notes |
|---|---|---|---|
| Canvas background | `body` bg `--color-canvas` | Fullscreen clear color / wallpaper layer | Opaque in all tiers |
| Aurora Dock (bottom nav) | `nav.dock` with `--glass-dock` | Dock surface layer; opaque in Tier 0, alpha-blended in Tier 1 | Persistent; on desktop becomes taskbar with running indicators |
| Dock buttons | `button.dock-btn` | Touch/keyboard buttons, 48×48 min, border + focus ring | Back · Home · Spaces · Pages · Ask/Search |
| Home / Desk card | `.card` with `--glass-surface` | Rounded window surface, 20px radius, 1px border | Content surface |
| Continue / pinned items | `.continue-item`, `.pin-btn` | List rows + grid buttons, accessible labels | Reachable without AI |
| Space selector / chip | `.space-card-item`, `.space-chip` | Workspace toggle with accent color | Per-Space pinned tools + notification prefs |
| Index search | `<input type="search">` + results list | Local search launcher; keyboard-navigable | Apps, objects, settings, actions |
| Glass Corner | `.glass-corner` (top-right) | Notification handle, tap/keyboard accessible | Held inbox indicator |
| Aurora Horizon | `.horizon` with `.horizon-dot` | Thin progress/status rail; no perpetual motion | Aura listening/transcription progress |
| Aura Plan sheet | `#screen-aura` modal panel | Opaque high-contrast plan approval sheet | Numbered editable steps, S0–S3 risk labels |
| Confirmation / receipt / failure panels | `.confirm-panel`, `.receipt-panel`, `.failure-panel` | Secure prompt surfaces — opaque, never translucent | S2/S3 require explicit confirmation |
| Focus ring | `:focus-visible` → `--od-focus-ring` | Keyboard focus indicator, 3px ring | Visible in all modes |

## Tokens → native values

| Token | Light | Dark | Native usage |
|---|---|---|---|
| canvas | #EEF3F8 | #0B1020 | Background clear |
| surface-opaque | #FFFFFF | #192338 | Secure prompts, content |
| surface-glass | rgba(255,255,255,0.74) | rgba(24,32,49,0.76) | Chrome only, Tier 1 |
| dock-glass | rgba(240,246,255,0.80) | rgba(18,27,43,0.82) | Dock surface |
| ink | #142136 | #F3F7FF | Primary text |
| ink-muted | #52627A | #B4C2D8 | Secondary text |
| accent | #3478F6 | #6AA3FF | Primary interaction |
| aura | #00AEB8 | #46D6DF | AI/status progress |
| focus | #175CD3 | #A8D7FF | Keyboard focus |

## Geometry → native values

| Item | Prototype | Native target |
|---|---|---|
| Window corner radius | 16–24 px (use 20 px) | Compositor rounded-rect radius |
| Control corner radius | 12–18 px (use 14 px) | Button / control radius |
| Border width | 1 px | 1 px stroke |
| Touch target | 48 × 48 logical px | Minimum hit-test area |
| Spacing | 4, 8, 12, 16, 24, 32 px | Layout grid unit |

## Motion → native values

| Item | Prototype | Native target |
|---|---|---|
| Transition duration | 120–220 ms one-shot | Compositor frame budget; no continuous animation |
| Reduced motion | 0 ms | Respect `prefers-reduced-motion` equivalent |
| Focus reveal | 3 px ring | Keyboard focus indication |

## Accessibility → native requirements

- Visible focus indicator on every interactive control
- Logical tab order matching visual order
- Semantic names for all controls (screen-reader tree)
- 200% text reflow without clipping or loss of function
- High-contrast mode: opaque surfaces, maximum-contrast text
- Reduced motion: disable nonessential transitions
- No gesture-only essentials (every gesture has a button/key equivalent)
- Color never used alone to convey status (icon + text + color)

## Security boundaries (prototype vs native)

- Prototype operations are simulated; they never affect a real device.
- Native: every app/system operation goes through Action Broker / Passport; no arbitrary commands, kernel privileges, or silent permission changes.
- Sensitive lock/authorization surfaces remain opaque and must never be exposed to untrusted app screenshots or window enumeration.
- Aura plans are typed, schema-validated, bounded, and logged. High-impact actions require touch confirmation.

## What must NOT be ported

- No React or browser DOM APIs in the native OS.
- No `backdrop-filter` / live CSS blur as a required rendering path.
- No dependency on host-browser APIs (localStorage, fetch, etc.) in native code.
- No copying of Apple/Microsoft UI assets, icons, or branded illustrations.

## OpenDesign provenance

- Official OpenDesign repo: https://github.com/nexu-io/open-design
- Pinned commit used for this prototype: `87a4f91c30` ([OPEND-4712] fix(desktop): refresh macOS window rendering after display and wake changes)
- `od` CLI built from `apps/daemon/bin/od.mjs` (daemon v0.25.1)
- OpenCode MCP integration: `od mcp install opencode` → `C:\Users\satvi\.config\opencode\opencode.json`
