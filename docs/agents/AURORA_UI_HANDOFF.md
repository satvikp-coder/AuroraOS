# Aurora Glass UI handoff

Last updated: 2026-11-13 (Sonnet 5.5; M1-M4 and kernel milestones B-C merged)
background, not current status.

## Current status (verified 2026-11-12, Sonnet 5.5)

Project model: one human owner (solo maintainer). No external reviewer is required; merges need a documented self-review, green CI
(all configured jobs), a mergeable PR and no GitHub-enforced block. `main` has no branch protection.

| | |
|---|---|
| `main` | `9d4afab61cc7705522537db981f62dcca1116903`. Main CI 6/6: https://github.com/satvikp-coder/AuroraOS/actions/runs/43284806714 |
| Merged (all with merge commits, after a documented self-review and 6/6 CI on the exact head) | #30 M1 shell, #31 M2 Passport, #32 M3 Trail, #33 M4 Vault (`1d5e2dc` / earlier), #28 AuroraFS (`aa37c17`), #29 EL0 file access (`4e5cae6`) |
| Open PRs | none |
| Kernel | milestones B (AuroraFS) and C (EL0 file access) are on main with the review fixes (per-owner names, exhausted-slot skip, real PID-reuse test). Issue #6 stays OPEN: init, program loader (D) and the native settings registry (E) are not built. Status comment posted on the issue. Kernel worktree: `..\AuroraOS-Claude-kernel` (use an LF copy inside WSL to run QEMU tests; see the dev-environment memory). The sibling `Desktop\AuroraOS` holds the user's uncommitted kernel work: never touch it |

| Task | Status |
|---|---|
| F0, M1 shell, M2 Passport, M3 Trail, M4 Vault | **merged to main** (unit 8/8 suites; Playwright 465 passed, 22 skipped, 0 failed; layout matrix 28/28) |
| M6 Calculator | **done, PR open** (branch `feature/aurora-calculator`; unit 122, Playwright 25, layout matrix 28/28) |
| M5 Notes, M7 Clock, M8 polish | not started |
| Kernel D program loader, E settings registry | not started |

M4 visual review: actually looked at Vault at 390 dark (select mode, two selected), 390 light (preview), 1280 dark (grid + side preview), 1280 light (list + preview). Not looked at by eye: 360, 412, 768, 1024, 1440, Clear mode, high contrast, 200% text, reduced motion (covered by assertions in vault.test.js and the layout matrix only).
Defects found and fixed in review: selected rows showed two check marks (redundant mark removed; the filled checkbox plus aria-checked remain); after a move or delete on a scrolled page the result with Undo / View in Trail was off-screen (now scrolled into view); the Glass Corner covered the sticky selection bar, the preview's top-right Close and the search field when scrolled (bar no longer sticky, Close moved into the action row, search reserves room for the corner below 1024 px); the Vault Space chips reused the Spaces screen's focus keys (renamed).
Behaviour changes that older tests had hard-coded: Files (Home Continue, Dock, Index, openApp) now opens Vault instead of the Pages screen; Index file results open Vault with that file previewed. Those assertions were updated, not removed. Tests that used Vault (or Trail) as "an app that is not built yet" now use a synthetic system app, so they no longer depend on which real apps exist.

Shell-wide limitation found in M4 (not fixed here): the Glass Corner is a fixed circle at the top right of the viewport, so on ANY long screen, once the page has scrolled, a full-width control that reaches the top edge can sit under it. Vault reserves room beside its search field below 1024 px and moves its preview Close button away from the corner, and the layout-matrix entries reset the scroll to the top (the scroll position Playwright leaves after a click depended on font metrics and differed between local runs and CI, which is how PR #33 first failed CI). A shell-level fix (for example a reserved gutter in `.content`) belongs in M8.

Known limits of Vault: it lists the sample files only (not AuroraFS) and shows nothing persistent. Moves skip files already in the destination and say so. More than 12 files are split into several reviews (the broker limit), each with its own approval; cancelling one stops the rest. A request Passport refuses never runs, so Trail has no receipt for it. Undo comes from the broker's receipts; if the broker rejects it (for example the file was deleted afterwards) Vault says "Undo failed" and the file is unchanged.

Known limits of Trail: groups are built from broker receipts plus `proposer.getProposal`. A request the review refuses (for example
Passport denies it before approval) never ran, so it has no receipt and does not appear. "Stopped" plans show only the steps that
ran. A failed Undo shows the broker's generic text ("Undo did not work"); the broker does not keep a finer reason.

Open product decision (unchanged): Aura mode "Ask before acting" behaves like "Quick Commands only" (only Off is checked).

Test side effect: normal runs no longer modify tracked files (CI asserts a clean tree).

## Historical notes (superseded)

- Fable's F0 handoff said the foundation had never run in a browser and that `index.html` still loaded the old scripts. That
  was true before S1 and is no longer true: the full chain loads with zero console errors.
- An Opus engineer started M1 and was cut off by a usage limit before writing any file; nothing from it exists.

## Exact next task

1. **M5 Notes** on `feature/aurora-notes` from `main` (see `AURORA_UI_TASKS.md` M5: DOM-free notes model, versioned `localStorage` envelope with validation and corrupt-data handling, autosave, undo-delete, two-pane layout). Then M6 Calculator, M7 Clock, M8 polish (including a shell-level fix for the Glass Corner floating over scrolled content).
2. **Kernel** in the separate worktree: first close the known limits listed in `docs/kernel/userspace.md` (per-principal quotas, id-based existence leak, the missing EL0 negative tests), then milestone D (program loader: validated executable format, run a file loaded from persistent storage at real EL0) and E (typed, authorized, persistent settings registry). Do not close issue #6 until all of its acceptance criteria are verified in QEMU.

```bash
cd C:/Users/satvi/Desktop/AuroraOS-Claude
git fetch && git checkout main && git pull --ff-only && git status
npm ci
for f in tests/unit/*.test.js; do node "$f" || break; done
npx playwright test --workers=1
```

## Decisions already made (do not reopen without a reason)

- Vanilla JS, no framework; incremental rework of the existing prototype.
- One broker instance (`system.js`), one trusted review surface (`review.js`); screens only propose.
- S3 permissions (`files.delete`, `external.send`) can never be set to Allow. `share.send` has no adapter.
- The old "plan runs when you tap the suggestion" behaviour is a contract violation and is removed; the two
  tests that relied on it must be rewritten to go through approval (not deleted).
- The unreachable "Prototype controls" panel is removed; its switches live in Settings.
- Apps self-register and create their own screen (`addScreen`); `index.html` only gains script/link tags.
- No persistence by default; Notes may use labelled, browser-only `localStorage`.
- Wallpaper is a static CSS gradient; no image assets, so there are no asset credits to track.
- UI milestones are stacked branches with one focused PR each.

## Deliberately deferred

Free-floating desktop windows, drag and drop, real localisation, sound design, Calendar/Camera/Phone apps,
any native compositor work, and anything that depends on kernel PRs #28/#29.

## Screenshots

`aurora-glass/screenshots/` still holds the **old** design's images. They are regenerated in S9.

## Known bugs / risks

See "F0 … Known things to check the first time it runs" and "Risks" in `AURORA_UI_TASKS.md`.

## CI links

- `main` at `81254bb`: https://github.com/satvikp-coder/AuroraOS/actions/runs/42500835937 (green)
- `main` at `b38f87b` (after #30): https://github.com/satvikp-coder/AuroraOS/actions/runs/43249737378 (6/6 green)
- `main` at `a9da43c` (after #32): https://github.com/satvikp-coder/AuroraOS/actions/runs/43257017512 (6/6 green)
- `main` at `9d4afab` (after #29): https://github.com/satvikp-coder/AuroraOS/actions/runs/43284806714 (6/6 green)
