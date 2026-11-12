# Aurora Glass UI handoff

Last updated: 2026-11-12 (Sonnet 5.5, after merging #30/#31). Update this file at every milestone commit. Anything under "Historical notes" is
background, not current status.

## Current status (verified 2026-11-12, Sonnet 5.5)

Project model: one human owner (solo maintainer). No external reviewer is required; merges need a documented self-review, green CI
(all configured jobs), a mergeable PR and no GitHub-enforced block. `main` has no branch protection.

| | |
|---|---|
| `main` | `29cc122624501097b935187f42bae87917e77856` (merge of #31). Previously `81254bb` -> #30 merged as `b38f87b` (main CI 6/6, run 43249737378) -> #31 merged as `29cc122` |
| Merged | #30 M1 Aurora Glass shell. #31 M2 Passport. Both merged with merge commits after a documented self-review (no kernel files, `broker.js` unchanged, no unsafe sinks, 16-file incremental diff for #31) |
| In progress | M3 Trail on `feature/aurora-trail` (from the #31 head, which is now in `main`): PR to `main` |
| Kernel PRs | #28 AuroraFS and #29 EL0 file access (#29 stacked on #28): open, mergeable, CI 6/6. An Opus model review (read-only, nothing built) says MERGE for both with fixes listed in the kernel section of the final report; the P1 is that all apps share one flat file namespace (name-existence oracle, squatting), so the docs' "indistinguishable from absent" claim is false and must be corrected before merge. **Not merged yet.** |
| Working checkout | `C:\Users\satvi\Desktop\AuroraOS-Claude`. Kernel worktree: `..\AuroraOS-Claude-kernel` (detached at #29 head). The sibling `Desktop\AuroraOS` holds the user's uncommitted kernel work: never touch it |

| Task | Status |
|---|---|
| F0 foundation, M1 shell (S1-S10), M2 Passport | **merged to main** |
| M3 Trail | done on `feature/aurora-trail`: trail-model.js (DOM-free), trail.js, trail.css; unit 21/21 (trail) and 7/7 suites; Playwright 410 passed, 22 skipped (curated screenshots, normal), 0 failed with one worker; 35 of those are trail.test.js; run leaves git status clean |
| M4 Vault, M5 Notes, M6 Calculator, M7 Clock, M8 polish | not started |

M3 visual review: actually looked at Trail at 390 light, 390 dark and 1280 dark (populated: plan, failure, deleted file, selected row,
side panel). Not looked at by eye: 360, 412, 768, 1024, 1440, Clear mode, high contrast, 200% text (the last two and 360/390/768/1280
overflow are only covered by assertions in trail.test.js and the layout matrix, which now includes two Trail states).
Defect found and fixed in review: selected row looked like a card inside a card (group padding tightened).

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

1. Open the M3 Trail PR to `main`, wait for CI 6/6, self-review the incremental diff, merge, verify main CI.
2. Kernel: fix #28/#29 review findings in the kernel worktree (docs honesty first, then the generation-exhaustion fix and the vacuous test), rerun QEMU tests under WSL, push without force, wait for CI, merge #28 then #29.
3. M4 Vault on `feature/aurora-vault` (from `main` once Trail merges). Remember `review-fixes.test.js` test 4 uses Vault as the "reserved app gets a screen" example and must be pointed at another unbuilt app when Vault ships.

```bash
cd C:/Users/satvi/Desktop/AuroraOS-Claude
git fetch && git checkout feature/aurora-trail && git status
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
- Trail PR CI: recorded on the PR.
