# Aurora Glass testing guide

All commands run from the repository root. Windows: use Git Bash or PowerShell; everything below is plain
Node/npm. The machine has little RAM: **one Playwright worker, one suite at a time**, and stop servers and
browsers you start.

## Setup (once)

```bash
npm ci
npx playwright install chromium
```

## Unit tests (Node, some with jsdom)

```bash
node tests/unit/aurora-glass.test.js   # structure of index.html, tokens, accessibility attributes
node tests/unit/core.test.js           # Pages + quick-settings models
node tests/unit/broker.test.js         # Action Broker contract (security)
node tests/unit/settings.test.js       # settings registry
node tests/unit/system.test.js         # system.js in a vm: permission policy, describe/explain, trusted surface, mock files
# all of them:
for f in tests/unit/*.test.js; do node "$f" || break; done
```

Each file prints PASS/FAIL lines and exits non-zero on failure. New DOM-free logic (calculator engine, clock
maths, notes model, ...) gets its own `tests/unit/<name>.test.js` and must be added to `.github/workflows/ci.yml`.

## Syntax check

```bash
for f in aurora-glass/prototype/*.js; do node --check "$f" || break; done
```

## Browser tests (Playwright, real Chromium)

```bash
npx playwright test --workers=1                              # everything
npx playwright test tests/ui-golden/settings.test.js --workers=1
npx playwright test -g "Aura plan" --workers=1               # by title
npx playwright test --workers=1 --headed                     # watch it
```

`playwright.config.js` starts `http-server` on port 3000 itself. If port 3000 is already taken by a stale
server, stop it first (`npx kill-port 3000`, or find the node process and end it).

## Responsive and accessibility matrix

`tests/ui-golden/layout-matrix.test.js` opens every screen at 360, 390, 412, 768, 1024, 1280 and 1440 px, in
light/dark and Soft/Clear, in default mode and in accessibility mode (200% text, high contrast, reduced
motion), and fails on: horizontal page scroll, controls outside the viewport, targets under 44 px, clipped
headings, dock overflow, controls under the Glass Corner, dialogs whose actions cannot be reached, console
errors. A wide-font stress run (in settings.test.js) catches layouts that only fit with narrow glyphs.
The matrix also has a 150% text mode. 28 viewport/theme/tier tests x 3 modes x every registered screen
(924 states at the time of writing). **Every new screen must be added to the matrix** by calling
`registerScreen({ id, label, root, open })` in `tests/ui-golden/screen-registry.js`.

```bash
npx playwright test tests/ui-golden/layout-matrix.test.js --workers=1
```

## Screenshots

Every spec writes screenshots through `tests/ui-golden/shots.js`: to `test-results/` (git-ignored) normally, so a
test run leaves `git status` clean (CI checks this). To refresh the curated reference set in
`aurora-glass/screenshots/` deliberately:

```bash
AURORA_SHOTS=1 npx playwright test --workers=1 tests/ui-golden/aurora-glass-browser.test.js   # PowerShell: $env:AURORA_SHOTS=1; npx ...
```

## Spec files (what each one guards)

| Spec | Guards |
|---|---|
| `shell.test.js` | AuroraShell contract: history/back, app registry and openApp results, Spaces API validation, global keys |
| `home-dock.test.js` | Home (clock, Ask, Continue, pins, Space card, Aura suggestion, Horizon) and the Dock |
| `spaces.test.js` | Spaces cards, create/rename/switch, limits, validation, renamed Spaces everywhere |
| `index.test.js` | Index combobox, ranking, limits, keyboard, Aura commands and the broker boundary |
| `screens.test.js` | Pages, Quick Center, Settings restyle, the simulated-failure switch, the shared reset dialog |
| `glass-corner.test.js` | Held-items popover, badge, dismissal, Horizon as one status source |
| `aura-plan.test.js` | Plan review/approval/undo/stop/failure, permissions, S3 hold, unsupported actions, Aura off |
| `layout-matrix.test.js` | Layout across 7 viewports x themes x tiers x modes x all registered screens |
| `pages-quick.test.js`, `settings.test.js`, `aurora-glass-browser.test.js` | Earlier specs; they guard ids, roles and data-* attributes the restyled screens must keep |

Looking at screenshots is part of the work: open the PNGs (the Read tool renders them) and check composition,
spacing, hierarchy, clipping and contrast. Automated checks do not replace this, and a report must say which
combinations were actually looked at.

For ad-hoc captures, a throwaway script is enough:

```js
// shot.js  ->  node shot.js   (with the server running on :3000)
const { chromium } = require("playwright");
(async () => {
  const b = await chromium.launch(); const p = await b.newPage({ viewport: { width: 390, height: 844 } });
  await p.goto("http://localhost:3000");
  await p.evaluate(() => { AuroraShell.setAppearance("dark"); AuroraShell.showScreen("screen-spaces"); });
  await p.screenshot({ path: "spaces-390-dark.png", fullPage: true }); await b.close();
})();
```

## Manual keyboard pass (do it for every new screen)

Tab through everything in order; Enter/Space activates; Escape closes or goes back; arrow keys work in
segmented controls, tabs and Index results; focus is always visible and never lands on `<body>` after a
re-render; dialogs trap focus and return it.

## CI

`.github/workflows/ci.yml` runs on pull requests targeting `main` and on pushes to `main`: UI unit tests,
Playwright (`--workers=1`), prototype file/syntax check, kernel build, QEMU suites, and repository integrity
(Gitleaks). Check a PR with:

```bash
gh pr checks <number>
gh run view <run-id> --log-failed
```

Report local and CI results separately, with exact counts, and say what was not run.

## Kernel tests (not needed for UI work)

WSL toolchain; scripts in `tests/qemu/` and `tests/kernel-host/run.sh`. Run one QEMU suite at a time. Never
attach a physical device or disk.
