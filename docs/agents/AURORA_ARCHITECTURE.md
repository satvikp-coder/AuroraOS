# Aurora Glass prototype: architecture

Scope: the browser prototype in `aurora-glass/prototype/`. It is a **design prototype in a host browser**. It is not
AuroraOS, it does not talk to the kernel, and nothing in it is a native OS service. The kernel lives in `kernel/`
and is documented in `docs/kernel/`.

## 1. Stack and constraints

- Plain HTML + CSS + JavaScript. No framework, no bundler, no runtime dependencies. Served as static files.
- Every module is an IIFE that publishes one `window.Aurora*` global. DOM-free models also export through
  `module.exports` so Node unit tests can `require` them.
- No `innerHTML`, no `eval`, no network, no real device access. All text goes through `textContent`
  (use `AuroraUI.el`). Names, queries, file metadata and user text are untrusted.
- Do not migrate to a framework. Extend what is here.

## 2. Files and load order

Order in `index.html` matters; each file may use the globals above it.

| # | File | Global | Role |
|---|---|---|---|
| 1 | `tokens.css` | | Design tokens (colour, type, spacing, radius, shadow, motion, z-index) |
| 2 | `components.css` | | Shared component classes (buttons, cards, lists, dialog, chips, ...) |
| 3 | `shell.css` | | Shell + built-in screen layout (dock, Home, Spaces, Pages, Index, Quick Center, Settings) |
| 4 | `<app>.css` | | One stylesheet per app, layout only |
| 5 | `core.js` | `AuroraCore` | DOM-free models: Pages (simulated multitasking), Quick settings |
| 6 | `settings.js` | `AuroraSettings` | DOM-free settings registry (typed entries, search, live/stored/locked) |
| 7 | `broker.js` | `AuroraBroker` | **Reviewed** Action Broker contract + Quick Commands parser. Do not weaken. |
| 8 | `ui.js` | `AuroraUI` | UI primitives: `el`, `icon`, `appTile`, `dialog`, `confirm`, `prompt`, `holdButton`, `toast`, `chip`, `riskBadge`, `emptyState`, `segmented`, `keepFocus`, `timeAgo` |
| 9 | `app.js` | `AuroraShell` | Shell: screens, navigation history, app registry, Spaces, Index, Dock, Home |
| 10 | `screens.js` | `AuroraModels` | Pages and Quick Center screens; owns the shared `pages` and `qs` models |
| 11 | `system.js` | `AuroraSystem` | The one broker instance, adapters, permission catalogue + policy, mock files, receipts, events |
| 12 | `review.js` | `AuroraReview` | The one trusted review surface: review dialog + Aura Plan screen |
| 13 | `settings-screen.js` | `AuroraSettingsModel` | Settings screen; two-way sync with Quick Center |
| 14 | `<app>.js` | | Apps (Passport, Trail, Vault, Notes, Calculator, Clock): one file each, self-registering |

## 3. Navigation model

- A *screen* is a `<section class="screen" id="screen-<name>">` inside `#content`. Exactly one has `.active`.
- `AuroraShell.showScreen(id)` activates a screen, pushes history, moves focus to the screen's first heading or
  control, announces it, and calls `onScreenShow` listeners. `AuroraShell.back()` pops history (Home at the bottom).
  Escape = back. Ctrl/Cmd+K = Index.
- `AuroraShell.addScreen(id, label)` creates and returns a new screen section (apps use this; they do not edit
  `index.html` beyond their `<script>`/`<link>` tags).
- **App registry**: `AuroraShell.registerApp({ id, name, icon, hue, screen, memMB, keywords })`.
  `AuroraShell.openApp(id, { background })` opens/resumes its simulated Page (`pages.open`) and shows its screen
  (apps without a screen land on Pages). Returns `{ ok, id, created }` or `{ ok:false, reason }`.
  `AuroraShell.apps()` lists registered apps. Home pins, Dock running apps and Index all read the registry.
- Dock: Back, Home, Spaces, Pages, Ask/Search (ids `dock-back|home|spaces|pages|ask`). At >= 768px it also shows
  running apps; at >= 1024px a tray (clock, Quick Center). It never overflows the viewport.

## 4. State

All state is in-memory JavaScript. Nothing is persisted unless an app says so explicitly (see section 8).

| State | Owner | Read / change through |
|---|---|---|
| Active screen, history, active Space, spaces list, theme/tier/contrast/motion | `app.js` | `AuroraShell.state`, `showScreen`, `switchSpace`, `setAppearance`, `setTier`, `setContrast`, `setMotion` |
| Open Pages (running/paused/saved), simulated memory | `core.js` model in `screens.js` | `AuroraModels.pages` |
| Quick settings (brightness, volume, wifi, focus, ...) | `core.js` model in `screens.js` | `AuroraModels.qs` |
| Settings registry | `settings.js` in `settings-screen.js` | `AuroraSettingsModel` |
| Permissions, receipts, mock files, timer | `system.js` | `AuroraSystem.permissions`, `.receipts()`, `.files`, `.world` |

Models notify with `onChange`; `AuroraSystem.on(event, fn)` carries `receipts`, `permission`, `files`, `timer`,
`status`. Screens re-render from models; they never keep a private copy of shared state.

**Settings sync**: Quick Center (`qs`) and the Settings registry describe the same values for theme, focus,
battery saver and Aura mode. `settings-screen.js` maps them both ways (`QS_TO_SETTING`). Add new shared settings
there, never by writing DOM from another screen.

## 5. Action Broker integration (security-critical)

```
any screen ──System.request(payload)──▶ review.js ──propose──▶ broker (system.js)
                                           │  needs review? show the opaque review dialog (S3: hold-to-confirm)
                                           │  approve(digest, confirmS3) ─▶ execute ─▶ adapters ─▶ receipts
Aura / Index ──AuroraReview.ask(text)──▶ parseCommand ─▶ plan ▶ Aura Plan screen (edit / skip / confirm)
                                                        └ single action ▶ System.request
```

Rules that must hold everywhere:

1. **Screens can only propose.** `AuroraSystem.request(payload, { source, title })` resolves
   `{ ok, cancelled?, reason?, steps?, proposalId? }` and never throws. A screen must not mutate
   `AuroraSystem.world` for anything the broker has an action for (file move/delete, focus, timer).
2. **One trusted surface.** `review.js` takes the broker's trusted facet once (`System.takeTrusted`). No other
   file may approve, edit, skip or execute. Do not export the facet.
3. **Risk contract** (DESIGN.md section 7): S0 single step may run directly; S1 plan preview; S2 explicit review; S3
   hold-to-confirm (`AuroraUI.holdButton`, with the visible two-step alternative). Every multi-step plan is
   reviewed.
4. **Permissions** are `allow | ask | deny`, changed only via `AuroraSystem.permissions.set(id, decision)`.
   Policy: unknown ids refused; S3 permissions can never be `allow`. Every change is logged
   (`permissions.log()`). `deny` blocks; the review surface offers "Open Passport", never a bypass.
5. **Receipts are the truth.** Trail and receipts panels render `AuroraSystem.receipts()` (broker receipts joined
   with their action and `describe()` text). Undo goes through `AuroraSystem.undo(receiptId)` /
   `undoProposal(id)` and shows the real result (`ok` or the explained reason). Never draw a success, or a
   successful undo, the broker did not report.
6. Review, confirmation and receipt panels are **opaque** surfaces, never glass.
7. `share.send` has no adapter: sending outside the device is honestly "not available in this prototype".

Simulation boundary: this is one JavaScript context, so it is a *model* of the trust design, not an OS trust
boundary. The native broker must be a privileged service with a secure input path (see `broker.js` header).

## 6. Prototype data model

- **Files** (`AuroraSystem.files.list()/get(id)/deleted()`): mock objects
  `{ id, name, kind: note|text|table|image, space, size, modified, preview }`. Ids match `/^[a-z0-9][a-z0-9_-]{0,63}$/i`.
  Deleting moves a file to `world.deleted` (what Trail restores) for the session.
- **Spaces**: `study`, `work`, `projects`, `personal` are the broker-known spaces. User-created Spaces exist in
  the shell only (they cannot be a broker action target).
- **Apps**: `notes, files, calculator, clock, settings, calendar` are broker-known (`app.open`); system surfaces
  (`passport`, `trail`, `vault`) are shell apps opened by direct navigation.
- **Receipt** (from `AuroraSystem.receipts()`): `{ id, at, type, risk, status: done|failed|denied|unsupported|needs-reapproval, undoState: available|used|failed|in-progress|unavailable, undoable, proposalId, stepIndex, error?, errorText?, action, describe: {title, detail, icon, kind}, source, requestText }`.

## 7. Native OS versus browser

| Thing | In this prototype | In native AuroraOS |
|---|---|---|
| Files, Vault, Trail | Mock objects in page memory | AuroraFS via `SYS_FS` (kernel PRs #28/#29, under review) |
| Settings | In-memory registry | Planned native settings registry (issue #6, milestone E) |
| Pages / memory | Simulated numbers | Real processes and memory accounting |
| Broker approvals | Same-context JS facet | Privileged service + secure input path |
| Glass | CSS alpha/blur | Compositor starts at Clear (Tier 0); see `aurora-glass/open-design-handoff/native-compositor-mapping.md` |
| Radios, camera, mic, location | Labelled simulations / "stored only" | Hardware dependent, unverified |

Never describe a prototype feature as a working OS feature. UI copy says "simulated" wherever a person could
otherwise believe a real device changed.

## 8. Persistence

Default: none (reload resets everything, which the tests rely on). An app that wants persistence must use a
namespaced, try/catch-guarded `localStorage` wrapper, label it in the UI as "saved in this browser only", and
never call it AuroraOS storage. Tests must clear it.

## 9. Adding an app (checklist)

1. `aurora-glass/prototype/<app>.js` (IIFE) + `<app>.css`; add both tags to `index.html` after `settings-screen.js`.
2. `const screen = Shell.addScreen("screen-<app>", "<Name>")`, then `Shell.registerApp({...})`.
3. Build with `AuroraUI.el` and component classes. Header = `.screen-header` with a Back button
   (`Shell.back()`), `.screen-title` in an `h1`.
4. Sensitive operations go through `AuroraSystem.request`. Pure calculation logic goes in a DOM-free module
   with Node unit tests in `tests/unit/`.
5. Empty, error and loading states; keyboard path for every pointer action; focus kept across re-renders
   (`AuroraUI.keepFocus` + `data-fk`).
6. Playwright spec in `tests/ui-golden/<app>.test.js`, and add the screen to the layout matrix
   (`tests/ui-golden/layout-matrix.test.js`).
7. Update `docs/agents/AURORA_UI_HANDOFF.md` and `AURORA_UI_TASKS.md`.
