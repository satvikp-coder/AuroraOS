# AuroraOS

An independent ARM64 operating system (QEMU `virt` only) plus **Aurora Glass**, its UI, which today exists as a
browser design prototype. Repository: https://github.com/satvikp-coder/AuroraOS

**Start here:** the active handoff is [`docs/agents/AURORA_UI_HANDOFF.md`](docs/agents/AURORA_UI_HANDOFF.md).
To resume UI work in a new session, follow [`docs/agents/SONNET_RESUME_PROMPT.md`](docs/agents/SONNET_RESUME_PROMPT.md).

## Two separate things

| | Kernel | Aurora Glass prototype |
|---|---|---|
| Where | `kernel/`, `boot/`, `build.sh`, `tests/qemu/`, `tests/kernel-host/` | `aurora-glass/prototype/`, `tests/unit/`, `tests/ui-golden/` |
| What | Real ARM64 kernel: memory, processes, IPC, virtio-blk, AuroraFS | Static HTML/CSS/JS design prototype run in a host browser |
| Docs | `docs/kernel/` | `docs/agents/AURORA_ARCHITECTURE.md`, `AURORA_DESIGN_SYSTEM.md` |

The prototype is **not** the OS and never talks to the kernel. Never describe a browser feature as a native OS
service, and never claim hardware or production readiness.

## Hard rules

- **No physical devices.** QEMU only. The Samsung Galaxy F04 stays disconnected: no ADB, fastboot, flashing.
  Only disposable QEMU disk images; never a physical disk.
- Never force-push or rewrite history. Never bypass review requirements, branch protection or tool permissions.
- Security-sensitive kernel changes need deeper verification and negative tests plus a documented review before merge (the
  owner is a solo maintainer; no external reviewer is required). Do not modify kernel PRs #28/#29 from UI branches; use a
  separate worktree.
- Never hide a failing test, weaken an assertion to get green, or report a result you did not run.
- No credentials in the repo; do not disable secret scanning (Gitleaks runs in CI).
- Do not touch the old workspace `C:\Users\satvi\Desktop\AuroraOS`. Work in `C:\Users\satvi\Desktop\AuroraOS-Claude`.
- Issue #6 (filesystem and storage) covers virtio-blk, AuroraFS, EL0 file access, the program loader and the native settings registry; all are implemented and tested in QEMU only (see docs/kernel/). Any change to them needs the same deeper verification and a documented self-review.

## Design source precedence

1. `DESIGN.md`: visual contract (tokens, tiers, components, accessibility).
2. `AuroraOS_Final_Blueprint_OpenDesign.md`: feature semantics, Aura risk/permission behaviour, feasibility.
3. `docs/agents/AURORA_DESIGN_SYSTEM.md`: how the contract is implemented (tokens, component classes).

## UI stack and workflow

Vanilla HTML/CSS/JS, no framework, no bundler, no runtime dependencies (dev: Playwright, jsdom, http-server).
Do not add dependencies or migrate frameworks.

```bash
npm ci                                              # once
npx http-server aurora-glass/prototype -p 3000 -s   # run the prototype at http://localhost:3000
for f in tests/unit/*.test.js; do node "$f" || break; done      # unit tests (Node + jsdom)
for f in aurora-glass/prototype/*.js; do node --check "$f"; done
npx playwright test --workers=1                     # real-browser tests (one worker: low-RAM machine)
```

Kernel build and QEMU tests need the WSL toolchain; see `docs/kernel/` and `tests/qemu/*.sh`. Run heavy QEMU
suites one at a time. Details for all UI test commands: `docs/agents/AURORA_TESTING.md`.

## UI security rules (short form)

- Screens only **propose** actions: `AuroraSystem.request(payload)`. Only `review.js` approves or executes.
- Risk contract: S0 runs; S1 preview; S2 explicit review; S3 hold-to-confirm. Do not add shortcuts.
- Permissions change only through `AuroraSystem.permissions.set`; S3 permissions can never be pre-approved.
- Receipts and undo results shown in the UI must be the broker's real results.
- No `innerHTML`, `eval` or network. Treat names, queries, file metadata and user text as untrusted.
- `broker.js` is reviewed security code. Do not weaken it.

## PR conventions

- One focused milestone per branch: `feature/aurora-<milestone>`. PRs target `main`; CI runs only for PRs to
  `main`. UI milestones may be stacked (say so in the PR body: "depends on #N").
- Commit style: `feat(ui): ...`, `fix(ui): ...`, `test(ui): ...`, `docs: ...`, `feat(kernel): ...`.
- PR body: what changed, how it was verified (commands + counts, local vs CI), what was not verified, limits.
- Merge only when CI is green and the repository's review requirements are met.

## Definition of done (UI)

No dead-end buttons. Works in Clear mode, light and dark, high contrast, reduced motion, 200% text, and
keyboard only. No horizontal overflow from 360 to 1440 px. 48 px touch targets. Honest copy for anything
simulated. Unit + Playwright tests added and passing. Screenshots looked at, not just captured. Handoff and
task list updated.
