# AuroraOS — Master Engineering Checklist

**Dependency-aware.** Foundational kernel tasks first; security and broken builds next; then native OS functionality. Independent tasks are grouped for concurrent execution.

**Legend:** `[ ]` not started, `[~]` in progress, `[x]` complete, `[B]` blocked, `[R]` awaiting review

---

## Phase 0 — Foundation & Recovery (must complete first)

| ID | Task | Dependencies | Owner | Status |
|----|------|--------------|-------|--------|
| P0-1 | Create AGENTS.md (governance file — was missing) | — | fast-worker | [x] |
| P0-2 | Create MASTER_CHECKLIST.md (this document) | — | coordinator | [x] |
| P0-3 | Audit git status, local changes, untracked files | — | coordinator | [x] |
| P0-4 | Review kernel memory-management code for correctness (static review; no cross-compiler on Windows) | P0-3 | kernel-worker | [x] |
| P0-5 | Verify kernel build passes in CI (kernel-build job) | P0-4 | CI / kernel-worker | [B] |
| P0-6 | Verify QEMU boot smoke test passes in CI (kernel-qemu-boot job) | P0-5 | CI / kernel-worker | [ ] |
| P0-7 | Run Node.js UI unit tests locally (jsdom, 81 tests) | — | fast-worker | [ ] |
| P0-8 | Fix repo-integrity CI job (fails when untracked files present) | P0-3 | fast-worker | [ ] |

---

## Ported Kernel Subsystems

| ID | Task | Dependencies | Owner | Status |
|----|------|--------------|-------|--------|
| K-001 | Port boot/aarch64/start.S (Image header, EL2→EL1, dual stack) | P0-4 | kernel-worker | [x] |
| K-002 | Port kernel/arch/aarch64/vectors.S + exceptions.c (EL1 VBAR, exc_frame, SP_EL1 fix) | K-001 | kernel-worker | [x] |
| K-003 | Port kernel/mm/{pmm.c,vmm.c,mmpolicy.c,fdt.c} (W^X MMU, FDT parsing) | K-002 | kernel-worker | [x] |
| K-004 | Port kernel/proc/ (EL0 processes, syscalls, scheduler, IPC) | K-003 | kernel-worker | [~] |
| K-005 | Port kernel/drivers/ (virtio-blk, virtio-input, fwcfg, virtqueue) | K-004 | kernel-worker | [ ] |

---

## Phase 1 — Kernel: Interrupts, Timers, Syscalls (foundational)

| ID | Task | Dependencies | Owner | Status |
|----|------|--------------|-------|--------|
| P1-1 | Implement ARM64 IRQ exception handling (extend exceptions.S) | P0-6 | kernel-worker | [ ] |
| P1-2 | Implement timer driver and timer interrupt handler | P1-1 | kernel-worker | [ ] |
| P1-3 | Implement syscall interface (svc handler, syscall numbers, dispatch) | P1-2 | kernel-worker | [ ] |
| P1-4 | Implement basic scheduler (context switch, round-robin, thread struct) | P1-3 | kernel-worker | [ ] |
| P1-5 | Implement userspace program loader (ELF parsing, load into user address space) | P1-4 | kernel-worker | [ ] |
| P1-6 | Implement IPC mechanism (message passing between processes) | P1-5 | kernel-worker | [ ] |
| P1-7 | Kernel unit tests: scheduler, syscalls, IPC | P1-6 | kernel-worker | [ ] |

---

## Phase 2 — Native OS: Graphics, Input, Init, Storage

| ID | Task | Dependencies | Owner | Status |
|----|------|--------------|-------|--------|
| P2-1 | QEMU display/framebuffer driver (virtio-gpu or simple framebuffer) | P1-7 | implementation-worker | [ ] |
| P2-2 | Tier 0 Clear renderer/compositor (fill, text, rounded rect, border, clipping) | P2-1 | implementation-worker | [ ] |
| P2-3 | Input driver (keyboard via QEMU) | P2-1 | implementation-worker | [ ] |
| P2-4 | Init process (launch system processes, crash recovery) | P1-7 | implementation-worker | [ ] |
| P2-5 | Storage/filesystem driver (start with simple tested FS) | P2-4 | implementation-worker | [ ] |
| P2-6 | Program loader + userspace app runtime | P2-5 | implementation-worker | [ ] |
| P2-7 | Native OS integration boot: renders Tier 0 screen in QEMU | P2-6 | implementation-worker | [ ] |

---

## Phase 3 — Core Apps (native)

| ID | Task | Dependencies | Owner | Status |
|----|------|--------------|-------|--------|
| P3-1 | Notes app | P2-7 | implementation-worker | [ ] |
| P3-2 | Calculator app | P2-7 | implementation-worker | [ ] |
| P3-3 | Clock/Timer app | P2-7 | implementation-worker | [ ] |
| P3-4 | Files app | P2-5, P2-7 | implementation-worker | [ ] |
| P3-5 | Settings app | P2-7 | implementation-worker | [ ] |

---

## Phase 4 — Security & Aura (offline assistant)

| ID | Task | Dependencies | Owner | Status |
|----|------|--------------|-------|--------|
| P4-1 | Action Broker (typed intents, schema validation) | P2-7 | implementation-worker | [ ] |
| P4-2 | Passport permissions (grants, access log, deny-by-default) | P4-1 | implementation-worker | [ ] |
| P4-3 | Trail audit (version history, recovery records) | P2-5 | implementation-worker | [ ] |
| P4-4 | Quick Commands (deterministic command interpreter) | P4-1 | implementation-worker | [ ] |
| P4-5 | Aura Plan sheet (numbered editable steps, S0–S3 risk tiers) | P4-1, P4-2 | implementation-worker | [ ] |
| P4-6 | Vault file management | P2-5, P4-2 | implementation-worker | [ ] |

---

## Phase 5 — UI Prototype & OpenDesign (host-side, independent)

| ID | Task | Dependencies | Owner | Status |
|----|------|--------------|-------|--------|
| P5-1 | Aurora Glass prototype: Home/Desk, Dock, Spaces, Index | — | implementation-worker | [ ] |
| P5-2 | Aura Plan interaction (plan/confirmation/receipt flow) | P5-1 | implementation-worker | [ ] |
| P5-3 | Prototype tests: 81 jsdom tests + Playwright E2E | P5-1 | fast-worker | [ ] |
| P5-4 | Native compositor mapping (design-to-kernel handoff) | P5-1, P2-2 | fast-worker | [ ] |

---

## Phase 6 — CI, Testing, Release

| ID | Task | Dependencies | Owner | Status |
|----|------|--------------|-------|--------|
| P6-1 | CI: kernel-build + kernel-qemu-boot passing on main | P0-6 | fast-worker | [ ] |
| P6-2 | CI: UI unit + browser tests passing on main | P5-3 | fast-worker | [ ] |
| P6-3 | QEMU regression test suite (boot + smoke) | P0-6 | fast-worker | [ ] |
| P6-4 | Public QEMU beta release artifacts | P6-1, P6-2 | coordinator | [ ] |

---

## Concurrent execution groups (no shared files)

**Group A (kernel, can run with Group B):**
- P1-1 through P1-7 (kernel-worker only — sequential within group)

**Group B (native OS, independent of kernel):**
- P2-1 through P2-7 (implementation-worker)
- P3-1 through P3-5 (implementation-worker, after P2-7)
- P4-1 through P4-6 (implementation-worker, after P2-7)

**Group C (fast, independent):**
- P0-7, P0-8, P5-3, P5-4, P6-1, P6-2, P6-3 (fast-worker)

**Blocking note:** Phase 2 depends on Phase 1 (kernel syscalls/scheduler). Phase 3-4 depend on Phase 2. Phase 5 is independent of kernel work.