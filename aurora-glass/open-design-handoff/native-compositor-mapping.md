# Aurora Glass: browser prototype to native compositor mapping

**Source of truth for visuals:** `DESIGN.md` v1.2 and `aurora-glass/prototype/tokens.css`.
**Prototype:** `aurora-glass/prototype/` (host-browser HTML/CSS/JS, simulation only).
**Target:** a future native AuroraOS compositor and UI toolkit, **Tier 0 Clear first**, on QEMU ARM64 `virt` only.
**Last reconciled with the prototype:** after M8 (A to H). `tests/unit/handoff-docs.test.js` fails if this file names a class, token or colour the prototype does not have.

> This is a **requirements reference**, not a description of anything that exists natively. **Native graphics is at the first-pixels stage, on QEMU only (issue #7, stages G2 and G3): a ramfb framebuffer with clear, filled rectangles, lines, rounded rectangles and borders, opaque and not anti-aliased. There is no text rendering, no input stack, no window composition and no UI toolkit.** The kernel has a UART console, memory
> management, processes, virtio-blk, AuroraFS and a settings registry; it has no font renderer, no input stack and no compositor, and the native Aurora Glass UI (issue #8) is not started. Nothing
> in the prototype may be embedded in the kernel or compositor, and a browser feature must never be described as a native one.

## 1. How to read this

Prototype code falls into two classes:

- **Portable logic (spec):** DOM-free modules that load in Node and define behaviour a native implementation must reproduce: `core.js`, `settings.js`, `broker.js`,
  `calc-engine.js`, `notes-model.js`, `clock-model.js`, `vault-model.js`, `passport-model.js`, `trail-model.js`, `index-model.js`, `contextmenu-model.js`,
  `strings-model.js`, `strings-catalog.js`. Their unit tests (`tests/unit/`) are the executable specification: port the tests, not just the code.
- **Browser-only presentation:** everything that builds DOM, uses CSS, `localStorage`, `sessionStorage`, `fetch`, `matchMedia`, `backdrop-filter`, or query strings. It shows
  what the result must look like and how it must behave, and is never ported line by line.

Every row below says what the native side must provide. **Native status is "not started" for every UI row** unless a row says otherwise.

## 2. Rendering tiers

| Tier | Prototype | Native requirement | Priority |
|---|---|---|---|
| Clear (Tier 0) | `data-glass-tier="clear"`: glass tokens become the opaque surface, `--backdrop-blur` is `none` | CPU framebuffer pipeline: fill, rounded rect, 1 px border, text, clipping, damage rectangles. No alpha blending, no blur. **Everything in this document must work in Clear.** | **Mandatory, first graphics deliverable** |
| Soft Glass (Tier 1) | `data-glass-tier="soft"` (the prototype's default): translucent chrome over the static `--wallpaper`, **plus a live CSS `blur(14px) saturate(1.15)` on cards, the dock and the Glass Corner** | Alpha blending of chrome over a **pre-rendered** wallpaper. **Do not port the live blur.** Add only after frame time and memory are measured on the real compositor. | Optional, post-benchmark |
| Dynamic Glass (Tier 2) | not in the prototype | Live backdrop blur on verified GPU hardware, bounded to small surfaces, with instant fallback to Tier 0. | Optional, never required |

High contrast (`data-contrast="high"`) forces opaque surfaces and `--border-width` of 2 px in every tier. **What was measured:** in headless software rendering Clear and Soft
scrolled at the same frame interval (`docs/agents/AURORA_UI_PERF.md`). That says nothing about GPU or native cost, which is unmeasured.

## 3. Primitives the compositor must provide

Derived from what the prototype actually draws and does. Only the first two rows exist natively, and only on QEMU: solid fill/clear (stage G2) and rounded rectangle with border (stage G3, opaque, not anti-aliased, `docs/kernel/native-graphics-plan.md` sections 10 and 11). Nothing else here exists natively yet.

| Primitive | Needed by | Detail |
|---|---|---|
| Solid fill and clear | every surface | opaque colours from section 5 |
| Rounded rectangle with border | cards, dialogs, buttons, menus, chips | radii in section 6; border 1 px (2 px in high contrast); anti-aliased corners are a quality item, not a blocker |
| Text drawing | everything | one Latin family at the sizes in section 6; weights regular and bold; wrapping at word boundaries; **200% scale must reflow without clipping**; right-to-left and Indic shaping are **not available** (section 9) |
| Clipping and scrolling regions | `#content`, lists, dialogs, menus | nested clip rectangles; a scrolling region under a fixed opaque band (the Glass Corner band, `--corner-band`) |
| Layered surfaces with a fixed z-order | whole shell | content, then dock, then Glass Corner, then sheets and menus, then dialogs, then toasts (`--z-content`, `--z-dock`, `--z-corner`, `--z-sheet`, `--z-dialog`, `--z-toast`) |
| Hit testing and focus | every control | one focus owner; a visible focus ring (3 px, `--color-focus`); a modal layer that traps focus and restores it to the opener on close |
| Keyboard and pointer/touch input | every control | every action reachable by keyboard; touch long-press (500 ms, cancelled by more than 10 px of movement) and right-click both open context menus; no gesture-only essentials |
| Monotonic timers | Clock, hold-to-confirm, autosave | a monotonic time source (never a count of ticks); timers must survive a stalled frame loop |
| A trusted, opaque surface | review and hold-to-confirm dialogs | drawn by the compositor or a privileged service, never by an app (section 7) |

## 4. Component mapping

Selectors below are real (checked by the drift test). "Spec" is the portable module whose unit tests define behaviour.

| Component | Prototype | Native requirement | Spec | Native status |
|---|---|---|---|---|
| Canvas and wallpaper | `body`, `--color-canvas`, `--wallpaper` | full-screen clear plus a static, pre-composed wallpaper layer, opaque in every tier | `tokens.css` | not started |
| Aurora Dock | `.dock`, `.dock-btn`, `.dock-label`, `#dock-apps`, `#dock-tray`, `--dock-height` | persistent bottom surface: Back, Home, Spaces, Pages, Ask, then running apps and a clock tray; 48 px targets (the tests require at least 44 px); labels are hidden at 150 percent text and above, buttons keep accessible names | `app.js` (shell contract) | not started |
| Home (Desk) | `.card`, `.home-continue`, `.pin-btn`, `.space-chip` | clock, Ask pill, a Continue list that mirrors the real Pages model (running first, then paused, never saved), pinned apps, Space card | `core.js` | not started |
| Spaces | `.space-card-item`, `.space-chip` | four built-in Spaces plus user Spaces; per-Space pinned tools and open Pages; validated names | `app.js` | not started |
| Pages | `#screen-pages`, `.page-card` | a view of real processes (the kernel has them); the prototype's memory figure is simulated and must become real accounting | `core.js` | kernel processes exist, no UI |
| Index | `#index-query`, `#index-results`, `.index-result` | local launcher: apps, files, settings, actions, Aura commands; keyboard combobox (Up, Down, Home, End, Enter); ranking is exact, prefix, word prefix, substring, keyword, then a typo suggestion that **is never the default and never runs on Enter** | `index-model.js` | not started |
| Glass Corner | `.glass-corner`, `.gc-panel`, `--corner-band` | top-right notification handle that opens a held-items panel; an opaque band appears behind it once content scrolls so nothing hides under it | `app.js` | not started |
| Aurora Horizon | `.horizon`, `.horizon-dot`, `#dock-horizon` | thin status rail, no perpetual motion; one status source | `app.js` | not started |
| Aura plan and review | `#screen-aura`, `#review-dialog` | opaque, high-contrast plan sheet: numbered editable steps with S0 to S3 risk labels; approval bound to the exact plan; permissions re-checked just before running | `broker.js`, `system.js` | broker is **mock only**; native broker must be a privileged service |
| Dialogs | `.scrim`, `.dialog`, `.dialog-danger` | modal, opaque, focus-trapped, Escape cancels, focus returns to the opener; the shortcut sheet and welcome tour are ordinary dialogs | `ui.js` | not started |
| Hold-to-confirm | `.hold-btn`, `.hold-fill`, `.hold-alt` | S3 actions need a 1.2 s press-and-hold, with a keyboard hold and a visible two-step alternative; a plain click does nothing | `broker.js` | not started |
| Context menu | `.ctx-layer`, `.ctx-menu`, `.ctx-item` | opens from right-click, Shift+F10 or the Menu key, or a 500 ms long press; placement flips then clamps inside an 8 px margin and scrolls if taller than the screen; opaque; **a launcher only: an item may call its owner's function but can never approve, skip a review or satisfy a hold** | `contextmenu-model.js` | not started |
| Desktop window card | `#window-bar`, `.is-window` | at 1024 px and wider an app screen is a centred card (at most 1120 px) with a title bar and a left Close that is the same as Back; **not** a window manager: no drag, no resize | `app.js` | not started |
| Toast | `.toast` | transient message layer above dialogs | `ui.js` | not started |
| Notes | `#screen-notes`, `.note-item` | list plus editor; autosave after 600 ms; limits of 200 notes, 120 characters of title, 20,000 characters of body; one-level undo; corrupt data is backed up, never discarded; **needs text input, selection and clipboard, none of which exist natively**; persistence moves from `localStorage` to AuroraFS | `notes-model.js` | not started; AuroraFS exists |
| Calculator | `#screen-calculator`, `.calc-key` | immediate-execution arithmetic (no operator precedence: 9 - 4 / 2 gives 2.5); error display; history | `calc-engine.js` | not started |
| Clock | `#screen-clock`, `.timer-inputs` | clock, stopwatch and countdown timer computed from a monotonic clock, never from ticks; one ticking timer; completion shown as an alert, no sound or system notification | `clock-model.js` | not started |
| Vault (Files) | `#screen-vault`, `.vault-item`, `.vault-open` | file browser over AuroraFS: sort, filter, multi-select, preview; **Move is reviewed (S2) and Delete needs the hold (S3)**, both through the broker; long lists skip off-screen rows | `vault-model.js` | not started; AuroraFS exists |
| Passport | `#screen-passport` | per-app permission list; S3 can never be Allow; every change is logged | `passport-model.js` | not started |
| Trail | `#screen-trail` | every row is a real broker receipt; Undo and Restore report the broker's real result | `trail-model.js` | not started |
| Settings | `#screen-settings` | one registry, simple and advanced views; each entry is live, stored-only or locked | `settings.js` | **kernel registry exists** (`docs/kernel/settings.md`); no UI |
| Welcome and shortcut sheet | `#welcome-dialog`, `#shortcuts-dialog`, `.kbd` | first-run tour (skippable, never over another dialog) and a table of only the shortcuts that exist | n/a | not started |
| Text and languages | `AuroraStrings`, `data-i18n` | message catalogs with `{name}` parameters and plurals; fallback to English; **only English is verified; Hindi and Gujarati are empty placeholders** | `strings-model.js` | not started |

## 5. Tokens

Colours (checked against `tokens.css`):

| Token | Light | Dark | Native usage |
|---|---|---|---|
| canvas | #EEF3F8 | #0B1020 | background clear |
| surface-opaque | #FFFFFF | #192338 | secure prompts, content, every surface in Clear |
| surface-glass | rgba(255,255,255,0.74) | rgba(24,32,49,0.76) | chrome, Tier 1 only |
| dock-glass | rgba(240,246,255,0.80) | rgba(18,27,43,0.82) | dock surface, Tier 1 only |
| ink | #142136 | #F3F7FF | primary text |
| ink-muted | #52627A | #B4C2D8 | secondary text |
| accent | #3478F6 | #6AA3FF | primary interaction |
| aura | #00AEB8 | #46D6DF | AI and status progress |
| focus | #175CD3 | #A8D7FF | keyboard focus ring |
| danger | #C62828 | #FF8A8A | destructive actions and errors (also `--color-danger-fill` for filled buttons) |

Also defined in `tokens.css`: `--color-border`, `--color-border-strong`, `--color-scrim`, success and warn colours, and the three shadows (`--shadow-1` to `--shadow-3`, which become a
2 px outline in high contrast). **Colour is never the only signal**: status always has an icon and text as well.

## 6. Geometry, type and spacing

| Item | Prototype token | Native target |
|---|---|---|
| Control radius | `--radius-control` 14 px | button and control radius |
| Card and window radius | `--radius-card` 20 px (`--radius-window` is an alias) | surface radius |
| Sheet radius | `--radius-sheet` 24 px | dialogs |
| Small radius | `--radius-sm` 8 px | chips, menu items |
| Border | `--border-width` 1 px (2 px in high contrast) | stroke |
| Touch target | `--touch-target` 48 px | minimum hit area (tests enforce at least 44 px) |
| Spacing | `--space-1` to `--space-8`: 4, 8, 12, 16, 24, 32, 48, 64 px | layout grid |
| Type scale | `--text-xs` 12, `--text-sm` 14, `--text-base` 16, `--text-lg` 18, `--text-xl` 24, `--text-2xl` 30, `--text-3xl` 34, `--text-display` 56 px | text sizes; scales of 125, 150 and 200 percent multiply them |
| Dock height | `--dock-height` 76 px | bottom surface |

## 7. Security and trust boundaries

- The prototype is **simulation only**. It changes a browser tab, never a computer or phone, and the broker in it is JavaScript in one browser context: **not a security boundary**.
- Native: every app and system operation goes through the Action Broker and Passport, which must be a privileged OS service. No arbitrary commands, no silent permission changes.
- **Trusted surfaces** (review dialog, hold-to-confirm, anything that authorises) must be drawn by the compositor or a privileged service, opaque, never covered by an app, and excluded from app screenshots and window enumeration.
- Only the review surface approves or runs anything. Screens **propose**. **A menu, a shortcut or a typo suggestion never authorises or runs a risky action.**
- Risk classes: S0 runs, S1 previews, S2 needs explicit review, S3 needs the hold. S3 permissions can never be pre-approved. Approvals are bound to the exact plan and permissions are re-checked immediately before running.
- Typed, schema-validated, bounded and logged Aura plans (at most 12 steps per review).

## 8. Motion and accessibility

| Item | Prototype | Native target |
|---|---|---|
| Transitions | `--motion-fast` 120 ms, `--motion-base` 180 ms, `--motion-slow` 220 ms, one-shot | bounded by the frame budget; no continuous animation |
| Reduced motion | all three become 0 ms | honour a reduced-motion setting |

Native requirements, all enforced by the prototype's tests and therefore testable natively: visible focus on every control; tab order that matches visual order; a semantic name for
every control; 200 percent text without clipping or loss of function; high contrast with opaque surfaces; reduced motion; every gesture has a key or button equivalent; no overflow
at 360, 390, 412, 768, 1024, 1280 and 1440 px wide.

## 9. Known gaps and risks (do not hide these)

- **No native text stack.** No font renderer, no layout, no input method, no clipboard. Hindi and Gujarati would also need script shaping and suitable fonts; the prototype only proves the
  string plumbing, with English as the only verified language.
- **No native input stack** (keyboard, pointer, touch) and no focus model.
- **Performance is unmeasured natively.** Prototype timings (`docs/agents/AURORA_UI_PERF.md`) are headless-browser numbers on one laptop and must not be quoted as OS performance. The one real
  finding there (list rebuild cost growing with item count) tells the native toolkit to update lists incrementally.
- **The broker is a mock.** Its contract and tests are the spec; its trust model is not.
- **Soft Glass cost on real hardware is unknown.**

## 10. What must NOT be ported

- No React, browser DOM, CSS engine or JavaScript runtime in the native OS.
- No `backdrop-filter` or live blur as a required rendering path.
- No dependency on host-browser APIs (`localStorage`, `sessionStorage`, `fetch`, `matchMedia`, query strings) in native code.
- No copying of Apple or Microsoft UI assets, icons or branded illustrations.
- No claim that a browser-prototype feature exists in AuroraOS.

## 11. OpenDesign provenance

- Official OpenDesign repo: https://github.com/nexu-io/open-design
- Pinned commit used for this prototype: `87a4f91c30` ([OPEND-4712] fix(desktop): refresh macOS window rendering after display and wake changes)
- `od` CLI built from `apps/daemon/bin/od.mjs` (daemon v0.25.1)
