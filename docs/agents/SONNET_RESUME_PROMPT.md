# Resume prompt (paste this into a new Claude Code session)

You are continuing UI work on AuroraOS in `C:\Users\satvi\Desktop\AuroraOS-Claude`. Everything you need is
checked in; do not rely on any earlier conversation.

1. Read `CLAUDE.md` (rules, commands, definition of done).
2. Read `docs/agents/AURORA_UI_HANDOFF.md` (current state, open PRs, exact next task).
3. Read `docs/agents/AURORA_DESIGN_SYSTEM.md` and `docs/agents/AURORA_ARCHITECTURE.md`.
4. Read `docs/agents/AURORA_UI_TASKS.md` and pick the first task that is not `done` and whose dependencies are met.
5. Inspect the repository: `git status`, `git branch -vv`, `git fetch`, `gh pr list --state open`.
   Preserve any uncommitted work (commit it to its branch or leave it; never discard it).
6. Verify the latest CI for the branch you will build on: `gh pr checks <number>`. If it is red, fixing it is
   the first task.
7. Create or check out the branch named in the task. UI milestones are stacked: branch from the branch the
   task depends on (see the handoff), or from `main` if that dependency has merged.
8. Implement the task following the existing patterns: `AuroraUI` primitives, `components.css` classes, the
   "Adding an app" checklist in `AURORA_ARCHITECTURE.md` section 9, sensitive actions only through
   `AuroraSystem.request`. Do not re-architect, add dependencies, or weaken `broker.js` / `review.js`.
9. Verify: unit tests, `node --check`, `npx playwright test --workers=1`, and look at real screenshots at
   360/390/768/1280 in light and dark (commands in `docs/agents/AURORA_TESTING.md`).
10. Commit, push, open or update the pull request (target `main`, note stacked dependencies), check CI, then
    update `AURORA_UI_HANDOFF.md` and `AURORA_UI_TASKS.md` with the new SHA, PR, CI link and next task.

Project model: one human owner, no external reviewer required. Merge only after a documented self-review, all CI jobs green,
the PR mergeable and no GitHub-enforced block; kernel/security-sensitive changes also need deeper verification and negative tests.

Never: touch a physical device (QEMU only), force-push, bypass GitHub-enforced protection, merge kernel PRs #28/#29 before their
review findings are resolved, hide failing tests, or call the browser prototype a native OS feature. If something cannot proceed safely,
commit what is done, record why in the handoff, and take the next independent task.
