# OpenCode prompt — Aurora Glass UI using official OpenDesign

You are building the **design prototype only** for AuroraOS, an independent ARM64 OS. Use official https://github.com/nexu-io/open-design with my existing OpenCode workflow. Do not use similarly named forks as the default source. Respect the repo's current README/QUICKSTART and pin the chosen release or commit.

Before touching files, read:
- `DESIGN.md` (canonical Aurora Glass visual contract)
- `AuroraOS_Final_Blueprint_OpenDesign.md` (architecture, OpenDesign use, technical limitations)
- `AuroraOS Folio.md` (historical functional flows and permission behavior, NOT its old flat paper visuals)

Goal: a unique design that combines Windows-like workspace/taskbar productivity with polished iOS-inspired glassmorphism, without copying Apple or Microsoft assets. Keep system behavior and Aura AI architecture unchanged.

Task scope, in order:
1. Audit existing design/OS directories and OpenDesign availability. Keep the external open-design repo/tool separate from AuroraOS. Do not overwrite existing sources or agent configuration.
2. If OpenDesign is already available, connect using the upstream documented OpenCode MCP integration (`od mcp install opencode` after verifying the real `od` command), or document the missing prerequisite rather than guessing paths. Never run unreviewed network install scripts or ask for account passwords.
3. Load `DESIGN.md` into OpenDesign as the project design system.
4. Generate an interactive **browser-only prototype** with reusable components and mock state for Home/Desk, Aurora Dock, Spaces selector, Index search, and Aura's editable numbered Plan sheet, confirmation, failure and receipt.
5. Deliver both **Clear** (fully opaque, no blur) and **Soft Glass** (translucent) variants, light and dark mode, 360/390/412px mobile preview, keyboard navigation, 200% type reflow, high contrast, and reduced motion.
6. Validate every visible control and state transition with automated tests and screenshots where available. Record missing or incomplete interactions instead of claiming success.
7. Export HTML/CSS/JS prototype, tokens, screenshots and a handoff mapping each component to the future native AuroraOS compositor. Do not introduce a dependency on React/browser APIs in the real OS.
8. Report exact files changed, commands run, test results, the pinned OpenDesign version, and ONE next UI task.

Nonnegotiable: NO kernel code; NO Android ROM changes; NO firmware builds; NO real device actions, USB flashing, bootloader unlocks, rooting, or data wipes. Do not push or merge without checking repository identity and permission. This is a visual UI design task, not OS implementation.
