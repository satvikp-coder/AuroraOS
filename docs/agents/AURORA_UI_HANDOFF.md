# Aurora Glass UI handoff

Last updated: 2026-11-12 (Sonnet 5.5 continuation). Update this file at every milestone commit.

## Repository state

| | |
|---|---|
| Verified `main` | `81254bb9c1ccf21aeff57d25285579262e06e504` (merge of #27; CI green, run 42500835937) |
| Active UI branch | `feature/aurora-shell-polish` (from `main`) |
| Open PRs | #28 AuroraFS recovery (kernel, awaiting independent review); #29 EL0 file access (kernel, depends on #28). **Do not modify or merge them from UI work.** UI PR for S1+S2: see the PR list (`gh pr list`). |
| Working tree | Clean after the docs commit that added this file |

## Done

- **F0 design-system foundation** (commit `fee394a`): `tokens.css` (extended), `components.css`, `ui.js`,
  `system.js`, `review.js`, `AURORA_ARCHITECTURE.md`, project agents in `.claude/agents/`.
- Handoff documents: `CLAUDE.md`, this file, `AURORA_DESIGN_SYSTEM.md`, `AURORA_UI_TASKS.md`,
  `AURORA_TESTING.md`, `AURORA_ARCHITECTURE.md`, `SONNET_RESUME_PROMPT.md`.

## Done in the Sonnet continuation (2026-11-12)

- Recovered Fable work: `docs/agents/plan/00-START-HERE.md` was untracked and is now committed. **Missing from Fable's handoff:** `plan/01..08-*.md` were announced but never written; `AURORA_UI_TASKS.md` is the plan of record.
- **S1 + S2 done** (commit `78d19e1`): foundation runs in the browser with zero console errors; `AuroraShell` implements the documented API; the fake plan flow is gone (plans go through review.js and the broker); `data-motion="reduce"` honoured in tokens. Verified: `node tests/unit/*.test.js` 4/4, `npx playwright test --workers=1` 97/97 (25 new in `shell.test.js`).
- Four older specs were rewritten (not weakened) to approve a plan before expecting a receipt.
- Repo state note: the working checkout for UI work is `C:UserssatviDesktopAuroraOS-Claude`. The sibling `DesktopAuroraOS` checkout is stale (`main` at 79e1ff8) and holds uncommitted kernel work; do not touch it.
- Test side effect to remember: the existing browser specs overwrite tracked PNGs in `aurora-glass/screenshots/`. Run `git checkout -- aurora-glass/screenshots` (and delete untracked PNGs there) before committing until S8 moves screenshots to `testInfo.outputPath`.

## Not done / important caveats

- **The foundation has never run in a browser.** Only `node --check` was run on `ui.js`, `system.js`,
  `review.js`. `index.html` still loads the old five scripts and two stylesheets, so the prototype on this
  branch currently behaves exactly like `main` (old look, fake Aura plan). The existing test suites were not
  re-run on this branch; since no loaded file except `tokens.css` changed, the only regression risk is from the
  token changes (new values for `--color-success/warn/danger`, `--radius-*`, `--backdrop-blur`, font stack).
  Run the full UI suite before building further.
- An Opus engineer was started on M1 (shell rework) and was cut off by a usage limit before writing any file.
  Nothing from that attempt exists on disk. No Opus review has happened.
- No UI feature of milestones M1–M8 is implemented. No screenshots have been inspected for the new design.
- `AURORA_DESIGN_SYSTEM.md` and `AURORA_ARCHITECTURE.md` describe the **target** shell API
  (`AuroraShell.back/addScreen/registerApp/openApp/openIndex`, `layout-matrix.test.js`, curated screenshots):
  those parts do not exist yet; they are specified in `AURORA_UI_TASKS.md` S1–S9.

## Exact next task

`AURORA_UI_TASKS.md` → **M1 / S3 "Home and Dock"**, then S4…S10 in order (S1, S2 are done).

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
