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

## Required workflow for every implementation task

1. Create a dedicated branch and worktree from the current integration base.
2. Implement the scoped change only.
3. Inspect the actual code changes (read the diff).
4. Run targeted tests (Node.js UI tests locally; ARM64 kernel build/boot via CI).
5. Obtain independent technical review from an agent that did not implement the change.
6. Correct identified defects.
7. Run regression tests.
8. Commit on the feature branch.
9. Open a pull request.
10. Verify required CI checks pass.
11. Merge only with required approvals and governance conditions satisfied.
12. Verify main-branch CI after merging.

An AI review does not replace required human GitHub approvals.

## Available agents and their roles

| Agent | Model | Role |
|-------|-------|------|
| coordinator | Nemotron Super | Architecture, integration, decisions requiring repository-wide understanding |
| kernel-worker | Nemotron Ultra | ARM64 kernel, MMU, scheduler, syscalls, exceptions, boot |
| implementation-worker | Nemotron Super | Independent OS features, userspace, drivers, apps |
| fast-worker | Laguna | Testing, documentation, CI, smaller tasks |
| reviewer | Independent | Technical review of changes not made by the reviewer |

## Current project state (as of audit)

- Branch: `feature/kernel-memory-management`
- Local modifications: `build.sh`, `kernel/kernel.c`, `kernel/linker.ld`, 5 screenshot PNGs
- Untracked: `kernel/arch/`, `kernel/include/`, `kernel/mm/` (new kernel source), `aurora-glass/screenshots/home-390-light-soft.png`, several `AuroraOS-*` directories, `opencode.json`
- `AGENTS.md`: previously missing — now created (this file)
- CI: `.github/workflows/ci.yml` with 6 jobs (ui-tests, ui-browser-tests, prototype-build, kernel-build, kernel-qemu-boot, repo-integrity)
- Kernel: memory management subsystem written (allocator, MMU, page tables, exceptions, fault handler) — code complete, **not yet built/tested locally** (no cross-compiler/QEMU on Windows; CI verifies)
- UI prototype: 81 jsdom tests, previously passed

## Build and test commands

- Kernel: `./build.sh` (requires aarch64-linux-gnu cross toolchain — not available on Windows host; verified in CI)
- QEMU boot: `./run-qemu.sh [kernel-image]` (requires qemu-system-aarch64 — verified in CI)
- Automated boot smoke test: `./tests/qemu/boot-smoke-test.sh`
- UI unit tests: `node tests/unit/aurora-glass.test.js` (Node.js, jsdom)
- UI browser tests: `npx playwright test` (requires Playwright Chromium)
