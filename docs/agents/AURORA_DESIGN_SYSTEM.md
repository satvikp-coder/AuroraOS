# Aurora Glass design system (implementation guide)

`DESIGN.md` is the contract. This file documents how the prototype implements it, so new screens look and behave
like the existing ones. Source of truth for values: `aurora-glass/prototype/tokens.css` and `components.css`.

## 1. Character

Atmospheric, crisp, calm, precise, tactile, productive. A serious desktop-class OS with soft glass chrome.

Do: one static aurora wallpaper; glass only on chrome; opaque surfaces for anything you read or decide on;
strong type hierarchy; original stroke icons; one accent colour; quiet status text.

Do not: neon, glowing orbs, perpetual animation, gradients on controls, glass under body text, text-only or
emoji "icons", dashboard-style stat tiles, colour as the only signal, fake "working" indicators for hardware.

## 2. Tokens (`tokens.css`)

**Colour.** The nine DESIGN.md tokens are fixed: `canvas, surface-opaque, surface-glass, dock-glass, ink,
ink-muted, accent, aura, focus` (light and dark). Additions:

| Token | Use |
|---|---|
| `--color-accent-fill` / `--color-on-accent` | Filled primary buttons and their text (AA contrast in both themes) |
| `--color-accent-ink` | Accent-coloured **text** and icons on surfaces (never use `--color-accent` for small text) |
| `--color-accent-soft`, `--color-aura-soft` | Tinted backgrounds: selection, hover, info chips |
| `--color-aura-ink` | Aura-coloured text |
| `--color-success / warn / danger` (+ `-soft`) | Status text/icons and their tinted chip backgrounds |
| `--color-danger-fill` | Destructive filled buttons (white text) |
| `--color-surface-sunken`, `--color-surface-hover` | Wells (meters, segmented track) and hover on opaque rows |
| `--color-border`, `--color-border-strong` | Hairlines; control outlines |
| `--color-scrim` | Dialog backdrop |
| `--shadow-1/2/3` | Resting card / raised / dialog |
| `--wallpaper` | The static aurora background (on `body`) |
| `--tile-bg`, `--tile-bg-2`, `--tile-ink` | App tile colours, derived from the element's `--h` hue |

**Type.** `--font-family` (Inter, then system fonts, with Devanagari/Gujarati/CJK fallbacks). Sizes `--text-xs` 12,
`sm` 14, `base` 16, `lg` 18, `xl` 24, `2xl` 30, `3xl` 34, `display` 56. Weights `--weight-regular` 400,
`medium` 550, `bold` 680. Tight tracking on headings only. Always use the tokens: `data-text-scale` (125/150/200)
rescales them, and layouts must reflow rather than clip.

**Space** `--space-1..8` = 4, 8, 12, 16, 24, 32, 48, 64. **Radius** `--radius-sm` 8, `control` 14, `card` 20,
`sheet` 24, `pill`. **Targets** `--touch-target` 48px. **Icons** `--icon-size` 22px. **Layers** `--z-dock` 50,
`corner` 60, `sheet` 200, `dialog` 300, `toast` 400. **Motion** `--motion-fast/base/slow` 120/180/220ms with
`--ease-standard`; one-shot only; zero under reduced motion (`prefers-reduced-motion` or `data-motion="reduce"`).

**Modes.** `<html data-theme="light|dark" data-glass-tier="soft|clear" data-contrast="normal|high"
data-text-scale data-motion>`. Clear and high contrast make every glass surface opaque and remove blur; high
contrast also removes the wallpaper and shadows and thickens borders. Everything must work in Clear.

## 3. Layout

- Content column: max `--content-max` (1120px), centred, `--space-4` gutters (phone) to `--space-6` (desktop).
- Phone 360–412: one column, bottom Dock. Tablet >= 768: two columns. Desktop >= 1024: taskbar Dock with
  running apps and tray, multi-column Home.
- Vertical rhythm: `--space-4` between cards, `--space-3` inside cards, `--space-2` between related controls.
- A screen = `.screen-header` (Back button, `.eyebrow` optional, `h1.screen-title`, `.screen-actions`), optional
  `.screen-intro`, then cards/lists. The header reserves room for the fixed Glass Corner.
- Flex rows use `flex-wrap` and `min-width: 0`; long text uses `overflow-wrap: anywhere`. Never fixed widths
  for text containers. Test at 200% text.

## 4. Components (`components.css`, built with `AuroraUI` in `ui.js`)

| Component | Classes | Notes |
|---|---|---|
| Button | `.btn` + `.accent` (one primary per view) / `.outline` / `.ghost` / `.danger`; `.btn-icon`, `.btn-small`; `.btn-link` | `AuroraUI.button(label, {kind, icon, iconOnly, onclick})`. Icon-only buttons need `aria-label`. Toggle buttons use `aria-pressed`. |
| Hold-to-confirm | `.hold-wrap .hold-btn` | `AuroraUI.holdButton`. Required for S3. Pointer or key hold 1.2s, plus the visible two-step alternative. |
| Icon | `.icon` | `AuroraUI.icon(name)`; names in `AuroraUI.ICONS`. Original 24px stroke glyphs. Decorative (`aria-hidden`). |
| App tile | `.app-tile` (`-sm`, `-lg`) | `AuroraUI.appTile(icon, hue)`. Each app owns one hue. |
| Card | `.card` (glass), `.card.opaque`, `.card-flush` | Glass cards hold chrome and short summaries only. |
| List | `.list > .list-row` with `.row-main (.row-title, .row-meta)`, `.row-trailing` | Opaque. Interactive rows are `<button class="list-row">`. Selected: `aria-selected` or `.is-selected`. |
| Field | `.field`, `.field-group`, `.field-error`, `.search-field` | Labels always visible or `aria-label`led. Errors as text, `aria-invalid`. |
| Switch | `.switch[role=switch][aria-checked]` containing a text span On/Off | State is never colour-only. |
| Segmented | `.segmented > .segment[role=radio]` | `AuroraUI.segmented`. Arrow keys move. |
| Tabs | `.tabs > .tab[role=tab][aria-selected]` | |
| Chip / status | `.chip` + `chip-ok|info|warn|error|muted|aura` | `AuroraUI.chip(text, tone, icon)`. Icon + text. |
| Risk badge | `.risk.risk-s0..s3` | `AuroraUI.riskBadge(risk)`. Always code + words + icon. |
| Dialog | `.scrim > .dialog` (`.dialog-danger`) | `AuroraUI.dialog/confirm/prompt`. Opaque, focus-trapped, Escape cancels, focus returns, safe button focused first. Bottom sheet on phones, centred >= 768. |
| Toast | `#toast.toast` | `AuroraUI.toast(text, {action, tone})`. Optional Undo action. Never the only place an error is shown for a blocking failure. |
| Empty / error | `.empty-state`, `.callout` (`-error`, `-warn`) | `AuroraUI.emptyState(icon, title, body, action)`. Every list has an empty state with a next step. |
| Progress | `.meter > .meter-fill` (`.meter-aura`, `.low`) | Determinate only. `role="meter"` or `progressbar` with values. |
| Plan / receipt | `.plan-steps .plan-step`, `.result-panel`, `.result-list .result-row` | Owned by `review.js`. Reuse the row classes in Trail. |
| Skeleton | `.skeleton` | Static block (no shimmer). |

State rules for every control: default, `:hover`, `:focus-visible` (3px `--color-focus` ring, never removed),
`:active`, `:disabled` (0.5 opacity + `not-allowed`), selected/on (tint + border + text/icon), error (text +
border), loading (disabled + label change such as "Deleting…").

## 5. Signature elements (do not change casually)

- **Aurora Dock**: glass bar, icon + label items, pill highlight + `aria-current` for the current screen.
- **Aurora Horizon**: thin determinate progress rail and status text driven by `AuroraSystem` `status` events.
- **Glass Corner**: fixed top-right handle that opens the held-items popover.
- **Aura**: teal (`--color-aura`) is reserved for Aura/action progress. No orb, no chat bubbles: Aura is a plan
  with numbered steps, risk badges, confirmation and a receipt.
- **App tiles**: 30% radius, two-stop tint from one hue, stroke glyph.
- **Wallpaper**: static layered radial gradients. No images, no animation.
- **Secure surfaces**: review, confirmation, permission and delete dialogs are opaque with a strong border.

## 6. Accessibility requirements

Keyboard path for everything; Escape goes back/closes; focus moves to the new screen heading or first control
and is preserved across re-renders (`AuroraUI.keepFocus` + `data-fk`). Accessible names on all controls; live
announcements via `AuroraUI.announce`. Contrast AA for text in every theme. 48px targets, 8px between critical
actions. 200% text reflows with no clipping or horizontal scroll at 360px. Reduced motion removes transitions.
Status always has text or an icon in addition to colour. No gesture without a visible control equivalent.
Strings are plain text in JS today; keep them out of layout decisions so they can be localised (en, hi, gu).

## 7. Reference screenshots

`aurora-glass/screenshots/` holds a small curated set of 22 images (regenerate with `AURORA_SHOTS=1 npx playwright test
--workers=1 tests/ui-golden/curated-shots.test.js`; the spec is skipped otherwise). Routine test runs write screenshots to `test-results/`, which is ignored.
