# Aurora Glass UI handoff

Last updated: 2026-11-12 (Sonnet 5.5). Update this file at every milestone commit. Anything under "Historical notes" is
background, not current status.

## Current status (verified)

| | |
|---|---|
| `main` | `81254bb9c1ccf21aeff57d25285579262e06e504` |
| UI PR | #30 `feature/aurora-shell-polish` (open, no review decision yet). Head `58e7007`: all 6 CI jobs passed (run 43206776136) |
| Kernel PRs | #28 AuroraFS recovery and #29 EL0 file access (#29 depends on #28): open, awaiting independent review. **UI work must not modify or merge them.** |
| Working checkout | `C:\Users\satvi\Desktop\AuroraOS-Claude`. The sibling `Desktop\AuroraOS` is stale and holds the user's uncommitted kernel work: never touch it |

| Task | Status |
|---|---|
| F0 foundation | done (commit `fee394a`) |
| M1 S1 wiring, S2 Shell API | done (commit `78d19e1`); 97/97 Playwright locally, 4/4 unit suites, 6/6 CI |
| M1 S3 Home and Dock | done (this branch; 152/152 Playwright, 4/4 unit locally) |
| M1 S4 Spaces | done (180/180 Playwright locally) |
| M1 S5 to S10 | not done (see `AURORA_UI_TASKS.md`; each step carries its own status) |
| M2 to M8 | not started |

Verification honesty: only one screenshot (Aura Plan review at 390 px) has been inspected for the new design. The full
viewport/theme matrix has NOT been run. `plan/01..08-*.md` were announced by Fable but never written;
`AURORA_UI_TASKS.md` is the plan of record.

Test side effect until S8: the existing browser specs overwrite tracked PNGs in `aurora-glass/screenshots/`. Before
committing, `git status` them; restore only files this run modified (`git checkout -- aurora-glass/screenshots`) and remove
untracked PNGs this run created.

## Historical notes (superseded)

- Fable's F0 handoff said the foundation had never run in a browser and that `index.html` still loaded the old scripts. That
  was true before S1 and is no longer true: the full chain loads with zero console errors.
- An Opus engineer started M1 and was cut off by a usage limit before writing any file; nothing from it exists.

## Exact next task

`AURORA_UI_TASKS.md` → **M1 / S5 "Index"**, then S6…S10 in order (S1 to S4 are done).

```bash
cd C:/Users/satvi/Desktop/AuroraOS-Claude
git fetch && git checkout feature/aurora-shell-polish && git status
npm ci
for f in tests/unit/*.test.js; do node "$f" || break; done      # baseline before changing anything
npx playwright test --workers=1                                  # baseline (expect green; if not, tokens.css is the cause)
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
- `feature/aurora-shell-polish`: no CI yet (CI runs only for pull requests targeting `main`).
