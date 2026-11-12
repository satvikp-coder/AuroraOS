# Aurora Glass build playbook: start here

Plan author: Claude Fable 5.2 (`claude-fable-5-2`), 2026-11-11.

This folder is a step-by-step playbook. It is written so that an agent with no memory of earlier sessions can
do the work by following it literally. `../AURORA_UI_TASKS.md` is the short index; these files are the detail.

| File | Milestone | Depends on |
|---|---|---|
| `01-M1-shell.md` | Shell: wiring, navigation, Home, Dock, Spaces, Index, Glass Corner, tests | foundation (done) |
| `02-M2-passport.md` | Passport (permissions) | M1 |
| `03-M3-trail.md` | Trail (history and undo) | M1 |
| `04-M4-vault.md` | Vault (files) | M1, M3 |
| `05-M5-notes.md` | Notes | M1 |
| `06-M6-calculator.md` | Calculator | M1 |
| `07-M7-clock.md` | Clock | M1 |
| `08-M8-polish-and-reviews.md` | Polish backlog, review gates, final report | all |

Do them in file order unless the handoff says otherwise. Inside a file, do the steps in order. Do not skip a
"CHECK" line.

## 0. Words used in this playbook

| Word | Meaning |
|---|---|
| prototype | The static web page in `aurora-glass/prototype/`. It is a design demo in a browser. It is **not** the operating system. |
| repo root | `C:\Users\satvi\Desktop\AuroraOS-Claude`. Every command runs from here. |
| screen | A `<section class="screen" id="screen-NAME">`. Only one is visible at a time. |
| Shell | `window.AuroraShell`, defined in `app.js`. Navigation, apps, Spaces. |
| UI | `window.AuroraUI`, defined in `ui.js`. Helpers: `el`, `icon`, `dialog`, `toast`, ... |
| System | `window.AuroraSystem`, defined in `system.js`. The broker, permissions, mock files, receipts. |
| Review | `window.AuroraReview`, defined in `review.js`. The only code allowed to approve and run actions. |
| Models | `window.AuroraModels`, defined in `screens.js`. `pages` (open apps) and `qs` (quick settings). |
| broker | `broker.js`. Reviewed security code. **Never edit it.** |
| receipt | The broker's record that an action ran, failed or was blocked. |
| S0..S3 | Risk classes. S0 safe, S1 reversible setting, S2 changes files, S3 destructive. |
| unit tests | Node scripts in `tests/unit/`. Fast. |
| browser tests | Playwright specs in `tests/ui-golden/`. Slow. Real Chromium. |
| full suite | All unit tests + syntax check + all browser tests (commands in section 3). |

## 1. Rules that are never broken

1. Never run anything against a phone or a physical disk. No `adb`, no `fastboot`. QEMU only, and UI work
   needs no QEMU at all.
2. Never edit `aurora-glass/prototype/broker.js`. Never edit anything under `kernel/`, `boot/`, `tests/qemu/`,
   `tests/kernel-host/`. Never touch pull requests #28 and #29.
3. Never `git push --force`, never `git reset --hard`, never `git rebase`, never delete a branch you did not
   create in this session, never `git stash drop`.
4. Never use `innerHTML`, `outerHTML`, `insertAdjacentHTML`, `document.write`, `eval` or `new Function`.
   Build elements with `UI.el(...)` and set text with `text:`.
5. Never change, delete, skip or loosen a test just to make it pass. If a test fails, the code is wrong until
   you can explain in one sentence why the test's expectation is obsolete; in that case rewrite the test to
   check the new correct behaviour and say so in the commit message.
6. Never write "passed" for a command you did not run. Copy the real numbers.
7. A screen never changes files, permissions or settings covered by the broker directly. It calls
   `System.request(...)` (section 6).
8. Never call the prototype "AuroraOS", "native", "the OS" or "real" in UI text. Simulated things say
   "simulated" or "in this prototype".
9. Do not add npm packages. Do not add a framework. Do not rename existing ids listed as "keep" in a step.
10. Work only in `C:\Users\satvi\Desktop\AuroraOS-Claude`. Never open `C:\Users\satvi\Desktop\AuroraOS`.

If a step in this playbook seems to require breaking a rule, stop, write what you found in
`../AURORA_UI_HANDOFF.md` under "Blocked", commit, and move to the next independent milestone.

## 2. Before you change anything (every session)

Run these exactly, in order, and read the output.

```bash
cd C:/Users/satvi/Desktop/AuroraOS-Claude
git status                      # CHECK: note any modified or untracked files. Do not delete them.
git fetch
git branch --show-current       # CHECK: compare with "Active UI branch" in ../AURORA_UI_HANDOFF.md
gh pr list --state open         # CHECK: note PR numbers
```

If `git status` shows changes you did not make: they are someone's unfinished work. Read the handoff. If the
handoff explains them, continue them. If not, commit them as they are with the message
`wip: preserve uncommitted work found at session start` and carry on.

First time on this machine only:

```bash
npm ci
npx playwright install chromium
```

## 3. Commands you will use all the time

```bash
# A. syntax check every prototype script (2 seconds)
for f in aurora-glass/prototype/*.js; do node --check "$f" || break; done

# B. unit tests (about 10 seconds). Each prints PASS/FAIL lines and a summary.
for f in tests/unit/*.test.js; do echo "== $f"; node "$f" || break; done

# C. one browser spec file (fast feedback)
npx playwright test tests/ui-golden/shell.test.js --workers=1

# D. one browser test by name
npx playwright test -g "name of the test" --workers=1

# E. all browser tests (several minutes). ALWAYS --workers=1 on this machine.
npx playwright test --workers=1

# F. run the prototype by hand
npx http-server aurora-glass/prototype -p 3000 -s -c-1      # open http://localhost:3000 ; Ctrl+C to stop
```

PowerShell does not understand the `for f in ...` lines. Run them in Git Bash (the Bash tool).

"Port 3000 already in use": a server is still running. Find and stop it:

```bash
netstat -ano | grep ":3000 "          # last column is the process id (PID)
taskkill //PID <PID> //F
```

Never leave a server or browser running when you finish.

## 4. The work loop (repeat for every step)

1. Read the step fully, including its CHECK lines, before typing.
2. Make the change. Small edits: the Edit tool. New files: the Write tool.
3. Run command A (syntax).
4. Run the targeted test named in the step (command C or D).
5. If it fails, read the first failure only, fix the **code**, and run it again. Three failed attempts at the
   same failure: read section 8.
6. When the step's CHECK lines are all true, commit (section 5).
7. Go to the next step.

At the end of every sub-task (S1, S2, ... or the last step of an app): run command B and command E, then
commit, then `git push`, then update the two handoff files (section 7).

## 5. Committing

```bash
git add <the exact files you changed>     # never "git add -A" and never "git add ."
git status                                # CHECK: only your files are staged
git commit -m "feat(ui): <what changed, one line>

<why, and what was verified: commands and counts>

Co-Authored-By: <the model name you are running as> <noreply@anthropic.com>"
git push
```

Prefixes: `feat(ui):` new behaviour, `fix(ui):` bug fix, `test(ui):` tests only, `docs:` documents only,
`refactor(ui):` no behaviour change.

Do not commit files under `test-results/`, `playwright-report/` or `node_modules/` (they are ignored already).

## 6. The one security pattern you must copy

Anything that moves or deletes a file, changes Focus mode, or starts a timer on behalf of Aura goes through
the broker. A screen writes this and nothing else:

```js
const result = await System.request(
  { type: "file.delete", arguments: { id: file.id } },      // ONE action
  { source: "vault", title: "Delete this file?" }
);
// or several actions as one plan:
// await System.request({ steps: [ {type, arguments}, {type, arguments} ] }, { source: "vault" });

if (result.ok)            { /* it really happened: re-render from System.files.list() */ }
else if (result.cancelled){ /* the person said no: do nothing, show nothing */ }
else                      { /* blocked or failed: review.js already showed why. Re-render. */ }
```

Valid action types and arguments (anything else is rejected by the broker):

| type | arguments | risk |
|---|---|---|
| `app.open` | `{ app: "notes"|"files"|"calculator"|"clock"|"settings"|"calendar" }` | S0 |
| `space.switch` | `{ space: "study"|"work"|"projects"|"personal" }` | S0 |
| `timer.start` | `{ seconds: 1..86400 }` (integer) | S0 |
| `search.query` | `{ query: "1..100 chars" }` | S0 |
| `focus.set` | `{ enabled: true|false }` | S1 |
| `file.move` | `{ id: "<file id>", destination: "<space id>" }` | S2 |
| `file.delete` | `{ id: "<file id>" }` | S3 |
| `share.send` | `{ id, to }` | S3, has no adapter: always "not available" |

You never write the approval dialog yourself. `System.request` shows it. You never call `takeTrusted`.

Undo: `await System.undo(receiptId)` or `await System.undoProposal(result.proposalId)`. Show what it returns
(`ok` true or false). Never show "Undone" unless `ok === true`.

## 7. Updating the handoff (after every sub-task)

Edit `docs/agents/AURORA_UI_TASKS.md`: change the sub-task's status word to `done` (only if every CHECK
passed) and add the commit hash.

Edit `docs/agents/AURORA_UI_HANDOFF.md`:

- "Repository state": active branch, open PRs.
- "Done": add one line with the commit hash.
- "Exact next task": the next sub-task id and the file in this folder that describes it.
- "Known bugs": anything you saw and did not fix.

Then commit both with `docs: update UI handoff after <task id>` and push.

## 8. When you are stuck

Work through this list from the top. Stop at the first one that applies.

| Symptom | What to do |
|---|---|
| `node --check` reports a syntax error | Open the file at the line it names. Usual causes: missing `)` or `}` after an `el(...)` call, a stray comma. |
| Browser test: "locator resolved to hidden element" or timeout waiting for an element | Run the prototype by hand (command F), open DevTools console, and type the selector: `document.querySelector("#the-id")`. If `null`, the id is missing or misspelt. If present, check whether an ancestor has `display:none` (the screen is not active) or the `hidden` attribute. |
| Console shows `Cannot read properties of undefined (reading 'x')` | A script loaded before the one it needs. Compare the `<script>` order in `index.html` with the order in `../AURORA_ARCHITECTURE.md` section 2. |
| A test passes alone but fails in the full run | State leaks between tests only through `localStorage` (Notes) or a server left running. Every test must start with `page.goto(BASE_URL)`. |
| Layout matrix reports "page scrolls horizontally" | Find the widest element: in DevTools run `[...document.querySelectorAll("*")].filter(n => n.getBoundingClientRect().right > innerWidth)`. Fix with `min-width: 0`, `flex-wrap: wrap`, `overflow-wrap: anywhere` or `max-width: 100%`. Never with `overflow-x: hidden` on the page. |
| Layout matrix reports "touch target < 44px" | Add `min-height: var(--touch-target); min-width: var(--touch-target)` to that control's class. |
| Something in `ui.js`, `system.js`, `review.js`, `components.css` is wrong | You may fix those files. Say exactly what and why in the commit message. Do not change what `system.js` permits (the `permissions.set` policy) and do not let anything but `review.js` approve. |
| The same failure three times | Stop changing code. Write the failing command, the first 30 lines of output and what you tried into the handoff under "Blocked". Commit. Start the next independent milestone (M5, M6 and M7 only need M1). |
| Usage or context is about to run out | Stop starting new steps. Make the tree consistent (syntax check passes), commit with `wip:` if tests are red and say so in the message, push, update the handoff with the exact step you were on. |

## 9. Looking at the screen (required, not optional)

Tests cannot see ugly. After every screen you build, capture and open screenshots.

Save this once as `C:\Users\satvi\AppData\Local\Temp\aurora-shot.js` (outside the repo):

```js
// usage: node aurora-shot.js <screen-id> [outDir]      (prototype server must be running on :3000)
const { chromium } = require("C:/Users/satvi/Desktop/AuroraOS-Claude/node_modules/playwright");
const screen = process.argv[2] || "screen-home";
const out = process.argv[3] || "C:/Users/satvi/AppData/Local/Temp/aurora-shots";
require("fs").mkdirSync(out, { recursive: true });
const combos = [
  { w: 360, theme: "light", tier: "soft" }, { w: 390, theme: "dark", tier: "soft" },
  { w: 390, theme: "light", tier: "clear" }, { w: 768, theme: "light", tier: "soft" },
  { w: 1280, theme: "light", tier: "soft" }, { w: 1280, theme: "dark", tier: "soft" },
  { w: 1440, theme: "dark", tier: "clear" },
  { w: 390, theme: "light", tier: "soft", a11y: true }, { w: 1280, theme: "dark", tier: "soft", a11y: true },
];
(async () => {
  const b = await chromium.launch();
  for (const c of combos) {
    const p = await b.newPage({ viewport: { width: c.w, height: c.w < 768 ? 844 : 900 } });
    await p.goto("http://localhost:3000");
    await p.evaluate(([c, screen]) => {
      AuroraShell.setAppearance(c.theme); AuroraShell.setTier(c.tier);
      if (c.a11y) { const s = AuroraSettingsModel; s.set("accessibility.textSize", "200"); s.set("accessibility.highContrast", true); }
      AuroraShell.showScreen(screen);
    }, [c, screen]);
    await p.waitForTimeout(250);
    await p.screenshot({ path: `${out}/${screen}-${c.w}-${c.theme}-${c.tier}${c.a11y ? "-a11y" : ""}.png`, fullPage: true });
    await p.close();
  }
  await b.close(); console.log("saved to", out);
})();
```

Then open each PNG with the Read tool and answer these ten questions. Any "no" is a defect to fix now.

1. Do all cards and headings share one left edge?
2. Is the padding inside every card the same?
3. Is there exactly one filled blue (primary) button in view, or none?
4. Do heading sizes go big → medium → small in reading order, with no two levels looking the same?
5. Is every piece of text fully visible (no cut-off letters, no "..." where the full text matters)?
6. Does anything sit under or touch the round Glass Corner button (top right) or the Dock (bottom)?
7. Can you read every label easily against its background, in the dark picture too?
8. In the `-a11y` pictures (200% text + high contrast): is everything still on the page, wrapped, not overlapping?
9. At 1280 and 1440: does it use the width sensibly (columns), not a thin phone column in a sea of empty space
   and not text lines wider than about 80 characters?
10. Does it look like the other screens (same buttons, same chips, same spacing)?

Write in the commit message or PR body which pictures you opened. Do not claim you looked at pictures you
did not open.

## 10. Pull request template

```bash
gh pr create --base main --head <branch> --title "feat(ui): <milestone>" --body-file <a file you wrote>
```

Body:

```
## What
<3 to 6 bullets>

## Depends on
<"Nothing" or "PR #N (stacked): merge that first">

## How it was verified
Local (Windows, Chromium, 1 worker):
- unit: <N> files, <P> passed, <F> failed
- node --check: <N> files OK
- playwright: <P> passed, <F> failed, <S> skipped
CI: <link to run> (<status>)

## Screenshots looked at
<list of viewport/theme combinations actually opened>

## Not verified / limits
<be specific>

Browser prototype only. Nothing here is a native AuroraOS feature.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
```

After creating it: `gh pr checks <number> --watch`. If a job fails: `gh run view <run-id> --log-failed`, fix,
commit, push. Do not merge unless the repository's review rules are satisfied. Never use `--admin`.
