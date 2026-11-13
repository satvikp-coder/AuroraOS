# Aurora Glass UI plan and task backlog


Plan author: Claude Fable 5.2 (`claude-fable-5-2`), lead architect, 2026-11-10. No other model contributed to this plan.
Dependency-ordered. Status values: `done`, `in progress`, `not started`, `blocked`. A task is `done` only when
every acceptance criterion has been verified by the listed tests. Update this file and
`AURORA_UI_HANDOFF.md` after every task.

Conventions used below:

- Paths are relative to `aurora-glass/prototype/` unless they start with `tests/`, `docs/` or `.github/`.
- "Full UI suite" = all `tests/unit/*.test.js`, `node --check` on every prototype JS file, and
  `npx playwright test --workers=1`.
- "Screenshot pass" = capture at 360, 390, 768, 1280, 1440 px in light and dark, Soft and Clear, plus high
  contrast and 200% text for the screens named, and open the PNGs to look at them.
- Branches are stacked: each milestone branches from the previous milestone's branch until that one merges.

## Dependency graph

```
F0 foundation (done)
 └─ M1 shell  (S1 wiring → S2 shell API → S3 Home/Dock → S4 Spaces → S5 Index → S6 Pages/Quick/Settings
     │         → S7 Glass Corner/Horizon → S8 tests → S9 visual loop → S10 PR)
     ├─ M2 Passport ─┐
     ├─ M3 Trail ────┼─ R1 security/UX review (Opus)
     ├─ M4 Vault ────┘   (Vault needs Trail for "restore")
     ├─ M5 Notes ─┐
     ├─ M6 Calculator ─┼─ R2 consistency review (Opus)
     ├─ M7 Clock ─┘      (Clock consumes the broker timer from F0)
     └─ M8 polish → R3 final visual + accessibility audit
```

M5, M6 and M7 depend only on M1 and are independent of M2–M4 and of each other.

---

## F0. Design-system foundation — `done` (commit `fee394a`, branch `feature/aurora-shell-polish`)

| | |
|---|---|
| Objective | Shared tokens, component classes, UI primitives, the single broker instance and the trusted review surface |
| Files | `tokens.css`, `components.css`, `ui.js`, `system.js`, `review.js`, `docs/agents/AURORA_ARCHITECTURE.md`, `.claude/agents/*.md` |
| Verified | `node --check` on the three JS files only |
| Not verified | **Nothing here has run in a browser yet.** `index.html` does not load these files. First real execution happens in S1; expect to fix defects in `ui.js`, `review.js`, `system.js` and `components.css` then |

Known things to check the first time it runs (found by reading, not by running):

1. `review.js` calls `Shell.back()`, `Shell.openApp()`, `Shell.openIndex()`, `Shell.switchSpace(id, {quiet})`:
   none exist until S2.
2. `system.js` `app.open` adapter expects `openApp` to return `{ ok, id, created }`; the undo closes the Page
   only when this call created it.
3. `components.css` sets `[hidden] { display:none !important }` and `.screen.active { display:flex }`; the old
   `shell.css` uses `display:block` and has its own `.btn/.card/.toast` rules that will fight until S1 removes them.
4. `AuroraUI.dialog` appends to `#app`; `#app` has `overflow:hidden` in the old shell. The scrim is
   `position:fixed`, which is fine unless an ancestor gets a `transform`/`backdrop-filter` (that would trap it):
   keep `#app` free of both.
5. `holdButton` cancels on `blur` and `pointerleave`; confirm a touch hold on a phone-sized viewport is not
   cancelled by a 1–2 px drift (may need `setPointerCapture`).
6. `review.js` `confirmPlan` re-renders on every progress event while running; `keepFocus` must keep focus on
   `#btn-plan-stop` (it has `data-fk`).
7. Mock file timestamps are relative to page load; `timeAgo` output will drift in long sessions (fine).

---

## M1. Shell polish — `in progress` — branch `feature/aurora-shell-polish` — PR: none yet

S1 and S2 are done (Claude Sonnet 5.5, 2026-11-12). Do the remaining sub-tasks in order; commit after each one that leaves the suite green.
Known deviations from the S1 text, to be closed by later steps: `.icon-btn` is kept (legacy, in shell.css) until S3/S6 rebuild the
screen headers; Index is still the old list until S5; the
the reset confirmation in Settings is still the inline panel until S6.

### S1. Wire the foundation into the page — `done` (commit 78d19e1; verified: 97 Playwright specs, 4 unit suites, zero console errors)

- Objective: `index.html` loads the full module chain and still boots.
- Files: `index.html`, `shell.css`, `app.js`, `screens.js`, `settings-screen.js`.
- Steps:
  1. `<head>`: `tokens.css`, `components.css`, `shell.css`. Scripts in this order: `core.js`, `settings.js`,
     `broker.js`, `ui.js`, `app.js`, `screens.js`, `system.js`, `review.js`, `settings-screen.js`.
  2. Replace the whole static `#screen-aura` markup with an empty
     `<section id="screen-aura" class="screen" aria-label="Aura Plan"></section>`.
  3. Delete from `app.js`: `renderPlan`, `startPlan`, `updateRunningUI`, `advancePlan`, `finishPlan`,
     `planTimer`, every `btn-plan-*`/`btn-failure-*`/`btn-receipt-close` handler, `wireControls`,
     `setPreviewWidth`, and the `auraFlow/plan*/width` state fields. Delete `#controls-panel` from `index.html`.
  4. Delete from `shell.css` everything `components.css` owns: reset/base, `.visually-hidden`, `.screen*`,
     `.panel-hidden`, `.btn*`, `.icon-btn*` (replace uses with `.btn.btn-icon`), `.card`, `.section-head/title`,
     plan/confirm/running/receipt/failure rules, `.toast`, `.meter*`, `.switch*`, `.control-group` field rules,
     focus rules, reduced-motion block, the "Layout fixes" overrides.
  5. Replace the local `el()` helpers in `screens.js` and `settings-screen.js` with `AuroraUI.el`.
- Acceptance: page loads with zero console errors; `window.AuroraUI/AuroraSystem/AuroraReview/AuroraShell/
  AuroraModels/AuroraSettingsModel` all defined; no `innerHTML` in any prototype file
  (`grep -n innerHTML aurora-glass/prototype/*.js` is empty).
- Test: `node --check`; load in Chromium and read the console; existing Settings and Pages specs still pass
  (Aura specs are expected to fail until S8).
- Depends on: F0.

### S2. Shell API — `done` (commit 78d19e1; tests/ui-golden/shell.test.js, 25 specs)

- Objective: implement the `AuroraShell` contract of `AURORA_ARCHITECTURE.md` sections 3 and 4.
- Files: `app.js`.
- API to implement (exact signatures):
  - `state` `{ theme, tier, contrast, motion, activeScreen, activeSpace, indexQuery }`
  - `showScreen(id, { push = true, focus = true })`: activate, push history unless same as top, focus the
    screen's `h1` (`tabindex=-1`) or first control, scroll content to top, announce, sync Dock, call listeners.
  - `back()`: pop history; Home when empty. Returns the screen shown.
  - `addScreen(id, label)` → `HTMLElement` appended to `#content`.
  - `registerApp({ id, name, icon, hue, screen = null, memMB = 100, keywords = [], system = false })`
  - `apps()` → copies; `app(id)` → one or null.
  - `openApp(id, { background = false })` → `{ ok, id, created }` | `{ ok:false, reason }`.
    Order: unknown id → `{ok:false, reason:"unknown-app"}`. System surface with no screen yet → toast
    "<Name> arrives in a later milestone", `{ok:false, reason:"not-available"}`, no Page created.
    Otherwise `pages.open(...)`; on `low-memory` show Pages + toast and return the failure; on success show the
    app's screen (or Pages if it has none) unless `background`.
  - `spaces()` → `[{ id, name, description, hue, pinned: [appId], builtin }]`; `addSpace(name)`,
    `renameSpace(id, name)` with validation (trimmed, 1–24 chars, case-insensitive unique, max 8 spaces, reject
    control and bidi characters with the same class the broker uses); `switchSpace(id, { quiet })`.
  - `openIndex(query = "")`: show Index, set and focus the input, render.
  - `onScreenShow(fn)`, `registerIndexSource(fn)`, `refreshIndex()`.
  - `setAppearance(theme)`, `setTier(tier)`, `setContrast(c)`, `setMotion(m)` (also sets/removes
    `data-motion="reduce"` on `<html>`).
  - `announce`, `showToast`: delegate to `AuroraUI`.
- Registered apps and hues: notes 42, files 212, calculator 228, clock 262, settings 220 (screen
  `screen-settings`), calendar 6, passport 152 (system), trail 280 (system), vault 188 (system).
- Default pins: study = notes, clock, calculator, settings, calendar, files; work = files, notes, calendar,
  settings; projects = files, notes, calculator, settings; personal = notes, calendar, clock, settings.
- Global keys: Escape closes the top-most layer (dialog handles its own; then Glass Corner popover; then
  `back()`); Ctrl/Cmd+K → `openIndex()`.
- Acceptance: every function above behaves as specified; `review.js` and `system.js` run without touching
  undefined shell functions; Back from a screen opened from another screen returns to that screen, not Home.
- Test: Playwright `tests/ui-golden/shell.test.js` (new): history/back, openApp results for each case,
  low-memory path, addSpace/renameSpace validation table, Escape layering, Ctrl+K focus.
- Depends on: S1.

### S3. Home and Dock — `done` (tests/ui-golden/home-dock.test.js, 55 specs; screenshots inspected: Home at 390 light/dark, 360 at 200% text, 768, 1280 light, 1440 dark Clear, 390 high contrast)

- Objective: a Home that shows real state and a Dock that reads as the OS's signature bar.
- Files: `index.html` (Home skeleton only), `app.js` (render functions), `shell.css`.
- Home, top to bottom:
  1. Greeting: live clock in `--text-display` (minute resolution; re-render on the minute boundary with a
     `setTimeout` to the next minute, not a 1 s interval), long date, chips for current Space and Focus.
     Header icon buttons `#btn-index` ("Open Index search") and `#btn-quick-center` ("Open Quick Center").
  2. Ask pill: full-width button styled like a search field, "Ask Aura or search", `Ctrl K` hint at >= 768.
  3. Continue: from `AuroraModels.pages.list()`, running first then paused, max 4, each a `button.list-row`
     with app tile, name, "Space · state", chevron; activating calls `openApp`. "View all" → Pages. Empty
     state: "Nothing in progress" + "Open Notes".
  4. Pinned: grid of `button.pin-btn` (tile above label, `aria-label="Open <Name>"`), from the active Space.
  5. Current Space: name, description, "n pages open here", `#btn-switch-space`.
  6. Aura suggestion: `#btn-aura-study` → `AuroraReview.ask("Prepare my phone for studying")`. Hidden when
     `qs.auraMode === "off"`. Copy says it opens a plan to review.
  7. `#horizon`: status text + thin `.meter` (hidden when idle), driven by `AuroraSystem.on("status")`.
- Re-render Home on `pages.onChange`, `qs.onChange`, space change and `status`.
- Layout: one column < 768; at >= 768 Continue and Pinned side by side; at >= 1024 a 12-column grid: greeting
  + Ask span full width, Continue 7 / Current Space + Aura 5, Pinned full width (6–9 tiles per row).
- Dock: `nav#dock` glass bar; five nav buttons with icon + label; current = accent-soft pill + bold label +
  `aria-current="page"`. `.dock-apps` (>= 768): one button per running/paused Page with a small tile and a
  state dot (`aria-label="<Name>, running"`), max 6 then "+n". `.dock-tray` (>= 1024): time and a Quick Center
  button. Dock top edge carries a 2 px Horizon line while a plan runs. Labels hidden at 150/200% text. Width
  never exceeds the viewport; at 360 px with 200% text all five buttons are still >= 48 px.
- Acceptance: nothing on Home is static sample data; every control does something; no overflow 360–1440;
  clock text on wallpaper passes AA in both themes; Dock current state visible without colour.
- Test: `shell.test.js`: Continue mirrors Pages after pause/close; pins change with Space; suggestion hidden
  when Aura is off; Horizon text after a plan; Dock `aria-current`; dock apps appear at 768+. Layout matrix.
  Screenshot pass for Home.
- Depends on: S2.

### S4. Spaces — `done` (tests/ui-golden/spaces.test.js, 28 specs; screenshots inspected: Spaces 390 light, 1280 dark with 5 Spaces, New Space dialog with inline error at 390)

- Files: `app.js`, `shell.css`, `index.html` (container only).
- Card = `article.space-card-item[data-space]` (+ `.selected` when active) containing: hue badge with initial,
  `h3` name, description, row of `app-tile-sm` for pins, "n pages open", state line ("Active" with check icon /
  "Switch to this Space"). A stretched `button.space-switch` (`::after { inset:0 }`) makes the whole card
  clickable; a separate `button` "Rename" sits above it in z-order. Header action "New Space".
- Create/rename use `AuroraUI.prompt` with the S2 validation; error text appears inline while typing and the
  confirm button stays disabled until valid.
- Switching: `switchSpace`, toast "Now in <Name>", focus stays on the card's switch button.
- Acceptance: create, rename, switch all work by mouse and keyboard; duplicate/empty/too-long/9th space are
  refused with a specific message; names render as text (try `<img onerror>` as a name).
- Test: `shell.test.js` Spaces block including the injection name; existing "switches between Spaces" spec.
- Depends on: S2.

### S5. Index (Ask + Search) — `done` (tests/ui-golden/index.test.js, 37 specs; screenshots inspected: Index empty at 390 light, Aura row at 1280 dark; the no-match state and 360/200% layouts are covered by assertions only)

- Files: `app.js`, `shell.css`, `index.html`.
- Structure: `.search-field` with `#index-query` (`role="combobox"`, `aria-controls="index-results"`,
  `aria-activedescendant`), clear button; `#index-results[role=listbox]` with group headings
  (`role="presentation"` headers, options carry `aria-describedby` to their group).
- Result order: Aura command (if `AuroraReview.describeCommand(q)`), Apps, Settings, Files, Actions.
  Ranking inside a group: label prefix > word prefix > substring > keyword. Max 6 per group, 20 total.
- Each option: `div.index-result[role=option][id]` with tile or icon, title, meta, trailing chip (risk badge
  for the Aura row). Activation: Aura → `AuroraReview.ask(q)`; app → `openApp`; setting → existing
  `openSetting`; file → `openApp("vault")` (later: Vault selects the file); actions → Quick Center, Pages.
- Empty query: "Try asking" suggestions (4 commands) and all apps. No match: empty state quoting the query
  via `textContent` + suggestion buttons.
- Keys on the input: ArrowDown/Up move the active option (wraps, scrolls into view), Home/End, Enter activates
  active or first, Escape = back. Announce result count politely (debounced 250 ms).
- Aura off: the Aura row is replaced by a muted row "Aura is off" that opens Quick Center.
- Acceptance: the three existing Index specs in `settings.test.js` pass unchanged; typing
  "start a 10 minute timer" + Enter starts the timer with a toast offering Undo; a plan query opens the plan
  screen and runs nothing.
- Test: `shell.test.js` Index block (arrow keys, Aura row, S0 immediate run + Undo, plan opens review,
  no-match, injection).
- Depends on: S2.

### S6. Pages, Quick Center, Settings restyle — `done` (tests/ui-golden/screens.test.js, 20 specs; existing pages-quick/settings specs unchanged except one selector; screenshots inspected: Pages 390 light, Quick Center 390 dark, Settings 390 light)

- Files: `screens.js`, `settings-screen.js`, `settings.js`, `shell.css`, `index.html`.
- Pages: card per page with app tile, name, state chip (icon + text), meta, actions. Keep every id, role,
  `data-page`, `data-state`, `data-fk` and button name used in `pages-quick.test.js`. Tapping the card title
  opens the app (`openApp`).
- Quick Center: sliders with leading icons; tiles with icon, label, On/Off, and a "Simulated" sub-label for
  Wi-Fi, Bluetooth and rotation lock; appearance as `AuroraUI.segmented` **only if** the tests that use
  `#qc-appearance` select are updated in the same commit, otherwise keep the select; privacy list with icons.
- Settings: restyle only. Add `developer.failAuraStep` (toggle, advanced, effect `live`, label "Make the next
  Aura plan fail at its third step (simulated)") → `AuroraSystem.dev.failNextType = on ? "app.open" : null`;
  turn itself off after one failed plan (listen to `receipts`).
- Acceptance: `pages-quick.test.js` and `settings.test.js` pass with at most the two plan-approval edits
  (S8); two-way Quick Center/Settings sync unchanged.
- Depends on: S1.

### S7. Glass Corner and Horizon — `done` (tests/ui-golden/glass-corner.test.js, 30 specs; screenshots inspected: popover with real items at 390 light, empty state at 1280 dark; Horizon rail/Dock line shipped in S3 and tested here)

- Files: `app.js`, `shell.css`.
- Glass Corner: `button.glass-corner[aria-expanded][aria-controls=gc-panel]` with a count badge when there
  are receipts newer than the last open. Popover `#gc-panel` (opaque, `--shadow-3`, max-height 70vh, scrolls):
  heading "Held for you", up to 5 latest receipts (title, status chip, `timeAgo`), up to 3 latest permission
  changes, "Open Trail" button, empty state "Nothing held". Escape or outside click closes; focus returns to
  the handle. Hidden entirely when the setting `notifications.glassCorner` is off (existing behaviour).
- Horizon: one `status` listener updates Home rail and Dock line; text is set with `textContent`.
- Test: `shell.test.js`: aria-expanded toggling (existing spec), popover lists a receipt after a command,
  Escape closes and restores focus, empty state on fresh load.
- Depends on: S3, S5.

### S8. Tests — `done` (aura-plan 28, layout-matrix 28 tests = 924 states, system.test.js 21, shots.js; full suite 324 Playwright + 5 unit locally; CI list updated)

- Files: `tests/ui-golden/aurora-glass-browser.test.js`, `pages-quick.test.js`, `settings.test.js`,
  new `shell.test.js`, new `aura-plan.test.js`, new `layout-matrix.test.js`, `tests/unit/aurora-glass.test.js`,
  new `tests/unit/system.test.js` (jsdom not needed: stub `window` with broker + fake Shell/Models to test the
  permission policy and `describe/explain`), `.github/workflows/ci.yml`.
- `aura-plan.test.js` cases (each asserts real state, not just visible panels):
  1. Review shows request text and 4 steps; before approval focus is off, Space unchanged, Notes page state
     unchanged.
  2. Confirm → dialog → Run → receipt "Plan complete"; Space = study, focus on (Quick Center tile and Settings
     row), Notes running.
  3. Per-step Undo reverts only that step; "Undo all" reverts the rest; chips change to "Undone"; a second
     undo is impossible (button gone).
  4. Skip step 2 → proposal id changes, confirm lists 3 steps, focus stays off after run.
  5. Edit timer to 10 minutes → step title updates, `AuroraSystem.world.timer.seconds === 600` after run.
  6. Stop mid-plan (set `dev.stepDelayMs = 600`) → "Plan stopped", later steps "Not run".
  7. Failure injection → `#plan-failure`, step 3 "Failed" with the explained reason, steps 1–2 undoable.
  8. `settings.write = deny` → `#plan-blocked` visible, `#btn-plan-confirm` disabled; skipping step 2 enables it.
  9. Permission flipped to `ask`→`deny` after review but before Run → run refused with the explained reason,
     nothing executed.
  10. S3: `AuroraSystem.request(file.delete)` → click on approve does nothing; 1.3 s hold deletes; file is in
      `files.deleted()`; undo restores. Two-step alternative path. Cancel path. Escape path.
  11. `permissions.set("files.delete","allow")` → `{ok:false, reason:"always-ask"}`; unknown id refused; every
      accepted change appears in `permissions.log()`.
  12. `share.send` request → "This can't run", no Approve button.
  13. Aura mode off → `ask()` refuses with the toast.
- Layout matrix: 7 viewports × 2 themes × 2 tiers × {default, 150%, a11y} over home, spaces, index (empty and
  with results), pages, quick, settings, aura review, aura receipt, review dialog open. Export
  `registerScreen({ id, open })` from a helper file for later apps. Keep the wide-font stress test.
- Screenshot specs write to `testInfo.outputPath` unless `AURORA_SHOTS=1`.
- CI: file list includes the new prototype files; run `tests/unit/system.test.js`.
- Acceptance: full UI suite green locally with one worker; no test weakened; no test writes into a tracked
  directory by default (`git status` clean after a run).
- Depends on: S1–S7.

### S9. Visual quality loop — `done` (see "S9 record" below)

- Screenshot pass for all seven screens plus the review dialog and the receipt. Checklist per image:
  alignment to a common left edge; consistent card padding; one primary button per view; heading sizes step
  down in order; nothing touches the Glass Corner or the Dock; text on glass is legible over the brightest
  part of the wallpaper; dark theme borders visible; high-contrast mode has no translucent surface; 200% text
  wraps without clipping; desktop does not look like a stretched phone.
- Record in the PR body which combinations were looked at and each defect fixed.
- Regenerate the curated set in `aurora-glass/screenshots/` (<= 24 PNGs) and `git rm` the stale ones.
- Depends on: S8.

**S9 record (what was actually looked at, 2026-11-12).** Images opened and inspected: Home at 390 light and dark, 360 at 200% text (before and after the fix),
768, 1280 light, 1440 dark Clear, 390 high contrast with the Focus chip; Spaces 390 light and 1280 dark, New Space dialog with an inline error at 390;
Index 390 (empty and with an Aura row) and 1280 dark; Pages 390 and 1280; Quick Center 390 dark and 1280 (before and after); Settings 390 light and
1280 dark (before and after); Aura review 390 dark, confirmation dialog 390, receipt 390; S3 delete dialog 390 dark Clear; Glass Corner popover 390 and
1280 dark. Defects found and fixed: all app tiles shared one hue; clock wrapped at 390; Quick Center and Settings were stretched phone layouts on desktop
(now two columns / a bounded sheet); root font size pinned to 16px on phones so inherited text did not scale (row title was smaller than its subtitle at 200%);
a padlock on "Can't be undone". NOT inspected by eye (covered by the layout matrix assertions only): 412 and 1024 widths, 1440 for non-Home screens,
Settings/Pages/Quick Center in Clear + high contrast, most of the final curated PNGs, reduced-motion appearance. The 22 curated PNGs in
`aurora-glass/screenshots/` replace the 24 stale images of the old design; regenerate with `AURORA_SHOTS=1` (curated-shots.test.js).

### S10. Deliver M1 — `done except the human review/merge`

**Independent review (2026-11-12).** A read-only Opus subagent (`token-saver-opus-architect`) reviewed app.js, review.js, system.js, ui.js, screens.js, settings-screen.js and
index.html against the architecture rules. Result: no P0 (no way around the review/approval boundary, no untrusted-text sink). Findings, all verified and fixed with
regression tests in `tests/ui-golden/review-fixes.test.js`: (P1) focus lost on the Plan screen; Ctrl+K reached behind a modal and stacked dialogs shared one focus trap;
a single "Open X" command reported success without opening anything; reserved system apps (Passport/Trail/Vault) could never gain a screen. (P2) a second plan could be
started during a run and corrupt its receipt; "Undo all" reported failure after a single step was undone; the Glass Corner badge counted items already seen.
**Open product decision (not changed):** Aura mode "Ask before acting" (the default) behaves exactly like "Quick Commands only": only "Off" is checked, so S0 commands run
without a prompt and Index says "Runs now". Either enforce a review in Ask mode or rename the option. This was a review by a model, not a human review: PR #30 still needs a human.

- Review the diff, run Gitleaks' scan via CI, push, open the PR to `main` titled
  `feat(ui): Aurora Glass shell on the shared design system (broker-backed Aura, Home, Dock, Spaces, Index)`,
  verify all CI jobs, record SHA/PR/run links in the handoff. Merge only if review requirements allow.

---

## M2. Passport — `done, awaiting review` (PR stacked on #30; passport.test.js 42 + unit 13; 375 Playwright locally) — branch `feature/aurora-passport` (from M1)

- Objective: permission management that changes real broker state.
- Files: `passport.js`, `passport.css`, `tests/ui-golden/passport.test.js`, `tests/unit/passport.test.js` (if
  a per-app model is added), `index.html` (tags), layout matrix registration.
- Screen: tabs **Overview / Apps / Activity**.
  - Overview: summary sentence ("5 allowed, 2 ask each time, 1 blocked"), then one `.list-row` per
    `AuroraSystem.permissions.catalog()` entry: label, plain-language `explain`, risk badge, and a
    three-option `AuroraUI.segmented` Allow / Ask / Deny. For S3 entries the Allow segment is disabled with
    the visible reason "Always asks: cannot be pre-approved". Unsupported actions show "Not available in this
    prototype".
  - Changing a decision: tightening (→ ask, → deny) applies at once with an Undo toast. Loosening to Allow
    opens `AuroraUI.confirm` explaining what will now run without review; only then
    `permissions.set`. The segmented control reflects the stored decision after the call (never optimistic).
  - Apps: per-app view. Model: `PASSPORT_APPS` = for each registered app, which broker permissions it can
    request (notes: files.edit; files/vault: files.edit, files.delete, external.send; clock: timer.control;
    aura: all). Shows per app the effective decision for each; "Revoke all for this app" sets its
    non-S0 permissions to Deny after a confirm. State clearly that grants are per-capability in this
    prototype (the broker has no per-app dimension) so a change affects every app using that capability.
    Camera, microphone, location rows are present and marked "Hardware-dependent · not available".
  - Activity: `permissions.log()` newest first (what changed, from → to, when), plus the last 20 receipts with
    status `denied` or `needs-reapproval`; empty state. "Reset to defaults" behind a confirm.
- Security acceptance: Passport never calls anything but `permissions.set/resetAll`; no path sets an S3
  permission to allow (test by driving the DOM and by calling the API); every change is in the log; a change
  made in Passport while a plan is in review makes the broker refuse the stale approval (covered by M1 case 9,
  re-tested from the Passport UI).
- Also: implement the "Open Passport" button of the review dialog end to end; add Passport to Index and pins.
- Tests: decision change by mouse and keyboard; confirm on loosening; cancel leaves state unchanged; S3 allow
  disabled and API refusal; revoke-all; activity entries; reset; deny then try the action from Vault/Aura and
  see it blocked; layout matrix; screenshot pass.
- Depends on: M1.

## M3. Trail — `done, PR open` (trail.test.js 35, unit 21; 410 Playwright locally) — branch `feature/aurora-trail` (from M2)

- Objective: truthful action history and recovery.
- Files: `trail.js`, `trail.css`, `tests/ui-golden/trail.test.js`, `tests/unit/trail.test.js` (filter/group
  logic as a DOM-free function).
- Screen:
  - Search field + filter chips (All / Done / Failed or blocked / Undoable / Undone) + risk filter.
  - Timeline grouped by proposal (a plan = one group with its request text and an overall chip: Complete,
    Partial, Failed, Stopped), then by day. Each receipt row: icon, `describe.title`, source (Aura, Vault,
    Passport…), time, risk badge, status chip, undo state.
  - Detail (inline expand on phone, side panel >= 1024): all receipt fields in plain language, `errorText`
    for failures, and why undo is or is not possible ("Can't be undone: searching changes nothing", "Already
    undone", "Undo failed: …").
  - Undo button only when `undoState === "available"`; while running the button is disabled and reads
    "Undoing…"; the row then shows the broker's result. "Undo all" per plan group via `undoProposal`.
  - "Deleted files" section from `AuroraSystem.files.deleted()` with Restore = undo of that delete receipt
    (find the `file.delete` receipt whose action id matches and is `available`); if none is available the row
    says so and offers nothing.
  - Honest footer: history is for this session only; the retention setting `storage.trailDays` is stored only.
- Acceptance: every row corresponds to a broker receipt; partial plans show which steps ran; no optimistic
  UI; an undo that the adapter rejects (delete a file, move it, then… e.g. undo move after the file was
  deleted → `no-such-file`) shows "Undo failed" and the chip stays failed.
- Tests: timeline after a plan; filters and search; failed and blocked receipts; partial success; undo
  success; undo failure path; restore deleted file; busy state; empty state; layout matrix; screenshots.
- Depends on: M1 (richer with M2 for blocked receipts). Both are merged.
- Delivered notes: filters operate per receipt (a plan stays while any step matches and says "showing N matching"); the selection is dropped when a filter hides it; detail is inline below 1024 px and a side panel at 1024 px and up; Restore calls `System.undo` on the newest `file.delete` receipt that is still `available`; Passport-refused requests never ran and so are not listed.

## M4. Vault — `merged (#33)` (vault.test.js 55, unit 15; 465 Playwright locally; layout matrix 28/28) — branch `feature/aurora-vault` (from main after M3)

- Objective: file manager over the mock files, with every mutation through the broker.
- Files: `vault.js`, `vault.css`, `tests/ui-golden/vault.test.js`, `tests/unit/vault.test.js` (sort/filter/
  size formatting as DOM-free functions).
- Screen:
  - Toolbar: search, Space filter (All + each Space), sort (Name, Modified, Size; asc/desc), grid/list
    `segmented`, "Select" toggle.
  - List view (`.list`) and grid view (cards with a kind tile); kind icons per `note|text|table|image`.
  - Selection: tap opens preview; in Select mode rows are checkboxes (`role="checkbox"`), Shift+click range,
    Ctrl/Cmd+A, a selection bar with count, Move, Delete, Clear. Keyboard: arrows move, Space toggles, Enter
    opens, Delete key starts delete.
  - Preview: side panel >= 1024, sheet below: name, kind, Space, size, modified, text preview via
    `textContent` in a `<pre>`; images show a labelled placeholder (mock data has no pixels). Shows the
    current decision for `files.edit` and `files.delete` with a link to Passport.
  - Move: destination picker (the four broker Spaces) → `System.request({type:"file.move"...})`; multiple
    files → one plan `{steps:[...]}` (max 12 per plan; more is split with a clear message).
  - Delete: `file.delete` request(s) → review dialog with hold-to-confirm. After success: toast with Undo
    (`undoProposal`), and "View in Trail".
  - States: empty Space, no search results, blocked by Passport (inline callout + "Open Passport"), request
    cancelled (no change, no error toast), partial failure (says which files).
  - Banner: "Sample files in this browser prototype. Not AuroraFS, nothing is saved."
  - Remove the temporary "arrives in a later milestone" path for `vault`; Index file results select the file.
  - The broker app `files` opens Vault (same screen) so "open files" / "show my files" land here.
- Security acceptance: `vault.js` never writes `AuroraSystem.world`; with `files.delete = deny` nothing can be
  deleted from the UI; a click cannot substitute for the hold.
- Tests: list/grid, search, each sort, Space filter, preview, single move + undo, multi move, delete with
  hold + undo, delete blocked, cancel, keyboard selection, 12-file cap, injection-safe file name rendering
  (add a mock file whose name contains markup in the test via `world.files`), layout matrix, screenshots.
- Depends on: M1; Trail link needs M3. Both are merged.
- Delivered notes: the `files` app now has `screen: "screen-vault"`; Index file results call `AuroraVault.preview(id)`; results (with Undo from the broker's receipts, and View in Trail) appear in a `#vault-result` callout; Move and Delete are disabled with a reason when Passport says Deny, and the trusted review still enforces the live policy.

**R1 (Opus `aurora-quality-reviewer`)** after M2–M4: security and UX review of Passport, Trail and Vault;
fix P0/P1 findings before the PRs are called ready.

## M5. Notes — `merged (#43)` (notes.test.js unit 43; notes.test.js Playwright 56 specs; layout matrix 28/28 with 4 new states) — branch `feature/aurora-notes` (from main)

**Record.** `notes-model.js` (DOM-free store with injected storage/clock/id; never throws), `notes.js`, `notes.css`, registered as the app's `screen`. Unreadable stored data (bad JSON, unknown/newer version) starts empty, shows a banner and is copied to `aurora-glass.notes.v1.unreadable` instead of being overwritten; individual invalid notes are dropped and counted. A failed write keeps all text in memory and the editor, shows "Not saved: <reason>" with a working Try again; over-limit text (title 120 / body 20,000) is never written, is never truncated, and blocks switching notes so it cannot be lost. The save status sits above the text so it is never below the fold on phones. A new note reuses an untouched empty one. Extra hooks `AuroraNotes.reload()/open()/flush()/state()` exist for the layout matrix and tests. Not done: multi-tab conflict handling (last write wins), Index search over notes. Visual review by eye at 360 (200% text), 390 light/dark/Clear, 412, 768, 1024, 1280, 1440; it found and fixed the hidden save status on phones. Not eyeballed: high contrast and Soft/Clear differences beyond the shots above (matrix assertions cover them).

Original plan:

- Files: `notes-model.js` (DOM-free), `notes.js`, `notes.css`, `tests/unit/notes.test.js`,
  `tests/ui-golden/notes.test.js`.
- Model: `{ id, title, body, pinned, space, created, updated }`; create, update, delete, restore (one-level
  undo buffer), pin, search (title + body, case-insensitive), sort pinned first then updated; limits (title
  120, body 20 000 chars, 200 notes) with explicit errors.
- Persistence: `localStorage` key `aurora-glass.notes.v1`, wrapped in try/catch with a versioned envelope and
  validation on load (bad data → start empty and show a callout, never crash). Storage unavailable or full →
  visible "Not saved" state. UI label: "Saved in this browser only · not AuroraOS storage". Playwright
  contexts start clean; unit tests inject a fake storage.
- Screen: two-pane >= 768 (list + editor), stacked on phone (list → editor with Back). Search, "New note",
  pin toggle, Space filter. Editor: title input + `textarea`, autosave 600 ms after the last keystroke with a
  status text ("Saving…", "Saved 14:02", "Not saved: …"), word count. Delete → `AuroraUI.confirm` (S2-style
  review wording, not hold: this is prototype-local data, not a broker file) then a toast with Undo.
- Keys: Ctrl/Cmd+N new, Ctrl/Cmd+S save now, Ctrl/Cmd+F focus search, Escape back to list on phone, arrows in
  the list.
- Tests: unit (model + storage failure injection + corrupt data); browser (create, edit, autosave survives
  reload, pin order, search, delete + undo, empty states, limits, markup in a note renders as text, storage
  failure state via `page.addInitScript` overriding `localStorage.setItem`), layout matrix, screenshots.
- Depends on: M1.

## M6. Calculator — `merged (#41)` (calc.test.js unit 122; calculator.test.js 25 Playwright specs; layout matrix 28/28 with 3 new states) — branch `feature/aurora-calculator` (from main)

**Record.** Engine `calc-engine.js` (no eval; scaled-BigInt decimals, 12 significant digits, exponent form, overflow at 1e+99, division by zero is an error until Clear), screen `calculator.js` + `calculator.css`, registered as the app's `screen`. Keypad is 4x5 (C, backspace, %, divide / digits / operators / sign, 0, dot, equals); Clear-Entry is the Delete key, Escape is Clear. History keeps the last 10 completed calculations (tap to reuse). Visual review done by eye at 390 light, 390 dark (error), 1280 dark, 360 at 200% text; it found and fixed: the keypad moved when the display font shrank (fixed line boxes), and at 360/200% the exponent was clipped and the title broke mid-word (the result now shrinks until the whole text fits; the title scales). Not looked at by eye: 412, 768, 1024, 1440, Soft/Clear differences, high contrast (covered by assertions and the layout matrix only).

Original plan:

- Files: `calc-engine.js` (DOM-free), `calculator.js`, `calculator.css`, `tests/unit/calc.test.js`,
  `tests/ui-golden/calculator.test.js`.
- Engine: a small state machine `{ acc, operand, operator, entry, justEvaluated, error }` with inputs
  `digit, dot, op(+ − × ÷), equals, percent, sign, clear, clearEntry, backspace`. **No `eval`/`Function`.**
  Immediate-execution semantics (like a pocket calculator), repeated `=` repeats the last operation.
  Arithmetic on decimal strings via scaled BigInt (so `0.1 + 0.2 = 0.3`), 12 significant digits, results
  beyond that shown in exponent form; division by zero → error state "Can't divide by zero" until Clear;
  entry capped at 12 digits.
- Screen: display (expression line + result in `--text-display`, `aria-live="polite"`, shrinks font to fit,
  horizontally scrollable as last resort), 4×5 keypad of >= 56 px buttons (48 min at 200% text), operator keys
  tinted, `=` primary. Short history list (last 10, tap to reuse) beside the pad >= 768.
- Keyboard: digits, `. ,`, `+ - * /`, Enter or `=`, Backspace, Escape (clear), `%`, F9 or `_` for sign.
  Keys only act while the Calculator screen is active and focus is not in another input.
- Tests: unit table (>= 60 cases: chaining, precedence-free immediate execution, decimals, negative numbers,
  percent, repeated equals, divide by zero and recovery, overflow, backspace edge cases, leading zeros,
  multiple dots); browser (pointer and keyboard paths, error display, history reuse, no layout shift on long
  results), layout matrix, screenshots.
- Depends on: M1.

## M7. Clock — `merged (#44)` (clock.test.js unit 33; clock.test.js Playwright 38 specs with page.clock; layout matrix with 4 new states) — branch `feature/aurora-clock` (from main)

**Record.** `clock-model.js` (stopwatch + timer from an injected monotonic `now()`, never from counting ticks; the clock source can never go backwards), `clock.js`, `clock.css`, registered as the app's `screen`. Tabs follow the ARIA tab pattern. Timer: presets 1/5/10/25 min set the length, h/m/s fields validated 1 s .. 24 h, Start/Pause/Resume/Reset, determinate progress bar (no animation under reduced motion), finish = `alertdialog` + announce + Horizon "Timer finished". A tab that slept recomputes from the clock at the next look; finish fires once. **Broker link:** `AuroraSystem.world.timer` is only the broker's record; the Clock owns the one ticking timer. An Aura `timer.start` is mirrored into it (remaining time from the record's `startedAt`, chip "Started by Aura"); Undo of that receipt cancels it only while the Clock still shows that timer (a timer the user started or reset in the meantime is left alone); a second Aura timer replaces the first and undoing it restores the first with its true remaining time. The Clock's own timers do not write the broker record or Trail. **Not done on purpose:** no sound and no browser Notification (never prompted), no persistence, nothing survives closing the page, no alarms. Visual review by eye at 360 (200% text), 390, 412 (alert), 768, 1024, 1280, 1440: it found and fixed the Stopwatch tab label wrapping inside a pill at 200% text and an empty-looking left-aligned Clock card on desktop.

Original plan:

- Files: `clock-model.js` (DOM-free, clock injected), `clock.js`, `clock.css`, `tests/unit/clock.test.js`,
  `tests/ui-golden/clock.test.js`.
- Model: stopwatch `{ running, startedAt, accumulated, laps[] }` and timer
  `{ state: idle|running|paused|done, duration, endsAt, remaining }`, all computed from an injected monotonic
  `now()` (`performance.now()` in the browser); never from counting ticks. Rendering uses
  `requestAnimationFrame` for the stopwatch while visible and a 250 ms timeout for the timer; when the tab is
  hidden nothing ticks and the next render recomputes from the clock, so there is no drift.
- Tabs: **Clock** (large local time, date, time zone name; no seconds animation under reduced motion),
  **Stopwatch** (start/pause/resume/reset/lap, lap list), **Timer** (presets 1, 5, 10, 25 min; h/m/s inputs
  with validation 1 s – 24 h; start/pause/resume/reset; determinate progress ring or bar).
- Timer end: in-app opaque alert dialog + `announce` + Horizon status "Timer finished"; optional short tone
  via WebAudio only after a user gesture and only if the simulated volume > 0; browser Notification only if
  permission is already granted (never prompt unasked). Copy: alarms that survive closing the page are not
  possible in this prototype.
- Broker link: listens to `AuroraSystem.on("timer")`; a `timer.start` from Aura starts this timer, and its
  receipt undo cancels it. Starting a timer from the Clock UI is a direct S0 user action.
- Tests: unit with a fake clock (pause/resume accumulation, laps, timer completion exactly at duration, large
  jumps as if the tab slept, invalid durations); browser with `page.clock` (install + fastForward) for
  end-state, pause/resume, Aura-started timer, undo cancels; layout matrix; screenshots.
- Depends on: M1.

**R2 (Opus `aurora-visual-director`)** after M5–M7: cross-app consistency review with screenshots.

## M8. Polish backlog — `in progress` (pick in this order, each its own small PR)

**M8-A done: Glass Corner overlap fixed at shell level** (see handoff; `shell-scroll.test.js`).


1. Desktop window feel: at >= 1024 an app screen renders in a centred "window" card with a title bar (app
   tile, name, close = `back()`); no free dragging yet.
2. First-run welcome (3 steps, skippable, never shown again in the session) + contextual hints.
3. Keyboard shortcut sheet (`?`), listing real shortcuts only.
4. Context menus (long-press / right-click / Shift+F10) for file rows and page cards, built on one `menu`
   primitive added to `ui.js` and documented.
5. Index relevance: recency boost, typo tolerance for app names.
6. String table extraction (`strings.js`) to prepare en/hi/gu.
7. Performance pass: render lists with keyed updates where a full re-render is measurable; verify no layout
   thrash while a plan runs.
8. README audit: remove stale claims, link the handoff docs.
9. `aurora-glass/open-design-handoff/native-compositor-mapping.md`: update for the new components (issues #7, #8).

**R3 (Opus)**: final visual and accessibility audit across everything before the last report.

---

## Review gates and model use

| Gate | Agent | When | Output |
|---|---|---|---|
| R0 | `aurora-visual-director` (Opus) | after S9 | Prioritized visual defects for the shell; P0/P1 fixed before S10 |
| R1 | `aurora-quality-reviewer` (Opus) | after M2–M4 | Security + UX findings on permission, history and file flows |
| R2 | `aurora-visual-director` (Opus) | after M5–M7 | Cross-app consistency |
| R3 | both | before the final report | Final audit |

Reviews must cite screenshots or file:line and say what was actually run. Record each review's findings and
what was fixed in the handoff. Never record a review that did not happen.

## Risks

| Risk | Mitigation |
|---|---|
| Foundation never executed (F0) | S1 is small and first; fix foundation defects before building on it |
| Usage limits end a session mid-task | Commit after every green sub-task; update the handoff at each commit; sub-tasks are sized to fit one sitting |
| Stacked PRs waiting for review | Keep each PR focused; M5–M7 branch from M1 only, so they are not blocked by M2–M4 |
| Test suite time on a low-RAM machine (matrix grows with every screen) | One worker; run targeted specs while developing; full suite before each push; CI as the second run |
| Font metrics differ on CI (Linux) | Wide-font stress test in the matrix; no layout that depends on glyph width |
| Security regressions hidden by UI churn | `broker.test.js` unchanged; `aura-plan.test.js` asserts state, not panels; R1 review |
