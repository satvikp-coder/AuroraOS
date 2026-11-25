# AuroraOS — Agent Governance (AGENTS.md)

## Repository identity

AuroraOS is an independent, open-source ARM64 operating system with the Aurora Glass UI and offline Aura AI assistant. It is **not** an Android ROM, Linux distribution, or launcher. First target: QEMU ARM64 `virt`. Experimental phone target: Samsung Galaxy F04 (research only, never flashed from this repo).

## Safety rules (non-negotiable)

1. **Never flash or modify the Samsung Galaxy F04.** All low-level development targets QEMU ARM64 `virt`.
2. **Never commit credentials, private keys, or secrets.** Gitleaks scans CI; `.gitleaksignore` only allows fingerprint exclusions.
3. **Do not modify or push to `main` directly.** All changes go through a feature branch → PR → review → CI → merge.
4. **Each worker must operate inside its assigned Git worktree.** Verify with `git worktree list` before editing.
5. **No overlapping implementation tasks on the same files.** Coordinate through the coordinator before touching shared files (kernel.c, linker.ld, build.sh, ci.yml, DESIGN.md).
6. **Read-only audits first.** Never implement before inspecting the actual current state (git status, existing code, CI config).
7. **Verify baseline before implementation.** Every agent must confirm `git rev-parse HEAD` matches `git rev-parse origin/main` (or the assigned base branch) before writing code.

## Required workflow for every implementation task

1. Create a dedicated branch and worktree from the current integration base (verified origin/main).
2. Implement the scoped change only.
3. Inspect the actual code changes (read the diff).
4. Run targeted tests (Node.js UI tests locally; ARM64 kernel build/boot via WSL or CI).
5. Obtain independent technical review from an agent that did not implement the change.
6. Correct identified defects.
7. Run regression tests.
8. Commit on the feature branch.
9. Open a pull request targeting `main`.
10. Verify required CI checks pass (all jobs on the exact head SHA).
11. Merge only with required approvals and governance conditions satisfied.
12. Verify main-branch CI after merging.

An AI review does not replace required human GitHub approvals.

## Available agents and their roles

| Agent | Model | Role |
|-------|-------|------|
| coordinator | Nemotron Super | Architecture, integration, decisions requiring repository-wide understanding |
| kernel-worker | Nemotron Ultra | ARM64 kernel, MMU, scheduler, syscalls, exceptions, boot, drivers |
| implementation-worker | Nemotron Super | Native OS features, userspace, drivers, apps, UI |
| fast-worker | Laguna | Testing, documentation, CI, smaller tasks |
| reviewer | Nemotron Super | Read-only review of changes not made by the reviewer |

## Current project state (as of verified origin/main)

- **Branch:** `main` at `e24f8914d3689c28215aeb6bbd82435b1657a443`
- **CI:** Latest run #350 SUCCESS (https://github.com/satvikp-coder/AuroraOS/actions/runs/45185240324)
- **CI jobs:** ui-tests, ui-browser-tests, prototype-build, kernel-build, kernel-qemu-boot (25+ test suites), repo-integrity
- **Open PR:** #158 (docs/model): trust and signing design (design only)
- **Open issues:** #7-#15 (9 issues)

**Already implemented (142+ closed PRs):**
- Production ARM64 kernel with W^X MMU, EL0 processes, scheduler, syscalls, IPC
- AuroraFS, virtio-blk, virtio-input, virtio-snd, fwcfg, virtqueue
- Native graphics (ramfb, drawing, text, Noto Sans Mono font)
- Native shell, Aurora Glass UI (Tier 0 Clear), home, dock, spaces, index
- Calculator, Notes, Files apps
- Passport, Trail, Trusted review, Action Broker
- SYS_MEM, FP/SIMD, NEON, SYS_MODEL, AURMODEL
- Native int8 matvec primitive
- 50+ host tests, 16 jsdom unit tests, 50+ QEMU test suites

## Build and test commands

- Kernel: `./build.sh` (requires aarch64-linux-gnu cross toolchain — available in WSL kali-linux)
- QEMU boot: `./run-qemu.sh [kernel-image]` (requires qemu-system-aarch64 — available in WSL kali-linux)
- Automated boot smoke test: `./tests/qemu/boot-smoke-test.sh`
- Host kernel tests: `cd tests/kernel-host && ./run.sh`
- UI unit tests: `node tests/unit/aurora-glass.test.js` (Node.js, jsdom)
- UI browser tests: `npx playwright test` (requires Playwright Chromium)

## Open issues (dependency chain: #7→#8→#9→#10→#11→#12→#13→#14→#15)

| # | Title | Status | Remaining |
|---|-------|--------|-----------|
| #7 | Native graphics renderer | Core done (PRs #55-#66) | Non-ASCII text, EL0 UI boundary |
| #8 | Aurora Glass native UI | Partially done (PRs #64-#80) | Pages screen, app windows, persistence |
| #9 | Spaces and native apps | Partially done (PRs #84, #90, #96) | Working timer, app launcher, native apps |
| #10 | Aura Quick Commands | Partially done (PRs #78, #85, #87, #91) | S1+ execution, trusted review, timers |
| #11 | Passport and Trail | Partially done (PRs #88, #103-#109) | Trusted review, S1/S2/S3 actions, hold-to-confirm |
| #12 | Local model inference | Host-side feasibility done | Native runtime on AuroraOS |
| #13 | Offline speech | NOT started | Audio capture, STT, TTS, mic permission |
| #14 | Public QEMU emulator beta | NOT started | Package manifest, integrity hashes, docs |
| #15 | Galaxy F04 feasibility | Research only | GO/CONDITIONAL GO/NO-GO decision |
