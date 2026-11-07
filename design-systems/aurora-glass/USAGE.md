# Aurora Glass — usage

Apply this design system to an OpenDesign project to generate the AuroraOS
Aurora Glass UI prototype. Tokens and visual rules are sourced from
`DESIGN.md` (canonical Aurora Glass visual contract, v1.2, 2026-11-07).

## Appearance tiers
- **Clear (Tier 0):** `data-glass-tier="clear"` — fully opaque, no blur. Default
  for the native QEMU compositor and low-end devices.
- **Soft Glass (Tier 1):** `data-glass-tier="soft"` — translucent panels over a
  static pre-blurred wallpaper. No live backdrop blur.

## Color mode
Set `data-theme="light"` or `data-theme="dark"` on the root element.

## High contrast
Set `data-contrast="high"` (forces Clear tier).

## Reduced motion
Honor `prefers-reduced-motion: reduce`; all transitions become 0ms.

## Components
- Aurora Dock (Back · Home · Spaces · Pages · Ask/Search)
- Home/Desk, Spaces selector, Index search
- Aura Plan sheet (numbered editable steps, confirmation, failure, receipt)
- Glass Corner, Aurora Horizon, Pages, Quick Center, Vault, Passport, Trail

## Native handoff
This is a browser-only HTML/CSS/JS design prototype. It must not become a
runtime dependency of the AuroraOS kernel or compositor. See
`open-design-handoff/` for the component-to-native-compositor mapping.
